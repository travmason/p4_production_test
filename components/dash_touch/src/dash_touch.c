#include "dash_touch.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "esp_private/usb_phy.h"
#include "usb/usb_host.h"

static const char *TAG = "dash_touch";

/* The board's USB-A ports hang off a hub held in reset by GPIO21 until
   released - the same pin the Olimex production test uses. */
#define USB_HUB_RST_GPIO   21

/* HID usages we care about, from the HID Usage Tables. */
#define UP_GENERIC_DESKTOP 0x01
#define UP_DIGITIZER       0x0D
#define USAGE_X            0x30
#define USAGE_Y            0x31
#define USAGE_TIP_SWITCH   0x42

/*
 * Where the interesting fields sit inside an input report.
 *
 * Decoded from the device's own report descriptor rather than assumed: touch
 * controllers differ in field order, in whether they use report IDs, and in
 * their coordinate range. Guessing wrong gives a cursor that lands in the wrong
 * place, or never moves at all.
 */
typedef struct {
    bool     valid;
    uint8_t  report_id;      /* 0 when the device does not use report IDs */
    uint16_t x_bit, x_size;
    uint16_t y_bit, y_size;
    uint16_t tip_bit;        /* UINT16_MAX when the device has no tip switch */
    int32_t  x_max, y_max;
} touch_layout_t;

static struct {
    lv_display_t  *disp;
    lv_indev_t    *indev;
    touch_layout_t layout;

    /* Written by the USB task, read by LVGL. Single writer, single reader, and
       a torn coordinate would only misplace one frame of input. */
    volatile int32_t x, y;
    volatile bool    pressed;
    volatile bool    connected;
} t;

/* ------------------------------------------------------------------------- *
 * HID report descriptor parsing
 * ------------------------------------------------------------------------- */

static uint32_t item_value(const uint8_t *p, int size)
{
    switch (size) {
    case 1:  return p[0];
    case 2:  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    case 4:  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    default: return 0;
    }
}

/*
 * Walk the report descriptor, tracking the running bit offset of each field,
 * and record where X, Y and the tip switch land.
 *
 * Only the first digitizer contact is taken - that is all a UI needs, and it
 * keeps this well short of a full HID parser.
 */
