#include "govee_protocol.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

const uint8_t GOVEE_SERVICE_UUID_LE[16] = {
    0x10, 0x19, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00};
const uint8_t GOVEE_CONTROL_UUID_LE[16] = {
    0x11, 0x2b, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00};
const uint8_t GOVEE_STATUS_UUID_LE[16] = {
    0x10, 0x2b, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00};

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

/* ------------------------------------------------------------- writes */

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

size_t govee_segment_mask(uint8_t segments, uint8_t out[GOVEE_MASK_MAX_BYTES]) {
    if(segments == 0) segments = GOVEE_DEFAULT_SEGMENTS;
    if(segments > GOVEE_MASK_MAX_BYTES * 8) segments = GOVEE_MASK_MAX_BYTES * 8;
    memset(out, 0, GOVEE_MASK_MAX_BYTES);
    for(uint8_t i = 0; i < segments; i++) out[i / 8] |= (uint8_t)(1u << (i % 8));
    size_t n = (segments + 7) / 8;
    return n < 2 ? 2 : n; /* always at least the two bytes every layout has room for */
}

void govee_frame_rgb_seg(
    uint8_t out[GOVEE_FRAME_LEN],
    uint8_t r,
    uint8_t g,
    uint8_t b,
    GoveeDialect dialect,
    uint8_t segments) {
    uint8_t mask[GOVEE_MASK_MAX_BYTES];
    switch(dialect) {
    case GoveeDialectRgbic: {
        /* 33 05 15 01 R G B 00 00 00 00 00 <mask>: the H60A1/H617A RGB form,
         * mask at bytes 10.., FF 7F for the common 15-segment strip. */
        size_t n = govee_segment_mask(segments, mask);
        uint8_t p[17] = {GOVEE_SUBMODE_COLOR_RGBIC, 0x01, r, g, b, 0, 0, 0, 0, 0};
        memcpy(&p[10], mask, n);
        govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, 10 + n);
        break;
    }
    case GoveeDialectDreamcolor: {
        /* 33 05 0B R G B mask0 mask1 00 00: spec form A
         * (dreamcolorlightv1/ble/SubModeColor.getWriteBytes). Two mask bytes
         * only; the layout has no room for more. Unverified on hardware. */
        govee_segment_mask(segments > 16 ? 16 : segments, mask);
        uint8_t p[] = {GOVEE_SUBMODE_COLOR_DREAMCOLOR, r, g, b, mask[0], mask[1], 0, 0};
        govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
        break;
    }
    case GoveeDialectRgbww: {
        /* 33 05 0D R G B 00 00 00 00 00: kelvin and tint zeroed. */
        uint8_t p[] = {GOVEE_SUBMODE_COLOR_RGBWW, r, g, b, 0, 0, 0, 0, 0};
        govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
        break;
    }
    case GoveeDialectLegacy:
    default: {
        /* 33 05 02 R G B 00 */
        uint8_t p[] = {GOVEE_SUBMODE_COLOR_LEGACY, r, g, b, 0x00};
        govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
        break;
    }
    }
}

void govee_frame_rgb(uint8_t out[GOVEE_FRAME_LEN], uint8_t r, uint8_t g, uint8_t b, GoveeDialect dialect) {
    govee_frame_rgb_seg(out, r, g, b, dialect, 0);
}

void govee_frame_scene(uint8_t out[GOVEE_FRAME_LEN], uint8_t scene_id) {
    uint8_t p[] = {GOVEE_SUBMODE_SCENE, scene_id};
    govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
}

void govee_frame_music(uint8_t out[GOVEE_FRAME_LEN], uint8_t mode, uint8_t r, uint8_t g, uint8_t b) {
    /* 33 05 01 <mode> 00 R G B (softgrass/govee-api-ble). The app uses sub-mode
     * 01 on the H6102/H6104/H6127 class; the H6159's own class is 03. */
    uint8_t p[] = {GOVEE_SUBMODE_MUSIC_LEGACY, mode, 0x00, r, g, b};
    govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
}

void govee_frame_music_dreamcolor(uint8_t out[GOVEE_FRAME_LEN], uint8_t mode, uint8_t sensitivity) {
    /* 33 05 11 <mode> <sensitivity> [style 00] [fixedColour 00]: legacy modes
     * 03-06 take the two trailing bytes, new modes (0x30+) need an A3 upload
     * first and are not attempted here. */
    uint8_t p[] = {GOVEE_SUBMODE_MUSIC_DREAMCOLOR, mode, sensitivity, 0x00, 0x00};
    govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
}

