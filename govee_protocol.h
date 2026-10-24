#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Govee BLE wire format, lifted from the Govee-Bluetooth Home Assistant
 * integration. Every frame is 20 bytes: type, command, up to 17 payload bytes
 * zero padded, then an XOR checksum of the first 19.
 *
 * This file, esp32/govee_party_esp32/govee_frames.h (its C++ port) and
 * custom_components/govee_bluetooth/protocol.py in the Govee-Bluetooth repo
 * must track each other. Citations ("SPEC:", "YAML:", "HGL:", "GENZERO") refer
 * to docs/govee-ble-protocol.md in that repo. */

#define GOVEE_FRAME_LEN 20

#define GOVEE_TYPE_WRITE  0x33
#define GOVEE_TYPE_READ   0xAA
#define GOVEE_TYPE_NOTIFY 0xEE

#define GOVEE_CMD_POWER         0x01
#define GOVEE_CMD_BRIGHTNESS    0x04
#define GOVEE_CMD_MODE          0x05
#define GOVEE_CMD_VERSION       0x06 /* AA 06 -> ASCII firmware version */
#define GOVEE_CMD_DEVICE_INFO   0x07 /* AA 07 <sel>; 03 = hardware version ASCII */
#define GOVEE_CMD_LIGHT_NUM     0x0F /* AA 0F -> light / bulb count */
#define GOVEE_CMD_IC_NUM        0x40 /* AA 40 -> ic u16 BE, segment u8 */
#define GOVEE_CMD_SEGMENT_COLOR 0xA5 /* AA A5 <group> -> N x [bri R G B] */
#define GOVEE_CMD_SECRET_READ   0xB1 /* AA B1 -> 01 + 8-byte pairing secret */
#define GOVEE_CMD_SECRET_WRITE  0xB2 /* 33 B2 <8 bytes> authorises the session */

#define GOVEE_INFO_HARDWARE 0x03

/* Sub-mode bytes under command 0x05. Colour: */
#define GOVEE_SUBMODE_COLOR_LEGACY     0x02
#define GOVEE_SUBMODE_COLOR_DREAMCOLOR 0x0B
#define GOVEE_SUBMODE_COLOR_RGBWW      0x0D
#define GOVEE_SUBMODE_COLOR_RGBIC      0x15
/* Music: 0x01 Telink-era (H6102/H6104/H6127), 0x11 dreamcolor (H617A, H6125),
 * 0x13 modern RGBIC stack (h60ax, h6047, h61d3, striplight). SPEC 8.2. */
#define GOVEE_SUBMODE_MUSIC_LEGACY     0x01
#define GOVEE_SUBMODE_MUSIC_DREAMCOLOR 0x11
#define GOVEE_SUBMODE_MUSIC_MODERN     0x13
#define GOVEE_SUBMODE_SCENE            0x04
#define GOVEE_SUBMODE_DIY              0x0A
#define GOVEE_SUBMODE_VIDEO            0x00

/* Legacy music modes (H6159 family, "33 05 01 <mode>"). */
#define GOVEE_MUSIC_ENERGIC  0x00
#define GOVEE_MUSIC_SPECTRUM 0x01
#define GOVEE_MUSIC_ROLLING  0x02
#define GOVEE_MUSIC_RHYTHM   0x03
/* Dreamcolor music modes ("33 05 11 <mode> <sensitivity>", HGL govee_common.ksy). */
#define GOVEE_MUSIC11_RHYTHM    0x03
#define GOVEE_MUSIC11_SPECTRUM  0x04
#define GOVEE_MUSIC11_ENERGETIC 0x05
#define GOVEE_MUSIC11_ROLLING   0x06
/* Modern music ("33 05 13 <code> <sensitivity>"): the only captured code is
 * 06 with sensitivity 0x63 (GENZERO H617A set_music_mode). */
#define GOVEE_MUSIC13_DEFAULT         0x06
#define GOVEE_MUSIC_SENSITIVITY_DEFAULT 0x63

/* Legacy built-in scenes ("33 05 04 <id>"). */
#define GOVEE_SCENE_BLINKING 0x08

/* Probe colours for the census writes: red (02), green (0B), blue (0D) and
 * magenta (15), saturated so a person can tell them apart (the first set,
 * four near-identical dark blues, could not be). One per dialect, so a colour that
 * landed through one dialect cannot be mistaken for an echo of the next
 * (the AA A5 segment page keeps whatever the last accepted write set). None
 * is one of the app's 178 kelvin tint colours, so a 0x0D firmware will not
 * reinterpret it as CCT, and every channel is distinct, non-zero and
 * non-full so a partial echo is easy to tell from a real one. The
 * GOVEE_PROBE_* triple is the legacy-dialect colour (kept for callers). */
#define GOVEE_PROBE_R 0xC8
#define GOVEE_PROBE_G 0x1E
#define GOVEE_PROBE_B 0x28

