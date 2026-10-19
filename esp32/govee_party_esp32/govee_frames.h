/* Govee BLE frames, ported from the Govee-Bluetooth Home Assistant integration
 * (same table as the Flipper app's govee_protocol.c). */
#pragma once
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#define GOVEE_FRAME_LEN 20

enum GoveeDialect { GoveeLegacy, GoveeRgbww, GoveeRgbic };

static inline void goveeBuild(uint8_t out[20], uint8_t type, uint8_t cmd, const uint8_t* p, size_t n) {
    memset(out, 0, 20);
    out[0] = type;
    out[1] = cmd;
    if(n > 17) n = 17;
    if(p && n) memcpy(out + 2, p, n);
    uint8_t x = 0;
    for(int i = 0; i < 19; i++) x ^= out[i];
    out[19] = x;
}

static inline void goveePower(uint8_t out[20], bool on) {
    uint8_t p = on ? 1 : 0;
    goveeBuild(out, 0x33, 0x01, &p, 1);
}

static inline void goveeBrightness(uint8_t out[20], uint8_t percent, GoveeDialect d) {
    if(percent < 1) percent = 1;
    if(percent > 100) percent = 100;
    uint8_t level = d == GoveeLegacy ? (uint8_t)((percent * 255u + 50u) / 100u) : percent;
    if(level == 0) level = 1;
    goveeBuild(out, 0x33, 0x04, &level, 1);
}

static inline void goveeRgb(uint8_t out[20], uint8_t r, uint8_t g, uint8_t b, GoveeDialect d) {
    if(d == GoveeRgbic) {
        uint8_t p[] = {0x15, 0x01, r, g, b, 0, 0, 0, 0, 0, 0xFF, 0x7F};
        goveeBuild(out, 0x33, 0x05, p, sizeof(p));
    } else if(d == GoveeRgbww) {
        uint8_t p[] = {0x0D, r, g, b, 0, 0, 0, 0, 0};
        goveeBuild(out, 0x33, 0x05, p, sizeof(p));
    } else {
        uint8_t p[] = {0x02, r, g, b, 0x00};
        goveeBuild(out, 0x33, 0x05, p, sizeof(p));
    }
}

static inline void goveeScene(uint8_t out[20], uint8_t id) {
    uint8_t p[] = {0x04, id};
    goveeBuild(out, 0x33, 0x05, p, 2);
}

/* H6159-family music mode: 33 05 01 <mode> 00 R G B. 0 = energic. */
static inline void goveeMusic(uint8_t out[20], uint8_t mode, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t p[] = {0x01, mode, 0x00, r, g, b};
    goveeBuild(out, 0x33, 0x05, p, sizeof(p));
}

static const char* const GOVEE_PREFIXES[] = {"ihoment_", "Govee_", "Minger_", "GBK_", "GVH", "GVR"};

/* "ihoment_H6159_1A2B" -> "H6159". Returns false if not a Govee name. */
static inline bool goveeParseName(const char* name, char model[6]) {
    size_t len = strlen(name);
    for(auto prefix : GOVEE_PREFIXES) {
        size_t plen = strlen(prefix);
        if(len < plen + 5 || strncasecmp(name, prefix, plen) != 0) continue;
        const char* sku = name + plen;
        if(toupper((unsigned char)sku[0]) != 'H') continue;
        bool ok = true;
        for(int k = 1; k < 5; k++) if(!isalnum((unsigned char)sku[k])) ok = false;
        if(!ok) continue;
        for(int k = 0; k < 5; k++) model[k] = (char)toupper((unsigned char)sku[k]);
        model[5] = 0;
        return true;
    }
    return false;
}

static const char* const GOVEE_RGBIC[] = {
    "H6143", "H6144", "H619A", "H619B", "H619C", "H619E", "H619Z", "H61A0", "H61A1", "H61A2",
    "H618A", "H61B5", "H61E1", "H61E5", "H6168", "H6199", "H6052", "H6056", "H6059", "H6061",
    "H6062", "H6065", "H6072", "H6076"};

static inline GoveeDialect goveeDialect(const char* model) {
    for(auto m : GOVEE_RGBIC) if(strcasecmp(model, m) == 0) return GoveeRgbic;
    if(strcasecmp(model, "H6181") == 0) return GoveeRgbww;
    return GoveeLegacy;
}

static inline void hsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    h %= 360;
    uint8_t region = h / 60;
    uint16_t rem = (h % 60) * 255 / 60;
    uint8_t p = (uint8_t)((v * (255 - s)) / 255);
    uint8_t q = (uint8_t)((v * (255 - (s * rem) / 255)) / 255);
    uint8_t t = (uint8_t)((v * (255 - (s * (255 - rem)) / 255)) / 255);
    switch(region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}
