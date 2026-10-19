/* Govee BLE frames: a straight C++ port of the Flipper app's govee_protocol.c
 * (same tables, same byte layouts). Both must track
 * custom_components/govee_bluetooth/protocol.py in the Govee-Bluetooth repo;
 * citations (SPEC, YAML, HGL, GENZERO) refer to docs/govee-ble-protocol.md
 * there. Keep the function order the same as govee_protocol.c so diffs line up. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define GOVEE_FRAME_LEN 20

#define GOVEE_TYPE_WRITE  0x33
#define GOVEE_TYPE_READ   0xAA
#define GOVEE_TYPE_NOTIFY 0xEE

#define GOVEE_CMD_POWER         0x01
#define GOVEE_CMD_BRIGHTNESS    0x04
#define GOVEE_CMD_MODE          0x05
#define GOVEE_CMD_VERSION       0x06
#define GOVEE_CMD_DEVICE_INFO   0x07
#define GOVEE_CMD_LIGHT_NUM     0x0F
#define GOVEE_CMD_IC_NUM        0x40
#define GOVEE_CMD_SEGMENT_COLOR 0xA5
#define GOVEE_CMD_SECRET_READ   0xB1
#define GOVEE_CMD_SECRET_WRITE  0xB2
#define GOVEE_INFO_HARDWARE     0x03

#define GOVEE_SUBMODE_COLOR_LEGACY     0x02
#define GOVEE_SUBMODE_COLOR_DREAMCOLOR 0x0B
#define GOVEE_SUBMODE_COLOR_RGBWW      0x0D
#define GOVEE_SUBMODE_COLOR_RGBIC      0x15
#define GOVEE_SUBMODE_MUSIC_LEGACY     0x01
#define GOVEE_SUBMODE_MUSIC_DREAMCOLOR 0x11
#define GOVEE_SUBMODE_MUSIC_MODERN     0x13
#define GOVEE_SUBMODE_SCENE            0x04
#define GOVEE_SUBMODE_DIY              0x0A
#define GOVEE_SUBMODE_VIDEO            0x00

#define GOVEE_MUSIC_ENERGIC  0x00
#define GOVEE_MUSIC_SPECTRUM 0x01
#define GOVEE_MUSIC_ROLLING  0x02
#define GOVEE_MUSIC_RHYTHM   0x03
#define GOVEE_MUSIC11_RHYTHM    0x03
#define GOVEE_MUSIC11_SPECTRUM  0x04
#define GOVEE_MUSIC11_ENERGETIC 0x05
#define GOVEE_MUSIC11_ROLLING   0x06
#define GOVEE_MUSIC13_DEFAULT           0x06 /* GENZERO H617A: 33 05 13 06 63 */
#define GOVEE_MUSIC_SENSITIVITY_DEFAULT 0x63

#define GOVEE_SCENE_BLINKING 0x08

#define GOVEE_PROBE_R 0x12
#define GOVEE_PROBE_G 0x34
#define GOVEE_PROBE_B 0x56

#define GOVEE_DEFAULT_SEGMENTS 15
#define GOVEE_MASK_MAX_BYTES   7

#define GOVEE_SERVICE_UUID_STR "00010203-0405-0607-0809-0a0b0c0d1910"
#define GOVEE_CONTROL_UUID_STR "00010203-0405-0607-0809-0a0b0c0d2b11"
#define GOVEE_STATUS_UUID_STR  "00010203-0405-0607-0809-0a0b0c0d2b10"

enum GoveeDialect { GoveeLegacy, GoveeRgbww, GoveeRgbic, GoveeDreamcolor, GoveeDialectCount };

/* Manufacturer data: [flags][88 EC][pactType u16 BE][pactCode] (SPEC 11.2). */
struct GoveeAdvInfo {
    bool valid = false;
    uint8_t flags = 0;
    uint8_t protoVersion = 0;
    bool encrypted = false; /* flags bit 6: AES-wrapped link, plaintext frames will not work */
    uint16_t pactType = 0;
    uint8_t pactCode = 0;
};

