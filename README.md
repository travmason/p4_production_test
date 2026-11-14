# Car instrument cluster on the Olimex ESP32-P4-PC

An LVGL instrument cluster for a 1600x720 bar panel (Waveshare 10.4HP-CAPQLED),
driven over HDMI through the board's LT8912B MIPI-DSI bridge.

This started from Olimex's `p4_production_test` sample, which is why the
directory still carries that name. The original test's entry point is kept for
reference in `main/production_test_main.c.ref`; the test components it used
(display, SD card, USB flash, audio, LAN) have been removed and are in the git
history.

## Status

- The cluster shows **simulated data only**. `dash_source_sim` generates a
  drive cycle, including indicators and warning lamps. No vehicle signals are
  decoded from CAN yet.
- CAN is used for screen-selection commands only.
- The clock is seeded from the build time at every boot, so it is wrong after a
  power cycle.
- Touch (USB HID from the panel) is present in the code but parked.

## Build and flash

Tested with ESP-IDF v5.5.2.

    . $IDF_PATH/export.sh
    idf.py build
    idf.py -p PORT flash monitor

`sdkconfig` is generated from `sdkconfig.defaults` and is not tracked. The
target (`esp32p4`), display mode, PSRAM, cache and partition settings are all
pinned there, with the reason for each.

## Layout

| Path                         | What it is                                                   |
|------------------------------|--------------------------------------------------------------|
| `main/main.c`                | Start-up: display, UI, touch, buttons, CAN, simulator        |
| `components/dash_signals`    | The vehicle signal model; the seam between data and pixels   |
| `components/dash_source_sim` | Synthetic vehicle feeding the model                          |
| `components/dash_can`        | CAN receive: screen commands today, signal decoding later    |
| `components/dash_ui`         | The three LVGL screens: cluster, track, data                 |
| `components/dash_assets`     | Artwork, embedded as binary, plus measured geometry          |
| `components/dash_touch`      | USB HID digitizer as an LVGL pointer                         |
| `components/esp-bsp`         | Vendored Espressif BSP, with 1600x720 HDMI modes added       |
| `tools/make_assets.py`       | Turns `assets/dash_bg_source.png` into the embedded assets   |

## Wiring

Everything is on the 20-pin EXT1 header.

| Function            | GPIO | EXT1 pin | Notes                                  |
|---------------------|------|----------|----------------------------------------|
| Screen button       | 14   | 8        | Button to GND (pin 3); internal pull-up |
| CAN TX              | 15   | 9        | To the transceiver's TXD               |
| CAN RX              | 16   | 10       | From the transceiver's RXD             |

The board has **no CAN transceiver**. An external 3.3 V transceiver (SN65HVD230,
TJA1051T/3, MCP2562FD or similar) is required between these pins and the bus.
Do not connect the GPIOs to CAN_H / CAN_L directly.

The on-board BOOT button (GPIO35) also changes screen.

## Changing screen

- BOOT button or the GPIO14 button: next screen.
- Swipe up / down, when touch is in use.
- CAN: a frame on ID `0x600` at 500 kbit/s, command in byte 0:
  `0x01` next, `0x02` previous, `0x10 + n` selects screen `n`.

The CAN defaults are in `components/dash_can/include/dash_can.h`. Note that
`listen_only` defaults to `false`, so the board acknowledges frames. That is
right for a private bus with a single controller on it; set it to `true` before
tapping a vehicle's own bus.

## Changing the artwork

The dashboard face is one PNG. After editing it, regenerate the embedded assets
and the geometry header:

    python tools/make_assets.py assets/dash_bg_source.png

The script measures dial centres, sweep angles and anchor points from the image
and writes `components/dash_assets/include/dash_geometry.h`, so the drawn arcs
stay aligned with the printed scale.

## Display constraints

These are documented where they are enforced (`sdkconfig.defaults` and the BSP),
but in short:

- The P4's DPI pixel clock is 240 MHz divided by an integer, so the panel runs
  at 80 MHz with trimmed blanking to land on 60 Hz.
- The frame buffer must be RGB888. On ESP32-P4 before rev 3.0 an RGB565 frame
  buffer gives a solid blue panel.
- The L2 cache is set to 256 KB with 128-byte lines; at the defaults the DSI
  bridge underruns streaming the frame buffer out of PSRAM.
