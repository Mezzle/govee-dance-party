#include "govee_protocol.h"

#include <string.h>
#include <ctype.h>

const uint8_t GOVEE_SERVICE_UUID_LE[16] = {
    0x10, 0x19, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00};
const uint8_t GOVEE_CONTROL_UUID_LE[16] = {
    0x11, 0x2b, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00};

uint8_t govee_checksum(const uint8_t* data, size_t len) {
    uint8_t x = 0;
    for(size_t i = 0; i < len; i++) x ^= data[i];
    return x;
}

void govee_frame_build(
    uint8_t out[GOVEE_FRAME_LEN],
    uint8_t type,
    uint8_t cmd,
    const uint8_t* payload,
    size_t payload_len) {
    memset(out, 0, GOVEE_FRAME_LEN);
    out[0] = type;
    out[1] = cmd;
    if(payload_len > GOVEE_FRAME_LEN - 3) payload_len = GOVEE_FRAME_LEN - 3;
    if(payload && payload_len) memcpy(&out[2], payload, payload_len);
    out[GOVEE_FRAME_LEN - 1] = govee_checksum(out, GOVEE_FRAME_LEN - 1);
}

bool govee_frame_verify(const uint8_t frame[GOVEE_FRAME_LEN]) {
    return govee_checksum(frame, GOVEE_FRAME_LEN - 1) == frame[GOVEE_FRAME_LEN - 1];
}

void govee_frame_power(uint8_t out[GOVEE_FRAME_LEN], bool on) {
    uint8_t p = on ? 1 : 0;
    govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_POWER, &p, 1);
}

void govee_frame_brightness(uint8_t out[GOVEE_FRAME_LEN], uint8_t percent, GoveeDialect dialect) {
    if(percent < 1) percent = 1;
    if(percent > 100) percent = 100;
    uint8_t level;
    if(dialect == GoveeDialectLegacy) {
        level = (uint8_t)((percent * 255u + 50u) / 100u);
        if(level == 0) level = 1;
    } else {
        level = percent;
    }
    govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_BRIGHTNESS, &level, 1);
}

void govee_frame_rgb(uint8_t out[GOVEE_FRAME_LEN], uint8_t r, uint8_t g, uint8_t b, GoveeDialect dialect) {
    switch(dialect) {
    case GoveeDialectRgbic: {
        /* 33 05 15 01 R G B 00 00 00 00 00 FF 7F: whole-strip mask. */
        uint8_t p[] = {0x15, 0x01, r, g, b, 0, 0, 0, 0, 0, 0xFF, 0x7F};
        govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
        break;
    }
    case GoveeDialectRgbww: {
        /* 33 05 0D R G B 00 00 00 00 00: kelvin and tint zeroed. */
        uint8_t p[] = {0x0D, r, g, b, 0, 0, 0, 0, 0};
        govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
        break;
    }
    case GoveeDialectLegacy:
    default: {
        /* 33 05 02 R G B 00 */
        uint8_t p[] = {0x02, r, g, b, 0x00};
        govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
        break;
    }
    }
}

void govee_frame_scene(uint8_t out[GOVEE_FRAME_LEN], uint8_t scene_id) {
    uint8_t p[] = {GOVEE_SUBMODE_SCENE, scene_id};
    govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
}

void govee_frame_music(uint8_t out[GOVEE_FRAME_LEN], uint8_t mode, uint8_t r, uint8_t g, uint8_t b) {
    /* 33 05 01 <mode> 00 R G B (softgrass/govee-api-ble, confirmed on H6159). */
    uint8_t p[] = {GOVEE_SUBMODE_MUSIC_LEGACY, mode, 0x00, r, g, b};
    govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
}

void govee_frame_keepalive(uint8_t out[GOVEE_FRAME_LEN]) {
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_POWER, NULL, 0);
}

/* ------------------------------------------------------------------------ */

static const char* const GOVEE_NAME_PREFIXES[] = {"ihoment_", "Govee_", "Minger_", "GBK_", "GVH", "GVR"};

bool govee_name_parse(const char* name, size_t name_len, char* model, size_t model_size) {
    if(!name || name_len == 0 || model_size < 6) return false;
    for(size_t i = 0; i < sizeof(GOVEE_NAME_PREFIXES) / sizeof(GOVEE_NAME_PREFIXES[0]); i++) {
        const char* prefix = GOVEE_NAME_PREFIXES[i];
        size_t plen = strlen(prefix);
        if(name_len < plen + 5) continue;
        if(strncasecmp(name, prefix, plen) != 0) continue;
        const char* sku = name + plen;
        if(toupper((unsigned char)sku[0]) != 'H') continue;
        bool ok = true;
        for(size_t k = 1; k < 5; k++) {
            if(!isalnum((unsigned char)sku[k])) ok = false;
        }
        if(!ok) continue;
        if(name_len > plen + 5 && sku[5] != '_') continue;
        for(size_t k = 0; k < 5; k++) model[k] = (char)toupper((unsigned char)sku[k]);
        model[5] = '\0';
        return true;
    }
    return false;
}

/* RGBIC strips, TV backlights, bars and panels that speak 33 05 15. */
static const char* const RGBIC_MODELS[] = {
    "H6143", "H6144", "H619A", "H619B", "H619C", "H619E", "H619Z", "H61A0", "H61A1", "H61A2",
    "H618A", "H61B5", "H61E1", "H61E5", "H6168", "H6199", "H6052", "H6056", "H6059", "H6061",
    "H6062", "H6065", "H6072", "H6076",
};

/* Verified on hardware (H6181, fw 2.03.11): RGBWW 0x0D dialect, brightness 0-100. */
static const char* const RGBWW_MODELS[] = {"H6181"};

GoveeDialect govee_dialect_for_model(const char* model) {
    if(!model) return GoveeDialectLegacy;
    for(size_t i = 0; i < sizeof(RGBIC_MODELS) / sizeof(RGBIC_MODELS[0]); i++) {
        if(strcasecmp(model, RGBIC_MODELS[i]) == 0) return GoveeDialectRgbic;
    }
    for(size_t i = 0; i < sizeof(RGBWW_MODELS) / sizeof(RGBWW_MODELS[0]); i++) {
        if(strcasecmp(model, RGBWW_MODELS[i]) == 0) return GoveeDialectRgbww;
    }
    return GoveeDialectLegacy;
}

const char* govee_dialect_name(GoveeDialect dialect) {
    switch(dialect) {
    case GoveeDialectRgbic:
        return "RGBIC";
    case GoveeDialectRgbww:
        return "RGBWW";
    default:
        return "RGB";
    }
}

void govee_hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
    h %= 360;
    uint8_t region = h / 60;
    uint16_t rem = (h % 60) * 255 / 60;
    uint8_t p = (uint8_t)((v * (255 - s)) / 255);
    uint8_t q = (uint8_t)((v * (255 - (s * rem) / 255)) / 255);
    uint8_t t = (uint8_t)((v * (255 - (s * (255 - rem)) / 255)) / 255);
    switch(region) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}