static bool parse_report_descriptor(const uint8_t *d, size_t len, touch_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    out->tip_bit = UINT16_MAX;

    uint16_t usage_page = 0;
    uint32_t usages[16];
    int      usage_cnt = 0;
    uint32_t logical_max = 0;
    uint32_t report_size = 0, report_count = 0;
    uint8_t  report_id = 0;
    uint32_t bit_offset = 0;
    bool     in_digitizer = false;
    bool     got_x = false, got_y = false;

    size_t i = 0;
    while (i < len) {
        const uint8_t item = d[i++];
        const int size = (item & 0x03) == 3 ? 4 : (item & 0x03);
        if (i + (size_t)size > len) {
            break;
        }
        const uint32_t val = item_value(&d[i], size);
        const uint8_t tag = item & 0xFC;
        i += size;

        switch (tag) {
        case 0x04:  /* Usage Page (global) */
            usage_page = (uint16_t)val;
            if (usage_page == UP_DIGITIZER) {
                in_digitizer = true;
            }
            break;
        case 0x08:  /* Usage (local) */
            if (usage_cnt < (int)(sizeof(usages) / sizeof(usages[0]))) {
                usages[usage_cnt++] = ((uint32_t)usage_page << 16) | (val & 0xFFFF);
            }
            break;
        case 0x24:  /* Logical Maximum (global) */
            logical_max = val;
            break;
        case 0x74:  /* Report Size (global) */
            report_size = val;
            break;
        case 0x94:  /* Report Count (global) */
            report_count = val;
            break;
        case 0x84:  /* Report ID (global) - restarts the bit offset */
            if (report_id != (uint8_t)val) {
                report_id = (uint8_t)val;
                bit_offset = 0;
                got_x = got_y = false;
            }
            break;
        case 0x80:  /* Input (main) */
            for (uint32_t f = 0; f < report_count; f++) {
                const uint32_t u = (f < (uint32_t)usage_cnt)
                                   ? usages[f]
                                   : (usage_cnt ? usages[usage_cnt - 1] : 0);
                const uint16_t up = (uint16_t)(u >> 16);
                const uint16_t id = (uint16_t)(u & 0xFFFF);
                const uint32_t at = bit_offset + f * report_size;

                if (in_digitizer && up == UP_GENERIC_DESKTOP && id == USAGE_X && !got_x) {
                    out->x_bit = (uint16_t)at;
                    out->x_size = (uint16_t)report_size;
                    out->x_max = (int32_t)logical_max;
                    out->report_id = report_id;
                    got_x = true;
                } else if (in_digitizer && up == UP_GENERIC_DESKTOP && id == USAGE_Y && !got_y) {
                    out->y_bit = (uint16_t)at;
                    out->y_size = (uint16_t)report_size;
                    out->y_max = (int32_t)logical_max;
                    got_y = true;
                } else if (up == UP_DIGITIZER && id == USAGE_TIP_SWITCH &&
                           out->tip_bit == UINT16_MAX) {
                    out->tip_bit = (uint16_t)at;
                }
            }
            bit_offset += report_size * report_count;
            usage_cnt = 0;
            break;
        case 0x90:  /* Output  */
        case 0xB0:  /* Feature */
            bit_offset += report_size * report_count;
            usage_cnt = 0;
            break;
        case 0xA0:  /* Collection */
        case 0xC0:  /* End Collection */
            usage_cnt = 0;
            break;
        default:
            break;
        }

        if (got_x && got_y) {
            out->valid = true;
        }
    }

    return out->valid && out->x_max > 0 && out->y_max > 0;
}

static uint32_t extract_bits(const uint8_t *data, size_t len, uint32_t bit, uint32_t nbits)
{
    uint32_t v = 0;
    for (uint32_t n = 0; n < nbits && n < 32; n++) {
        const uint32_t b = bit + n;
        if (b / 8 >= len) {
            break;
        }
        if (data[b / 8] & (1u << (b % 8))) {
            v |= (1u << n);
        }
    }
    return v;
}

/* ------------------------------------------------------------------------- *
 * HID callbacks
 * ------------------------------------------------------------------------- */

static void decode_report(const uint8_t *data, size_t len)
{
    const touch_layout_t *l = &t.layout;
    if (!l->valid || t.disp == NULL) {
        return;
    }

    /* When the device uses report IDs, byte 0 is the ID and the descriptor's
       bit offsets are relative to what follows. */
    const uint8_t *body = data;
    size_t body_len = len;
    if (l->report_id != 0) {
        if (len < 1 || data[0] != l->report_id) {
            return;      /* a different report - contact count, features, etc. */
        }
        body++;
        body_len--;
    }

    const uint32_t rx = extract_bits(body, body_len, l->x_bit, l->x_size);
    const uint32_t ry = extract_bits(body, body_len, l->y_bit, l->y_size);
    const bool tip = (l->tip_bit == UINT16_MAX)
                     ? true
                     : extract_bits(body, body_len, l->tip_bit, 1) != 0;

    const int32_t hor = lv_display_get_horizontal_resolution(t.disp);
    const int32_t ver = lv_display_get_vertical_resolution(t.disp);

    int32_t x = (int32_t)((uint64_t)rx * (uint32_t)hor / (uint32_t)l->x_max);
    int32_t y = (int32_t)((uint64_t)ry * (uint32_t)ver / (uint32_t)l->y_max);
    if (x < 0) {
        x = 0;
    } else if (x >= hor) {
        x = hor - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= ver) {
        y = ver - 1;
    }

    t.x = x;
    t.y = y;
    t.pressed = tip;
}

