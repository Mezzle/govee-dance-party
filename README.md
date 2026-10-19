<img src="assets/icon.svg" width="64" height="64" alt="">

# Govee Dance Party

Finds nearby Govee Bluetooth LE lights and makes them dance. Or shuts them up.
Runs on a Flipper Zero, or on an M5Stack ATOM with nothing else.
The polite version of this protocol work lives in the
[Govee-Bluetooth](../govee-bluetooth) Home Assistant integration; this is the
impolite one.

## What it does

| Menu | What happens |
| --- | --- |
| **Dance Party!** | Scans, then visits every Govee light in range: power on, brightness 100 %, a few colour pops, then the blinking scene and the mic-driven *energic* music mode. The light keeps partying after the Flipper walks away. |
| **Live DJ** | Pick one light and stream patterns to it: Rainbow, Strobe, Police, Disco, Pulse, Fire. Left/Right changes the pattern, Up/Down the speed, OK holds the current colour. The Flipper's own LED mirrors the colour. |
| **Govee B-Gone** | Scans, then turns every Govee light in range off. With *B-Gone lurks* on in Settings it keeps rescanning and switching off anything that dares come back on, until you press Back. |
| **Census** | Scans, then visits every light and asks what it speaks: firmware, hardware, current mode, segment count, whether it still hands out its pairing secret. Nothing is written unless *Census probes* is on in Settings. Results go on screen and to `apps_data/govee_party/census.jsonl` as `govee-evidence/1` records (see [docs/evidence.md](docs/evidence.md)). Needs the ESP32 bridge (v3 or later). |

No pairing, no app, no cloud. Most Govee firmware accepts writes from anyone
who connects, which is the whole joke. The exceptions are the families the
app tags `secret_key_pairing` (dreamcolor H617A/H619A/H61A0/H6168, H61D3
and kin, the newer string lights, the hollow lamps, H60A6, H6047): those
take the connection and then ignore every frame until they see
`33 B2 <8-byte secret>`, and the secret can only be read with `AA B1` while
the light is unpaired. Lights advertising an AES-wrapped link (bit 6 of the
manufacturer-data flags) cannot be driven with plaintext frames at all, and
are skipped. The census classifies both cases: see *Census mode* below.

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
  | triple | Census: catalogue every light in range (see below) |
  | long | Govee B-Gone: every light in range off |
  | held at power-on | Lurk: rescan every ten seconds and switch off anything that is on, until the button is pressed |

The LED shows what it is doing: blue idle, cyan scanning, green connected,
red flash on a failed connection, purple breathing while lurking, yellow
during a census, the current colour during a rave. While a button job runs,
the Flipper gets `ERR busy`; while the Flipper holds a session, the button
only flashes red.

```sh
arduino-cli core install esp32:esp32
arduino-cli lib install NimBLE-Arduino
arduino-cli compile --fqbn esp32:esp32:m5stack_atoms3 esp32/govee_party_esp32   # or m5stack_atom
arduino-cli upload  --fqbn esp32:esp32:m5stack_atoms3 -p /dev/ttyACM0 esp32/govee_party_esp32
```

Or open it in the Arduino IDE, pick the board, install NimBLE-Arduino 2.x
from the Library Manager and upload. `govee_frames.h` is a straight port of
the Flipper app's `govee_protocol.c`; both must track
`custom_components/govee_bluetooth/protocol.py` in the
[Govee-Bluetooth](../govee-bluetooth) repo, whose `docs/govee-ble-protocol.md`
is the source of truth for every frame here. The model tables are not
hand-written in either: `generated/govee_models_gen.h` (and its identical
copy next to the sketch) comes out of `tools/export_fixtures.py` in
Govee-Bluetooth from `fixtures/govee-models.json`, and
`generated/govee_frame_vectors_gen.h` carries every frame `protocol.py`
builds so `make -C tests/host` can compare byte for byte. To take a new
model: add it to the JSON there, regenerate, copy both headers here.

### Bridge protocol

Line based, 115200 8N1, on the Flipper UART and on the USB port alike (so a
PC can drive the board without a Flipper). Hex is 40 characters for a
20-byte frame.

