/*
 * dash_ubx - picks u-blox UBX-NAV-PVT messages out of a byte stream.
 *
 * Both receivers the GPS node supports speak this: a Dragy Pro forwards its
 * u-blox module's NAV-PVT messages over Bluetooth, split across notifications
 * at arbitrary points, and a u-blox module on a UART sends the same bytes
 * directly. Feed whatever arrives, in whatever pieces, and get fixes out.
 *
 * Pure C with no ESP-IDF dependencies; tested on the host.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dash_gps_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_UBX_NAV_PVT_FRAME_LEN  100u   /* 6 header + 92 payload + 2 checksum */

typedef struct {
    uint8_t  buf[DASH_UBX_NAV_PVT_FRAME_LEN];
    size_t   len;          /*!< bytes of a frame collected so far */
    size_t   skip;         /*!< bytes of an unwanted message still to discard */
    uint32_t frames;       /*!< NAV-PVT messages accepted */
    uint32_t bad_checksum;
} dash_ubx_parser_t;

void dash_ubx_init(dash_ubx_parser_t *p);

/**
 * Feed one byte. Returns true when it completed a valid NAV-PVT message, in
 * which case @p fix has been filled in (everything except battery_pct and
 * counter, and the LINKED and SRC flags, which are the caller's).
 */
bool dash_ubx_feed(dash_ubx_parser_t *p, uint8_t byte, dash_gps_fix_t *fix);

/** Decode a complete 100-byte NAV-PVT frame; false if it is not one. */
bool dash_ubx_decode_nav_pvt(const uint8_t *frame, size_t len, dash_gps_fix_t *fix);

#ifdef __cplusplus
}
#endif
