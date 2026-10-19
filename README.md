<img src="assets/icon.svg" width="64" height="64" alt="">

# Govee Party for Flipper Zero

Finds nearby Govee Bluetooth LE lights and makes them dance. Or shuts them up.
The polite version of this protocol work lives in the
[Govee-Bluetooth](../govee-bluetooth) Home Assistant integration; this is the
impolite one.

## What it does

| Menu | What happens |
| --- | --- |
| **Dance Party!** | Scans, then visits every Govee light in range: power on, brightness 100 %, a few colour pops, then the blinking scene and the mic-driven *energic* music mode. The light keeps partying after the Flipper walks away. |
| **Live DJ** | Pick one light and stream patterns to it: Rainbow, Strobe, Police, Disco, Pulse, Fire. Left/Right changes the pattern, Up/Down the speed, OK holds the current colour. The Flipper's own LED mirrors the colour. |
| **Govee B-Gone** | Scans, then turns every Govee light in range off. With *B-Gone lurks* on in Settings it keeps rescanning and switching off anything that dares come back on, until you press Back. |

No pairing, no app, no cloud. Govee firmware accepts writes from anyone who
connects, which is the whole joke.

## The ESP32 image: bridge and standalone in one

`esp32/govee_party_esp32/` is a single sketch for an M5Stack ATOM Lite or
ATOMS3 Lite (any ESP32 with Bluetooth works) that does two jobs at once:

- **Bridge for the Flipper.** Plugged into the Flipper's GPIO header it
  answers the line protocol the Flipper app speaks, and the Flipper drives.
- **Standalone party box.** Unplug it, power it from anything, and the
  button does it all:

  | Press | Does |
  | --- | --- |
  | short | Dance Party: every Govee light in range on, 100 %, colour pops, blinking scene, music mode |
  | double | Rave: toggles a streamed rainbow/strobe show that hops between the lights in range |
  | long | Govee B-Gone: every light in range off |
  | held at power-on | Lurk: rescan every ten seconds and switch off anything that is on, until the button is pressed |

The LED shows what it is doing: blue idle, cyan scanning, green connected,
red flash on a failed connection, purple breathing while lurking, the
current colour during a rave. While a button job runs, the Flipper gets
`ERR busy`; while the Flipper holds a session, the button only flashes red.

```sh
arduino-cli core install esp32:esp32
arduino-cli lib install NimBLE-Arduino
arduino-cli compile --fqbn esp32:esp32:m5stack_atoms3 esp32/govee_party_esp32   # or m5stack_atom
arduino-cli upload  --fqbn esp32:esp32:m5stack_atoms3 -p /dev/ttyACM0 esp32/govee_party_esp32
```

Or open it in the Arduino IDE, pick the board, install NimBLE-Arduino 2.x
from the Library Manager and upload. `govee_frames.h` is a straight port of
the Flipper app's `govee_protocol.c`.

## The catch: the Flipper's radio

Stock Flipper firmware (and Momentum, Unleashed, RogueMaster) runs the
STM32WB *Light* BLE stack, which is peripheral-only. It cannot scan for or
connect to other devices, and the Momentum maintainers have said the *Full*
stack does not fit alongside their firmware. So there are two ways to run this:

### Option A: ESP32 bridge (works on any firmware, including Momentum)

An ESP32 with Bluetooth on the GPIO header does the BLE work. The official
Flipper WiFi devboard is an ESP32-S2 and has **no Bluetooth**, so it will not
do. Any classic ESP32, S3, C3 or C6 board will.

1. Flash `esp32/govee_party_esp32/` as described above.
2. Wire it to the Flipper:

   | Flipper pin | ESP32 |
   | --- | --- |
   | 13 (TX) | `BRIDGE_RX_PIN` |
   | 14 (RX) | `BRIDGE_TX_PIN` |
   | 1 (5V) | 5V / VIN |
   | 8, 11 or 18 (GND) | GND |

   **M5Stack boards** are the easy option: pick the board in Arduino and the
   sketch uses Grove Port A automatically. Grove wire colours: black G to
   Flipper GND, red V to Flipper pin 1 (5V), white SDA to Flipper pin 13,
   yellow SCL to Flipper pin 14.

   | Board | RX (from Flipper 13) | TX (to Flipper 14) |
   | --- | --- | --- |
   | ATOM Lite | G26 | G32 |
   | ATOMS3 Lite | G2 | G1 |
   | Core / Core Basic | G21 | G22 |
   | Core2 | G32 | G33 |
   | CoreS3 | G2 | G1 |
   | StickC Plus / Plus2 | G32 | G33 |
   | NanoC6 | G2 | G1 |

   For other boards the defaults are at the top of the sketch (classic
   ESP32: RX 16, TX 17; S3: RX 44, TX 43; C3: RX 20, TX 21; C6: RX 17,
   TX 16). Change them to suit.