static void iface_event_cb(hid_host_device_handle_t dev,
                           const hid_host_interface_event_t event, void *arg)
{
    (void)arg;
    uint8_t data[64];
    size_t len = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(dev, data, sizeof(data), &len) == ESP_OK) {
            decode_report(data, len);
        }
        break;

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "digitizer disconnected");
        t.connected = false;
        t.pressed = false;
        t.layout.valid = false;
        hid_host_device_close(dev);
        break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "transfer error");
        break;

    default:
        break;
    }
}

static void device_event_cb(hid_host_device_handle_t dev,
                            const hid_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    hid_host_dev_params_t params;
    if (hid_host_device_get_params(dev, &params) != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "HID connected: addr=%d iface=%d subclass=%d proto=%d",
             params.addr, params.iface_num, params.sub_class, params.proto);

    const hid_host_device_config_t cfg = {
        .callback = iface_event_cb,
        .callback_arg = NULL,
    };
    if (hid_host_device_open(dev, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_open failed");
        return;
    }

    size_t desc_len = 0;
    uint8_t *desc = hid_host_get_report_descriptor(dev, &desc_len);
    if (desc == NULL || desc_len == 0) {
        ESP_LOGE(TAG, "no report descriptor - cannot decode this device");
        return;
    }

    touch_layout_t layout;
    if (!parse_report_descriptor(desc, desc_len, &layout)) {
        /* Not a digitizer, or a layout this parser does not understand. Dump
           the descriptor so it can be decoded by hand. */
        ESP_LOGW(TAG, "no digitizer X/Y in this %d byte descriptor; dumping it",
                 (int)desc_len);
        ESP_LOG_BUFFER_HEX(TAG, desc, desc_len > 256 ? 256 : desc_len);
        return;
    }

    t.layout = layout;
    t.connected = true;
    ESP_LOGI(TAG,
             "digitizer decoded: report_id=%d x@bit%u(%ub, max %ld) y@bit%u(%ub, max %ld) tip@bit%u",
             layout.report_id, layout.x_bit, layout.x_size, (long)layout.x_max,
             layout.y_bit, layout.y_size, (long)layout.y_max, layout.tip_bit);

    if (hid_host_device_start(dev) != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_start failed");
    }
}

/* ------------------------------------------------------------------------- *
 * USB host plumbing
 * ------------------------------------------------------------------------- */

static void usb_task(void *arg)
{
    (void)arg;

    /*
     * Default PHY setup - do NOT take USB_PHY_TARGET_INT here.
     *
     * The board's four USB-A sockets hang off an FE1.1s, a USB 1.1 hub, so
     * ESP-IDF sees full-speed devices behind it from a high-speed-capable root
     * and refuses them:
     *
     *   HUB: Connected device is FS, transaction translator (TT) is not supported
     *
     * Forcing the root to full speed would sidestep that, but on the ESP32-P4
     * the internal full-speed PHY is shared with the USB-Serial-JTAG console:
     * claiming it takes the console down with it and the board goes dark. If
     * this is worth pursuing, it needs the high-speed PHY told to negotiate at
     * full speed, or TT support in the host stack - not this.
     */
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    if (usb_host_install(&host_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed");
        vTaskDelete(NULL);
        return;
    }

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = device_event_cb,
        .callback_arg = NULL,
    };
    if (hid_host_install(&hid_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB host up, waiting for a HID digitizer");

    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = t.x;
    data->point.y = t.y;
    data->state = t.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void hub_release_reset(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << USB_HUB_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(USB_HUB_RST_GPIO, 0);    /* reset is active low */
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(USB_HUB_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t dash_touch_start(lv_display_t *disp)
{
    if (disp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    t.disp = disp;
    t.layout.tip_bit = UINT16_MAX;

    hub_release_reset();

    if (xTaskCreate(usb_task, "usb_host", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    t.indev = lv_indev_create();
    lv_indev_set_type(t.indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(t.indev, indev_read_cb);
    lv_indev_set_display(t.indev, disp);

    return ESP_OK;
}

bool dash_touch_is_connected(void)
{
    return t.connected;
}
