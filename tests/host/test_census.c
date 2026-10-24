/* Host unit tests for govee_protocol.c and govee_census.c. No Flipper SDK:
 *   make -C tests/host
 * A scripted fake light answers the census the way real firmware families
 * do, so the evidence rules (checksums, per-dialect echoes, redaction,
 * restoration, bounded reads, JSON shape) can be checked without hardware. */

#include "../../govee_census.h"
#include "../../generated/govee_frame_vectors_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if(!(cond)) {                                                            \
            failures++;                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while(0)

static void hex(const uint8_t* f, char out[41]) {
    for(int i = 0; i < GOVEE_FRAME_LEN; i++) snprintf(out + i * 2, 3, "%02x", f[i]);
}

static void reply_frame(uint8_t out[GOVEE_FRAME_LEN], uint8_t cmd, const uint8_t* body, size_t n) {
    govee_frame_build(out, GOVEE_TYPE_READ, cmd, body, n);
}

/* ----------------------------------------------------------- fake light */

typedef struct {
    GoveeDialect speaks; /* the one colour dialect the firmware understands */
    bool speaks_none; /* locked / foreign: answers nothing */
    bool power_on;
    uint8_t brightness;
    uint8_t submode; /* current mode byte */
    uint8_t rgb[3]; /* current colour (02 / 0D report it in AA 05) */
    bool has_page; /* answers AA A5 */
    uint8_t reported_segments; /* topology must not select write masks */
    bool has_secret;
    bool corrupt_mode_reply; /* flip the checksum on AA 05 replies */
    uint16_t white_kelvin; /* 0D white state, reported with RGB 00 00 00 */
    volatile bool abort;
    int writes;
    uint8_t written[32][GOVEE_FRAME_LEN];
    int logged;
    char log_notes[64][16];
    uint8_t log_frames[64][GOVEE_FRAME_LEN];
    bool log_redacted[64];
} Fake;

static bool fake_write(void* ctx, const uint8_t f[GOVEE_FRAME_LEN]) {
    Fake* d = ctx;
    if(d->writes < 32) memcpy(d->written[d->writes], f, GOVEE_FRAME_LEN);
    d->writes++;
    if(d->speaks_none) return true; /* transport accepts; application ignores */
    if(f[1] == GOVEE_CMD_POWER) d->power_on = f[2] != 0;
    if(f[1] == GOVEE_CMD_BRIGHTNESS) d->brightness = f[2];
    if(f[1] == GOVEE_CMD_MODE && f[2] == govee_dialect_submode(d->speaks)) {
        d->submode = f[2];
        d->white_kelvin = (f[2] == GOVEE_SUBMODE_COLOR_RGBWW) ? (uint16_t)((f[6] << 8) | f[7]) : 0;
        if(d->speaks == GoveeDialectRgbic) {
            memcpy(d->rgb, &f[4], 3); /* 15 01 R G B */
        } else {
            memcpy(d->rgb, &f[3], 3); /* 02/0B/0D R G B */
        }
    }
    return true;
}

