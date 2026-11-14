/* Host test for the GPS CAN frames and the UBX NAV-PVT parser.
   Run with `ctest --test-dir sim/build`. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dash_gps_proto.h"
#include "dash_ubx.h"

static int s_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            s_failed++;                                                 \
        }                                                               \
    } while (0)

#define NEAR(a, b, tol)  CHECK(fabs((double)(a) - (double)(b)) <= (tol))

static void put16(uint8_t *d, unsigned v) { d[0] = v & 0xFF; d[1] = (v >> 8) & 0xFF; }
static void put32(uint8_t *d, uint32_t v) { put16(d, v & 0xFFFF); put16(d + 2, v >> 16); }

/* Build a NAV-PVT frame by hand from the u-blox layout. */
static void make_pvt(uint8_t f[100], int32_t gspeed_mm_s, uint8_t fix_type, bool fix_ok)
{
    memset(f, 0, 100);
    f[0] = 0xB5; f[1] = 0x62; f[2] = 0x01; f[3] = 0x07;
    put16(f + 4, 92);
    uint8_t *p = f + 6;
    put16(p + 4, 2026); p[6] = 11; p[7] = 15; p[8] = 9; p[9] = 41; p[10] = 27;
    p[11] = 0x07;                        /* validDate | validTime | fullyResolved */
    p[20] = fix_type;
    p[21] = fix_ok ? 0x01 : 0x00;
    p[23] = 14;
    put32(p + 24, (uint32_t)1512093000);          /* lon 151.2093 */
    put32(p + 28, (uint32_t)(int32_t)-338688000); /* lat -33.8688 */
    put32(p + 36, 58400);                         /* hMSL 58.4 m */
    put32(p + 40, 1250);                          /* hAcc 1.25 m */
    put32(p + 60, (uint32_t)gspeed_mm_s);
    put32(p + 64, 27512345);                      /* headMot 275.12345 deg */
    put32(p + 68, 150);                           /* sAcc 0.15 m/s */
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 98; i++) { a = (uint8_t)(a + f[i]); b = (uint8_t)(b + a); }
    f[98] = a; f[99] = b;
}

static int feed(dash_ubx_parser_t *p, const uint8_t *d, size_t n, dash_gps_fix_t *fix)
{
    int got = 0;
    for (size_t i = 0; i < n; i++) {
        got += dash_ubx_feed(p, d[i], fix);
    }
    return got;
}

