/*
 * A Dragy Pro as the receiver, over Bluetooth LE.
 *
 * The Dragy has no published interface. What follows is what the community
 * worked out by watching one (github.com/jremick/dragy-dash-esp32, Apache-2.0,
 * observed on a DRG70) and re-implemented here; nothing is copied from it. It
 * may differ on other models or firmware, so every step logs what it finds.
 *
 *   service 0xFD00
 *     0xFD02  notify        the GNSS stream: u-blox UBX-NAV-PVT messages,
 *                           split across notifications at arbitrary points
 *     0xFD03  read, write   the handshake
 *     0xFD04  read, notify  status; byte 1 is the battery percentage
 *
 *   1. scan for a device named "DRG..." / "...Dragy..." or advertising 0xFD00
 *   2. connect, find the three characteristics
 *   3. subscribe to FD02 (and FD04)
 *   4. read FD03: the first two bytes are a challenge, a and b
 *   5. write FD03: a, b, a XOR b, a AND b
 *   6. NAV-PVT messages start arriving on FD02
 *
 * Nothing else is ever written to the Dragy. In particular no u-blox
 * configuration commands are sent: its receiver is not ours to reconfigure.
 *
 * The Dragy accepts one connection. While this node holds it the phone app
 * cannot connect, and while the phone app holds it this node cannot - it
 * keeps retrying and takes over when the app lets go.
 */

#include <inttypes.h>
#include <string.h>

#include "dash_ubx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps_node.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "gps_dragy";

#define UUID_SERVICE   0xFD00
#define UUID_STREAM    0xFD02
#define UUID_SHAKE     0xFD03
#define UUID_STATUS    0xFD04
#define UUID_CCCD      0x2902

#define SCAN_MS              5000    /* one scan pass */
#define RETRY_MS             3000    /* pause before scanning again */
#define CONNECT_TIMEOUT_MS   10000
#define STREAM_START_MS      8000    /* connected, but no fix arrives */
#define STREAM_STALL_MS      4000    /* was streaming, then stopped */

typedef enum {
    ST_IDLE,        /* nothing in progress; the watchdog starts a scan */
    ST_SCANNING,
    ST_CONNECTING,
    ST_SETUP,       /* discovery, subscription, handshake */
    ST_STREAMING,
} state_t;

/* One characteristic: where its value is, and where its attributes end. */
typedef struct {
    uint16_t val_handle;
    uint16_t end_handle;
    uint16_t cccd_handle;
} chr_t;

static volatile state_t s_state = ST_IDLE;
static uint8_t    s_own_addr_type;
static uint16_t   s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t   s_svc_start, s_svc_end;
static chr_t      s_stream, s_shake, s_status;
static dash_ubx_parser_t s_parser;
static dash_gps_fix_t    s_fix;
static volatile TickType_t s_state_since;
static volatile TickType_t s_last_fix;
static volatile bool       s_got_fix;

uint8_t gps_source_flag(void)
{
    return 0;
}

static void set_state(state_t st)
{
    s_state = st;
    s_state_since = xTaskGetTickCount();
}

static int gap_event(struct ble_gap_event *event, void *arg);

/* Give up on this connection; the disconnect event sends us back to scanning. */
static void drop(const char *why)
{
    ESP_LOGW(TAG, "%s; disconnecting", why);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        set_state(ST_IDLE);
    }
}

/* ------------------------------------------------------------------------- *
 * Scanning
 * ------------------------------------------------------------------------- */

static void start_scan(void)
{
    const struct ble_gap_disc_params params = {
        .passive = 0,            /* active: the name may only be in the scan response */
        .filter_duplicates = 1,
        .itvl = 0,               /* stack defaults */
        .window = 0,
    };
    const int rc = ble_gap_disc(s_own_addr_type, SCAN_MS, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan failed to start: %d", rc);
        set_state(ST_IDLE);
        return;
    }
    set_state(ST_SCANNING);
}

static bool looks_like_dragy(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids16; i++) {
        if (ble_uuid_u16(&f->uuids16[i].u) == UUID_SERVICE) {
            return true;
        }
    }
    if (f->name != NULL && f->name_len >= 3) {
        if (memcmp(f->name, "DRG", 3) == 0) {
            return true;
        }
        for (int i = 0; i + 5 <= f->name_len; i++) {
            if (memcmp(f->name + i, "Dragy", 5) == 0 || memcmp(f->name + i, "dragy", 5) == 0) {
                return true;
            }
        }
    }
    return false;
}

static void on_advert(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0 || !looks_like_dragy(&f)) {
        return;
    }
    ESP_LOGI(TAG, "found %.*s, rssi %d; connecting", f.name_len,
             f.name != NULL ? (const char *)f.name : "", d->rssi);

    /* Scanning has to stop before a connection can start. */
    ble_gap_disc_cancel();
    const int rc = ble_gap_connect(s_own_addr_type, &d->addr, CONNECT_TIMEOUT_MS, NULL,
                                   gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "connect failed to start: %d", rc);
        set_state(ST_IDLE);
        return;
    }
    set_state(ST_CONNECTING);
}