static bool fake_read(void* ctx, const uint8_t q[GOVEE_FRAME_LEN], uint8_t a[GOVEE_FRAME_LEN], uint32_t timeout) {
    (void)timeout;
    Fake* d = ctx;
    if(d->speaks_none) return false;
    switch(q[1]) {
    case GOVEE_CMD_POWER: {
        uint8_t b[] = {d->power_on};
        reply_frame(a, q[1], b, 1);
        return true;
    }
    case GOVEE_CMD_BRIGHTNESS: {
        uint8_t b[] = {d->brightness};
        reply_frame(a, q[1], b, 1);
        return true;
    }
    case GOVEE_CMD_MODE: {
        uint8_t b[9] = {d->submode};
        if(d->white_kelvin && d->submode == GOVEE_SUBMODE_COLOR_RGBWW) {
            /* H6006 white: 0D 000000 kHi kLo 000000 */
            b[4] = (uint8_t)(d->white_kelvin >> 8);
            b[5] = (uint8_t)d->white_kelvin;
        } else if(d->submode == GOVEE_SUBMODE_COLOR_LEGACY || d->submode == GOVEE_SUBMODE_COLOR_RGBWW) {
            memcpy(&b[1], d->rgb, 3);
        } else if(d->submode == GOVEE_SUBMODE_COLOR_RGBIC) {
            b[1] = 0x01; /* kelvin 0: colour, no RGB in the reply */
        }
        reply_frame(a, q[1], b, sizeof(b));
        if(d->corrupt_mode_reply) a[GOVEE_FRAME_LEN - 1] ^= 0x55;
        return true;
    }
    case GOVEE_CMD_VERSION:
        reply_frame(a, q[1], (const uint8_t*)"3.02.24", 7);
        return true;
    case GOVEE_CMD_DEVICE_INFO: {
        uint8_t b[] = {0x03, '1', '.', '0', '0', '.', '0', '4'};
        reply_frame(a, q[1], b, sizeof(b));
        return true;
    }
    case GOVEE_CMD_IC_NUM: {
        uint8_t b[] = {0x00, 0x0F, d->reported_segments};
        reply_frame(a, q[1], b, 3);
        return true;
    }
    case GOVEE_CMD_SECRET_READ: {
        if(!d->has_secret) return false;
        uint8_t b[9] = {0x01, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
        reply_frame(a, q[1], b, 9);
        return true;
    }
    case GOVEE_CMD_SEGMENT_COLOR: {
        if(!d->has_page) return false;
        uint8_t b[13] = {q[2]};
        for(int i = 0; i < 3; i++) {
            b[1 + i * 4] = 0x64;
            memcpy(&b[2 + i * 4], d->rgb, 3);
        }
        reply_frame(a, q[1], b, sizeof(b));
        return true;
    }
    default:
        return false; /* AA 0F unanswered, like the H617A */
    }
}

static void fake_delay(void* ctx, uint32_t ms) {
    (void)ctx;
    (void)ms;
}

static void fake_log(void* ctx, const char* dir, const uint8_t f[GOVEE_FRAME_LEN], const char* note, bool redacted) {
    (void)dir;
    Fake* d = ctx;
    if(d->logged < 64) {
        snprintf(d->log_notes[d->logged], 16, "%s", note);
        memcpy(d->log_frames[d->logged], f, GOVEE_FRAME_LEN);
        d->log_redacted[d->logged] = redacted;
    }
    d->logged++;
}

static GoveeCensusIo io_for(Fake* d) {
    GoveeCensusIo io = {
        .ctx = d, .write = fake_write, .read = fake_read, .delay_ms = fake_delay, .log = fake_log, .abort = &d->abort};
    return io;
}

static Fake fake_speaking(GoveeDialect d) {
    Fake f;
    memset(&f, 0, sizeof(f));
    f.speaks = d;
    f.power_on = true;
    f.brightness = 50;
    f.reported_segments = 15;
    f.submode = govee_dialect_submode(d);
    f.rgb[0] = 0xAA;
    f.rgb[1] = 0xBB;
    f.rgb[2] = 0xCC;
    f.has_page = (d == GoveeDialectRgbic || d == GoveeDialectDreamcolor);
    return f;
}

static int count_notes(const Fake* d, const char* note) {
    int n = 0;
    for(int i = 0; i < d->logged && i < 64; i++) {
        if(strcmp(d->log_notes[i], note) == 0) n++;
    }
    return n;
}

/* ----------------------------------------------------------------- tests */

static void test_name_parse(void) {
    char m[6];
    CHECK(govee_name_parse("ihoment_H6159_1A2B", 18, m, 6) && strcmp(m, "H6159") == 0);
    CHECK(govee_name_parse("GBK_H6181_FB1E", 14, m, 6) && strcmp(m, "H6181") == 0);
    CHECK(govee_name_parse("Govee_H617A_ABCD", 16, m, 6) && strcmp(m, "H617A") == 0);
    /* GVH names carry the SKU letter as their third character (app rule). */
    CHECK(govee_name_parse("GVH6159_1A2B", 12, m, 6) && strcmp(m, "H6159") == 0);
    CHECK(govee_name_parse("GVH600B4F5A", 11, m, 6) && strcmp(m, "H600B") == 0);
    CHECK(govee_name_parse("ihoment_h619a_x", 15, m, 6) && strcmp(m, "H619A") == 0);
    CHECK(!govee_name_parse("GVR6184_1A2B", 12, m, 6)); /* not an H SKU */
    CHECK(!govee_name_parse("GV5122ABCD", 10, m, 6)); /* sensor fallback form */
    CHECK(!govee_name_parse("ihoment_H61", 11, m, 6));
    CHECK(!govee_name_parse("Hue_H6159", 9, m, 6));
    CHECK(!govee_name_parse("ihoment_H6159X1A2B", 18, m, 6));
}

static void test_frame_vectors(void) {
    /* Cross-checked against custom_components/govee_bluetooth/protocol.py. */
    uint8_t f[GOVEE_FRAME_LEN];
    char h[41];
    govee_frame_power(f, true);
    hex(f, h);
    CHECK(strcmp(h, "3301010000000000000000000000000000000033") == 0);
    govee_frame_rgb_seg(f, 0x12, 0x34, 0x56, GoveeDialectRgbic, 15);
    hex(f, h);
    CHECK(strcmp(h, "330515011234560000000000ff7f0000000000d2") == 0);
    govee_frame_rgb_seg(f, 0x12, 0x34, 0x56, GoveeDialectRgbic, 13);
    hex(f, h);
    CHECK(strcmp(h, "330515011234560000000000ff1f0000000000b2") == 0);
    govee_frame_rgb_seg(f, 0x12, 0x34, 0x56, GoveeDialectDreamcolor, 0);
    hex(f, h);
    CHECK(strcmp(h, "33050b123456ff7f0000000000000000000000cd") == 0);
    govee_frame_rgb_seg(f, 0x12, 0x34, 0x56, GoveeDialectRgbww, 0);
    hex(f, h);
    CHECK(strcmp(h, "33050d123456000000000000000000000000004b") == 0);
    govee_frame_rgb_seg(f, 0x12, 0x34, 0x56, GoveeDialectLegacy, 0);
    hex(f, h);
    CHECK(strcmp(h, "3305021234560000000000000000000000000044") == 0);
    govee_frame_read_mode(f);
    hex(f, h);
    CHECK(strcmp(h, "aa050100000000000000000000000000000000ae") == 0);
    govee_frame_music_modern(f, 0x06, 0x63);
    hex(f, h);
    CHECK(strcmp(h, "3305130663000000000000000000000000000040") == 0);
    /* Scene codes in the app are little-endian 16-bit, not just one byte. */
    govee_frame_scene(f, 0x1234);
    hex(f, h);
    CHECK(strcmp(h, "3305043412000000000000000000000000000014") == 0);
    uint8_t m[GOVEE_MASK_MAX_BYTES];
    CHECK(govee_segment_mask(23, m) == 3 && m[0] == 0xFF && m[1] == 0xFF && m[2] == 0x7F);
}

/* Every frame protocol.py also builds, byte for byte (generated header). */
static void test_generated_frame_vectors(void) {
    uint8_t f[GOVEE_FRAME_LEN];
    char h[41];
    size_t n = sizeof(GOVEE_FRAME_VECTORS) / sizeof(GOVEE_FRAME_VECTORS[0]);
    CHECK(n >= 20);
    for(size_t i = 0; i < n; i++) {
        const char* name = GOVEE_FRAME_VECTORS[i].name;
        bool built = true;
        if(!strcmp(name, "power_on")) govee_frame_power(f, true);
        else if(!strcmp(name, "power_off")) govee_frame_power(f, false);
        else if(!strcmp(name, "brightness_legacy_255")) govee_frame_brightness(f, 100, GoveeDialectLegacy);
        else if(!strcmp(name, "brightness_percent_100")) govee_frame_brightness(f, 100, GoveeDialectRgbic);
        else if(!strcmp(name, "rgb_legacy_123456")) govee_frame_rgb_seg(f, 0x12, 0x34, 0x56, GoveeDialectLegacy, 0);
        else if(!strcmp(name, "rgb_rgbww_123456")) govee_frame_rgb_seg(f, 0x12, 0x34, 0x56, GoveeDialectRgbww, 0);
        else if(!strcmp(name, "rgb_rgbic_123456_15seg")) govee_frame_rgb_seg(f, 0x12, 0x34, 0x56, GoveeDialectRgbic, 15);
        else if(!strcmp(name, "rgb_dreamcolor_123456_15seg")) govee_frame_rgb_seg(f, 0x12, 0x34, 0x56, GoveeDialectDreamcolor, 15);
        else if(!strcmp(name, "scene_blinking")) govee_frame_scene(f, GOVEE_SCENE_BLINKING);
        else if(!strcmp(name, "read_power")) govee_frame_read_power(f);
        else if(!strcmp(name, "read_brightness")) govee_frame_read_brightness(f);
        else if(!strcmp(name, "read_mode")) govee_frame_read_mode(f);
        else if(!strcmp(name, "read_firmware")) govee_frame_read_firmware(f);
        else if(!strcmp(name, "read_hardware")) govee_frame_read_hardware(f);
        else if(!strcmp(name, "read_ic_count")) govee_frame_read_ic_count(f);
        else if(!strcmp(name, "read_light_count")) govee_frame_read_light_count(f);
        else if(!strcmp(name, "read_secret")) govee_frame_read_secret(f);
        else if(!strcmp(name, "read_segment_colors_1")) govee_frame_read_segment_colors(f, 1);
        else if(!strncmp(name, "probe_", 6)) {
            /* The census probe colours must match census.PROBE_COLORS in Python. */
            GoveeDialect pd = !strcmp(name, "probe_02") ? GoveeDialectLegacy :
                              !strcmp(name, "probe_0B") ? GoveeDialectDreamcolor :
                              !strcmp(name, "probe_0D") ? GoveeDialectRgbww :
                                                          GoveeDialectRgbic;
            uint8_t c[3];
            govee_probe_color(pd, c);
            govee_frame_rgb_seg(f, c[0], c[1], c[2], pd, 15);
        }
        else built = false; /* HA-only builders (colour temperature, HA brightness scaling) */
        if(!built) continue;
        hex(f, h);
        if(strcmp(h, GOVEE_FRAME_VECTORS[i].hex) != 0) printf("vector %s: C %s py %s\n", name, h, GOVEE_FRAME_VECTORS[i].hex);
        CHECK(strcmp(h, GOVEE_FRAME_VECTORS[i].hex) == 0);
    }
}

static void test_adv_parse(void) {
    GoveeAdvInfo a;
    const uint8_t nimble[] = {0x03, 0x88, 0xEC, 0x00, 0x0A, 0x01, 0x01};
    CHECK(govee_adv_parse_mfr(nimble, sizeof(nimble), &a));
    CHECK(a.valid && a.pact_type == 10 && a.pact_code == 1 && a.flags == 3 && !a.encrypted && a.proto_version == 3);
    const uint8_t enc[] = {0x43, 0x88, 0xEC, 0x00, 0x01, 0x01};
    CHECK(govee_adv_parse_mfr(enc, sizeof(enc), &a) && a.encrypted && a.pact_type == 1);
    const uint8_t split[] = {0xEC, 0x00, 0x02, 0x01};
    CHECK(govee_adv_parse_mfr(split, sizeof(split), &a) && a.pact_type == 2 && a.flags == 0);
    const uint8_t other[] = {0x4C, 0x00, 0x02, 0x15};
    CHECK(!govee_adv_parse_mfr(other, sizeof(other), &a) && !a.valid);
}

static void test_probe_colours_distinct(void) {
    uint8_t c[GoveeDialectCount][3];
    for(int d = 0; d < GoveeDialectCount; d++) govee_probe_color((GoveeDialect)d, c[d]);
    for(int i = 0; i < GoveeDialectCount; i++) {
        for(int j = i + 1; j < GoveeDialectCount; j++) CHECK(memcmp(c[i], c[j], 3) != 0);
        CHECK(c[i][0] && c[i][1] && c[i][2] && c[i][0] != 0xFF);
    }
}

static void test_redaction(void) {
    uint8_t f[GOVEE_FRAME_LEN];
    uint8_t b[9] = {0x01, 1, 2, 3, 4, 5, 6, 7, 8};
    reply_frame(f, GOVEE_CMD_SECRET_READ, b, 9);
    CHECK(govee_frame_has_secret(f));
    CHECK(govee_frame_redact(f));
    CHECK(f[0] == 0xAA && f[1] == 0xB1 && f[2] == 0x01);
    for(int i = 3; i < 19; i++) CHECK(f[i] == 0);
    CHECK(govee_frame_verify(f));
    /* An "absent" reply has nothing to hide. */
    uint8_t none[1] = {0x00};
    reply_frame(f, GOVEE_CMD_SECRET_READ, none, 1);
    CHECK(!govee_frame_has_secret(f) && !govee_frame_redact(f));
    uint8_t secret[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    govee_frame_secret_write(f, secret);
    CHECK(govee_frame_redact(f) && f[2] == 0 && f[9] == 0 && govee_frame_verify(f));
    govee_frame_power(f, true);
    CHECK(!govee_frame_redact(f));
}

static void test_census_reads_only(void) {
    Fake d = fake_speaking(GoveeDialectLegacy);
    d.has_secret = true;
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r;
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H6159", false, &r);
    CHECK(d.writes == 0);
    CHECK(r.power_known && r.power_on && r.brightness_known && r.brightness == 50);
    CHECK(r.submode_known && r.submode == 0x02 && r.mode_body[0] == 0xAA);
    CHECK(strcmp(r.firmware, "3.02.24") == 0 && strcmp(r.hardware, "1.00.04") == 0);
    CHECK(r.ic_known && r.segments == 15 && !r.light_count_known);
    CHECK(r.secret_present && r.secret[0] == 0xDE);
    CHECK(!r.silent && !r.probed && r.reads == 9 && r.replies == 7);
    /* The secret reply was logged redacted. */
    bool saw_secret_log = false;
    for(int i = 0; i < d.logged; i++) {
        if(d.log_frames[i][0] == 0xAA && d.log_frames[i][1] == 0xB1 && strcmp(d.log_notes[i], "reply") == 0) {
            saw_secret_log = true;
            CHECK(d.log_redacted[i]);
            CHECK(d.log_frames[i][3] == 0 && d.log_frames[i][4] == 0);
        }
    }
    CHECK(saw_secret_log);
    CHECK(count_notes(&d, "timeout") == 2);
}

static void test_census_silent_is_bounded(void) {
    Fake d = fake_speaking(GoveeDialectRgbic);
    d.speaks_none = true;
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r;
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H617A", true, &r);
    CHECK(r.silent);
    CHECK(r.reads == GOVEE_CENSUS_SILENT_AFTER);
    CHECK(!r.probed && d.writes == 0);
}

static void test_census_rejects_bad_checksum(void) {
    Fake d = fake_speaking(GoveeDialectRgbww);
    d.corrupt_mode_reply = true;
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r;
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H6181", false, &r);
    CHECK(!r.submode_known);
    CHECK(r.bad_frames == 1);
    CHECK(count_notes(&d, "bad_checksum") == 1);
    CHECK(r.power_known); /* the other reads still count */
}

static void test_probe_echo_is_per_dialect(void) {
    /* A light that only speaks 0x02 must echo 02 and nothing else, even
     * though the 0x02 probe colour stays on the strip for the later reads. */
    Fake d = fake_speaking(GoveeDialectLegacy);
    d.has_page = true; /* an AA A5 page that mirrors the current colour */
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r;
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H6159", true, &r);
    CHECK(r.probed);
    CHECK(r.echoed == (1u << GoveeDialectLegacy));

    /* An unqualified A5 page stays raw, even when it contains the colour. */
    Fake e = fake_speaking(GoveeDialectRgbic);
    io = io_for(&e);
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H617A", true, &r);
    CHECK(r.echoed == 0);
    CHECK(r.segment_page && r.page_before[0] == GOVEE_TYPE_READ);

    /* A 0x0D light whose page is answered too. */
    Fake g = fake_speaking(GoveeDialectRgbww);
    g.has_page = true;
    io = io_for(&g);
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H6181", true, &r);
    CHECK(r.echoed == (1u << GoveeDialectRgbww));
}

static void test_probe_ignores_stale_colour(void) {
    /* The light already shows the legacy probe colour: that is not an echo. */
    Fake d = fake_speaking(GoveeDialectLegacy);
    d.has_page = true;
    uint8_t c[3];
    govee_probe_color(GoveeDialectLegacy, c);
    memcpy(d.rgb, c, 3);
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r;
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H6159", true, &r);
    CHECK(r.echoed == 0);
    CHECK(r.segment_page && r.page_before[0] == GOVEE_TYPE_READ);
}

static void test_topology_does_not_select_probe_mask(void) {
    Fake d = fake_speaking(GoveeDialectRgbic);
    d.reported_segments = 32;
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r = {0};
    govee_census_run(&io, "H617A", true, &r);
    CHECK(r.ic_known && r.segments == 32); /* still preserved as evidence */
    bool saw_rgbic = false;
    for(int i = 0; i < d.writes && i < 32; i++) {
        const uint8_t* f = d.written[i];
        if(f[1] == GOVEE_CMD_MODE && f[2] == GOVEE_SUBMODE_COLOR_RGBIC) {
            saw_rgbic = true;
            CHECK(f[12] == 0xFF && f[13] == 0x7F);
            CHECK(f[14] == 0 && f[15] == 0);
        }
    }
    CHECK(saw_rgbic);
}

static void test_probe_restores_colour_and_power(void) {
    Fake d = fake_speaking(GoveeDialectRgbww);
    d.power_on = false;
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r;
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H6181", true, &r);
    CHECK(r.echoed == (1u << GoveeDialectRgbww));
    /* Powered on for the probe, colour put back, powered off again. */
    CHECK(d.written[0][1] == GOVEE_CMD_POWER && d.written[0][2] == 1);
    CHECK(!d.power_on);
    CHECK(d.rgb[0] == 0xAA && d.rgb[1] == 0xBB && d.rgb[2] == 0xCC);
    CHECK(r.restored == GoveeCensusRestoredFull && r.restore_note == NULL);
    /* Three resets to the baseline between dialects, the final colour, power. */
    CHECK(count_notes(&d, "restore") == 5);

    /* An RGBIC light: the pre-probe colour is not readable, say so. */
    Fake e = fake_speaking(GoveeDialectRgbic);
    io = io_for(&e);
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H617A", true, &r);
    CHECK(r.restored == GoveeCensusRestoredPower && r.restore_note != NULL);
    CHECK(e.power_on);
}

static void test_white_0d_restored_verbatim(void) {
    /* The H6006 on the bench: 0D white at 2700 K. An RGB restore would be black. */
    Fake d = fake_speaking(GoveeDialectRgbww);
    d.white_kelvin = 2700;
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r;
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H6006", true, &r);
    CHECK(r.restored == GoveeCensusRestoredFull);
    CHECK(d.white_kelvin == 2700);
    CHECK(d.submode == GOVEE_SUBMODE_COLOR_RGBWW);
    uint8_t want[GOVEE_FRAME_LEN];
    uint8_t body[] = {0x0D, 0, 0, 0, 0x0A, 0x8C, 0, 0, 0};
    govee_frame_build(want, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, body, sizeof(body));
    bool found = false;
    for(int i = 0; i < d.writes && i < 32; i++) {
        if(memcmp(d.written[i], want, GOVEE_FRAME_LEN) == 0) found = true;
    }
    CHECK(found);
}

static void test_abort_stops_between_reads(void) {
    Fake d = fake_speaking(GoveeDialectLegacy);
    d.abort = true;
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r;
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H6159", true, &r);
    CHECK(r.aborted && !r.silent && r.reads == 0 && d.writes == 0 && !r.probed);
}

static void test_notify_acks(void) {
    Fake d = fake_speaking(GoveeDialectLegacy);
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r;
    memset(&r, 0, sizeof(r));
    uint8_t ack[GOVEE_FRAME_LEN];
    uint8_t body[] = {0x00};
    govee_frame_build(ack, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, body, 1);
    govee_census_note_notify(&io, ack, &r);
    CHECK(r.acks == 1 && count_notes(&d, "ack") == 1);
    uint8_t push[GOVEE_FRAME_LEN];
    govee_frame_build(push, GOVEE_TYPE_NOTIFY, 0x20, body, 1);
    govee_census_note_notify(&io, push, &r);
    CHECK(r.acks == 1 && count_notes(&d, "unsolicited") == 1);
    push[5] ^= 1;
    govee_census_note_notify(&io, push, &r);
    CHECK(r.bad_frames == 1);
}

static bool json_balanced(const char* s) {
    size_t n = strlen(s);
    if(n < 2 || s[0] != '{' || s[n - 1] != '}') return false;
    bool in_str = false;
    int quotes = 0;
    for(size_t i = 0; i < n; i++) {
        if(s[i] == '\\') {
            i++;
            continue;
        }
        if(s[i] == '"') {
            in_str = !in_str;
            quotes++;
        }
    }
    return !in_str && quotes % 2 == 0;
}

static void test_summary_json(void) {
    Fake d = fake_speaking(GoveeDialectRgbic);
    d.has_secret = true;
    GoveeCensusIo io = io_for(&d);
    GoveeCensusResult r;
    memset(&r, 0, sizeof(r));
    r.connected = r.subscribed = true;
    govee_census_run(&io, "H617A", true, &r);
    GoveeAdvInfo adv = {.valid = true, .flags = 3, .proto_version = 3, .pact_type = 10, .pact_code = 1};
    GoveeCensusMeta meta = {
        .source = "dance_party",
        .at = "2026-10-19T12:00:00",
        .software = "0.2",
        .dialect = GoveeDialectRgbic,
        .dialect_source = "pact"};
    char out[GOVEE_CENSUS_JSON_MAX];
    govee_census_summary_json("aa:bb:cc:dd:ee:ff", "ihoment_H617A_1234", "H617A", &adv, -61, &r, &meta, false, out, sizeof(out));
    CHECK(json_balanced(out));
    printf("summary (%zu bytes): %s\n", strlen(out), out);
    CHECK(strstr(out, "\"schema\":\"govee-evidence/1\"") != NULL);
    CHECK(strstr(out, "\"source\":\"dance_party\"") != NULL);
    CHECK(strstr(out, "\"known_model\":true") != NULL);
    CHECK(strstr(out, "\"pact\":\"10/1\"") != NULL);
    CHECK(strstr(out, "\"dialect\":\"RGBIC\",\"dialect_source\":\"pact\"") != NULL);
    CHECK(strstr(out, "\"secret_present\":true") != NULL);
    CHECK(strstr(out, "\"secret\":") == NULL); /* never without print_secret */
    CHECK(strstr(out, "deadbeef") == NULL);
    CHECK(strstr(out, "\"echoed\":\"\"") != NULL);
    CHECK(strstr(out, "\"restored\":\"power\"") != NULL);
    CHECK(strstr(out, "\"truncated\"") == NULL);

    /* With print_secret the bytes appear, and only then. */
    govee_census_summary_json("aa:bb:cc:dd:ee:ff", "x", "H617A", &adv, -61, &r, &meta, true, out, sizeof(out));
    CHECK(strstr(out, "\"secret\":\"deadbeef01020304\"") != NULL);

    /* Escaping. */
    govee_census_summary_json("aa", "na\"me\\x\x01y", "H6159", NULL, -1, &r, NULL, false, out, sizeof(out));
    CHECK(json_balanced(out));
    CHECK(strstr(out, "\"name\":\"na\\\"me\\\\xy\"") != NULL);
    CHECK(strstr(out, "\"pact\":null") != NULL);
    CHECK(strstr(out, "\"known_model\":true") != NULL); /* H6159 is a tabled legacy strip */

    /* Truncation stays well formed and says so. */
    for(size_t size = 3; size < 400; size += 7) {
        char small[400];
        memset(small, 'X', sizeof(small));
        govee_census_summary_json("aa:bb:cc:dd:ee:ff", "ihoment_H617A_1234", "H617A", &adv, -61, &r, &meta, false, small, size);
        CHECK(strlen(small) < size);
        CHECK(json_balanced(small));
        if(size >= 32) CHECK(strstr(small, "\"truncated\":true") != NULL);
    }
}

static void test_dialect_source(void) {
    GoveeAdvInfo adv = {.valid = true, .pact_type = 1, .pact_code = 1};
    CHECK(strcmp(govee_census_dialect_source("H6199", &adv), "pact") == 0);
    CHECK(govee_dialect_for_light("H6199", &adv) == GoveeDialectDreamcolor);
    adv.pact_type = 2;
    CHECK(govee_dialect_for_light("H6199", &adv) == GoveeDialectRgbic);
    CHECK(strcmp(govee_census_dialect_source("H6199", NULL), "table") == 0);
    CHECK(strcmp(govee_census_dialect_source("H6181", &adv), "table") == 0);
    CHECK(strcmp(govee_census_dialect_source("H9999", &adv), "default") == 0);
    CHECK(!govee_model_known("H9999") && govee_dialect_for_model("H9999") == GoveeDialectLegacy);
}

int main(void) {
    test_name_parse();
    test_frame_vectors();
    test_generated_frame_vectors();
    test_adv_parse();
    test_probe_colours_distinct();
    test_redaction();
    test_census_reads_only();
    test_census_silent_is_bounded();
    test_census_rejects_bad_checksum();
    test_probe_echo_is_per_dialect();
    test_probe_ignores_stale_colour();
    test_topology_does_not_select_probe_mask();
    test_probe_restores_colour_and_power();
    test_white_0d_restored_verbatim();
    test_abort_stops_between_reads();
    test_notify_acks();
    test_summary_json();
    test_dialect_source();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