/* The default whole-strip mask FF 7F addresses 15 segments; see
 * govee_segment_mask() for other counts. */
#define GOVEE_DEFAULT_SEGMENTS 15
#define GOVEE_MASK_MAX_BYTES   7

/* GATT identifiers shared by nearly every Govee light, as 128-bit UUIDs in
 * the little-endian byte order the STM32WB stack (and NimBLE's raw API) use. */
extern const uint8_t GOVEE_SERVICE_UUID_LE[16];
extern const uint8_t GOVEE_CONTROL_UUID_LE[16];
extern const uint8_t GOVEE_STATUS_UUID_LE[16];
#define GOVEE_SERVICE_UUID_STR "00010203-0405-0607-0809-0a0b0c0d1910"
#define GOVEE_CONTROL_UUID_STR "00010203-0405-0607-0809-0a0b0c0d2b11"
#define GOVEE_STATUS_UUID_STR  "00010203-0405-0607-0809-0a0b0c0d2b10"

/* Which colour command format a model speaks. */
typedef enum {
    GoveeDialectLegacy, /* 33 05 02 R G B, brightness 0-255 */
    GoveeDialectRgbww, /* 33 05 0D R G B, brightness 0-100 */
    GoveeDialectRgbic, /* 33 05 15 01 R G B ... mask, brightness 0-100 */
    GoveeDialectDreamcolor, /* 33 05 0B R G B mask0 mask1, old-pact dreamcolor */
    GoveeDialectCount,
} GoveeDialect;

/* What the advertisement's manufacturer data says. SPEC 11.2:
 *   [flags][88 EC][pactType u16 BE][pactCode u8]...
 * flags bit 6 = the link is AES-wrapped (plaintext frames will not work);
 * low nibble = broadcast protocol version. */
typedef struct {
    bool valid; /* manufacturer data was present and Govee-shaped */
    uint8_t flags;
    uint8_t proto_version;
    bool encrypted;
    uint16_t pact_type;
    uint8_t pact_code;
} GoveeAdvInfo;

uint8_t govee_checksum(const uint8_t* data, size_t len);
void govee_frame_build(
    uint8_t out[GOVEE_FRAME_LEN],
    uint8_t type,
    uint8_t cmd,
    const uint8_t* payload,
    size_t payload_len);
bool govee_frame_verify(const uint8_t frame[GOVEE_FRAME_LEN]);

/* ---- writes ---- */
void govee_frame_power(uint8_t out[GOVEE_FRAME_LEN], bool on);
/* percent 1-100; scaled to the dialect's native range. Never sends zero. */
void govee_frame_brightness(uint8_t out[GOVEE_FRAME_LEN], uint8_t percent, GoveeDialect dialect);
/* Whole-strip colour with the default 15-segment mask. */
void govee_frame_rgb(uint8_t out[GOVEE_FRAME_LEN], uint8_t r, uint8_t g, uint8_t b, GoveeDialect dialect);
/* Same, with the segment mask sized for `segments` (0 = default 15). Only
 * the RGBIC and dreamcolor dialects carry a mask. */
void govee_frame_rgb_seg(
    uint8_t out[GOVEE_FRAME_LEN],
    uint8_t r,
    uint8_t g,
    uint8_t b,
    GoveeDialect dialect,
    uint8_t segments);
/* Builds the all-segments mask for `segments` segments: bit i = segment i,
 * LSB first, little-endian across bytes (SPEC BleUtils.makeSelectedTwoBytes).
 * Returns the number of mask bytes written (2 for <= 16 segments). */
size_t govee_segment_mask(uint8_t segments, uint8_t out[GOVEE_MASK_MAX_BYTES]);
/* The app uses little-endian 16-bit scene codes. */
void govee_frame_scene(uint8_t out[GOVEE_FRAME_LEN], uint16_t scene_id);
/* 33 05 01 <mode> 00 R G B (softgrass/govee-api-ble; H6102/H6104/H6127 class). */
void govee_frame_music(uint8_t out[GOVEE_FRAME_LEN], uint8_t mode, uint8_t r, uint8_t g, uint8_t b);
/* 33 05 11 <mode> <sensitivity> (dreamcolor SubModeMusicV3; legacy modes 03-06). */
void govee_frame_music_dreamcolor(uint8_t out[GOVEE_FRAME_LEN], uint8_t mode, uint8_t sensitivity);
/* 33 05 13 <code> <sensitivity> (SubModeNewMusic on the modern RGBIC stack). */
void govee_frame_music_modern(uint8_t out[GOVEE_FRAME_LEN], uint8_t code, uint8_t sensitivity);
/* 33 B2 <8 raw bytes>: authorises a session on secret-key families. */
void govee_frame_secret_write(uint8_t out[GOVEE_FRAME_LEN], const uint8_t secret[8]);
void govee_frame_keepalive(uint8_t out[GOVEE_FRAME_LEN]);