| Send | Get back | Notes |
| --- | --- | --- |
| `PING` | `PONG govee-bridge 4` | works even while a button job runs, and between lights during a census |
| `SCAN <ms>` | `ADV <mac> <addrtype> <rssi> <pactType> <pactCode> <flags> <enc> <name>` per light, then `SCAN_DONE` | pact columns are `-` when the advertisement has no manufacturer data; `flags` is hex, `enc` is `1` when bit 6 is set (AES link). Version 2 bridges sent `ADV <mac> <addrtype> <rssi> <name>`; the app reads the version from `PONG` and parses either. |
| `STOP` | `OK` | abort a scan |
| `CONN <mac> <addrtype>` | `OK` or `ERR <reason>` | |
| `W <hex> [R]` | `OK` or `ERR <reason>` | `R` = write with response |
| `N` | `OK` or `ERR <reason>` | subscribe to the status characteristic `...2b10` |
| `R <hex> [timeout_ms]` | `RX <hex>` or `ERR timeout` | writes a read frame on `...2b11` and returns the matching `AA` reply (same command; same selector for `07`/`A2`/`A5`). Subscribes first if needed. |
| (unsolicited) | `NOTIFY <hex>` | any other notification while subscribed |
| `BGC` | `BGC <hex>` or `ERR no bgc` | reads the BGC-info characteristic `...2b12` (the `encryptVersion` record); version 4 |
| `SVC` | `SVC <service> <characteristic> <props>` per characteristic, then `SVC_DONE` | GATT table of the connected light; props are `r w W n i`; version 4 |
| `DISC` | `OK` | |
| `CENSUS [PROBE]` | `CENSUS <json>` per light, then `CENSUS_DONE` | runs the standalone census; `PROBE` adds the colour writes |
| `DUMP` | `OK` | prints and clears the LittleFS frame log on the USB port |

Version 4 added `BGC` and `SVC` and changed nothing a version 3 app sends;
`N` and `R` need version 3 or later. A version 2 app cannot read the
version 3+ `ADV` columns, so flash the app and the bridge together.

### Census mode

Triple press, or `CENSUS` over the line protocol. For each Govee light in
range the image records the advertisement (name, model, pact type/code,
flags, encryption bit), connects, notes which Govee characteristics the
light exposes (`1910/2b11/2b10/2b12`, or the `ffe0` UART service of the
H6101/H6104) and reads the raw `2b12` bytes, subscribes to `...2b10` and
sends the reads from the protocol reference: `AA 01` power, `AA 04`
brightness, `AA 06` firmware, then, if any of those three answered,
`AA 05 01` mode, `AA 07 03` hardware, `AA 40` IC and segment count, `AA 0F`
light count, `AA B1` pairing secret (only unpaired lights answer), `AA A5 01`
segment colour page. A light that answers none of the first three is
recorded as `silent` and left alone: three timeouts, not nine. A reply only
counts when its XOR checksum verifies; a corrupt one is logged as
`bad_checksum`.

