# Evidence: what the census records and what it means

Dance Party is the naughty project, but the evidence it gathers is meant to
be taken seriously. This note says what a census writes, which conclusions
the fields support, and which they do not. The schema itself
(`govee-evidence/1`) and the report generator live in the Govee-Bluetooth
repository (`schema/govee-evidence-v1.schema.json`, `tools/govee_report.py`);
this project only produces records in that shape.

## Record kinds

Every line of `census.jsonl` (Flipper SD card) or of the ESP32's USB log is
one JSON object. Three kinds exist:

| `summary` | meaning |
| --- | --- |
| absent | a frame record: `{"at","direction","frame","note","decoded","address"}`, the Pi bench rig's shape, plus `"redacted":true` when secret bytes were blanked |
| `"session"` | written once at the start: `schema`, `source`, `software`, `at` (Flipper) or `board` (ESP32), `lights`, `probe` |
| `"light"` | written once per light after its visit; the keys are listed in the README |

`source` is `dance_party` for a Flipper session (any mode, through the
bridge), `esp32` for the standalone image and `pi` for the bench rig. The
report generator keeps the source of every observation; a report may mix
them.

## What each field is evidence of

| field | evidence level | supports |
| --- | --- | --- |
| `name`, `model`, `pact`, `flags`, `encrypted`, `rssi` | advertisement | identity; the pact/flags are what the Govee app itself keys on |
| `known_model`, `dialect`, `dialect_source` | table lookup | the dialect this code *would* use; `default` means an unknown SKU got 0x02 by fallback |
| `secret_family` | table lookup | the app family is tagged `secret_key_pairing`; not a measurement |
| `gatt`, `bgc` | GATT read (ESP32 only) | which Govee characteristics exist; the raw `2b12` bytes, where the app reads `encryptVersion` |
| `connected`, `subscribed` | transport | the link came up and notifications were enabled; says nothing about the application layer |
| `silent`, `reads`, `replies`, `bad_frames` | protocol | whether verified `AA` replies arrived at all |
| `power`, `brightness`, `submode`, `firmware`, `hardware`, `ic_count`, `segments`, `light_count`, `segment_page` | protocol | each is a verified reply to its own read |
| `secret_present` | protocol | `AA B1` answered with `01`: the light is unbound and hands out its secret; `false` means it did not answer, which bound lights and plain lights both do |
| `acks` | protocol | `33 xx` echoes with byte 2 = 0, the firmware's own write acknowledgement |
| `echoed` | application | the dialect's own probe colour appeared in the mode reply or the segment page after the write, and was not showing before |
| `restored`, `restore_note` | housekeeping | whether the light was put back; never a capability claim |

What no field proves: that the light *looked* right. A probe that echoed
`15` shows the firmware stored the colour in that dialect; whether every
segment lit is for the operator's eyes and the hardware checklist. The
report generator marks such conclusions `observed` only when a human
entered them.

## Reading the common outcomes

| summary | likely reading | still open |
| --- | --- | --- |
| `silent:true`, `secret_family:true`, `secret_present:false` | the pairing lock: writes and reads are ignored until `33 B2 <secret>` | a foreign dialect would look the same |
| `silent:true`, `encrypted:true` or `bgc` with a non-zero version byte | AES-wrapped link; plaintext frames are discarded | which cipher (V1 ECB+RC4 or V2 GCM) needs a handshake attempt |
| `silent:true`, `known_model:false`, no encryption signs | an unknown SKU that may speak on another characteristic or need the secret | everything |
| `subscribed:false` | the status characteristic was missing or refused notifications; the `gatt` field says which | H6101/H6104 (`ffe0`) land here |
| `echoed:"15"` on a model tabled `0B` | the pact decided, and it was right | brightness scale, segment count |
| `echoed:""` with `probed:true` | none of the four colour layouts changed the light's reported colour | the light may be in a mode that overrides colour, or need Form B of `0B` |

## Secrets

The 8-byte pairing secret is the only credential in this protocol family.
The census never prints it in a summary unless the ESP32 sketch is built
with `CENSUS_PRINT_SECRET 1`, and never in the Flipper app. Frame records
that would carry it (`AA B1 01 ...` replies and `33 B2` writes) are written
with the secret bytes zeroed and `"redacted":true`; the checksum is
recomputed so the record still parses. A capture that must keep the bytes
(for your own light, to unlock it later) belongs in a private file, not in
anything shared. The report generator redacts again on ingest, in case a
raw bench log is fed to it.

## 🪩 Dance Party Certified

A report whose evidence includes at least one `dance_party` record carries
the badge. It means "gathered while a Flipper was running Dance Party, Live
DJ or Census through the bridge", nothing more: not that every capability
was tested, not that a suite passed, and a Pi bench session does not get it
by default. Multiple sources can appear in one report; the badge reflects
one of them, the findings reflect all of them.

## Limitations of this census

- Read-only by default; the probe is opt-in and changes the light's colour.
  Scenes, music modes and `0B`/`15` colours are not restored.
- Only Form A of the `0B` layout is tried (`0B R G B mask0 mask1`); the
  H6199 pact-1 producer's Form B (`0B R G B kHi kLo mask0 mask1`) is not.
- `AA A5 01` is read for one page only; segment counts above the page size
  are not confirmed.
- The ESP32 drops notifications that are not 20 bytes (encrypted V2 frames
  are longer); it only counts them (`odd_notifies`).
- No handshake is attempted on encrypted lights. The plan and test vectors
  for that live in Govee-Bluetooth (`docs/encrypted-protocol-matrix.md`).
- Nothing here has run against a real light yet. The host tests cover the
  logic with a scripted fake; the hardware checklist covers the rest.