void govee_frame_music_modern(uint8_t out[GOVEE_FRAME_LEN], uint8_t code, uint8_t sensitivity) {
    /* 33 05 13 <code> <sensitivity>; GENZERO captured 33 05 13 06 63 on an H617A. */
    uint8_t p[] = {GOVEE_SUBMODE_MUSIC_MODERN, code, sensitivity};
    govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, p, sizeof(p));
}

void govee_frame_secret_write(uint8_t out[GOVEE_FRAME_LEN], const uint8_t secret[8]) {
    govee_frame_build(out, GOVEE_TYPE_WRITE, GOVEE_CMD_SECRET_WRITE, secret, 8);
}

void govee_frame_keepalive(uint8_t out[GOVEE_FRAME_LEN]) {
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_POWER, NULL, 0);
}

/* -------------------------------------------------------------- reads */

void govee_frame_read_power(uint8_t out[GOVEE_FRAME_LEN]) {
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_POWER, NULL, 0);
}

void govee_frame_read_brightness(uint8_t out[GOVEE_FRAME_LEN]) {
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_BRIGHTNESS, NULL, 0);
}

void govee_frame_read_mode(uint8_t out[GOVEE_FRAME_LEN]) {
    uint8_t sel = 0x01; /* AbsModeController selector, required (SPEC 4.3) */
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_MODE, &sel, 1);
}

void govee_frame_read_firmware(uint8_t out[GOVEE_FRAME_LEN]) {
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_VERSION, NULL, 0);
}

void govee_frame_read_hardware(uint8_t out[GOVEE_FRAME_LEN]) {
    uint8_t sel = GOVEE_INFO_HARDWARE;
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_DEVICE_INFO, &sel, 1);
}

void govee_frame_read_ic_count(uint8_t out[GOVEE_FRAME_LEN]) {
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_IC_NUM, NULL, 0);
}

void govee_frame_read_light_count(uint8_t out[GOVEE_FRAME_LEN]) {
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_LIGHT_NUM, NULL, 0);
}

void govee_frame_read_secret(uint8_t out[GOVEE_FRAME_LEN]) {
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_SECRET_READ, NULL, 0);
}

void govee_frame_read_segment_colors(uint8_t out[GOVEE_FRAME_LEN], uint8_t group) {
    govee_frame_build(out, GOVEE_TYPE_READ, GOVEE_CMD_SEGMENT_COLOR, &group, 1);
}

bool govee_frame_read_matches(const uint8_t request[GOVEE_FRAME_LEN], const uint8_t reply[GOVEE_FRAME_LEN]) {
    if(reply[0] != GOVEE_TYPE_READ || reply[1] != request[1]) return false;
    switch(request[1]) {
    case GOVEE_CMD_DEVICE_INFO:
    case GOVEE_CMD_SEGMENT_COLOR:
    case 0xA2: /* BulbStringColorController pages */
        return reply[2] == request[2];
    default:
        return true;
    }
}