/* ------------------------------------------------------------------------- *
 * Set-up after connecting. Each step's callback starts the next.
 * ------------------------------------------------------------------------- */

static void setup_subscribe_stream(void);
static void setup_subscribe_status(void);
static void setup_read_challenge(void);

static int on_shake_written(uint16_t conn, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (error->status != 0) {
        drop("handshake write rejected");
        return 0;
    }
    ESP_LOGI(TAG, "handshake accepted; waiting for the GNSS stream");
    dash_ubx_init(&s_parser);
    s_got_fix = false;
    set_state(ST_STREAMING);
    return 0;
}

static int on_challenge_read(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (error->status != 0 || attr == NULL || OS_MBUF_PKTLEN(attr->om) < 2) {
        drop("could not read the handshake challenge");
        return 0;
    }
    uint8_t c[2];
    os_mbuf_copydata(attr->om, 0, 2, c);
    const uint8_t reply[4] = { c[0], c[1], (uint8_t)(c[0] ^ c[1]), (uint8_t)(c[0] & c[1]) };

    if (ble_gattc_write_flat(conn, s_shake.val_handle, reply, sizeof(reply),
                             on_shake_written, NULL) != 0) {
        drop("could not send the handshake reply");
    }
    return 0;
}

static void setup_read_challenge(void)
{
    if (ble_gattc_read(s_conn, s_shake.val_handle, on_challenge_read, NULL) != 0) {
        drop("could not start the handshake");
    }
}

static int on_status_subscribed(uint16_t conn, const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "status subscription refused (%d); carrying on without battery level",
                 error->status);
    }
    setup_read_challenge();
    return 0;
}

/* Battery level is a nicety: never let it stop the stream being set up. */
static void setup_subscribe_status(void)
{
    static const uint8_t on[2] = {0x01, 0x00};
    if (s_status.cccd_handle == 0 ||
        ble_gattc_write_flat(s_conn, s_status.cccd_handle, on, sizeof(on),
                             on_status_subscribed, NULL) != 0) {
        setup_read_challenge();
    }
}

static int on_stream_subscribed(uint16_t conn, const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (error->status != 0) {
        drop("stream subscription refused");
        return 0;
    }
    setup_subscribe_status();
    return 0;
}

static void setup_subscribe_stream(void)
{
    static const uint8_t on[2] = {0x01, 0x00};
    if (s_stream.cccd_handle == 0) {
        drop("the stream characteristic has no notification descriptor");
        return;
    }
    if (ble_gattc_write_flat(s_conn, s_stream.cccd_handle, on, sizeof(on),
                             on_stream_subscribed, NULL) != 0) {
        drop("could not subscribe to the stream");
    }
}

/* Descriptors, one characteristic at a time: stream, then status. */
static int on_descriptor(uint16_t conn, const struct ble_gatt_error *error,
                         uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    chr_t *chr = arg;
    (void)chr_val_handle;

    if (error->status == 0) {
        if (ble_uuid_u16(&dsc->uuid.u) == UUID_CCCD) {
            chr->cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        drop("descriptor discovery failed");
        return 0;
    }

    if (chr == &s_stream && s_status.val_handle != 0 &&
        s_status.end_handle > s_status.val_handle) {
        if (ble_gattc_disc_all_dscs(conn, s_status.val_handle, s_status.end_handle,
                                    on_descriptor, &s_status) == 0) {
            return 0;
        }
    }
    setup_subscribe_stream();
    return 0;
}

static int on_characteristic(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg)
{
    /* The characteristic before this one ends where this one's declaration
       begins; arg carries it from one callback to the next. */
    static chr_t *prev;
    (void)arg;

    if (error->status == 0) {
        if (prev != NULL) {
            prev->end_handle = (uint16_t)(chr->def_handle - 1);
            prev = NULL;
        }
        chr_t *slot = NULL;
        switch (ble_uuid_u16(&chr->uuid.u)) {
        case UUID_STREAM: slot = &s_stream; break;
        case UUID_SHAKE:  slot = &s_shake;  break;
        case UUID_STATUS: slot = &s_status; break;
        default: break;
        }
        if (slot != NULL) {
            slot->val_handle = chr->val_handle;
            slot->end_handle = s_svc_end;
            slot->cccd_handle = 0;
            prev = slot;
        }
        return 0;
    }
    prev = NULL;
    if (error->status != BLE_HS_EDONE) {
        drop("characteristic discovery failed");
        return 0;
    }
    if (s_stream.val_handle == 0 || s_shake.val_handle == 0) {
        drop("this device does not have the Dragy stream and handshake characteristics");
        return 0;
    }
    ESP_LOGI(TAG, "characteristics: stream %u, handshake %u, status %u",
             s_stream.val_handle, s_shake.val_handle, s_status.val_handle);

    if (s_stream.end_handle <= s_stream.val_handle ||
        ble_gattc_disc_all_dscs(conn, s_stream.val_handle, s_stream.end_handle,
                                on_descriptor, &s_stream) != 0) {
        drop("could not look for the stream's notification descriptor");
    }
    return 0;
}

static int on_service(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (error->status == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE || s_svc_start == 0) {
        drop("the Dragy service (FD00) was not found");
        return 0;
    }
    if (ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_characteristic, NULL) != 0) {
        drop("could not list the service's characteristics");
    }
    return 0;
}

