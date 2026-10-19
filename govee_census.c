#include "govee_census.h"

#include <stdio.h>
#include <string.h>

static void note_log(const GoveeCensusIo* io, const char* dir, const uint8_t* f, const char* note) {
    if(!io->log) return;
    uint8_t copy[GOVEE_FRAME_LEN];
    memcpy(copy, f, GOVEE_FRAME_LEN);
    bool redacted = govee_frame_redact(copy);
    io->log(io->ctx, dir, copy, note, redacted);
}

static bool aborted(const GoveeCensusIo* io) {
    return io->abort && *io->abort;
}

/* One read with logging. Returns true when a verified matching reply arrived. */
static bool census_read(
    const GoveeCensusIo* io,
    const uint8_t request[GOVEE_FRAME_LEN],
    uint8_t reply[GOVEE_FRAME_LEN],
    GoveeCensusResult* r) {
    if(aborted(io)) {
        r->aborted = true;
        return false;
    }
    r->reads++;
    note_log(io, "tx", request, "query");
    if(!io->read(io->ctx, request, reply, GOVEE_CENSUS_READ_TIMEOUT_MS)) {
        note_log(io, "rx", request, "timeout");
        return false;
    }
    if(!govee_frame_verify(reply)) {
        /* A corrupt or foreign frame is evidence of a reply, not of its
         * contents: log it, count it, use nothing from it. */
        note_log(io, "rx", reply, "bad_checksum");
        r->bad_frames++;
        return false;
    }
    note_log(io, "rx", reply, "reply");
    r->replies++;
    return true;
}

static void census_write(const GoveeCensusIo* io, const uint8_t frame[GOVEE_FRAME_LEN], const char* note) {
    note_log(io, "tx", frame, note);
    io->write(io->ctx, frame);
    io->delay_ms(io->ctx, GOVEE_CENSUS_SETTLE_MS);
}

void govee_census_note_notify(const GoveeCensusIo* io, const uint8_t frame[GOVEE_FRAME_LEN], GoveeCensusResult* r) {
    if(!govee_frame_verify(frame)) {
        note_log(io, "rx", frame, "bad_checksum");
        r->bad_frames++;
        return;
    }
    if(frame[0] == GOVEE_TYPE_WRITE) {
        /* The device echoes 33 <cmd> with byte 2 == 0 to ack a write (SPEC 3.3). */
        note_log(io, "rx", frame, frame[2] == 0 ? "ack" : "ack_error");
        if(frame[2] == 0 && r->acks < 255) r->acks++;
        return;
    }
    note_log(io, "rx", frame, "unsolicited");
}

static const GoveeDialect PROBE_ORDER[] = {
    GoveeDialectLegacy, GoveeDialectDreamcolor, GoveeDialectRgbww, GoveeDialectRgbic};

/* Whether the pre-probe state can be written back: 02 and 0D replies mirror
 * the write layout on every kind seen on hardware (H6159, H6181, H6006), so
 * the reported body is rewritten verbatim. An RGB write would not do: a 0D
 * light in white mode reports RGB 00 00 00 beside its kelvin (H6006:
 * 0D 000000 0A8C 000000), and writing that RGB leaves it dark. 0B and 15
 * replies carry no colour at all. */
static bool original_restorable(const GoveeCensusResult* r, GoveeDialect* dialect) {
    if(!r->submode_known) return false;
    if(r->submode == GOVEE_SUBMODE_COLOR_LEGACY) {
        *dialect = GoveeDialectLegacy;
    } else if(r->submode == GOVEE_SUBMODE_COLOR_RGBWW) {
        *dialect = GoveeDialectRgbww;
    } else {
        return false;
    }
    return true;
}

