# GPS node

A second, small board for the car: an ESP32-S3 that reads GPS from a Dragy Pro
over Bluetooth and puts speed, position and time on the CAN bus, where the
dashboard reads them. The dashboard's ESP32-P4 has no radio, which is why this
is a separate board.

A u-blox GPS module wired straight to the node can replace the Dragy later. It
is a build option; the dashboard sees the same frames either way.

**Status: builds, never run on hardware.** The Dragy's Bluetooth interface is
not published. This follows what the community worked out on a DRG70
([dragy-dash-esp32](https://github.com/jremick/dragy-dash-esp32)); expect to
need a round of fixes on first contact with your unit. The node logs every step
over USB so that what goes wrong is visible.

## What you need

- An ESP32-S3 development board (any, e.g. ESP32-S3-DevKitC-1).
- A 3.3 V CAN transceiver board (SN65HVD230, TJA1051T/3 or similar) - the same
  kind the dashboard uses.
- 5 V from an ignition-switched supply (a USB car adapter is fine).

## Wiring

| ESP32-S3 | To |
|----------|----|
| GPIO5    | Transceiver TXD (some boards label it CTX or D) |
| GPIO4    | Transceiver RXD (CRX or R) |
| 3V3      | Transceiver VCC |
| GND      | Transceiver GND |
| 5V / USB | Ignition-switched 5 V |

Transceiver CAN_H and CAN_L join the same pair as the ECU and the dashboard.

The bus needs a 120 ohm resistor at each of its **two ends** and nowhere else.
With three devices, the one in the middle must have none: if this node is in
the middle and its transceiver board has a 120 ohm resistor fitted (most do),
remove it or cut its jumper.

The pins are build settings (`idf.py menuconfig`, "GPS node"). Avoid GPIO0, 3,
45 and 46 (strapping), 19 and 20 (USB), and 26-37 on boards with octal flash
or PSRAM.

## Build and flash

    . ~/.espressif/v5.5.2/esp-idf/export.sh
    cd gps_node
    idf.py set-target esp32s3        # first time only
    idf.py build flash monitor

## First run

With the Dragy switched on and **the phone app closed** (the Dragy accepts one
connection, and the app will hold it), the log should read:

    gps_can: CAN up: TX=GPIO5 RX=GPIO4 500000 bit/s, frames 0x610-0x613
    gps_dragy: Bluetooth ready; looking for a Dragy
    gps_dragy: found DRG70-xxxxxx, rssi -52; connecting
    gps_dragy: connected
    gps_dragy: characteristics: stream 12, handshake 15, status 18
    gps_dragy: handshake accepted; waiting for the GNSS stream
    gps_dragy: GNSS stream running

and then a summary line every ten seconds. On the dashboard, speed now follows
GPS and the clock sets itself within a second or two of a fix.

There is no pairing step. The node connects to the first Dragy it finds.

| The log says | Meaning |
|--------------|---------|
| nothing after "looking for a Dragy" | Dragy off, out of range, or advertising under a name this does not recognise. |
| `connection failed` | Usually the phone app has it. Close the app; the node retries every few seconds. |
| `the Dragy service (FD00) was not found` | This unit's interface differs from the DRG70's. Send me the log. |
| `handshake write rejected` | The challenge-response differs on this firmware. Send me the log. |
| `handshake done but nothing is streaming` | Handshake accepted but no data; often the app connecting at the same moment. |
| `tx failures` climbing | Nothing is acknowledging on CAN: dashboard off, wiring, termination or bit rate. |

To use the Dragy app for a timed run, just open it: when the node's connection
drops, the app can connect. Speed on the dashboard falls back to the ECU's
until the app is closed again.

## On the dashboard

The node's frames are defined in
`../components/dash_gps_proto/include/dash_gps_proto.h`:

| ID      | Rate      | Contents |
|---------|-----------|----------|
| `0x610` | every fix | speed, heading, fix type, satellites, flags (twice a second with no fix) |
| `0x611` | 5 Hz      | latitude, longitude |
| `0x612` | 1 Hz      | UTC date and time |
| `0x613` | 1 Hz      | accuracy, altitude, Dragy battery |

## Changing to a u-blox module later

1. Wire the module: TX to GPIO18, GND to GND, VCC to 3V3.
2. Configure the module once, with u-center, to send **UBX-NAV-PVT** on its
   UART at 115200 baud and 10 Hz, saved to its flash. u-blox modules send NMEA
   out of the box, which this does not read. The settings are listed at the top
   of `main/gps_ublox.c`.
3. `idf.py menuconfig` -> GPS node -> GPS receiver -> "u-blox module on a UART",
   then build and flash.

This path is untested too.