int main(void)
{
    uint8_t f[100];
    dash_gps_fix_t fix;
    dash_ubx_parser_t p;

    /* ---- NAV-PVT decode: 27.5 m/s is 99 km/h ---- */
    make_pvt(f, 27500, 3, true);
    memset(&fix, 0, sizeof(fix));
    CHECK(dash_ubx_decode_nav_pvt(f, 100, &fix));
    NEAR(fix.speed_kmh, 99.0, 0.001);
    NEAR(fix.heading_deg, 275.12345, 0.001);
    CHECK(fix.fix_type == 3 && fix.satellites == 14);
    CHECK(fix.flags == (DASH_GPS_FLAG_FIX_OK | DASH_GPS_FLAG_TIME_VALID));
    NEAR(fix.latitude_deg, -33.8688, 1e-9);
    NEAR(fix.longitude_deg, 151.2093, 1e-9);
    CHECK(fix.year == 2026 && fix.month == 11 && fix.day == 15);
    CHECK(fix.hour == 9 && fix.minute == 41 && fix.second == 27);
    NEAR(fix.h_accuracy_m, 1.25, 0.0001);
    NEAR(fix.speed_accuracy_kmh, 0.54, 0.0001);
    CHECK(fix.altitude_m == 58);

    /* No fix: speed is forced to zero whatever the receiver reports. */
    make_pvt(f, 4000, 0, false);
    CHECK(dash_ubx_decode_nav_pvt(f, 100, &fix));
    CHECK(fix.speed_kmh == 0.0f && fix.fix_type == 0);
    CHECK((fix.flags & DASH_GPS_FLAG_FIX_OK) == 0);
    /* Time-only (5) and a 3D type without gnssFixOK are not fixes either. */
    make_pvt(f, 4000, 5, true);
    CHECK(dash_ubx_decode_nav_pvt(f, 100, &fix) && !(fix.flags & DASH_GPS_FLAG_FIX_OK));
    make_pvt(f, 4000, 3, false);
    CHECK(dash_ubx_decode_nav_pvt(f, 100, &fix) && !(fix.flags & DASH_GPS_FLAG_FIX_OK));

    /* A corrupted byte fails the checksum. */
    make_pvt(f, 27500, 3, true);
    f[70] ^= 0x10;
    CHECK(!dash_ubx_decode_nav_pvt(f, 100, &fix));

    /* ---- stream parser ---- */
    /* Two frames with junk before, between and after, fed in 20-byte pieces
       as Bluetooth notifications would deliver them. */
    uint8_t stream[400];
    size_t n = 0;
    const uint8_t junk[] = {0x00, 0xB5, 0x11, 0xB5, 0xB5};   /* false starts */
    memcpy(stream + n, junk, sizeof(junk)); n += sizeof(junk);
    make_pvt(f, 10000, 3, true); memcpy(stream + n, f, 100); n += 100;
    stream[n++] = 0x62; stream[n++] = 0xFF;
    make_pvt(f, 20000, 3, true); memcpy(stream + n, f, 100); n += 100;
    stream[n++] = 0xB5;

    dash_ubx_init(&p);
    int got = 0;
    float last = 0;
    for (size_t at = 0; at < n; at += 20) {
        const size_t len = n - at < 20 ? n - at : 20;
        if (feed(&p, stream + at, len, &fix)) {
            got++;
            last = fix.speed_kmh;
        }
    }
    CHECK(got == 2 && p.frames == 2 && p.bad_checksum == 0);
    NEAR(last, 72.0, 0.001);

    /* Another message type in between is skipped whole, even though its
       payload contains what looks like the start of a NAV-PVT. */
    n = 0;
    const uint8_t other[] = {0xB5, 0x62, 0x01, 0x03, 0x08, 0x00,      /* NAV-STATUS-ish */
                             0xB5, 0x62, 0x01, 0x07, 0x5C, 0x00, 0x01, 0x02,
                             0xAA, 0xBB};
    memcpy(stream + n, other, sizeof(other)); n += sizeof(other);
    make_pvt(f, 30000, 3, true); memcpy(stream + n, f, 100); n += 100;
    dash_ubx_init(&p);
    CHECK(feed(&p, stream, n, &fix) == 1);
    NEAR(fix.speed_kmh, 108.0, 0.001);

    /* A bad frame is counted and the next good one still gets through. */
    n = 0;
    make_pvt(f, 30000, 3, true); f[50] ^= 1; memcpy(stream + n, f, 100); n += 100;
    make_pvt(f, 5000, 3, true); memcpy(stream + n, f, 100); n += 100;
    dash_ubx_init(&p);
    CHECK(feed(&p, stream, n, &fix) == 1 && p.bad_checksum == 1);
    NEAR(fix.speed_kmh, 18.0, 0.001);

    /* ---- CAN frames: pack on the node, unpack on the dash ---- */
    dash_gps_fix_t in = {
        .speed_kmh = 123.45f, .heading_deg = 359.99f, .fix_type = 3, .satellites = 17,
        .flags = DASH_GPS_FLAG_FIX_OK | DASH_GPS_FLAG_TIME_VALID | DASH_GPS_FLAG_LINKED,
        .counter = 200,
        .latitude_deg = -33.8688123, .longitude_deg = 151.2093456,
        .year = 2026, .month = 11, .day = 15, .hour = 23, .minute = 59, .second = 58,
        .h_accuracy_m = 0.87f, .speed_accuracy_kmh = 0.4f, .altitude_m = -12,
        .battery_pct = 84,
    };
    dash_gps_fix_t out;
    memset(&out, 0, sizeof(out));
    uint8_t d[8];
    /* Time first, so the motion frame has a time-valid bit to preserve. */
    dash_gps_pack_time(&in, d);     CHECK(dash_gps_unpack(DASH_GPS_ID_TIME, d, 8, &out));
    dash_gps_pack_motion(&in, d);   CHECK(dash_gps_unpack(DASH_GPS_ID_MOTION, d, 8, &out));
    dash_gps_pack_position(&in, d); CHECK(dash_gps_unpack(DASH_GPS_ID_POSITION, d, 8, &out));
    dash_gps_pack_quality(&in, d);  CHECK(dash_gps_unpack(DASH_GPS_ID_QUALITY, d, 8, &out));

    NEAR(out.speed_kmh, 123.45, 0.006);
    NEAR(out.heading_deg, 359.99, 0.006);
    CHECK(out.fix_type == 3 && out.satellites == 17 && out.counter == 200);
    CHECK(out.flags == in.flags);
    NEAR(out.latitude_deg, in.latitude_deg, 1e-7);
    NEAR(out.longitude_deg, in.longitude_deg, 1e-7);
    CHECK(out.year == 2026 && out.month == 11 && out.day == 15);
    CHECK(out.hour == 23 && out.minute == 59 && out.second == 58);
    NEAR(out.h_accuracy_m, 0.87, 0.006);
    NEAR(out.speed_accuracy_kmh, 0.4, 0.006);
    CHECK(out.altitude_m == -12 && out.battery_pct == 84);

    /* Out-of-range values saturate rather than wrap. */
    in.speed_kmh = 900.0f; in.heading_deg = 360.0f; in.h_accuracy_m = -1.0f;
    dash_gps_pack_motion(&in, d);  dash_gps_unpack(DASH_GPS_ID_MOTION, d, 8, &out);
    dash_gps_pack_quality(&in, d); dash_gps_unpack(DASH_GPS_ID_QUALITY, d, 8, &out);
    NEAR(out.speed_kmh, 655.35, 0.001);
    CHECK(out.heading_deg == 0.0f && out.h_accuracy_m == 0.0f);

    /* Not ours, or short. */
    CHECK(!dash_gps_unpack(0x600, d, 8, &out));
    CHECK(!dash_gps_unpack(0x614, d, 8, &out));
    CHECK(!dash_gps_unpack(DASH_GPS_ID_MOTION, d, 7, &out));

    if (s_failed == 0) {
        printf("gps: all checks passed\n");
        return 0;
    }
    printf("gps: %d check(s) FAILED\n", s_failed);
    return 1;
}