3. In the app, Settings → Radio → **ESP32 bridge**. The app switches the 5 V
   rail on, takes over the UART from the expansion-module service, and pings
   the bridge before doing anything.

The bridge speaks a tiny line protocol (`PING`, `SCAN`, `CONN`, `W`, `DISC`);
it is documented at the top of the sketch and in `govee_transport_uart.c`.
An ESPHome proxy and this image are different firmware, so a board can only
be one at a time; reflashing takes seconds.

### Option B: native Flipper BLE (custom firmware build)

Upstream PR [flipperdevices/flipperzero-firmware#4436](https://github.com/flipperdevices/flipperzero-firmware/pull/4436)
adds a BLE central role and GATT client API (`furi_ble/central.h`). It is
open, not merged, and only works with the Full BLE stack installed on the
radio core. If you are happy running that:

```sh
git clone --recursive -b ble-central-role https://github.com/palmdalian/flipperzero-firmware
cd flipperzero-firmware
./fbt flash_usb_full COMPACT=1 DEBUG=0 \
    COPRO_OB_DATA=scripts/ob_custradio.data \
    COPRO_STACK_BIN=stm32wb5x_BLE_Stack_full_fw.bin \
    COPRO_STACK_TYPE=ble_full
./fbt updater_package           # also produces dist/f7-C/flipper-z-f7-sdk-local.zip
```

Then build the app against that SDK:

```sh
ufbt update --local=/path/to/flipperzero-firmware/dist/f7-C/flipper-z-f7-sdk-local.zip
ufbt
```

The native backend is compiled in automatically when the SDK has
`furi_ble/central.h`; on any other SDK it becomes a stub and Settings shows
*Flipper BLE (n/a)*. At runtime the app also checks
`furi_hal_bt_is_central_supported()` so a Light-stack Flipper fails
gracefully rather than hanging the radio.

## Releases

Every push builds the Flipper app (Momentum and official SDKs) and the ESP32
images in GitHub Actions. Tagging `v*` publishes them as a release with
ready-to-flash `.fap` and merged `.bin` files.

## Building the app

```sh
pip install ufbt
ufbt update --index-url https://up.momentum-fw.dev/firmware/directory.json   # Momentum SDK
ufbt                      # builds dist/govee_party.fap
ufbt launch               # copies it over USB and starts it
```

For stock firmware drop the `--index-url` line.

## Protocol notes

Every frame is 20 bytes: `33` write, command, payload zero padded, XOR
checksum. Power is `33 01 <00|01>`, brightness `33 04 <level>`, colour
`33 05 02 R G B` on the H6159 family, `33 05 0D R G B` on the H6181, and
`33 05 15 01 R G B .. FF 7F` on RGBIC strips. The model table that picks the
dialect from the advertised name is ported from the Home Assistant
integration (`govee_protocol.c`). Music mode `33 05 01 <mode>` and scene
`33 05 04 <id>` are the H6159-family codes from softgrass/govee-api-ble; on
other families the party falls back to whatever the light accepts, and Live
DJ (plain colour frames) works everywhere.

Only the H6159 and H6181 have been exercised on real hardware by the HA
project; this app has been compiled but not yet run against a light. Expect
to tweak.

## Files

- `govee_party.c` — UI, worker thread, patterns.
- `govee_protocol.[ch]` — frames, checksum, model → dialect table, HSV.
- `govee_transport.[ch]` — backend interface and light list.
- `govee_transport_native.c` — `furi_ble/central.h` backend (PR #4436).
- `govee_transport_uart.c` — ESP32 bridge backend.
- `esp32/govee_party_esp32/` — the ESP32 image: Flipper bridge plus standalone button mode.
