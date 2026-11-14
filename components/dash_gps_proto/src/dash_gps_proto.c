#include "dash_gps_proto.h"

#include <stddef.h>
#include <string.h>

static void put16(uint8_t *d, int at, uint32_t v)
{
    d[at]     = (uint8_t)(v & 0xFF);
    d[at + 1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put32(uint8_t *d, int at, uint32_t v)
{
    put16(d, at, v & 0xFFFF);
    put16(d, at + 2, v >> 16);
}

static uint16_t get16(const uint8_t *d, int at)
{
    return (uint16_t)(d[at] | ((uint16_t)d[at + 1] << 8));
}

static uint32_t get32(const uint8_t *d, int at)
{
    return (uint32_t)get16(d, at) | ((uint32_t)get16(d, at + 2) << 16);
}

/* Scale to hundredths in 16 bits, rounding and saturating. */
static uint32_t centi16(float v)
{
    if (!(v > 0.0f)) {          /* also catches NaN */
        return 0;
    }
    const float scaled = v * 100.0f + 0.5f;
    return scaled >= 65535.0f ? 65535u : (uint32_t)scaled;
}

static int32_t deg_e7(double deg)
{
    const double scaled = deg * 1e7;
    return (int32_t)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
}

void dash_gps_pack_motion(const dash_gps_fix_t *f, uint8_t d[8])
{
    float heading = f->heading_deg;
    if (!(heading >= 0.0f) || heading >= 360.0f) {
        heading = 0.0f;
    }
    put16(d, 0, centi16(f->speed_kmh));
    put16(d, 2, centi16(heading));
    d[4] = f->fix_type;
    d[5] = f->satellites;
    d[6] = f->flags;
    d[7] = f->counter;
}

void dash_gps_pack_position(const dash_gps_fix_t *f, uint8_t d[8])
{
    put32(d, 0, (uint32_t)deg_e7(f->latitude_deg));
    put32(d, 4, (uint32_t)deg_e7(f->longitude_deg));
}

void dash_gps_pack_time(const dash_gps_fix_t *f, uint8_t d[8])
{
    put16(d, 0, f->year);
    d[2] = f->month;
    d[3] = f->day;
    d[4] = f->hour;
    d[5] = f->minute;
    d[6] = f->second;
    d[7] = (f->flags & DASH_GPS_FLAG_TIME_VALID) ? 1 : 0;
}

void dash_gps_pack_quality(const dash_gps_fix_t *f, uint8_t d[8])
{
    put16(d, 0, centi16(f->h_accuracy_m));
    put16(d, 2, centi16(f->speed_accuracy_kmh));
    put16(d, 4, (uint16_t)f->altitude_m);
    d[6] = f->battery_pct;
    d[7] = 0;
}

bool dash_gps_unpack(uint32_t id, const uint8_t *d, uint8_t dlc, dash_gps_fix_t *f)
{
    if (d == NULL || f == NULL || dlc < 8) {
        return false;
    }
    switch (id) {
    case DASH_GPS_ID_MOTION:
        f->speed_kmh   = (float)get16(d, 0) * 0.01f;
        f->heading_deg = (float)get16(d, 2) * 0.01f;
        f->fix_type    = d[4];
        f->satellites  = d[5];
        /* The time-valid bit belongs to the time frame; keep what it said. */
        f->flags       = (uint8_t)((d[6] & ~DASH_GPS_FLAG_TIME_VALID) |
                                   (f->flags & DASH_GPS_FLAG_TIME_VALID));
        f->counter     = d[7];
        return true;

    case DASH_GPS_ID_POSITION:
        f->latitude_deg  = (double)(int32_t)get32(d, 0) * 1e-7;
        f->longitude_deg = (double)(int32_t)get32(d, 4) * 1e-7;
        return true;

    case DASH_GPS_ID_TIME:
        f->year   = get16(d, 0);
        f->month  = d[2];
        f->day    = d[3];
        f->hour   = d[4];
        f->minute = d[5];
        f->second = d[6];
        if (d[7] & 1) {
            f->flags |= DASH_GPS_FLAG_TIME_VALID;
        } else {
            f->flags &= (uint8_t)~DASH_GPS_FLAG_TIME_VALID;
        }
        return true;

    case DASH_GPS_ID_QUALITY:
        f->h_accuracy_m       = (float)get16(d, 0) * 0.01f;
        f->speed_accuracy_kmh = (float)get16(d, 2) * 0.01f;
        f->altitude_m         = (int16_t)get16(d, 4);
        f->battery_pct        = d[6];
        return true;

    default:
        return false;
    }
}
