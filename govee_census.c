#include "govee_census.h"

#include <stdio.h>
#include <string.h>

static void note_log(const GoveeCensusIo* io, const char* dir, const uint8_t* f, const char* note) {
    if(io->log) io->log(io->ctx, dir, f, note);
}

/* One read with logging. Returns true when a matching reply arrived. */
static bool census_read(
    const GoveeCensusIo* io,
    const uint8_t request[GOVEE_FRAME_LEN],
    uint8_t reply[GOVEE_FRAME_LEN],
    GoveeCensusResult* r) {
    note_log(io, "tx", request, "query");
    if(!io->read(io->ctx, request, reply, GOVEE_CENSUS_READ_TIMEOUT_MS)) {
        note_log(io, "rx", request, "timeout");
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

static const GoveeDialect PROBE_ORDER[] = {
    GoveeDialectLegacy, GoveeDialectDreamcolor, GoveeDialectRgbww, GoveeDialectRgbic};

void govee_census_run(const GoveeCensusIo* io, const char* model, bool probe_writes, GoveeCensusResult* r) {
    uint8_t q[GOVEE_FRAME_LEN], a[GOVEE_FRAME_LEN];
    (void)model;

    govee_frame_read_power(q);
    if(census_read(io, q, a, r)) {
        r->power_known = true;
        r->power_on = a[2] != 0;
    }
    govee_frame_read_brightness(q);
    if(census_read(io, q, a, r)) {
        r->brightness_known = true;
        r->brightness = a[2];
    }
    govee_frame_read_mode(q);
    if(census_read(io, q, a, r)) {
        r->submode_known = true;
        r->submode = a[2];
        memcpy(r->mode_body, &a[3], sizeof(r->mode_body));
    }
    govee_frame_read_firmware(q);
    if(census_read(io, q, a, r)) govee_reply_version_string(a, r->firmware, sizeof(r->firmware));
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
    if(census_read(io, q, a, r)) r->segment_page = true;

    r->silent = r->replies == 0;

    if(probe_writes && !r->silent) {
        r->probed = true;
        uint8_t segments = (r->ic_known && r->segments) ? r->segments : 0;
        for(size_t i = 0; i < sizeof(PROBE_ORDER) / sizeof(PROBE_ORDER[0]); i++) {
            GoveeDialect d = PROBE_ORDER[i];
            uint8_t want = govee_dialect_submode(d);
            uint8_t f[GOVEE_FRAME_LEN];
            govee_frame_rgb_seg(f, GOVEE_PROBE_R, GOVEE_PROBE_G, GOVEE_PROBE_B, d, segments);
            census_write(io, f, "probe");
            bool echoed = false;
            govee_frame_read_mode(q);
            if(census_read(io, q, a, r) && a[2] == want) {
                /* 02 and 0D replies carry RGB at bytes 3-5 on the families
                 * verified so far; 0B and 15 never do (SPEC 6.2, 6.4). */
                if(d == GoveeDialectLegacy || d == GoveeDialectRgbww) {
                    echoed = a[3] == GOVEE_PROBE_R && a[4] == GOVEE_PROBE_G && a[5] == GOVEE_PROBE_B;
                }
            }
            govee_frame_read_segment_colors(q, 1);
            if(census_read(io, q, a, r) && govee_reply_segment_page_has(a, GOVEE_PROBE_R, GOVEE_PROBE_G, GOVEE_PROBE_B)) {
                echoed = true;
            }
            if(echoed) r->echoed |= (uint8_t)(1u << d);
        }
        /* Restore what we can: power. Colour and mode are gone; the summary
         * records what they were. */
        if(r->power_known) {
            govee_frame_power(q, r->power_on);
            census_write(io, q, "restore");
        }
    }
}

void govee_census_echoed_string(uint8_t echoed, char* out, size_t out_size) {
    out[0] = '\0';
    size_t n = 0;
    for(int d = 0; d < GoveeDialectCount; d++) {
        if(!(echoed & (1u << d))) continue;
        n += (size_t)snprintf(out + n, out_size > n ? out_size - n : 0, "%s%02X", n ? "," : "", govee_dialect_submode((GoveeDialect)d));
    }
}

static void json_str(char* out, size_t size, size_t* n, const char* key, const char* value) {
    /* values here are ASCII from our own tables or version strings; escape
     * quotes and backslashes only */
    *n += (size_t)snprintf(out + *n, size > *n ? size - *n : 0, "%s\"%s\":\"", *n > 1 ? "," : "", key);
    for(const char* p = value; *p && *n + 2 < size; p++) {
        if(*p == '"' || *p == '\\') out[(*n)++] = '\\';
        if((unsigned char)*p < 0x20) continue;
        out[(*n)++] = *p;
    }
    *n += (size_t)snprintf(out + *n, size > *n ? size - *n : 0, "\"");
}

static void json_raw(char* out, size_t size, size_t* n, const char* key, const char* fmt, long value) {
    *n += (size_t)snprintf(out + *n, size > *n ? size - *n : 0, "%s\"%s\":", *n > 1 ? "," : "", key);
    *n += (size_t)snprintf(out + *n, size > *n ? size - *n : 0, fmt, value);
}

static void json_bool(char* out, size_t size, size_t* n, const char* key, bool v) {
    *n += (size_t)snprintf(out + *n, size > *n ? size - *n : 0, "%s\"%s\":%s", *n > 1 ? "," : "", key, v ? "true" : "false");
}

void govee_census_summary_json(
    const char* addr,
    const char* name,
    const char* model,
    const GoveeAdvInfo* adv,
    int8_t rssi,
    const GoveeCensusResult* r,
    bool print_secret,
    char* out,
    size_t out_size) {
    size_t n = 0;
    n += (size_t)snprintf(out, out_size, "{");
    json_str(out, out_size, &n, "summary", "light");
    json_str(out, out_size, &n, "address", addr);
    json_str(out, out_size, &n, "name", name);
    json_str(out, out_size, &n, "model", model);
    json_raw(out, out_size, &n, "rssi", "%ld", rssi);
    if(adv && adv->valid) {
        char pact[16];
        snprintf(pact, sizeof(pact), "%u/%u", adv->pact_type, adv->pact_code);
        json_str(out, out_size, &n, "pact", pact);
        json_raw(out, out_size, &n, "flags", "%ld", adv->flags);
        json_bool(out, out_size, &n, "encrypted", adv->encrypted);
    } else {
        n += (size_t)snprintf(out + n, out_size > n ? out_size - n : 0, ",\"pact\":null,\"flags\":null,\"encrypted\":null");
    }
    json_bool(out, out_size, &n, "secret_family", govee_model_needs_secret(model));
    json_bool(out, out_size, &n, "connected", r->connected);
    json_bool(out, out_size, &n, "subscribed", r->subscribed);
    json_bool(out, out_size, &n, "silent", r->connected && r->silent);
    if(r->power_known) json_bool(out, out_size, &n, "power", r->power_on);
    if(r->brightness_known) json_raw(out, out_size, &n, "brightness", "%ld", r->brightness);
    if(r->submode_known) {
        char sm[24];
        snprintf(sm, sizeof(sm), "%02X %02X%02X%02X%02X", r->submode, r->mode_body[0], r->mode_body[1], r->mode_body[2], r->mode_body[3]);
        json_str(out, out_size, &n, "submode", sm);
        json_str(out, out_size, &n, "submode_name", govee_submode_name(r->submode));
    }
    if(r->firmware[0]) json_str(out, out_size, &n, "firmware", r->firmware);
    if(r->hardware[0]) json_str(out, out_size, &n, "hardware", r->hardware);
    if(r->ic_known) {
        json_raw(out, out_size, &n, "ic_count", "%ld", r->ic_count);
        json_raw(out, out_size, &n, "segments", "%ld", r->segments);
    }
    if(r->light_count_known) json_raw(out, out_size, &n, "light_count", "%ld", r->light_count);
    json_bool(out, out_size, &n, "secret_present", r->secret_present);
    if(r->secret_present && print_secret) {
        char hex[17];
        for(int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", r->secret[i]);
        json_str(out, out_size, &n, "secret", hex);
    }
    json_bool(out, out_size, &n, "segment_page", r->segment_page);
    json_bool(out, out_size, &n, "probed", r->probed);
    if(r->probed) {
        char e[24];
        govee_census_echoed_string(r->echoed, e, sizeof(e));
        json_str(out, out_size, &n, "echoed", e);
    }
    n += (size_t)snprintf(out + n, out_size > n ? out_size - n : 0, "}");
    if(n >= out_size) out[out_size - 1] = '\0';
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
