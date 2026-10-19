/* UART backend: an ESP32 on the GPIO header runs esp32/govee_bridge and does
 * the BLE work. Line protocol at 115200 8N1 on Flipper pins 13 (TX) / 14 (RX):
 *
 *   > PING                       < PONG govee-bridge <version>
 *   > SCAN <ms>                  < ADV <aa:bb:cc:dd:ee:ff> <type> <rssi> <name>  (repeated)
 *                                < SCAN_DONE
 *   > CONN <aa:bb:cc:dd:ee:ff> <type>   < OK | ERR <reason>
 *   > W <40 hex chars> [R]       < OK | ERR <reason>   (R = write with response)
 *   > DISC                       < OK
 */

#include "govee_transport.h"

#include <furi.h>
#include <furi_hal.h>
#include <expansion/expansion.h>
#include <string.h>
#include <stdlib.h>

#define TAG "GoveeUart"

#define UART_BAUD    115200
#define LINE_MAX     96
#define RX_BUF_SIZE  512
#define REPLY_TIMEOUT 1500

typedef struct {
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx;
    Expansion* expansion;
    bool otg_was_on;
    bool connected;
} UartImpl;

static void uart_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    UartImpl* impl = context;
    if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t b = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(impl->rx, &b, 1, 0);
        }
    }
}

static void uart_send_line(UartImpl* impl, const char* line) {
    FURI_LOG_D(TAG, "> %s", line);
    furi_hal_serial_tx(impl->serial, (const uint8_t*)line, strlen(line));
    furi_hal_serial_tx(impl->serial, (const uint8_t*)"\n", 1);
}

/* Reads one line (without the newline) into buf. Returns false on timeout. */
static bool uart_read_line(UartImpl* impl, char* buf, size_t size, uint32_t timeout_ms) {
    size_t len = 0;
    uint32_t start = furi_get_tick();
    while(true) {
        uint32_t elapsed = furi_get_tick() - start;
        if(elapsed >= furi_ms_to_ticks(timeout_ms)) return false;
        uint8_t b;
        size_t got = furi_stream_buffer_receive(
            impl->rx, &b, 1, furi_ms_to_ticks(timeout_ms) - elapsed);
        if(got == 0) return false;
        if(b == '\r') continue;
        if(b == '\n') {
            if(len == 0) continue; /* skip blank lines */
            buf[len] = '\0';
            FURI_LOG_D(TAG, "< %s", buf);
            return true;
        }
        if(len < size - 1) buf[len++] = (char)b;
    }
}

static void uart_flush(UartImpl* impl) {
    uint8_t junk[32];
    while(furi_stream_buffer_receive(impl->rx, junk, sizeof(junk), 0) > 0) {
    }
}

/* Sends a command and waits for OK / ERR. */
static bool uart_command(GoveeTransport* t, const char* cmd, uint32_t timeout_ms) {
    UartImpl* impl = t->impl;
    static char reply[LINE_MAX];
    uart_flush(impl);
    uart_send_line(impl, cmd);
    uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < furi_ms_to_ticks(timeout_ms)) {
        if(!uart_read_line(impl, reply, sizeof(reply), timeout_ms)) break;
        if(strncmp(reply, "OK", 2) == 0) return true;
        if(strncmp(reply, "ERR", 3) == 0) {
            t->last_error = reply + (reply[3] == ' ' ? 4 : 3);
            return false;
        }
        /* ignore chatter such as late ADV lines */
    }
    t->last_error = "Bridge timeout";
    return false;
}

