# Changelog

## v0.2.0

- Install ESP32 / M5Stack release images from a GitHub Pages browser flasher, with board selection, Flipper app downloads, and wiring instructions. Pages uses the exact published release assets and updates after each tagged release.
- Census mode on Flipper and standalone ESP32, with protocol reads, opt-in colour probes, restoration, redacted pairing secrets, and evidence logs.
- Bridge protocol v4 adds BGC information and GATT service discovery; update both the Flipper app and ESP32 bridge together.
- More Live DJ patterns: Fire, Candy, Aurora, and Heartbeat.
- Sync Govee-Bluetooth protocol fixes, including write-procedure fallback and full 16-bit scene IDs; refresh generated model tables.

Build and host checks pass in CI. Hardware validation remains in progress; see `docs/hardware-testing-checklist.md`.