static inline uint8_t goveeChecksum(const uint8_t* d, size_t n) {
    uint8_t x = 0;
    for(size_t i = 0; i < n; i++) x ^= d[i];
    return x;
}

static inline void goveeBuild(uint8_t out[20], uint8_t type, uint8_t cmd, const uint8_t* p, size_t n) {
    memset(out, 0, 20);
    out[0] = type;
    out[1] = cmd;
    if(n > 17) n = 17;
    if(p && n) memcpy(out + 2, p, n);
    out[19] = goveeChecksum(out, 19);
}

static inline bool goveeVerify(const uint8_t f[20]) {
    return goveeChecksum(f, 19) == f[19];
}

/* ------------------------------------------------------------- writes */

static inline void goveePower(uint8_t out[20], bool on) {
    uint8_t p = on ? 1 : 0;
    goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_POWER, &p, 1);
}

static inline void goveeBrightness(uint8_t out[20], uint8_t percent, GoveeDialect d) {
    if(percent < 1) percent = 1;
    if(percent > 100) percent = 100;
    uint8_t level = d == GoveeLegacy ? (uint8_t)((percent * 255u + 50u) / 100u) : percent;
    if(level == 0) level = 1;
    goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_BRIGHTNESS, &level, 1);
}

/* All-segments mask: bit i = segment i, LSB first, little-endian across bytes.
 * Returns the byte count (at least 2). 0 segments = the default 15 (FF 7F). */
static inline size_t goveeSegmentMask(uint8_t segments, uint8_t out[GOVEE_MASK_MAX_BYTES]) {
    if(segments == 0) segments = GOVEE_DEFAULT_SEGMENTS;
    if(segments > GOVEE_MASK_MAX_BYTES * 8) segments = GOVEE_MASK_MAX_BYTES * 8;
    memset(out, 0, GOVEE_MASK_MAX_BYTES);
    for(uint8_t i = 0; i < segments; i++) out[i / 8] |= (uint8_t)(1u << (i % 8));
    size_t n = (segments + 7) / 8;
    return n < 2 ? 2 : n;
}

static inline void goveeRgbSeg(uint8_t out[20], uint8_t r, uint8_t g, uint8_t b, GoveeDialect d, uint8_t segments) {
    uint8_t mask[GOVEE_MASK_MAX_BYTES];
    if(d == GoveeRgbic) {
        /* 33 05 15 01 R G B 00 00 00 00 00 <mask> (H60A1 / H617A RGB form) */
        size_t n = goveeSegmentMask(segments, mask);
        uint8_t p[17] = {GOVEE_SUBMODE_COLOR_RGBIC, 0x01, r, g, b, 0, 0, 0, 0, 0};
        memcpy(p + 10, mask, n);
        goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, 10 + n);
    } else if(d == GoveeDreamcolor) {
        /* 33 05 0B R G B mask0 mask1 00 00 (spec form A, unverified on hardware) */
        goveeSegmentMask(segments > 16 ? 16 : segments, mask);
        uint8_t p[] = {GOVEE_SUBMODE_COLOR_DREAMCOLOR, r, g, b, mask[0], mask[1], 0, 0};
        goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
    } else if(d == GoveeRgbww) {
        uint8_t p[] = {GOVEE_SUBMODE_COLOR_RGBWW, r, g, b, 0, 0, 0, 0, 0};
        goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
    } else {
        uint8_t p[] = {GOVEE_SUBMODE_COLOR_LEGACY, r, g, b, 0x00};
        goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
    }
}

static inline void goveeRgb(uint8_t out[20], uint8_t r, uint8_t g, uint8_t b, GoveeDialect d) {
    goveeRgbSeg(out, r, g, b, d, 0);
}

static inline void goveeScene(uint8_t out[20], uint8_t id) {
    uint8_t p[] = {GOVEE_SUBMODE_SCENE, id};
    goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, 2);
}

