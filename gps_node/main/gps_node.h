/*
 * GPS node internals: one fix, shared between whichever receiver is compiled
 * in and the CAN sender.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dash_gps_proto.h"
#include "esp_err.h"

/* ---- gps_can.c ---- */

/** Bring up CAN and start sending. */
esp_err_t gps_can_start(void);

/**
 * A new fix from the receiver. Copies it and wakes the sender, which puts a
 * motion frame on the bus straight away. Safe from any task.
 *
 * The receiver fills in everything it knows; DASH_GPS_FLAG_LINKED and the
 * counter are set here.
 */
void gps_can_publish(const dash_gps_fix_t *fix);

/** The receiver's battery level, 0..100, when it reports one. */
void gps_can_set_battery(uint8_t pct);

/** The receiver went quiet: stop presenting the last fix as current. */
void gps_can_link_lost(void);

/* ---- the receiver: gps_dragy.c or gps_ublox.c ---- */

esp_err_t gps_source_start(void);

/** DASH_GPS_FLAG_SRC_UBLOX or 0, for the motion frame. */
uint8_t gps_source_flag(void);
