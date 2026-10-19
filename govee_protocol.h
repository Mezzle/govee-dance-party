#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Govee BLE wire format, lifted from the Govee-Bluetooth Home Assistant
 * integration. Every frame is 20 bytes: type, command, up to 17 payload bytes
 * zero padded, then an XOR checksum of the first 19. */

#define GOVEE_FRAME_LEN 20

#define GOVEE_TYPE_WRITE 0x33
#define GOVEE_TYPE_READ  0xAA

#define GOVEE_CMD_POWER      0x01
#define GOVEE_CMD_BRIGHTNESS 0x04
#define GOVEE_CMD_MODE       0x05

#define GOVEE_SUBMODE_MUSIC_LEGACY 0x01
#define GOVEE_SUBMODE_SCENE        0x04

/* Legacy music modes (H6159 family, "33 05 01 <mode>"). */
#define GOVEE_MUSIC_ENERGIC  0x00
#define GOVEE_MUSIC_SPECTRUM 0x01
#define GOVEE_MUSIC_ROLLING  0x02
#define GOVEE_MUSIC_RHYTHM   0x03

/* Legacy built-in scenes ("33 05 04 <id>"). */
#define GOVEE_SCENE_BLINKING 0x08

/* GATT identifiers shared by nearly every Govee light, as 128-bit UUIDs in
 * the little-endian byte order the STM32WB stack (and NimBLE's raw API) use. */
extern const uint8_t GOVEE_SERVICE_UUID_LE[16];
extern const uint8_t GOVEE_CONTROL_UUID_LE[16];
#define GOVEE_SERVICE_UUID_STR "00010203-0405-0607-0809-0a0b0c0d1910"
#define GOVEE_CONTROL_UUID_STR "00010203-0405-0607-0809-0a0b0c0d2b11"

/* Which colour command format a model speaks. */
typedef enum {
    GoveeDialectLegacy, /* 33 05 02 R G B, brightness 0-255 */
    GoveeDialectRgbww, /* 33 05 0D R G B, brightness 0-100 */
    GoveeDialectRgbic, /* 33 05 15 01 R G B ... mask, brightness 0-100 */
} GoveeDialect;

uint8_t govee_checksum(const uint8_t* data, size_t len);
void govee_frame_build(
    uint8_t out[GOVEE_FRAME_LEN],
    uint8_t type,
    uint8_t cmd,
    const uint8_t* payload,
    size_t payload_len);
bool govee_frame_verify(const uint8_t frame[GOVEE_FRAME_LEN]);

void govee_frame_power(uint8_t out[GOVEE_FRAME_LEN], bool on);
/* percent 1-100; scaled to the dialect's native range. Never sends zero. */
void govee_frame_brightness(uint8_t out[GOVEE_FRAME_LEN], uint8_t percent, GoveeDialect dialect);
void govee_frame_rgb(uint8_t out[GOVEE_FRAME_LEN], uint8_t r, uint8_t g, uint8_t b, GoveeDialect dialect);
void govee_frame_scene(uint8_t out[GOVEE_FRAME_LEN], uint8_t scene_id);
void govee_frame_music(uint8_t out[GOVEE_FRAME_LEN], uint8_t mode, uint8_t r, uint8_t g, uint8_t b);
void govee_frame_keepalive(uint8_t out[GOVEE_FRAME_LEN]);

/* Model table. `name` is the advertised local name, e.g. "ihoment_H6159_1A2B".
 * Returns false if it does not look like a Govee light. `model` gets the SKU
 * ("H6159"), NUL terminated, at least 6 bytes. */
bool govee_name_parse(const char* name, size_t name_len, char* model, size_t model_size);
GoveeDialect govee_dialect_for_model(const char* model);
const char* govee_dialect_name(GoveeDialect dialect);

/* HSV (h 0-359, s/v 0-255) to RGB, for the patterns. */
void govee_hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b);