/* 33 05 01 <mode> 00 R G B (softgrass; H6102/H6104/H6127 class). */
static inline void goveeMusic(uint8_t out[20], uint8_t mode, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t p[] = {GOVEE_SUBMODE_MUSIC_LEGACY, mode, 0x00, r, g, b};
    goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
}

/* 33 05 11 <mode> <sensitivity> 00 00 (dreamcolor SubModeMusicV3, legacy modes 03-06). */
static inline void goveeMusicDreamcolor(uint8_t out[20], uint8_t mode, uint8_t sensitivity) {
    uint8_t p[] = {GOVEE_SUBMODE_MUSIC_DREAMCOLOR, mode, sensitivity, 0x00, 0x00};
    goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
}

/* 33 05 13 <code> <sensitivity> (SubModeNewMusic, modern RGBIC stack). */
static inline void goveeMusicModern(uint8_t out[20], uint8_t code, uint8_t sensitivity) {
    uint8_t p[] = {GOVEE_SUBMODE_MUSIC_MODERN, code, sensitivity};
    goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
}

static inline void goveeSecretWrite(uint8_t out[20], const uint8_t secret[8]) {
    goveeBuild(out, GOVEE_TYPE_WRITE, GOVEE_CMD_SECRET_WRITE, secret, 8);
}

static inline void goveeKeepalive(uint8_t out[20]) {
    goveeBuild(out, GOVEE_TYPE_READ, GOVEE_CMD_POWER, nullptr, 0);
}

/* -------------------------------------------------------------- reads */

static inline void goveeReadPower(uint8_t out[20]) { goveeBuild(out, GOVEE_TYPE_READ, GOVEE_CMD_POWER, nullptr, 0); }
static inline void goveeReadBrightness(uint8_t out[20]) { goveeBuild(out, GOVEE_TYPE_READ, GOVEE_CMD_BRIGHTNESS, nullptr, 0); }
static inline void goveeReadMode(uint8_t out[20]) {
    uint8_t sel = 0x01; /* required selector (SPEC 4.3) */
    goveeBuild(out, GOVEE_TYPE_READ, GOVEE_CMD_MODE, &sel, 1);
}
static inline void goveeReadFirmware(uint8_t out[20]) { goveeBuild(out, GOVEE_TYPE_READ, GOVEE_CMD_VERSION, nullptr, 0); }
static inline void goveeReadHardware(uint8_t out[20]) {
    uint8_t sel = GOVEE_INFO_HARDWARE;
    goveeBuild(out, GOVEE_TYPE_READ, GOVEE_CMD_DEVICE_INFO, &sel, 1);
}
static inline void goveeReadIcCount(uint8_t out[20]) { goveeBuild(out, GOVEE_TYPE_READ, GOVEE_CMD_IC_NUM, nullptr, 0); }
static inline void goveeReadLightCount(uint8_t out[20]) { goveeBuild(out, GOVEE_TYPE_READ, GOVEE_CMD_LIGHT_NUM, nullptr, 0); }
static inline void goveeReadSecret(uint8_t out[20]) { goveeBuild(out, GOVEE_TYPE_READ, GOVEE_CMD_SECRET_READ, nullptr, 0); }
static inline void goveeReadSegmentColors(uint8_t out[20], uint8_t group) {
    goveeBuild(out, GOVEE_TYPE_READ, GOVEE_CMD_SEGMENT_COLOR, &group, 1);
}

static inline bool goveeReadMatches(const uint8_t req[20], const uint8_t rep[20]) {
    if(rep[0] != GOVEE_TYPE_READ || rep[1] != req[1]) return false;
    switch(req[1]) {
    case GOVEE_CMD_DEVICE_INFO:
    case GOVEE_CMD_SEGMENT_COLOR:
    case 0xA2:
        return rep[2] == req[2];
    default:
        return true;
    }
}

