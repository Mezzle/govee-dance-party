#pragma once

/* Census: catalogue what a Govee light speaks without (by default) changing
 * it. Pure C over an abstract write/read pair so it can be unit tested on a
 * host (tests/host/); the Flipper app feeds it a GoveeTransport. The ESP32
 * image has its own copy of this flow in the sketch (same reads, same
 * summary keys, same probe rules). Change one, change both.
 *
 * Evidence rules the census follows (see docs/evidence.md):
 *  - a reply is only counted when its XOR checksum verifies;
 *  - a probe write only "echoes" when the dialect's own probe colour shows
 *    up after the write and was not already there before it;
 *  - a transport-level write success is never recorded as an accepted
 *    command: only an echo or an application ack (33 xx on the notify
 *    characteristic) is;
 *  - pairing secrets never reach the summary unless `print_secret`, and
 *    the frame log receives a redacted copy. */

#include "govee_protocol.h"

#define GOVEE_CENSUS_READ_TIMEOUT_MS 1500
#define GOVEE_CENSUS_SETTLE_MS       150
/* After this many consecutive unanswered core reads (power, brightness,
 * firmware) the light is declared silent and the remaining reads are
 * skipped: a locked or foreign-dialect light costs three timeouts, not nine. */
#define GOVEE_CENSUS_SILENT_AFTER 3

#define GOVEE_CENSUS_SCHEMA "govee-evidence/1"
/* Enough for every field of govee_census_summary_json() with a 31-char name
 * and the longest restore note; a smaller buffer gets a truncated object. */
#define GOVEE_CENSUS_JSON_MAX 896

typedef enum {
    GoveeCensusRestoredNone = 0, /* nothing was written, or nothing could be restored */
    GoveeCensusRestoredPower, /* power put back; colour / mode not */
    GoveeCensusRestoredFull, /* power and the pre-probe colour put back */
} GoveeCensusRestored;

typedef struct {
    bool connected;
    bool subscribed;
    bool aborted; /* the operator stopped the census mid-light */
    bool silent; /* connected and subscribed but not one verified AA reply */
    bool power_known, power_on;
    bool brightness_known;
    uint8_t brightness;
    bool submode_known;
    uint8_t submode;
    uint8_t mode_body[4]; /* bytes after the sub-mode in the AA 05 reply */
    char firmware[12];
    char hardware[12];
    bool ic_known;
    uint16_t ic_count;
    uint8_t segments;
    bool light_count_known;
    uint8_t light_count;
    bool secret_present; /* AA B1 answered with 01 + 8 bytes */
    uint8_t secret[8];
    bool segment_page; /* AA A5 01 answered */
    uint8_t page_before[GOVEE_FRAME_LEN]; /* the AA A5 01 reply before any probe */
    bool probed; /* probe writes were attempted */
    uint8_t echoed; /* bit (1 << GoveeDialect) set when that dialect's probe write echoed */
    uint8_t acks; /* 33 xx application acks seen on the notify characteristic */
    uint8_t reads; /* read requests sent */
    uint8_t replies; /* verified replies received */
    uint8_t bad_frames; /* replies that failed the checksum */
    GoveeCensusRestored restored;
    const char* restore_note; /* static string, or NULL */
} GoveeCensusResult;

typedef struct {
    void* ctx;
    bool (*write)(void* ctx, const uint8_t frame[GOVEE_FRAME_LEN]);
    /* Returns false on timeout. */
    bool (*read)(void* ctx, const uint8_t request[GOVEE_FRAME_LEN], uint8_t reply[GOVEE_FRAME_LEN], uint32_t timeout_ms);
    void (*delay_ms)(void* ctx, uint32_t ms);
    /* direction "tx" / "rx"; note as in the bench rig ("query", "reply",
     * "timeout", "bad_checksum", "probe", "restore"). Frames carrying a
     * secret arrive already redacted, with `redacted` set. May be NULL. */
    void (*log)(void* ctx, const char* direction, const uint8_t frame[GOVEE_FRAME_LEN], const char* note, bool redacted);
    /* Optional: set from another thread to stop between reads. */
    const volatile bool* abort;
} GoveeCensusIo;

/* Runs the read battery (and the probe writes if `probe_writes`) on an
 * already connected and subscribed light, then restores what it can.
 * `model` picks the segment count fallback. */
void govee_census_run(const GoveeCensusIo* io, const char* model, bool probe_writes, GoveeCensusResult* out);

/* Feed application acks / unsolicited frames the transport saw while the
 * census was running (33 xx echoes count as acks). Logs them too. */
void govee_census_note_notify(const GoveeCensusIo* io, const uint8_t frame[GOVEE_FRAME_LEN], GoveeCensusResult* r);

/* Where the evidence came from, for the summary line. */
typedef struct {
    const char* source; /* "dance_party" (Flipper session), "esp32", "pi" */
    const char* at; /* ISO timestamp or "T+12.345" uptime; may be NULL */
    const char* software; /* app / sketch version string; may be NULL */
    GoveeDialect dialect; /* the dialect the party would use for this light */
    const char* dialect_source; /* "pact", "table" or "default" */
} GoveeCensusMeta;

/* One JSON object, no newline, always well formed: if `out_size` is too
 * small the object is cut at a field boundary and ends with
 * "truncated":true. `name`/`model`/`addr` from the scan; `adv` may be
 * invalid. The secret bytes are only printed when `print_secret`. */
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
    size_t out_size);

/* Short one-line reading for a screen: "fw 1.04.08 colour02 15seg ok". */
void govee_census_summary_short(const GoveeCensusResult* r, char* out, size_t out_size);

/* Shared helper: "02,0B" style list of the dialects whose writes echoed. */
void govee_census_echoed_string(uint8_t echoed, char* out, size_t out_size);

/* The dialect name the model table / pact would pick, and where it came from. */
const char* govee_census_dialect_source(const char* model, const GoveeAdvInfo* adv);