/* ---- reads (AA ...). The light answers on the status characteristic with
 * AA <cmd> <state...>; govee_frame_read_matches() pairs them up. ---- */
void govee_frame_read_power(uint8_t out[GOVEE_FRAME_LEN]); /* AA 01 */
void govee_frame_read_brightness(uint8_t out[GOVEE_FRAME_LEN]); /* AA 04 */
void govee_frame_read_mode(uint8_t out[GOVEE_FRAME_LEN]); /* AA 05 01 (selector required) */
void govee_frame_read_firmware(uint8_t out[GOVEE_FRAME_LEN]); /* AA 06 */
void govee_frame_read_hardware(uint8_t out[GOVEE_FRAME_LEN]); /* AA 07 03 */
void govee_frame_read_ic_count(uint8_t out[GOVEE_FRAME_LEN]); /* AA 40 */
void govee_frame_read_light_count(uint8_t out[GOVEE_FRAME_LEN]); /* AA 0F */
void govee_frame_read_secret(uint8_t out[GOVEE_FRAME_LEN]); /* AA B1 00, unpaired lights only */
void govee_frame_read_segment_colors(uint8_t out[GOVEE_FRAME_LEN], uint8_t group); /* AA A5 <group> */
/* True if `reply` is the AA answer to `request` (same command; same selector
 * for the commands that echo one: 07, A2, A5). */
bool govee_frame_read_matches(const uint8_t request[GOVEE_FRAME_LEN], const uint8_t reply[GOVEE_FRAME_LEN]);
/* Pulls the NUL/space terminated ASCII out of an AA 06 / AA 07 03 reply. */
void govee_reply_version_string(const uint8_t reply[GOVEE_FRAME_LEN], char* out, size_t out_size);
/* Short human reading of a frame for logs. `from_light` tells a read request
 * ("read mode") from its reply ("mode colour15 ..."); the bytes alone cannot. */
void govee_frame_describe(const uint8_t frame[GOVEE_FRAME_LEN], bool from_light, char* out, size_t out_size);
const char* govee_submode_name(uint8_t submode);
bool govee_submode_is_color(uint8_t submode);

/* ---- advertisement ---- */
/* `data` is the manufacturer-specific AD payload as it appears on air
 * (company-id bytes included), or as a stack reports it after eating a
 * "company id" of 0x8803 / 0x8843 (then it starts at EC). Both are handled.
 * Returns false (and clears *out) when it is not Govee-shaped. */
bool govee_adv_parse_mfr(const uint8_t* data, size_t len, GoveeAdvInfo* out);

/* ---- model table ---- */
/* `name` is the advertised local name, e.g. "ihoment_H6159_1A2B". Returns
 * false if it does not look like a Govee light. `model` gets the SKU
 * ("H6159"), NUL terminated, at least 6 bytes. */
bool govee_name_parse(const char* name, size_t name_len, char* model, size_t model_size);
GoveeDialect govee_dialect_for_model(const char* model);
/* Same, but lets the advertised pact decide for dreamcolor SKUs: pact type 1
 * speaks 0x0B, pact 2 and 10 speak 0x15 (HGL h6199-pact1, SPEC 6.2). `adv`
 * may be NULL or invalid, in which case the model table alone decides. */
GoveeDialect govee_dialect_for_light(const char* model, const GoveeAdvInfo* adv);
/* True for SKUs in families tagged secret_key_pairing (SPEC 10.1): these may
 * accept the connection and ignore every write until 33 B2 <secret>. */
bool govee_model_needs_secret(const char* model);
/* True if the SKU is in any of the model tables (RGBIC, dreamcolor, RGBWW,
 * legacy). Unknown SKUs fall back to the legacy 0x02 dialect too, but the
 * census reports them as unknown rather than as a known legacy strip. */
bool govee_model_known(const char* model);
/* True for SKUs whose advertised pact decides between 0x0B and 0x15. */
bool govee_model_is_dreamcolor(const char* model);
const char* govee_dialect_name(GoveeDialect dialect);
uint8_t govee_dialect_submode(GoveeDialect dialect);
/* The census probe colour for `dialect` (see GOVEE_PROBE_*). */
void govee_probe_color(GoveeDialect dialect, uint8_t rgb[3]);

/* ---- evidence hygiene ---- */
/* True if `frame` carries a pairing secret: an AA B1 reply with 01 at byte 2
 * (8 secret bytes follow) or a 33 B2 write. */
bool govee_frame_has_secret(const uint8_t frame[GOVEE_FRAME_LEN]);
/* Blanks the secret bytes of such a frame in place and recomputes the
 * checksum, so the record stays parseable. Returns true if anything was
 * changed; callers should then mark the record as redacted. */
bool govee_frame_redact(uint8_t frame[GOVEE_FRAME_LEN]);

/* HSV (h 0-359, s/v 0-255) to RGB, for the patterns. */
void govee_hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b);