static inline void goveeReplyVersionString(const uint8_t rep[20], char* out, size_t size) {
    size_t start = rep[1] == GOVEE_CMD_DEVICE_INFO ? 3 : 2, n = 0;
    for(size_t i = start; i < 19 && n + 1 < size; i++) {
        uint8_t c = rep[i];
        if(c == 0 || c < 0x20 || c > 0x7e) break;
        out[n++] = (char)c;
    }
    while(n && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

static inline bool goveeReplySegmentPageHas(const uint8_t rep[20], uint8_t r, uint8_t g, uint8_t b) {
    if(rep[0] != GOVEE_TYPE_READ || rep[1] != GOVEE_CMD_SEGMENT_COLOR) return false;
    for(size_t i = 3; i + 2 < 19; i++) {
        if(rep[i] == r && rep[i + 1] == g && rep[i + 2] == b) return true;
    }
    return false;
}

static inline const char* goveeSubmodeName(uint8_t s) {
    switch(s) {
    case GOVEE_SUBMODE_VIDEO: return "video";
    case GOVEE_SUBMODE_MUSIC_LEGACY: return "music01";
    case GOVEE_SUBMODE_COLOR_LEGACY: return "colour02";
    case 0x03: return "music03";
    case GOVEE_SUBMODE_SCENE: return "scene";
    case 0x05: return "mic";
    case 0x07: return "diy07";
    case 0x09: return "scene09";
    case GOVEE_SUBMODE_DIY: return "diy";
    case GOVEE_SUBMODE_COLOR_DREAMCOLOR: return "colour0B";
    case 0x0C: return "music0C";
    case GOVEE_SUBMODE_COLOR_RGBWW: return "colour0D";
    case 0x0E: return "music0E";
    case 0x0F: return "music0F";
    case GOVEE_SUBMODE_MUSIC_DREAMCOLOR: return "music11";
    case 0x12: return "music12";
    case GOVEE_SUBMODE_MUSIC_MODERN: return "music13";
    case 0x14: return "music14";
    case GOVEE_SUBMODE_COLOR_RGBIC: return "colour15";
    case 0x16: case 0x17: case 0x19: case 0x22: return "music";
    case 0xFF: return "mic";
    default: return "?";
    }
}

static inline bool goveeSubmodeIsColor(uint8_t s) {
    return s == GOVEE_SUBMODE_COLOR_LEGACY || s == GOVEE_SUBMODE_COLOR_DREAMCOLOR ||
           s == GOVEE_SUBMODE_COLOR_RGBWW || s == GOVEE_SUBMODE_COLOR_RGBIC;
}

static inline const char* goveeCmdName(uint8_t cmd) {
    switch(cmd) {
    case GOVEE_CMD_POWER: return "power";
    case GOVEE_CMD_BRIGHTNESS: return "brightness";
    case GOVEE_CMD_MODE: return "mode";
    case GOVEE_CMD_VERSION: return "firmware";
    case GOVEE_CMD_DEVICE_INFO: return "info";
    case GOVEE_CMD_LIGHT_NUM: return "lightnum";
    case GOVEE_CMD_IC_NUM: return "icnum";
    case GOVEE_CMD_SEGMENT_COLOR: return "segcolour";
    case GOVEE_CMD_SECRET_READ: return "secret";
    case GOVEE_CMD_SECRET_WRITE: return "secretwrite";
    default: return nullptr;
    }
}

/* Short human reading for logs; fromLight tells a read request from its reply. */
static inline void goveeDescribe(const uint8_t f[20], bool fromLight, char* out, size_t size) {
    const char* name = goveeCmdName(f[1]);
    char cmdbuf[12];
    if(!name) {
        snprintf(cmdbuf, sizeof(cmdbuf), "cmd 0x%02X", f[1]);
        name = cmdbuf;
    }
    if(!goveeVerify(f)) {
        snprintf(out, size, "bad checksum");
        return;
    }
    if(f[0] == GOVEE_TYPE_WRITE) {
        if(f[1] == GOVEE_CMD_MODE) {
            snprintf(out, size, "write mode %s %02X %02X %02X %02X %02X", goveeSubmodeName(f[2]), f[3], f[4], f[5], f[6], f[7]);
        } else if(f[1] == GOVEE_CMD_SECRET_WRITE) {
            snprintf(out, size, "write secret");
        } else {
            snprintf(out, size, "write %s %u", name, f[2]);
        }
        return;
    }
    if(f[0] == GOVEE_TYPE_READ && !fromLight) {
        if(f[1] == GOVEE_CMD_DEVICE_INFO || f[1] == GOVEE_CMD_SEGMENT_COLOR) {
            snprintf(out, size, "read %s %u", name, f[2]);
        } else {
            snprintf(out, size, "read %s", name);
        }
        return;
    }
    if(f[0] == GOVEE_TYPE_READ) {
        switch(f[1]) {
        case GOVEE_CMD_POWER: snprintf(out, size, "power %s", f[2] ? "on" : "off"); return;
        case GOVEE_CMD_BRIGHTNESS: snprintf(out, size, "brightness %u", f[2]); return;
        case GOVEE_CMD_MODE:
            snprintf(out, size, "mode %s %02X %02X %02X %02X", goveeSubmodeName(f[2]), f[3], f[4], f[5], f[6]);
            return;
        case GOVEE_CMD_VERSION:
        case GOVEE_CMD_DEVICE_INFO: {
            char s[16];
            goveeReplyVersionString(f, s, sizeof(s));
            snprintf(out, size, "%s %s", name, s);
            return;
        }
        case GOVEE_CMD_IC_NUM: snprintf(out, size, "ic %u segments %u", (unsigned)((f[2] << 8) | f[3]), f[4]); return;
        case GOVEE_CMD_LIGHT_NUM: snprintf(out, size, "lights %u", f[2]); return;
        case GOVEE_CMD_SEGMENT_COLOR: snprintf(out, size, "segcolour group %u", f[2]); return;
        case GOVEE_CMD_SECRET_READ: snprintf(out, size, "secret %s", f[2] == 1 ? "present" : "absent"); return;
        default: snprintf(out, size, "read %s", name); return;
        }
    }
    if(f[0] == GOVEE_TYPE_NOTIFY) {
        snprintf(out, size, "push 0x%02X", f[1]);
        return;
    }
    snprintf(out, size, "type 0x%02X %s", f[0], name);
}

/* ------------------------------------------------------- advertisement */

/* `data` is the manufacturer-specific payload as NimBLE's getManufacturerData()
 * returns it (company-id bytes included: [flags][88][EC]...). A stack that
 * already ate a "company id" of 0x8803 / 0x8843 leaves [EC]... with the
 * flags lost; both are accepted. */
static inline bool goveeParseMfr(const uint8_t* data, size_t len, GoveeAdvInfo& out) {
    out = GoveeAdvInfo();
    if(!data) return false;
    size_t off;
    uint8_t flags;
    if(len >= 6 && data[1] == 0x88 && data[2] == 0xEC) {
        flags = data[0];
        off = 3;
    } else if(len >= 5 && data[0] == 0x88 && data[1] == 0xEC) {
        flags = 0;
        off = 2;
    } else if(len >= 4 && data[0] == 0xEC) {
        flags = 0;
        off = 1;
    } else {
        return false;
    }
    out.valid = true;
    out.flags = flags;
    out.protoVersion = flags & 0x0F;
    out.encrypted = (flags & 0x40) != 0;
    out.pactType = (uint16_t)((data[off] << 8) | data[off + 1]);
    out.pactCode = data[off + 2];
    return true;
}

/* ---------------------------------------------------------- model table */

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
        if(len > plen + 5 && sku[5] != '_') continue;
        for(int k = 0; k < 5; k++) model[k] = (char)toupper((unsigned char)sku[k]);
        model[5] = 0;
        return true;
    }
    return false;
}