void govee_reply_version_string(const uint8_t reply[GOVEE_FRAME_LEN], char* out, size_t out_size) {
    size_t start = reply[1] == GOVEE_CMD_DEVICE_INFO ? 3 : 2;
    size_t n = 0;
    for(size_t i = start; i < GOVEE_FRAME_LEN - 1 && n + 1 < out_size; i++) {
        uint8_t c = reply[i];
        if(c == 0) break;
        if(c < 0x20 || c > 0x7e) break;
        out[n++] = (char)c;
    }
    while(n && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

bool govee_reply_segment_page_has(const uint8_t reply[GOVEE_FRAME_LEN], uint8_t r, uint8_t g, uint8_t b) {
    if(reply[0] != GOVEE_TYPE_READ || reply[1] != GOVEE_CMD_SEGMENT_COLOR) return false;
    /* Records start after [group]: either [bri R G B] x N or [R G B] x N. Scan
     * every position; the probe colour is distinctive enough. */
    for(size_t i = 3; i + 2 < GOVEE_FRAME_LEN - 1; i++) {
        if(reply[i] == r && reply[i + 1] == g && reply[i + 2] == b) return true;
    }
    return false;
}

const char* govee_submode_name(uint8_t submode) {
    switch(submode) {
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

bool govee_submode_is_color(uint8_t submode) {
    return submode == GOVEE_SUBMODE_COLOR_LEGACY || submode == GOVEE_SUBMODE_COLOR_DREAMCOLOR ||
           submode == GOVEE_SUBMODE_COLOR_RGBWW || submode == GOVEE_SUBMODE_COLOR_RGBIC;
}

static const char* cmd_name(uint8_t cmd) {
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
    default: return NULL;
    }
}

void govee_frame_describe(const uint8_t frame[GOVEE_FRAME_LEN], bool from_light, char* out, size_t out_size) {
    const char* name = cmd_name(frame[1]);
    char cmdbuf[12];
    if(!name) {
        snprintf(cmdbuf, sizeof(cmdbuf), "cmd 0x%02X", frame[1]);
        name = cmdbuf;
    }
    if(!govee_frame_verify(frame)) {
        snprintf(out, out_size, "bad checksum");
        return;
    }
    if(frame[0] == GOVEE_TYPE_WRITE) {
        if(frame[1] == GOVEE_CMD_MODE) {
            snprintf(out, out_size, "write mode %s %02X %02X %02X %02X %02X", govee_submode_name(frame[2]), frame[3], frame[4], frame[5], frame[6], frame[7]);
        } else if(frame[1] == GOVEE_CMD_SECRET_WRITE) {
            snprintf(out, out_size, "write secret");
        } else {
            snprintf(out, out_size, "write %s %u", name, frame[2]);
        }
        return;
    }
    if(frame[0] == GOVEE_TYPE_READ && !from_light) {
        if(frame[1] == GOVEE_CMD_DEVICE_INFO || frame[1] == GOVEE_CMD_SEGMENT_COLOR) {
            snprintf(out, out_size, "read %s %u", name, frame[2]);
        } else {
            snprintf(out, out_size, "read %s", name);
        }
        return;
    }
    if(frame[0] == GOVEE_TYPE_READ) {
        switch(frame[1]) {
        case GOVEE_CMD_POWER:
            snprintf(out, out_size, "power %s", frame[2] ? "on" : "off");
            return;
        case GOVEE_CMD_BRIGHTNESS:
            snprintf(out, out_size, "brightness %u", frame[2]);
            return;
        case GOVEE_CMD_MODE:
            snprintf(out, out_size, "mode %s %02X %02X %02X %02X", govee_submode_name(frame[2]), frame[3], frame[4], frame[5], frame[6]);
            return;
        case GOVEE_CMD_VERSION:
        case GOVEE_CMD_DEVICE_INFO: {
            char s[16];
            govee_reply_version_string(frame, s, sizeof(s));
            snprintf(out, out_size, "%s %s", name, s);
            return;
        }
        case GOVEE_CMD_IC_NUM:
            snprintf(out, out_size, "ic %u segments %u", (unsigned)((frame[2] << 8) | frame[3]), frame[4]);
            return;
        case GOVEE_CMD_LIGHT_NUM:
            snprintf(out, out_size, "lights %u", frame[2]);
            return;
        case GOVEE_CMD_SEGMENT_COLOR:
            snprintf(out, out_size, "segcolour group %u", frame[2]);
            return;
        case GOVEE_CMD_SECRET_READ:
            snprintf(out, out_size, "secret %s", frame[2] == 1 ? "present" : "absent");
            return;
        default:
            snprintf(out, out_size, "read %s", name);
            return;
        }
    }
    if(frame[0] == GOVEE_TYPE_NOTIFY) {
        snprintf(out, out_size, "push 0x%02X", frame[1]);
        return;
    }
    snprintf(out, out_size, "type 0x%02X %s", frame[0], name);
}

/* ------------------------------------------------------- advertisement */

bool govee_adv_parse_mfr(const uint8_t* data, size_t len, GoveeAdvInfo* out) {
    memset(out, 0, sizeof(*out));
    if(!data) return false;
    /* On air: [flags][88][EC][ptHi][ptLo][code]. BlueZ / HA / NimBLE's
     * getManufacturerData() hand us exactly that (NimBLE keeps the two
     * "company id" bytes in the string). A stack that already split off a
     * company id of 0x8803 / 0x8843 leaves [EC][ptHi][ptLo][code] and the
     * flags byte is lost; accept that too, with flags unknown. */
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
    out->valid = true;
    out->flags = flags;
    out->proto_version = flags & 0x0F;
    out->encrypted = (flags & 0x40) != 0;
    out->pact_type = (uint16_t)((data[off] << 8) | data[off + 1]);
    out->pact_code = data[off + 2];
    return true;
}

/* ---------------------------------------------------------- model table */

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

static bool in_table(const char* model, const char* const* table, size_t n) {
    for(size_t i = 0; i < n; i++) {
        if(strcasecmp(model, table[i]) == 0) return true;
    }
    return false;
}
#define IN_TABLE(model, table) in_table(model, table, sizeof(table) / sizeof(table[0]))

/* RGBIC strips, TV backlights, bars and panels that speak 33 05 15 on current
 * firmware. Dreamcolor-family members also appear in DREAMCOLOR_MODELS. */
static const char* const RGBIC_MODELS[] = {
    "H6143", "H6144", "H619A", "H619B", "H619C", "H619E", "H619Z", "H61A0", "H61A1", "H61A2",
    "H618A", "H61B5", "H61E1", "H61E5", "H6168", "H6199", "H6052", "H6056", "H6059", "H6061",
    "H6062", "H6065", "H6072", "H6076", "H617A", "H6198", "H60A0", "H60A1", "H60A4", "H60A6",
};

/* Families dreamcolorlightv1 / dreamcolorlightv2 / h612526 / barelightv1 /
 * pact_tvlightv2 (YAML 746-758, 841-877, 929-942, 1150-1164): the app's
 * primary colour sub-mode is 0x0B with 0x15 as the modern-pact alternate.
 * Pact type 1 -> 0x0B, pact 2 / 10 -> 0x15 (HGL h6199-pact1). */
static const char* const DREAMCOLOR_MODELS[] = {
    "H611A", "H611B", "H611Z", "H6123", "H6125", "H6126", "H612A", "H612B", "H612C", "H612D",
    "H612E", "H612F", "H6143", "H6144", "H6145", "H6146", "H6147", "H6168", "H6171", "H617A",
    "H6184", "H6198", "H6199", "H619A", "H61A0", "H61A8", "H61A9", "H61E0", "H61E1", "H70A1",
    "H70A2", "H70A3",
};

/* Verified on hardware (H6181, fw 2.03.11): RGBWW 0x0D dialect, brightness 0-100. */
static const char* const RGBWW_MODELS[] = {"H6181"};

/* Families tagged secret_key_pairing (SPEC 10.1): dreamcolorlightv1, h61d3,
 * stringlightv2, hollowlamp, h60ax, h6047. SKUs that also sit in an untagged
 * legacy family (H6159, H6160, H6109, H6110) are left out: the bench H6159
 * takes writes from anyone. */
static const char* const SECRET_MODELS[] = {
    /* dreamcolorlightv1 */
    "H611B", "H611Z", "H6123", "H612A", "H612B", "H612C", "H612D", "H612E", "H612F", "H6168",
    "H617A", "H619A", "H61A0", "H61A8", "H61A9", "H61E0", "H61E1", "H70A1", "H70A2", "H70A3",
    /* h61d3 */
    "H61B5", "H61E5", "H61D3", "H6640", "H6641",
    /* stringlightv2 (only the SKUs not also in a legacy family) */
    "H6141", "H6142", "H6154", "H6170", "H6178", "H6192", "H6195", "H6196", "H6197",
    /* hollowlamp */
    "H6050", "H6051", "H6055", "H6058", "H6059", "H6073",
    /* h60ax */
    "H60A0", "H60A1", "H60A4", "H60A6", "H60C1",
    /* h6047 */
    "H6047", "H6043", "H6042", "H6039", "H6038", "H6048",
};

GoveeDialect govee_dialect_for_model(const char* model) {
    if(!model) return GoveeDialectLegacy;
    if(IN_TABLE(model, RGBIC_MODELS)) return GoveeDialectRgbic;
    if(IN_TABLE(model, DREAMCOLOR_MODELS)) return GoveeDialectRgbic; /* modern pact assumed */
    if(IN_TABLE(model, RGBWW_MODELS)) return GoveeDialectRgbww;
    return GoveeDialectLegacy;
}

GoveeDialect govee_dialect_for_light(const char* model, const GoveeAdvInfo* adv) {
    GoveeDialect d = govee_dialect_for_model(model);
    if(adv && adv->valid && model && IN_TABLE(model, DREAMCOLOR_MODELS)) {
        d = adv->pact_type == 1 ? GoveeDialectDreamcolor : GoveeDialectRgbic;
    }
    return d;
}

bool govee_model_needs_secret(const char* model) {
    return model && IN_TABLE(model, SECRET_MODELS);
}

const char* govee_dialect_name(GoveeDialect dialect) {
    switch(dialect) {
    case GoveeDialectRgbic:
        return "RGBIC";
    case GoveeDialectDreamcolor:
        return "Dream";
    case GoveeDialectRgbww:
        return "RGBWW";
    default:
        return "RGB";
    }
}

uint8_t govee_dialect_submode(GoveeDialect dialect) {
    switch(dialect) {
    case GoveeDialectRgbic: return GOVEE_SUBMODE_COLOR_RGBIC;
    case GoveeDialectDreamcolor: return GOVEE_SUBMODE_COLOR_DREAMCOLOR;
    case GoveeDialectRgbww: return GOVEE_SUBMODE_COLOR_RGBWW;
    default: return GOVEE_SUBMODE_COLOR_LEGACY;
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
