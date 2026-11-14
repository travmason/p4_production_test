#include "dash_ubx.h"

#include <string.h>

#define SYNC1        0xB5
#define SYNC2        0x62
#define CLASS_NAV    0x01
#define ID_NAV_PVT   0x07
#define PVT_PAYLOAD  92u

/* Byte offsets into the NAV-PVT payload (u-blox interface description). */
#define PVT_YEAR      4
#define PVT_MONTH     6
#define PVT_DAY       7
#define PVT_HOUR      8
#define PVT_MIN       9
#define PVT_SEC       10
#define PVT_VALID     11   /* bit0 validDate, bit1 validTime */
#define PVT_FIX_TYPE  20
#define PVT_FLAGS     21   /* bit0 gnssFixOK */
#define PVT_NUM_SV    23
#define PVT_LON       24   /* 1e-7 deg */
#define PVT_LAT       28
#define PVT_HMSL      36   /* mm */
#define PVT_HACC      40   /* mm */
#define PVT_GSPEED    60   /* mm/s */
#define PVT_HEADMOT   64   /* 1e-5 deg */
#define PVT_SACC      68   /* mm/s */

static uint16_t u16le(const uint8_t *d)
{
    return (uint16_t)(d[0] | ((uint16_t)d[1] << 8));
}

static uint32_t u32le(const uint8_t *d)
{
    return (uint32_t)u16le(d) | ((uint32_t)u16le(d + 2) << 16);
}

void dash_ubx_init(dash_ubx_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

bool dash_ubx_decode_nav_pvt(const uint8_t *f, size_t len, dash_gps_fix_t *fix)
{
    if (len != DASH_UBX_NAV_PVT_FRAME_LEN || f[0] != SYNC1 || f[1] != SYNC2 ||
        f[2] != CLASS_NAV || f[3] != ID_NAV_PVT || u16le(f + 4) != PVT_PAYLOAD) {
        return false;
    }
    /* 8-bit Fletcher over class, id, length and payload. */
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < len - 2; i++) {
        a = (uint8_t)(a + f[i]);
        b = (uint8_t)(b + a);
    }
    if (a != f[len - 2] || b != f[len - 1]) {
        return false;
    }

    const uint8_t *p = f + 6;
    const uint8_t fix_type = p[PVT_FIX_TYPE];
    const bool fix_ok = (p[PVT_FLAGS] & 0x01) != 0 && (fix_type == 2 || fix_type == 3 ||
                                                       fix_type == 4);

    /* 4 is GNSS plus dead reckoning: a 3D fix as far as the dash cares. 1
       (dead reckoning only) and 5 (time only) are no fix. */
    fix->fix_type   = fix_type == 2 ? 2 : (fix_type == 3 || fix_type == 4) ? 3 : 0;
    fix->satellites = p[PVT_NUM_SV];

    fix->flags &= (uint8_t)~(DASH_GPS_FLAG_FIX_OK | DASH_GPS_FLAG_TIME_VALID);
    if (fix_ok) {
        fix->flags |= DASH_GPS_FLAG_FIX_OK;
    }
    if ((p[PVT_VALID] & 0x03) == 0x03) {
        fix->flags |= DASH_GPS_FLAG_TIME_VALID;
    }

    const int32_t gspeed = (int32_t)u32le(p + PVT_GSPEED);
    fix->speed_kmh = fix_ok && gspeed > 0 ? (float)gspeed * 0.0036f : 0.0f;

    float heading = (float)(int32_t)u32le(p + PVT_HEADMOT) * 1e-5f;
    while (heading < 0.0f) {
        heading += 360.0f;
    }
    while (heading >= 360.0f) {
        heading -= 360.0f;
    }
    fix->heading_deg = heading;

    fix->longitude_deg = (double)(int32_t)u32le(p + PVT_LON) * 1e-7;
    fix->latitude_deg  = (double)(int32_t)u32le(p + PVT_LAT) * 1e-7;

    fix->year   = u16le(p + PVT_YEAR);
    fix->month  = p[PVT_MONTH];
    fix->day    = p[PVT_DAY];
    fix->hour   = p[PVT_HOUR];
    fix->minute = p[PVT_MIN];
    fix->second = p[PVT_SEC];

    fix->h_accuracy_m       = (float)u32le(p + PVT_HACC) * 0.001f;
    fix->speed_accuracy_kmh = (float)u32le(p + PVT_SACC) * 0.0036f;

    int32_t alt = (int32_t)u32le(p + PVT_HMSL) / 1000;
    if (alt > 32767) {
        alt = 32767;
    } else if (alt < -32768) {
        alt = -32768;
    }
    fix->altitude_m = (int16_t)alt;
    return true;
}

bool dash_ubx_feed(dash_ubx_parser_t *p, uint8_t byte, dash_gps_fix_t *fix)
{
    /* In the body of a message that is not NAV-PVT: let it go by. Its bytes
       are not searched for a sync pattern, which a payload may well contain. */
    if (p->skip > 0) {
        p->skip--;
        return false;
    }

    if (p->len == 0) {
        if (byte == SYNC1) {
            p->buf[p->len++] = byte;
        }
        return false;
    }
    if (p->len == 1) {
        if (byte == SYNC2) {
            p->buf[p->len++] = byte;
        } else {
            p->len = (byte == SYNC1) ? 1 : 0;
        }
        return false;
    }

    p->buf[p->len++] = byte;
    if (p->len < 6) {
        return false;
    }
    if (p->len == 6) {
        const size_t payload = u16le(p->buf + 4);
        if (p->buf[2] != CLASS_NAV || p->buf[3] != ID_NAV_PVT || payload != PVT_PAYLOAD) {
            /* Some other message. Skip its payload and checksum - unless the
               length is absurd, which means this was not a header at all. */
            p->skip = payload <= 1024 ? payload + 2 : 0;
            p->len = 0;
        }
        return false;
    }
    if (p->len < DASH_UBX_NAV_PVT_FRAME_LEN) {
        return false;
    }

    p->len = 0;
    if (!dash_ubx_decode_nav_pvt(p->buf, DASH_UBX_NAV_PVT_FRAME_LEN, fix)) {
        p->bad_checksum++;
        return false;
    }
    p->frames++;
    return true;
}
