# Car instrument cluster on the Olimex ESP32-P4-PC

An LVGL instrument cluster for a 1600x720 bar panel (Waveshare 10.4HP-CAPQLED),
driven over HDMI through the board's LT8912B MIPI-DSI bridge.

This started from Olimex's `p4_production_test` sample, which is why the
directory still carries that name. The original test's entry point is kept for
reference in `main/production_test_main.c.ref`; the test components it used
(display, SD card, USB flash, audio, LAN) have been removed and are in the git
history.

## Status

- The gauges show either **simulated data** (`dash_source_sim`, a made-up drive
  cycle) or the ECU's, chosen by the data source mode below. A label in the
  top-left corner of every screen marks simulated data and a silent ECU.
- ECU data is rusEFI's native "verbose" CAN broadcast. Not yet tried against a
  real ECU.
- Lights, indicators, odometer and outside temperature have no real source yet.
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

## Running it on the Mac

`sim/` builds the same UI sources into a desktop window, so the graphics can be
seen and changed without the board. It needs SDL2 (`brew install sdl2`) and the
LVGL sources the ESP-IDF component manager downloads, so build the firmware (or
run `idf.py reconfigure`) once first.

    cmake -S sim -B sim/build
    cmake --build sim/build -j
    ./sim/build/dash_sim

Keys: Space / Right / Down for the next screen, Left / Up for the previous one,
1-3 to pick a screen, Esc or Q to quit. Dragging up or down with the mouse
swipes. `--zoom 0.5` changes the window size and `--screen N` picks the starting
screen.

To save a picture without opening a window:

    ./sim/build/dash_sim --screen 1 --screenshot track.bmp --after 20

The simulator shows what the UI draws, not how fast the board draws it. DSI
underruns, PSRAM bandwidth and frame rate can only be judged on the hardware.

## Layout

| Path                         | What it is                                                   |
|------------------------------|--------------------------------------------------------------|
| `main/main.c`                | Start-up: display, UI, touch, buttons, CAN, simulator        |
| `components/dash_signals`    | The vehicle signal model; the seam between data and pixels   |
| `components/dash_source_sim` | Synthetic vehicle feeding the model                          |
| `components/dash_can`        | CAN receive: rusEFI ECU data and screen commands              |
| `components/dash_ui`         | The three LVGL screens: cluster, track, data                 |
| `components/dash_assets`     | Artwork, embedded as binary, plus measured geometry          |
| `components/dash_touch`      | USB HID digitizer as an LVGL pointer                         |
| `components/esp-bsp`         | Vendored Espressif BSP, with 1600x720 HDMI modes added       |
| `tools/make_assets.py`       | Turns `assets/dash_bg_source.png` into the embedded assets   |
| `components/dash_source`     | Chooses simulator or ECU; keeps the mode in flash            |
| `sim/`                       | Mac simulator: SDL window plus host stand-ins for ESP-IDF    |

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

- BOOT button or the GPIO14 button, pressed briefly: next screen.
- Swipe up / down, when touch is in use.
- CAN: a frame on ID `0x600` at 500 kbit/s, command in byte 0:
  `0x01` next, `0x02` previous, `0x10 + n` selects screen `n`.

## Data source

A label in the top-left corner appears whenever the numbers are not a
healthy live feed: amber **SIM**, or red **NO ECU** (live selected, ECU silent).
With the ECU feeding the gauges nothing is shown.

| Mode | Behaviour |
|------|-----------|
| auto (default) | Simulated until the ECU is first heard, then live until power-off. It never goes back to simulated data by itself. |
| simulated | Always simulated, even with the ECU connected. |
| live | Never simulated. With no ECU the gauges read zero and the chip says NO ECU. **Leave the car in this mode.** |

The mode is kept in flash. To step through auto, simulated, live:

- press and hold the top-left corner of the touch screen, where the label
  appears (it responds even when nothing is shown);
- hold either screen button for 1.5 seconds;
- or send CAN ID `0x600` with byte 0 = `0x20` auto, `0x21` simulated, `0x22` live.

In the Mac simulator, `M` steps the mode and `E` connects a pretend ECU;
`--source live` and `--ecu` set them from the command line.

## ECU data

The dash reads rusEFI's native "verbose" broadcast: twelve frames on IDs
`0x200`-`0x20B`. Set these in TunerStudio's CAN settings:

| Setting                      | Value        |
|------------------------------|--------------|
| CAN baud rate                | 500 kbit/s   |
| rusEFI (verbose) CAN broadcast | enabled    |
| Verbose CAN base address     | `0x200` (512) |
| ID length                    | 11 bit       |

A Haltech (or other) dash broadcast can stay enabled alongside it, but Haltech
forces the bus to 1 Mbit/s, so turn it off to run at 500 kbit/s.

Taken from the ECU: RPM, vehicle speed, coolant temperature, oil temperature
and pressure, manifold pressure (shown as boost against a standard atmosphere),
fuel level, gear, check-engine, and a charge warning when the engine is turning
below 12.0 V. The trip meter shows the ECU's distance since power-up.

If the ECU stops sending for one second, what it supplied drops to zero and the
label reads NO ECU, rather than the last values staying frozen. Speed and
distance are meant to come from the GPS node on the same bus, with the ECU's
vehicle speed as the fallback, so an ECU fault will not zero them once that
node exists. Until it does, the ECU is the only speed source and speed drops
to zero with it.

The decoder is `components/dash_can/src/dash_rusefi.c`. It is tested on the Mac
against the layout in rusEFI's `rusEFI_CAN_verbose.dbc`:

    ctest --test-dir sim/build

The CAN defaults are in `components/dash_can/include/dash_can.h`. The dash runs
in normal mode, so it acknowledges frames, which a bus of just ECU and dash
needs. `listen_only` is for tapping a bus that already has two other nodes.

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