static void census_probe(const GoveeCensusIo* io, GoveeCensusResult* r) {
    uint8_t q[GOVEE_FRAME_LEN], a[GOVEE_FRAME_LEN], f[GOVEE_FRAME_LEN];
    r->probed = true;
    uint8_t segments = (r->ic_known && r->segments) ? r->segments : 0;

    /* Mode writes on a powered-off light may not be reflected by the mode
     * read; switch on for the duration and switch off again after. */
    bool powered_on_for_probe = r->power_known && !r->power_on;
    if(powered_on_for_probe) {
        govee_frame_power(f, true);
        census_write(io, f, "probe");
    }

    uint8_t page[GOVEE_FRAME_LEN];
    bool have_page = r->segment_page;
    if(have_page) memcpy(page, r->page_before, GOVEE_FRAME_LEN);
    uint8_t last_mode_rgb[3] = {0, 0, 0};
    bool have_mode_rgb = r->submode_known && govee_submode_is_color(r->submode);
    if(have_mode_rgb) memcpy(last_mode_rgb, r->mode_body, 3);

    for(size_t i = 0; i < sizeof(PROBE_ORDER) / sizeof(PROBE_ORDER[0]) && !aborted(io); i++) {
        GoveeDialect d = PROBE_ORDER[i];
        uint8_t want = govee_dialect_submode(d);
        uint8_t rgb[3];
        govee_probe_color(d, rgb);
        /* Stale-state guard: the colour must not already be showing. */
        bool stale_page = have_page && govee_reply_segment_page_has(page, rgb[0], rgb[1], rgb[2]);
        bool stale_mode = have_mode_rgb && memcmp(last_mode_rgb, rgb, 3) == 0;

        govee_frame_rgb_seg(f, rgb[0], rgb[1], rgb[2], d, segments);
        census_write(io, f, "probe");
        bool echoed = false;
        govee_frame_read_mode(q);
        if(census_read(io, q, a, r) && a[2] == want) {
            /* 02 and 0D replies carry RGB at bytes 3-5 on the families
             * verified so far; 0B and 15 never do (SPEC 6.2, 6.4). */
            if(d == GoveeDialectLegacy || d == GoveeDialectRgbww) {
                bool match = a[3] == rgb[0] && a[4] == rgb[1] && a[5] == rgb[2];
                if(match && !stale_mode) echoed = true;
            }
            have_mode_rgb = govee_submode_is_color(a[2]);
            memcpy(last_mode_rgb, &a[3], 3);
        }
        govee_frame_read_segment_colors(q, 1);
        if(census_read(io, q, a, r)) {
            if(govee_reply_segment_page_has(a, rgb[0], rgb[1], rgb[2]) && !stale_page) echoed = true;
            memcpy(page, a, GOVEE_FRAME_LEN);
            have_page = true;
        }
        if(echoed) r->echoed |= (uint8_t)(1u << d);
    }

    /* Restore. Colour comes back only when the pre-probe mode reply named
     * it (02 / 0D) and that dialect is known to work (it echoed, or it was
     * the reported mode). Scenes, music and 0B/15 colours are not rewritten:
     * the summary says so. */
    GoveeDialect od;
    if(original_restorable(r, &od)) {
        uint8_t body[1 + sizeof(r->mode_body)];
        body[0] = r->submode;
        memcpy(&body[1], r->mode_body, sizeof(r->mode_body));
        govee_frame_build(f, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, body, sizeof(body));
        census_write(io, f, "restore");
        r->restored = GoveeCensusRestoredFull;
        r->restore_note = (r->echoed & (1u << od)) ? NULL : "colour rewritten in the reported dialect, which did not echo";
    } else if(r->submode_known && govee_submode_is_color(r->submode)) {
        r->restored = GoveeCensusRestoredPower;
        r->restore_note = "pre-probe colour not readable from the mode reply (0B/15); left on the last probe colour";
    } else if(r->submode_known) {
        r->restored = GoveeCensusRestoredPower;
        r->restore_note = "pre-probe mode was not a static colour; not rewritten";
    } else {
        r->restored = GoveeCensusRestoredPower;
        r->restore_note = "pre-probe mode unknown; left on the last probe colour";
    }
    if(r->power_known) {
        govee_frame_power(f, r->power_on);
        census_write(io, f, "restore");
    } else {
        r->restored = GoveeCensusRestoredNone;
        r->restore_note = "pre-probe power unknown; nothing restored";
    }
}

