/*
 * dash_gps_proto - the CAN frames the GPS node sends to the dashboard.
 *
 * Shared by both ends: the ESP32-S3 GPS node (gps_node/) packs these and the
 * dashboard unpacks them, from this one file, so the two cannot drift apart.
 * Pure C with no ESP-IDF dependencies; tested on the host.
 *
 * The frames say nothing about where the fix came from. Today that is a Dragy
 * Pro over Bluetooth; a u-blox module wired to the node's UART fills in the
 * same dash_gps_fix_t and nothing on the dashboard changes.
 *
 * Bus: 500 kbit/s, 11-bit IDs, all values little-endian (as rusEFI's are).
 *
 *   ID     rate        contents
 *   0x610  every fix   motion: speed, heading, fix type, satellites, flags
 *                      (also sent twice a second with no fix, as a heartbeat)
 *   0x611  5 Hz        position: latitude, longitude
 *   0x612  1 Hz        UTC date and time
 *   0x613  1 Hz        quality: accuracy, altitude, receiver battery
 *
 * 0x600 is the dashboard's command ID and 0x200-0x20B is the ECU, so 0x610
 * upward is clear of both.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_GPS_ID_MOTION    0x610u
#define DASH_GPS_ID_POSITION  0x611u
#define DASH_GPS_ID_TIME      0x612u
#define DASH_GPS_ID_QUALITY   0x613u

/** Bits in dash_gps_fix_t.flags, and in byte 6 of the motion frame. */
typedef enum {
    DASH_GPS_FLAG_FIX_OK     = 1u << 0,  /*!< position and speed are usable */
    DASH_GPS_FLAG_TIME_VALID = 1u << 1,  /*!< UTC date and time are both valid */
    DASH_GPS_FLAG_LINKED     = 1u << 2,  /*!< the node is hearing its receiver */
    DASH_GPS_FLAG_SRC_UBLOX  = 1u << 3,  /*!< wired u-blox; clear = Dragy */
} dash_gps_flag_t;

#define DASH_GPS_BATTERY_UNKNOWN  0xFFu

/** One GPS reading, in the units the frames carry. */
typedef struct {
    /* motion */
    float    speed_kmh;      /*!< 0 .. 655.35 */
    float    heading_deg;    /*!< 0 .. 359.99, direction of travel */
    uint8_t  fix_type;       /*!< 0 none, 2 = 2D, 3 = 3D */
    uint8_t  satellites;
    uint8_t  flags;          /*!< dash_gps_flag_t */
    uint8_t  counter;        /*!< increments every motion frame */

    /* position */
    double   latitude_deg;
    double   longitude_deg;

    /* time, UTC */
    uint16_t year;
    uint8_t  month, day, hour, minute, second;

    /* quality */
    float    h_accuracy_m;   /*!< 0 .. 655.35 */
    float    speed_accuracy_kmh;
    int16_t  altitude_m;     /*!< above mean sea level */
    uint8_t  battery_pct;    /*!< 0..100, or DASH_GPS_BATTERY_UNKNOWN */
} dash_gps_fix_t;

/* Each packs one frame into data[8]; the DLC is always 8. */
void dash_gps_pack_motion(const dash_gps_fix_t *fix, uint8_t data[8]);
void dash_gps_pack_position(const dash_gps_fix_t *fix, uint8_t data[8]);
void dash_gps_pack_time(const dash_gps_fix_t *fix, uint8_t data[8]);
void dash_gps_pack_quality(const dash_gps_fix_t *fix, uint8_t data[8]);

/**
 * Unpack one frame into the matching members of @p fix, leaving the rest.
 * Returns false if @p id is not a GPS frame or the frame is short.
 */
bool dash_gps_unpack(uint32_t id, const uint8_t *data, uint8_t dlc, dash_gps_fix_t *fix);

#ifdef __cplusplus
}
#endif
