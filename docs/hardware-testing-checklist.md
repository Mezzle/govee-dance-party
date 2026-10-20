# Hardware testing checklist: M5Stack ATOM Lite and ATOMS3 Lite

For the first session with the incoming boards. Work top to bottom; every
step names what to record and where the evidence ends up. Only test lights
you own. Keep the Govee app closed on every phone nearby: the lights take
one connection at a time.

Record findings as you go in `bench/notes-<date>.md` (git-ignored) and keep
the USB log of each census run; those files are what the report generator
in Govee-Bluetooth consumes (`tools/govee_report.py census.jsonl ...`).

## 0. Bench

- [ ] Govee-Bluetooth checkout on the Pi with the rig working (`govee_rig.py scan` lists the lights).
- [ ] This checkout on the laptop with `arduino-cli`, the ESP32 core and NimBLE-Arduino (README).
- [ ] A serial terminal that can log to a file (`picocom --logfile`, `screen -L`, Arduino's serial monitor with copy-out).
- [ ] At least one plain light (H6159 or H6181 class) and, if available, one `secret_family` light (H617A/H619A/H61A0 class) and one newer light (H61F6, H1270, H600B class) for the encrypted path.

## 1. Flash and boot

| board | fqbn | flash | notes |
| --- | --- | --- | --- |
| ATOM Lite | `esp32:esp32:m5stack_atom` | 4 MB | UART-to-USB chip: no host detection, log always stored in LittleFS |
| ATOMS3 Lite | `esp32:esp32:m5stack_atoms3` | 8 MB | native USB CDC: host detection works |

- [ ] `arduino-cli compile --fqbn <fqbn> --export-binaries esp32/govee_party_esp32` succeeds; note the "Sketch uses" line (the ATOM Lite must stay under the 1.3 MB app partition of the default 4 MB layout; it was 633 KB at commit time).
- [ ] Upload; the LED goes blue; the USB port prints `govee party: ready`.
- [ ] `PING` on USB answers `PONG govee-bridge 4`.
- [ ] Hold the button at power-on: `govee party: LURK mode` and purple breathing; press once to leave.

## 2. Scan

- [ ] `SCAN 5000` prints one `ADV` line per Govee light, then `SCAN_DONE`.
- [ ] For each light note the eight columns. Check the pact against what the light is expected to be (H617A current firmware advertises `10 1`, an H6199 pact 1 `1 1`); `enc` must be `0` for every plain light.
- [ ] A light named `GVH...` (newer SKUs) appears with the right model. This was broken before the name-parse fix; it is the one thing in this checklist that was never right on hardware.
- [ ] `SCAN 5000` then `STOP` mid-scan: `OK`, then `SCAN_DONE`, and the next `SCAN` works.

## 3. Bridge session against one plain light

- [ ] `CONN <mac> <type>` answers `OK`; LED green.
- [ ] `W 3301010000000000000000000000000000000033 R` answers `OK` and the light turns on.
- [ ] `N` answers `OK`.
- [ ] `R aa010000000000000000000000000000000000ab` answers `RX aa0101...` (power on) within the timeout.
- [ ] After a `W` of a colour, a `NOTIFY 3305...` line appears if the firmware acks writes (the H6159 class does; record whether yours does).
- [ ] `BGC` answers `BGC <hex>` or `ERR no bgc`; record the bytes. On a plain light the version byte should be `00`.
- [ ] `SVC` lists `...1910` with `2b11` (`wW`) and `2b10` (`n`), and `2b12` if present.
- [ ] `DISC` answers `OK`; LED blue.
- [ ] Repeat `CONN`/`DISC` five times: no `ERR connect failed` streak (the H6181 "stops answering every 4-6 connections" note in Govee-Bluetooth is worth reproducing here).

## 4. Read-only census

- [ ] Triple press (or `CENSUS` on USB). LED yellow. One `{"summary":"session"...}` line, then frame lines, then one `{"summary":"light"...}` per light, then `census done`.
- [ ] Every `reply` frame's `decoded` text makes sense next to its `query` (power, brightness, firmware string, hardware string, `ic N segments M`).
- [ ] `"silent":false` on the plain light; `"reads"` and `"replies"` agree with the frame lines.
- [ ] `AA 0F` and `AA B1` may time out on strips: expected, recorded as `timeout` lines, not errors.
- [ ] The light's colour, brightness and power are unchanged afterwards (no probes were sent). Check by eye.
- [ ] On the `secret_family` light: either `"secret_present":true` (unbound, never paired with the app) or `"silent":true`. Record which, and whether the frame log line for the `AA B1` reply has `"redacted":true` and zeroed bytes.
- [ ] Unplug USB, run a census, plug back in: the stored log is dumped (ATOMS3) or `DUMP` fetches it (ATOM Lite). Compare line counts.
- [ ] Press the button during a multi-light census: it stops after the current light, the summary of that light has `"aborted":true` if it was mid-read.

## 5. Probe census (changes colours; one light at a time)

- [ ] Set the light to a known colour in the Govee app, close the app, note the colour.
- [ ] `CENSUS PROBE`. Watch the strip: which of the four writes visibly changes it? Write that down per dialect; the log cannot see it.
- [ ] `"echoed"` lists exactly the dialects that visibly changed the strip to *their* probe colour (`02` red, `0B` green, `0D` blue, `15` magenta). A dialect listed that did not visibly change the strip is a false echo: file it with the frame log.
- [ ] `"restored"`: `full` on a 02/0D light and the original colour is back; `power` on a 0B/15 light with the note, and the strip is on the `15` probe colour.
- [ ] Repeat with the light off: it switches on for the probes and off again.
- [ ] Repeat with the light in a scene: `"restored":"power"` with the "not a static colour" note; the scene is gone (expected).

## 6. Flipper through the bridge

- [ ] Wire per the README table; Settings, Radio, ESP32 bridge; the app pings and the status screen shows the transport.
- [ ] Census from the menu writes `apps_data/govee_party/census.jsonl`; the `session` line says `"source":"dance_party"` and the `at` fields are real timestamps.
- [ ] The on-screen list shows `(unknown)` after the dialect for an SKU not in the tables.
- [ ] Back during a census returns to the menu within a few seconds (the abort flag is checked between reads; a single read can still take 1.5 s, a connect 6 s).
- [ ] Dance Party, Live DJ and B-Gone still work exactly as before the census branch (no regression from the music sub-mode fallback).

## 7. Encrypted or locked lights (observation only)

- [ ] A light with `enc 1` in `ADV`: the census connects, records `gatt` and `bgc`, sends no frame, reports `"subscribed":false`.
- [ ] A locked `secret_family` light: `"silent":true` after exactly three timeouts (`reads:3`).
- [ ] Record `bgc` for every such light; the second byte is where lasswellt's H1270 carried `02`, the first byte is where the decompiled app reads it. Those bytes decide the next step in Govee-Bluetooth's encrypted-protocol plan.

## 8. Hand the evidence over

- [ ] Copy `census.jsonl` files and USB logs into the Govee-Bluetooth checkout and run `tools/govee_report.py` on them; read the Markdown report for anything the generator classified differently from what you saw.
- [ ] For every light, add the human observations (`observed` entries) with the `--observe` option so the report distinguishes "echoed in the log" from "seen on the strip".
- [ ] Models that behaved differently from the tables go into an issue with the report attached, secrets redacted (the generator does that; check anyway).
