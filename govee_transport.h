#pragma once

#include "govee_protocol.h"

/* A BLE link to Govee lights. Two implementations:
 *  - Native: the Flipper's own radio via furi_ble/central.h. Needs a firmware
 *    built with the BLE central role (upstream PR #4436) and the STM32WB Full
 *    BLE stack installed.
 *  - UART: an ESP32 running esp32/govee_bridge on the GPIO header, which does
 *    the BLE work and takes line commands over serial. Works on any firmware.
 *
 * All calls are blocking and must be made from a worker thread, never from a
 * GUI callback. */

#define GOVEE_LIGHT_NAME_LEN 24
#define GOVEE_MAX_LIGHTS     16

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    char name[GOVEE_LIGHT_NAME_LEN];
    char model[6];
    GoveeDialect dialect;
} GoveeLight;

typedef enum {
    GoveeTransportNative = 0,
    GoveeTransportUart = 1,
    GoveeTransportCount,
} GoveeTransportKind;

typedef struct GoveeTransport GoveeTransport;

/* Called from the scan as each new light is found (progress reporting). It may
 * run on an interrupt-fed worker or the BLE event thread: keep it short. */
typedef void (*GoveeScanFoundCallback)(const GoveeLight* light, size_t count, void* context);

const char* govee_transport_kind_name(GoveeTransportKind kind);
/* Can this transport be used on this firmware / hardware? `why` gets a short
 * human-readable reason when not. */
bool govee_transport_available(GoveeTransportKind kind, const char** why);

GoveeTransport* govee_transport_alloc(GoveeTransportKind kind);
void govee_transport_free(GoveeTransport* transport);

/* Scans for `duration_ms`, collecting up to `max` distinct Govee lights.
 * Returns early once `*abort` becomes true (set it from another thread). */
bool govee_transport_scan(
    GoveeTransport* transport,
    uint32_t duration_ms,
    GoveeLight* lights,
    size_t max,
    size_t* count,
    const volatile bool* abort,
    GoveeScanFoundCallback on_found,
    void* context);

bool govee_transport_connect(GoveeTransport* transport, const GoveeLight* light, uint32_t timeout_ms);
bool govee_transport_is_connected(GoveeTransport* transport);
bool govee_transport_write(
    GoveeTransport* transport,
    const uint8_t frame[GOVEE_FRAME_LEN],
    bool with_response);
void govee_transport_disconnect(GoveeTransport* transport);

const char* govee_transport_last_error(GoveeTransport* transport);

/* Implementation detail shared by the two backends. */
typedef struct {
    bool (*scan)(
        GoveeTransport*,
        uint32_t,
        GoveeLight*,
        size_t,
        size_t*,
        const volatile bool*,
        GoveeScanFoundCallback,
        void*);
    bool (*connect)(GoveeTransport*, const GoveeLight*, uint32_t);
    bool (*is_connected)(GoveeTransport*);
    bool (*write)(GoveeTransport*, const uint8_t*, bool);
    void (*disconnect)(GoveeTransport*);
    void (*free)(GoveeTransport*);
} GoveeTransportVtable;

struct GoveeTransport {
    const GoveeTransportVtable* vt;
    GoveeTransportKind kind;
    const char* last_error;
    void* impl;
};

/* Backend constructors (return NULL on failure). */
GoveeTransport* govee_transport_native_alloc(void);
bool govee_transport_native_available(const char** why);
GoveeTransport* govee_transport_uart_alloc(void);
bool govee_transport_uart_available(const char** why);

/* Helper for both backends: add a light to a list if it is a Govee and not
 * already present. Returns true if added. */
bool govee_light_list_add(
    GoveeLight* lights,
    size_t max,
    size_t* count,
    const uint8_t addr[6],
    uint8_t addr_type,
    int8_t rssi,
    const char* name,
    size_t name_len);
