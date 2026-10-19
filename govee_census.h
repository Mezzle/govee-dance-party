#pragma once

/* Census: catalogue what a Govee light speaks without (by default) changing
 * it. Pure C over an abstract write/read pair so it can be unit tested on a
 * host; the Flipper app feeds it a GoveeTransport. The ESP32 image has its own
 * copy of this flow in the sketch (same reads, same summary keys). */

#include "govee_protocol.h"

#define GOVEE_CENSUS_READ_TIMEOUT_MS 1500
#define GOVEE_CENSUS_SETTLE_MS       150

typedef struct {
    bool connected;
    bool subscribed;
    bool silent; /* connected and subscribed but not one AA reply */
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
    bool probed; /* probe writes were attempted */
    uint8_t echoed; /* bit (1 << GoveeDialect) set when that dialect's probe write echoed */
    uint8_t replies; /* number of reads answered */
} GoveeCensusResult;

typedef struct {
    void* ctx;
    bool (*write)(void* ctx, const uint8_t frame[GOVEE_FRAME_LEN]);
    /* Returns false on timeout. */
    bool (*read)(void* ctx, const uint8_t request[GOVEE_FRAME_LEN], uint8_t reply[GOVEE_FRAME_LEN], uint32_t timeout_ms);
    void (*delay_ms)(void* ctx, uint32_t ms);
    /* direction "tx" / "rx"; note as in the bench rig ("query", "reply",
     * "timeout", "probe", "restore"). May be NULL. */
    void (*log)(void* ctx, const char* direction, const uint8_t frame[GOVEE_FRAME_LEN], const char* note);
} GoveeCensusIo;

/* Runs the read battery (and the probe writes if `probe_writes`) on an
 * already connected and subscribed light, then restores the power state.
 * `model` picks the segment count fallback and the secret-family hint. */
void govee_census_run(const GoveeCensusIo* io, const char* model, bool probe_writes, GoveeCensusResult* out);

/* One JSON object, no newline. `name`/`model`/`addr` from the scan; `adv` may
 * be invalid. The secret bytes are only printed when `print_secret`. */
void govee_census_summary_json(
    const char* addr,
    const char* name,
    const char* model,
    const GoveeAdvInfo* adv,
    int8_t rssi,
    const GoveeCensusResult* r,
    bool print_secret,
    char* out,
    size_t out_size);

/* Short one-line reading for a screen: "fw 1.04.08 colour02 15seg ok". */
void govee_census_summary_short(const GoveeCensusResult* r, char* out, size_t out_size);

/* Shared helper: "02,0B" style list of the dialects whose writes echoed. */
void govee_census_echoed_string(uint8_t echoed, char* out, size_t out_size);