void govee_census_run(const GoveeCensusIo* io, const char* model, bool probe_writes, GoveeCensusResult* r) {
    uint8_t q[GOVEE_FRAME_LEN], a[GOVEE_FRAME_LEN];
    (void)model;
    uint8_t core_timeouts = 0;

    govee_frame_read_power(q);
    if(census_read(io, q, a, r)) {
        r->power_known = true;
        r->power_on = a[2] != 0;
    } else {
        core_timeouts++;
    }
    govee_frame_read_brightness(q);
    if(census_read(io, q, a, r)) {
        r->brightness_known = true;
        r->brightness = a[2];
    } else {
        core_timeouts++;
    }
    govee_frame_read_firmware(q);
    if(census_read(io, q, a, r)) {
        govee_reply_version_string(a, r->firmware, sizeof(r->firmware));
    } else {
        core_timeouts++;
    }
    if(core_timeouts >= GOVEE_CENSUS_SILENT_AFTER && r->replies == 0) {
        /* Locked, encrypted or foreign dialect: more reads buy nothing. */
        r->silent = !r->aborted;
        return;
    }
    govee_frame_read_mode(q);
    if(census_read(io, q, a, r)) {
        r->submode_known = true;
        r->submode = a[2];
        memcpy(r->mode_body, &a[3], sizeof(r->mode_body));
    }
    govee_frame_read_hardware(q);
    if(census_read(io, q, a, r)) govee_reply_version_string(a, r->hardware, sizeof(r->hardware));
    govee_frame_read_ic_count(q);
    if(census_read(io, q, a, r)) {
        r->ic_known = true;
        r->ic_count = (uint16_t)((a[2] << 8) | a[3]);
        r->segments = a[4];
    }
    govee_frame_read_light_count(q);
    if(census_read(io, q, a, r)) {
        r->light_count_known = true;
        r->light_count = a[2];
    }
    govee_frame_read_secret(q);
    if(census_read(io, q, a, r) && a[2] == 0x01) {
        r->secret_present = true;
        memcpy(r->secret, &a[3], 8);
    }
    govee_frame_read_segment_colors(q, 1);
    if(census_read(io, q, a, r)) {
        r->segment_page = true;
        memcpy(r->page_before, a, GOVEE_FRAME_LEN);
    }

    r->silent = r->replies == 0 && !r->aborted;

    if(probe_writes && !r->silent && !r->aborted) census_probe(io, r);
}

void govee_census_echoed_string(uint8_t echoed, char* out, size_t out_size) {
    out[0] = '\0';
    size_t n = 0;
    for(int d = 0; d < GoveeDialectCount; d++) {
        if(!(echoed & (1u << d))) continue;
        n += (size_t)snprintf(out + n, out_size > n ? out_size - n : 0, "%s%02X", n ? "," : "", govee_dialect_submode((GoveeDialect)d));
    }
}

const char* govee_census_dialect_source(const char* model, const GoveeAdvInfo* adv) {
    if(adv && adv->valid && govee_model_is_dreamcolor(model)) return "pact";
    if(govee_model_known(model)) return "table";
    return "default";
}

/* ------------------------------------------------------------ JSON writer */

/* A bounded JSON object writer. Every field is written whole or not at all;
 * on overflow the object is closed with "truncated":true so a consumer
 * never sees a cut string. */
typedef struct {
    char* out;
    size_t size;
    size_t n; /* bytes written (always < size) */
    size_t fields;
    bool truncated;
} JsonW;

#define JSON_TAIL_RESERVE 20 /* ,"truncated":true} and the NUL */

static void jw_init(JsonW* w, char* out, size_t size) {
    w->out = out;
    w->size = size;
    w->n = 0;
    w->fields = 0;
    w->truncated = false;
    if(size) {
        out[0] = '{';
        w->n = size > 1 ? 1 : 0;
        out[w->n] = '\0';
    }
}