template <size_t N>
static inline bool goveeInTable(const char* model, const char* const (&table)[N]) {
    for(auto m : table) if(strcasecmp(model, m) == 0) return true;
    return false;
}

static const char* const GOVEE_RGBIC[] = {
    "H6143", "H6144", "H619A", "H619B", "H619C", "H619E", "H619Z", "H61A0", "H61A1", "H61A2",
    "H618A", "H61B5", "H61E1", "H61E5", "H6168", "H6199", "H6052", "H6056", "H6059", "H6061",
    "H6062", "H6065", "H6072", "H6076", "H617A", "H6198", "H60A0", "H60A1", "H60A4", "H60A6"};

/* dreamcolorlightv1/v2, h612526, barelightv1, pact_tvlightv2: 0x0B on pact 1,
 * 0x15 on pact 2 / 10 (HGL h6199-pact1). */
static const char* const GOVEE_DREAMCOLOR[] = {
    "H611A", "H611B", "H611Z", "H6123", "H6125", "H6126", "H612A", "H612B", "H612C", "H612D",
    "H612E", "H612F", "H6143", "H6144", "H6145", "H6146", "H6147", "H6168", "H6171", "H617A",
    "H6184", "H6198", "H6199", "H619A", "H61A0", "H61A8", "H61A9", "H61E0", "H61E1", "H70A1",
    "H70A2", "H70A3"};

