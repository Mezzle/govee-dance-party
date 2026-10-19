#include "govee_transport.h"

#include <furi.h>
#include <string.h>

const char* govee_transport_kind_name(GoveeTransportKind kind) {
    switch(kind) {
    case GoveeTransportNative:
        return "Flipper BLE";
    case GoveeTransportUart:
        return "ESP32 bridge";
    default:
        return "?";
    }
}

bool govee_transport_available(GoveeTransportKind kind, const char** why) {
    switch(kind) {
    case GoveeTransportNative:
        return govee_transport_native_available(why);
    case GoveeTransportUart:
        return govee_transport_uart_available(why);
    default:
        if(why) *why = "Unknown transport";
        return false;
    }
}

GoveeTransport* govee_transport_alloc(GoveeTransportKind kind) {
    switch(kind) {
    case GoveeTransportNative:
        return govee_transport_native_alloc();
    case GoveeTransportUart:
        return govee_transport_uart_alloc();
    default:
        return NULL;
    }
}

void govee_transport_free(GoveeTransport* transport) {
    if(!transport) return;
    transport->vt->free(transport);
}

bool govee_transport_scan(
    GoveeTransport* transport,
    uint32_t duration_ms,
    GoveeLight* lights,
    size_t max,
    size_t* count,
    const volatile bool* abort,
    GoveeScanFoundCallback on_found,
    void* context) {
    furi_check(transport);
    *count = 0;
    return transport->vt->scan(transport, duration_ms, lights, max, count, abort, on_found, context);
}

bool govee_transport_connect(GoveeTransport* transport, const GoveeLight* light, uint32_t timeout_ms) {
    furi_check(transport);
    return transport->vt->connect(transport, light, timeout_ms);
}

bool govee_transport_is_connected(GoveeTransport* transport) {
    return transport && transport->vt->is_connected(transport);
}

bool govee_transport_write(
    GoveeTransport* transport,
    const uint8_t frame[GOVEE_FRAME_LEN],
    bool with_response) {
    furi_check(transport);
    return transport->vt->write(transport, frame, with_response);
}

void govee_transport_disconnect(GoveeTransport* transport) {
    if(transport) transport->vt->disconnect(transport);
}

const char* govee_transport_last_error(GoveeTransport* transport) {
    return (transport && transport->last_error) ? transport->last_error : "";
}

bool govee_light_list_add(
    GoveeLight* lights,
    size_t max,
    size_t* count,
    const uint8_t addr[6],
    uint8_t addr_type,
    int8_t rssi,
    const char* name,
    size_t name_len) {
    char model[6];
    if(!govee_name_parse(name, name_len, model, sizeof(model))) return false;
    for(size_t i = 0; i < *count; i++) {
        if(memcmp(lights[i].addr, addr, 6) == 0) {
            lights[i].rssi = rssi;
            return false;
        }
    }
    if(*count >= max) return false;
    GoveeLight* l = &lights[*count];
    memset(l, 0, sizeof(*l));
    memcpy(l->addr, addr, 6);
    l->addr_type = addr_type;
    l->rssi = rssi;
    size_t n = name_len < GOVEE_LIGHT_NAME_LEN - 1 ? name_len : GOVEE_LIGHT_NAME_LEN - 1;
    memcpy(l->name, name, n);
    l->name[n] = '\0';
    memcpy(l->model, model, sizeof(model));
    l->dialect = govee_dialect_for_model(model);
    (*count)++;
    return true;
}