With probes enabled (`CENSUS PROBE`, triple press with `CENSUS_PROBE_WRITES 1`
at the top of the sketch, or *Census probes* in the Flipper's Settings) it
then writes a colour in each dialect `02` / `0B` / `0D` / `15`, each followed
by a mode read and `AA A5 01`. Every dialect has its own probe colour
(`12 34 56`, `15 37 59`, `13 35 57`, `14 36 58`), and a dialect only counts
as echoed when *its* colour shows up where it was not showing before, so a
colour that landed through `02` cannot be credited to `0B` by the page read
that follows. A light that was off is switched on for the probes and off
again after. The colour is put back when the pre-probe mode reply named it
(`02` and `0D` carry RGB); `0B` and `15` replies do not, and scenes and
music modes are never rewritten, so the summary says `"restored":"power"`
with a `restore_note` in those cases.

Output on the USB port is one JSON line per frame in the shape of the
bench rig's log (`tools/govee_rig.py` in Govee-Bluetooth):

```
{"at":"T+12.345","direction":"tx","frame":"aa050100...","note":"query","decoded":"read mode","address":"aa:bb:cc:dd:ee:ff"}
```

`at` is uptime; the ESP32 has no clock. Notes are `query`, `reply`,
`timeout`, `bad_checksum`, `ack` (a `33 xx` echo with byte 2 = 0, the
light's own acknowledgement of a write), `ack_error`, `unsolicited`,
`probe`, `restore`. Frames carrying a pairing secret (`AA B1 01 ...`
replies, `33 B2` writes) are logged with the secret bytes blanked and
`"redacted":true`. The session starts with a header line and each light
ends with one summary line:

```
{"summary":"session","schema":"govee-evidence/1","source":"esp32","software":"govee-party-esp32 0.2 bridge 4","board":"M5STACK_ATOMS3","lights":2,"probe":false}
{"summary":"light","schema":"govee-evidence/1","source":"esp32","at":"T+14.020","software":"govee-party-esp32 0.2 bridge 4","address":...,"name":...,"model":...,"known_model":true,"rssi":-61,"pact":"10/1","flags":3,"encrypted":false,"dialect":"RGBIC","dialect_source":"pact","secret_family":true,"connected":true,"subscribed":true,"silent":false,"gatt":"1910/2b11/2b10/2b12","bgc":"0100","reads":9,"replies":7,"power":true,"brightness":100,"submode":"15 010BB800","submode_name":"colour15","firmware":"3.02.24","hardware":"3.01.01","ic_count":0,"segments":15,"secret_present":false,"segment_page":true,"probed":true,"echoed":"15","restored":"power","restore_note":"pre-probe colour not readable from the mode reply (0B/15); left on the last probe colour"}
```

`silent` means the light connected and subscribed but answered nothing: on
a `secret_family` model that is the pairing lock, on a light with
`"encrypted":true` or a non-zero `bgc` it is the cipher, and on an unknown
model it may just be a dialect this code does not speak. The summary never
decides which; the report generator in Govee-Bluetooth (`tools/govee_report.py`)
does, with a confidence. `dialect` is the dialect the party would use and
`dialect_source` says whether the advertised pact, the model table or the
unknown-model default chose it. `secret_present` reports whether `AA B1`
answered; the bytes themselves are only printed with `CENSUS_PRINT_SECRET 1`.
Lights advertising the encryption bit are connected to for the GATT table
and the `2b12` bytes only; no frame is sent to them. A button press between
lights stops a running census.

While no USB host is attached the log is appended to LittleFS
(`/census.jsonl`, rotated to `/census.old.jsonl` at 96 KB, older data
dropped) and dumped the next time a host appears. Only native-USB boards
with the CDC console (ATOMS3 Lite, C3, C6) can tell whether a host is
there; the ATOM Lite's UART bridge chip cannot, so there the log is always
stored as well and `DUMP` (on either port) fetches and clears it.

The Flipper app's Census does the same battery through the bridge's `N`
and `R` commands and writes the same kinds of line to
`apps_data/govee_party/census.jsonl` on the SD card, with
`"source":"dance_party"` and real timestamps from the Flipper's clock.
Evidence gathered this way earns the 🪩 *Dance Party Certified* badge in the
generated report: a provenance mark, not a test pass (see
[docs/evidence.md](docs/evidence.md)).

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

The bridge speaks a tiny line protocol (`PING`, `SCAN`, `CONN`, `W`, `N`,
`R`, `DISC`, `CENSUS`); see the table above, the top of the sketch and
`govee_transport_uart.c`.
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

Every frame is 20 bytes: `33` write / `AA` read, command, payload zero
padded, XOR checksum. Power is `33 01 <00|01>`, brightness `33 04 <level>`,
colour `33 05 02 R G B` on the H6159 family, `33 05 0D R G B` on the H6181,
`33 05 15 01 R G B .. <mask>` on RGBIC strips and `33 05 0B R G B <mask>` on
old-pact dreamcolor lights. The model table that picks the dialect from the
advertised name is ported from the Home Assistant integration
(`govee_protocol.c`); for the dreamcolor SKUs (H617A, H619A, H6199, H6143
and friends) the advertised pact decides instead: pact type 1 speaks `0B`,
pact 2 and 10 speak `15`.

Two things the research corrected. The whole-strip mask `FF 7F` is only
right for 15 segments (13 panels want `FF 1F`), so the mask is now sized from
the `AA 40` reply when the light answers it. And the music sub-mode byte is
per family: the softgrass `33 05 01 <mode>` the party used to send is what
the app uses on the H6102/H6104/H6127 class only; the modern RGBIC stack
wants `33 05 13 <code> <sensitivity>` and dreamcolor `33 05 11 <mode>
<sensitivity>`. The party now tries `13`, then `11`, then `01` in the order
the family suggests and, with a read path, stops at the first one the light
echoes back in `AA 05 01`. Scene `33 05 04 <id>` is unchanged.

Only the H6159 and H6181 have been exercised on real hardware by the HA
project. The read path, the census and the pact parsing compile for every
ESP32 target in CI and the pure-C protocol and census code passes the host
tests in `tests/host/`, but none of it has yet been run against a light.
Expect to tweak; [docs/hardware-testing-checklist.md](docs/hardware-testing-checklist.md)
is the first-power-on plan for the M5Stack boards.

Advertised names are parsed the way the Govee app does it: `ihoment_`,
`Govee_`, `Minger_` and `GBK_` are followed by `_`-separated SKU and tail,
while `GVH`-style names carry the SKU letter as their third character
(`GVH6159_1A2B`, or `GVH600B4F5A` with no separator at all).

## Files

- `govee_party.c` — UI, worker thread, patterns.
- `govee_protocol.[ch]` — frames (writes and reads), checksum, manufacturer-data parsing, model → dialect table, HSV.
- `govee_census.[ch]` — the census read battery, probe rules, restoration and summary JSON, over an abstract write/read pair.
- `generated/` — model tables and frame vectors exported from Govee-Bluetooth; never edited here.
- `tests/host/` — host unit tests for the two files above (`make -C tests/host`); run in CI.
- `docs/evidence.md`, `docs/hardware-testing-checklist.md` — what the census records and how to first-test the M5Stack boards.
- `govee_transport.[ch]` — backend interface and light list.
- `govee_transport_native.c` — `furi_ble/central.h` backend (PR #4436).
- `govee_transport_uart.c` — ESP32 bridge backend.
- `esp32/govee_party_esp32/` — the ESP32 image: Flipper bridge plus standalone button mode.