/* Appends `s` if it fits in front of the reserved tail; otherwise marks truncation. */
static bool jw_raw(JsonW* w, const char* s) {
    if(w->truncated) return false;
    size_t len = strlen(s);
    if(w->n + len + JSON_TAIL_RESERVE > w->size) {
        w->truncated = true;
        return false;
    }
    memcpy(w->out + w->n, s, len);
    w->n += len;
    w->out[w->n] = '\0';
    return true;
}

static void jw_key(JsonW* w, char* buf, size_t buf_size, const char* key) {
    snprintf(buf, buf_size, "%s\"%s\":", w->fields ? "," : "", key);
}

static void jw_str(JsonW* w, const char* key, const char* value) {
    if(w->truncated) return;
    char esc[96]; /* names are <= 31 chars, versions <= 11; escaping doubles at worst */
    size_t m = 0;
    for(const char* p = value; *p && m + 2 < sizeof(esc); p++) {
        unsigned char c = (unsigned char)*p;
        if(c < 0x20) continue;
        if(c == '"' || c == '\\') esc[m++] = '\\';
        esc[m++] = (char)c;
    }
    esc[m] = '\0';
    char head[40];
    jw_key(w, head, sizeof(head), key);
    size_t need = strlen(head) + m + 2;
    if(w->n + need + JSON_TAIL_RESERVE > w->size) {
        w->truncated = true;
        return;
    }
    jw_raw(w, head);
    jw_raw(w, "\"");
    jw_raw(w, esc);
    jw_raw(w, "\"");
    w->fields++;
}

static void jw_num(JsonW* w, const char* key, long value) {
    if(w->truncated) return;
    char buf[56];
    jw_key(w, buf, sizeof(buf), key);
    size_t h = strlen(buf);
    snprintf(buf + h, sizeof(buf) - h, "%ld", value);
    if(jw_raw(w, buf)) w->fields++;
}

static void jw_lit(JsonW* w, const char* key, const char* literal) {
    if(w->truncated) return;
    char buf[56];
    jw_key(w, buf, sizeof(buf), key);
    size_t h = strlen(buf);
    snprintf(buf + h, sizeof(buf) - h, "%s", literal);
    if(jw_raw(w, buf)) w->fields++;
}

static void jw_bool(JsonW* w, const char* key, bool v) {
    jw_lit(w, key, v ? "true" : "false");
}

static void jw_end(JsonW* w) {
    if(w->size == 0) return;
    if(!w->truncated) {
        w->out[w->n++] = '}';
        w->out[w->n] = '\0';
        return;
    }
    const char* tail = w->fields ? ",\"truncated\":true}" : "\"truncated\":true}";
    size_t len = strlen(tail);
    /* jw_raw keeps JSON_TAIL_RESERVE free after every accepted field, so the
     * tail fits whenever at least one field was written. With none, the
     * buffer may be smaller than the tail itself: fall back to "{}". */
    if(w->n + len + 1 <= w->size) {
        memcpy(w->out + w->n, tail, len);
        w->n += len;
    } else if(w->size >= 3) {
        w->out[0] = '{';
        w->out[1] = '}';
        w->n = 2;
    } else {
        w->n = 0;
    }
    w->out[w->n] = '\0';
}