static const char* const GOVEE_RGBWW[] = {"H6181"};

/* secret_key_pairing families (SPEC 10.1), minus SKUs that also sit in an
 * untagged legacy family. */
static const char* const GOVEE_SECRET[] = {
    "H611B", "H611Z", "H6123", "H612A", "H612B", "H612C", "H612D", "H612E", "H612F", "H6168",
    "H617A", "H619A", "H61A0", "H61A8", "H61A9", "H61E0", "H61E1", "H70A1", "H70A2", "H70A3",
    "H61B5", "H61E5", "H61D3", "H6640", "H6641",
    "H6141", "H6142", "H6154", "H6170", "H6178", "H6192", "H6195", "H6196", "H6197",
    "H6050", "H6051", "H6055", "H6058", "H6059", "H6073",
    "H60A0", "H60A1", "H60A4", "H60A6", "H60C1",
    "H6047", "H6043", "H6042", "H6039", "H6038", "H6048"};

static inline GoveeDialect goveeDialect(const char* model) {
    if(goveeInTable(model, GOVEE_RGBIC)) return GoveeRgbic;
    if(goveeInTable(model, GOVEE_DREAMCOLOR)) return GoveeRgbic; /* modern pact assumed */
    if(goveeInTable(model, GOVEE_RGBWW)) return GoveeRgbww;
    return GoveeLegacy;
}

static inline GoveeDialect goveeDialectForLight(const char* model, const GoveeAdvInfo& adv) {
    GoveeDialect d = goveeDialect(model);
    if(adv.valid && goveeInTable(model, GOVEE_DREAMCOLOR)) d = adv.pactType == 1 ? GoveeDreamcolor : GoveeRgbic;
    return d;
}

static inline bool goveeNeedsSecret(const char* model) {
    return goveeInTable(model, GOVEE_SECRET);
}

static inline const char* goveeDialectName(GoveeDialect d) {
    switch(d) {
    case GoveeRgbic: return "RGBIC";
    case GoveeDreamcolor: return "Dream";
    case GoveeRgbww: return "RGBWW";
    default: return "RGB";
    }
}

static inline uint8_t goveeDialectSubmode(GoveeDialect d) {
    switch(d) {
    case GoveeRgbic: return GOVEE_SUBMODE_COLOR_RGBIC;
    case GoveeDreamcolor: return GOVEE_SUBMODE_COLOR_DREAMCOLOR;
    case GoveeRgbww: return GOVEE_SUBMODE_COLOR_RGBWW;
    default: return GOVEE_SUBMODE_COLOR_LEGACY;
    }
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