static void begin_setup(void)
{
    s_svc_start = s_svc_end = 0;
    memset(&s_stream, 0, sizeof(s_stream));
    memset(&s_shake, 0, sizeof(s_shake));
    memset(&s_status, 0, sizeof(s_status));
    set_state(ST_SETUP);

    const ble_uuid16_t uuid = BLE_UUID16_INIT(UUID_SERVICE);
    if (ble_gattc_disc_svc_by_uuid(s_conn, &uuid.u, on_service, NULL) != 0) {
        drop("could not look for the Dragy service");
    }
}

/* ------------------------------------------------------------------------- *
 * Data
 * ------------------------------------------------------------------------- */

static void on_notify(uint16_t attr_handle, struct os_mbuf *om)
{
    uint8_t buf[64];
    const uint16_t total = OS_MBUF_PKTLEN(om);

    if (attr_handle == s_status.val_handle) {
        if (total >= 2 && os_mbuf_copydata(om, 0, 2, buf) == 0) {
            gps_can_set_battery(buf[1]);
        }
        return;
    }
    if (attr_handle != s_stream.val_handle) {
        return;
    }
    for (uint16_t at = 0; at < total;) {
        const uint16_t n = (uint16_t)(total - at < sizeof(buf) ? total - at : sizeof(buf));
        if (os_mbuf_copydata(om, at, n, buf) != 0) {
            return;
        }
        for (uint16_t i = 0; i < n; i++) {
            if (dash_ubx_feed(&s_parser, buf[i], &s_fix)) {
                if (!s_got_fix) {
                    s_got_fix = true;
                    ESP_LOGI(TAG, "GNSS stream running");
                }
                s_last_fix = xTaskGetTickCount();
                gps_can_publish(&s_fix);
            }
        }
        at = (uint16_t)(at + n);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (s_state == ST_SCANNING) {
            on_advert(&event->disc);
        }
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_state == ST_SCANNING) {
            set_state(ST_IDLE);   /* nothing found this pass */
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection failed: %d (is the phone app connected to it?)",
                     event->connect.status);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            set_state(ST_IDLE);
            break;
        }
        s_conn = event->connect.conn_handle;
        ESP_LOGI(TAG, "connected");
        begin_setup();
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason %d)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        gps_can_link_lost();
        set_state(ST_IDLE);
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        on_notify(event->notify_rx.attr_handle, event->notify_rx.om);
        break;

    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Supervision
 * ------------------------------------------------------------------------- */

/* Starts scans, and notices a connection that has stopped being useful. BLE
   calls from here are safe: the NimBLE host API may be used from any task. */
static void watchdog_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        const TickType_t in_state = xTaskGetTickCount() - s_state_since;

        switch (s_state) {
        case ST_IDLE:
            if (in_state >= pdMS_TO_TICKS(RETRY_MS) && ble_hs_synced()) {
                start_scan();
            }
            break;

        case ST_SETUP:
            if (in_state >= pdMS_TO_TICKS(STREAM_START_MS)) {
                drop("set-up did not finish");
            }
            break;

        case ST_STREAMING:
            if (!s_got_fix) {
                if (in_state >= pdMS_TO_TICKS(STREAM_START_MS)) {
                    drop("handshake done but nothing is streaming");
                }
            } else if (xTaskGetTickCount() - s_last_fix >= pdMS_TO_TICKS(STREAM_STALL_MS)) {
                drop("the GNSS stream stopped");
            }
            break;

        default:
            break;
        }
    }
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable Bluetooth address");
        return;
    }
    ESP_LOGI(TAG, "Bluetooth ready; looking for a Dragy");
    /* The watchdog starts the first scan, as it does every later one. */
    set_state(ST_IDLE);
    s_state_since = xTaskGetTickCount() - pdMS_TO_TICKS(RETRY_MS);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "Bluetooth host reset (%d)", reason);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    gps_can_link_lost();
    set_state(ST_IDLE);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t gps_source_start(void)
{
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    set_state(ST_IDLE);
    nimble_port_freertos_init(host_task);

    if (xTaskCreate(watchdog_task, "dragy_wd", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
