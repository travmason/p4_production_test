/*
 * dash_can - CAN bus receive.
 *
 * Today this listens for screen-selection commands from the rotary controller.
 * It is also the intended home for real vehicle signal decoding: frames arrive
 * here, get decoded, and are pushed into dash_signals, at which point
 * dash_source_sim can be switched off and nothing in the UI needs to change.
 *
 * HARDWARE NOTE
 * -------------
 * The ESP32-P4-PC has no CAN transceiver fitted. The P4's TWAI controller
 * provides logic-level TX/RX only, so an external 3.3 V transceiver
 * (SN65HVD230, TJA1051T/3, MCP2562FD or similar) is required between these
 * pins and the bus. Do not connect these GPIOs to CAN_H/CAN_L directly.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Default wiring, all on the 20-pin EXT1 header, which is the only place the
 * board brings out unassigned GPIOs. GPIO34-38 are avoided because they are the
 * P4's strapping pins, and GPIO35 is already the BOOT button.
 */
#define DASH_CAN_DEFAULT_TX_GPIO   15   /* EXT1 pin 9  */
#define DASH_CAN_DEFAULT_RX_GPIO   16   /* EXT1 pin 10 */

/** Commands carried in byte 0 of a screen-command frame. */
typedef enum {
    DASH_CAN_SCREEN_NEXT = 0x01,
    DASH_CAN_SCREEN_PREV = 0x02,
    /** 0x10 + n selects screen n outright, so a rotary detent can map 1:1. */
    DASH_CAN_SCREEN_ABS_BASE = 0x10,
} dash_can_screen_cmd_t;

typedef struct {
    int      tx_gpio;
    int      rx_gpio;
    uint32_t bitrate_bps;    /*!< 125000, 250000, 500000 or 1000000 */
    uint32_t screen_cmd_id;  /*!< CAN ID carrying screen commands */
    bool     listen_only;    /*!< true = never ACK or transmit; safe to tap a
                                  live vehicle bus without disturbing it */
} dash_can_cfg_t;

#define DASH_CAN_DEFAULT_CFG() {                     \
        .tx_gpio       = DASH_CAN_DEFAULT_TX_GPIO,   \
        .rx_gpio       = DASH_CAN_DEFAULT_RX_GPIO,   \
        .bitrate_bps   = 500000,                     \
        .screen_cmd_id = 0x600,                      \
        .listen_only   = false,                      \
    }

/**
 * Start the CAN receiver.
 *
 * Returns ESP_OK once the driver is running. With no transceiver fitted the
 * driver still installs; it simply never sees a frame.
 */
esp_err_t dash_can_start(const dash_can_cfg_t *cfg);

/** Frames seen since start, for checking the bus is alive. */
uint32_t dash_can_rx_count(void);

#ifdef __cplusplus
}
#endif