static bool parse_mac(const char* s, uint8_t out[6]) {
    unsigned v[6];
    if(sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for(int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

static bool uart_scan(
    GoveeTransport* t,
    uint32_t duration_ms,
    GoveeLight* lights,
    size_t max,
    size_t* count,
    const volatile bool* abort,
    GoveeScanFoundCallback on_found,
    void* context) {
    UartImpl* impl = t->impl;
    char line[LINE_MAX];
    uart_flush(impl);
    snprintf(line, sizeof(line), "SCAN %lu", (unsigned long)duration_ms);
    uart_send_line(impl, line);
    uint32_t start = furi_get_tick();
    uint32_t budget = duration_ms + 2000;
    while(furi_get_tick() - start < furi_ms_to_ticks(budget)) {
        if(*abort) {
            uart_send_line(impl, "STOP");
                    return true;
        }
        if(!uart_read_line(impl, line, sizeof(line), 200)) continue;
        if(strcmp(line, "SCAN_DONE") == 0) return true;
        if(strncmp(line, "ERR", 3) == 0) {
            t->last_error = "Bridge scan error";
            return false;
        }
        if(strncmp(line, "ADV ", 4) != 0) continue;
        /* ADV <mac> <type> <rssi> <name...> */
        char* p = line + 4;
        uint8_t addr[6];
        if(!parse_mac(p, addr)) continue;
        p = strchr(p, ' ');
        if(!p) continue;
        uint8_t addr_type = (uint8_t)strtoul(p + 1, &p, 10);
        if(*p != ' ') continue;
        int rssi = (int)strtol(p + 1, &p, 10);
        if(*p != ' ') continue;
        const char* name = p + 1;
        if(govee_light_list_add(lights, max, count, addr, addr_type, (int8_t)rssi, name, strlen(name))) {
            if(on_found) on_found(&lights[*count - 1], *count, context);
        }
    }
    t->last_error = "Bridge not answering";
    return *count > 0;
}

static bool uart_connect(GoveeTransport* t, const GoveeLight* light, uint32_t timeout_ms) {
    UartImpl* impl = t->impl;
    char cmd[64];
    snprintf(
        cmd,
        sizeof(cmd),
        "CONN %02x:%02x:%02x:%02x:%02x:%02x %u",
        light->addr[0],
        light->addr[1],
        light->addr[2],
        light->addr[3],
        light->addr[4],
        light->addr[5],
        light->addr_type);
    impl->connected = uart_command(t, cmd, timeout_ms + 3000);
    return impl->connected;
}

static bool uart_is_connected(GoveeTransport* t) {
    UartImpl* impl = t->impl;
    return impl->connected;
}

static bool uart_write(GoveeTransport* t, const uint8_t* frame, bool with_response) {
    UartImpl* impl = t->impl;
    if(!impl->connected) {
        t->last_error = "Not connected";
        return false;
    }
    char cmd[2 + GOVEE_FRAME_LEN * 2 + 4];
    char* p = cmd;
    p += snprintf(p, sizeof(cmd) - (p - cmd), "W ");
    for(int i = 0; i < GOVEE_FRAME_LEN; i++) p += snprintf(p, sizeof(cmd) - (p - cmd), "%02x", frame[i]);
    if(with_response) p += snprintf(p, sizeof(cmd) - (p - cmd), " R");
    bool ok = uart_command(t, cmd, REPLY_TIMEOUT);
    if(!ok && strncmp(t->last_error, "disconnected", 12) == 0) impl->connected = false;
    return ok;
}

static void uart_disconnect(GoveeTransport* t) {
    UartImpl* impl = t->impl;
    if(!impl->connected) return;
    uart_command(t, "DISC", REPLY_TIMEOUT);
    impl->connected = false;
}

static void uart_free(GoveeTransport* t) {
    UartImpl* impl = t->impl;
    uart_disconnect(t);
    furi_hal_serial_async_rx_stop(impl->serial);
    furi_hal_serial_deinit(impl->serial);
    furi_hal_serial_control_release(impl->serial);
    furi_stream_buffer_free(impl->rx);
    if(!impl->otg_was_on) furi_hal_power_disable_otg();
    expansion_enable(impl->expansion);
    furi_record_close(RECORD_EXPANSION);
    free(impl);
    free(t);
}

static const GoveeTransportVtable uart_vt = {
    .scan = uart_scan,
    .connect = uart_connect,
    .is_connected = uart_is_connected,
    .write = uart_write,
    .disconnect = uart_disconnect,
    .free = uart_free,
};

bool govee_transport_uart_available(const char** why) {
    UNUSED(why);
    return true;
}

GoveeTransport* govee_transport_uart_alloc(void) {
    GoveeTransport* t = malloc(sizeof(GoveeTransport));
    UartImpl* impl = malloc(sizeof(UartImpl));
    memset(impl, 0, sizeof(*impl));

    /* The expansion-module service listens on this UART; take it over. */
    impl->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(impl->expansion);

    impl->otg_was_on = furi_hal_power_is_otg_enabled();
    if(!impl->otg_was_on) furi_hal_power_enable_otg();

    impl->rx = furi_stream_buffer_alloc(RX_BUF_SIZE, 1);
    impl->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    furi_check(impl->serial);
    furi_hal_serial_init(impl->serial, UART_BAUD);
    furi_hal_serial_async_rx_start(impl->serial, uart_rx_cb, impl, false);

    t->vt = &uart_vt;
    t->kind = GoveeTransportUart;
    t->last_error = NULL;
    t->impl = impl;

    /* Give a freshly powered bridge a moment, then check it answers. */
    bool alive = false;
    for(int attempt = 0; attempt < 6 && !alive; attempt++) {
        char reply[LINE_MAX];
        uart_flush(impl);
        uart_send_line(impl, "PING");
        uint32_t start = furi_get_tick();
        while(furi_get_tick() - start < furi_ms_to_ticks(500)) {
            if(!uart_read_line(impl, reply, sizeof(reply), 500)) break;
            if(strncmp(reply, "PONG", 4) == 0) {
                alive = true;
                break;
            }
        }
    }
    if(!alive) {
        t->last_error = "No bridge on pins 13/14";
        uart_free(t);
        return NULL;
    }
    return t;
}