void govee_census_summary_json(
    const char* addr,
    const char* name,
    const char* model,
    const GoveeAdvInfo* adv,
    int8_t rssi,
    const GoveeCensusResult* r,
    const GoveeCensusMeta* meta,
    bool print_secret,
    char* out,
    size_t out_size) {
    JsonW w;
    jw_init(&w, out, out_size);
    jw_str(&w, "summary", "light");
    jw_str(&w, "schema", GOVEE_CENSUS_SCHEMA);
    if(meta && meta->source) jw_str(&w, "source", meta->source);
    if(meta && meta->at) jw_str(&w, "at", meta->at);
    if(meta && meta->software) jw_str(&w, "software", meta->software);
    jw_str(&w, "address", addr);
    jw_str(&w, "name", name);
    jw_str(&w, "model", model);
    jw_bool(&w, "known_model", govee_model_known(model));
    jw_num(&w, "rssi", rssi);
    if(adv && adv->valid) {
        char pact[16];
        snprintf(pact, sizeof(pact), "%u/%u", adv->pact_type, adv->pact_code);
        jw_str(&w, "pact", pact);
        jw_num(&w, "flags", adv->flags);
        jw_bool(&w, "encrypted", adv->encrypted);
    } else {
        jw_lit(&w, "pact", "null");
        jw_lit(&w, "flags", "null");
        jw_lit(&w, "encrypted", "null");
    }
    if(meta) {
        jw_str(&w, "dialect", govee_dialect_name(meta->dialect));
        jw_str(&w, "dialect_source", meta->dialect_source ? meta->dialect_source : "default");
    }
    jw_bool(&w, "secret_family", govee_model_needs_secret(model));
    jw_bool(&w, "connected", r->connected);
    jw_bool(&w, "subscribed", r->subscribed);
    jw_bool(&w, "silent", r->connected && r->silent);
    if(r->aborted) jw_bool(&w, "aborted", true);
    jw_num(&w, "reads", r->reads);
    jw_num(&w, "replies", r->replies);
    if(r->bad_frames) jw_num(&w, "bad_frames", r->bad_frames);
    if(r->acks) jw_num(&w, "acks", r->acks);
    if(r->power_known) jw_bool(&w, "power", r->power_on);
    if(r->brightness_known) jw_num(&w, "brightness", r->brightness);
    if(r->submode_known) {
        char sm[24];
        snprintf(sm, sizeof(sm), "%02X %02X%02X%02X%02X", r->submode, r->mode_body[0], r->mode_body[1], r->mode_body[2], r->mode_body[3]);
        jw_str(&w, "submode", sm);
        jw_str(&w, "submode_name", govee_submode_name(r->submode));
    }
    if(r->firmware[0]) jw_str(&w, "firmware", r->firmware);
    if(r->hardware[0]) jw_str(&w, "hardware", r->hardware);
    if(r->ic_known) {
        jw_num(&w, "ic_count", r->ic_count);
        jw_num(&w, "segments", r->segments);
    }
    if(r->light_count_known) jw_num(&w, "light_count", r->light_count);
    jw_bool(&w, "secret_present", r->secret_present);
    if(r->secret_present && print_secret) {
        char hex[17];
        for(int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", r->secret[i]);
        jw_str(&w, "secret", hex);
    }
    jw_bool(&w, "segment_page", r->segment_page);
    jw_bool(&w, "probed", r->probed);
    if(r->probed) {
        char e[24];
        govee_census_echoed_string(r->echoed, e, sizeof(e));
        jw_str(&w, "echoed", e);
        jw_str(
            &w,
            "restored",
            r->restored == GoveeCensusRestoredFull ? "full" :
            r->restored == GoveeCensusRestoredPower ? "power" :
                                                      "none");
        if(r->restore_note) jw_str(&w, "restore_note", r->restore_note);
    }
    jw_end(&w);
}

void govee_census_summary_short(const GoveeCensusResult* r, char* out, size_t out_size) {
    if(!r->connected) {
        snprintf(out, out_size, "no connection");
        return;
    }
    if(!r->subscribed) {
        snprintf(out, out_size, "no notify");
        return;
    }
    if(r->silent) {
        snprintf(out, out_size, "silent");
        return;
    }
    char e[24] = "";
    if(r->probed) govee_census_echoed_string(r->echoed, e, sizeof(e));
    snprintf(
        out,
        out_size,
        "fw %s %s%s%s%s",
        r->firmware[0] ? r->firmware : "?",
        r->submode_known ? govee_submode_name(r->submode) : "",
        r->ic_known ? " seg" : "",
        r->probed ? " echo:" : "",
        r->probed ? (e[0] ? e : "none") : "");
}
