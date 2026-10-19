/* Native backend: the Flipper's own radio as a BLE central.
 *
 * Needs furi_ble/central.h from flipperdevices/flipperzero-firmware PR #4436
 * and the STM32WB Full BLE stack. On an SDK without that header this file
 * compiles to a stub that reports the transport as unavailable. */

#include "govee_transport.h"

#include <furi.h>
#include <furi_hal_bt.h>
#include <bt/bt_service/bt.h>

#if __has_include(<furi_ble/central.h>)
#include <furi_ble/central.h>
#define GOVEE_HAVE_NATIVE_CENTRAL 1
#endif

#define TAG "GoveeNative"

#ifdef GOVEE_HAVE_NATIVE_CENTRAL

#define AD_TYPE_SHORT_NAME    0x08
#define AD_TYPE_COMPLETE_NAME 0x09
#define AD_TYPE_MANUFACTURER  0xFF

typedef struct {
    Bt* bt;
    bool was_advertising;
    uint16_t control_handle;
    bool connected;
    /* scan state */
    FuriMutex* mutex;
    GoveeLight* lights;
    size_t max;
    size_t* count;
    GoveeScanFoundCallback on_found;
    void* on_found_ctx;
} NativeImpl;

static void native_scan_cb(const BleCentralAdvReport* report, void* context) {
    NativeImpl* impl = context;
    const uint8_t* name = NULL;
    uint8_t name_len = 0;
    if(!ble_central_adv_find(report, AD_TYPE_COMPLETE_NAME, &name, &name_len) &&
       !ble_central_adv_find(report, AD_TYPE_SHORT_NAME, &name, &name_len)) {
        return;
    }
    const uint8_t* mfr = NULL;
    uint8_t mfr_len = 0;
    GoveeAdvInfo adv = {0};
    if(ble_central_adv_find(report, AD_TYPE_MANUFACTURER, &mfr, &mfr_len)) {
        govee_adv_parse_mfr(mfr, mfr_len, &adv);
    }
    furi_mutex_acquire(impl->mutex, FuriWaitForever);
    bool added = govee_light_list_add(
        impl->lights,
        impl->max,
        impl->count,
        report->addr,
        report->addr_type,
        report->rssi,
        (const char*)name,
        name_len,
        &adv);
    GoveeLight copy;
    size_t n = *impl->count;
    if(added) copy = impl->lights[n - 1];
    furi_mutex_release(impl->mutex);
    if(added && impl->on_found) impl->on_found(&copy, n, impl->on_found_ctx);
}

static bool native_scan(
    GoveeTransport* t,
    uint32_t duration_ms,
    GoveeLight* lights,
    size_t max,
    size_t* count,
    const volatile bool* abort,
    GoveeScanFoundCallback on_found,
    void* context) {
    NativeImpl* impl = t->impl;
    impl->lights = lights;
    impl->max = max;
    impl->count = count;
    impl->on_found = on_found;
    impl->on_found_ctx = context;

    /* The controller refuses to scan while we advertise. */
    impl->was_advertising = furi_hal_bt_is_advertising();
    if(impl->was_advertising) furi_hal_bt_stop_advertising();

    if(!ble_central_scan_start(native_scan_cb, impl)) {
        FURI_LOG_E(TAG, "scan start failed, status 0x%02x", ble_central_last_status());
        t->last_error = "Scan refused by radio";
        return false;
    }
    uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < furi_ms_to_ticks(duration_ms) && !*abort) {
        furi_delay_ms(50);
    }
    for(int i = 0; i < 10 && !ble_central_scan_stop(); i++) furi_delay_ms(100);
    return true;
}

static bool native_connect(GoveeTransport* t, const GoveeLight* light, uint32_t timeout_ms) {
    NativeImpl* impl = t->impl;
    if(impl->connected) t->vt->disconnect(t);
    if(furi_hal_bt_is_advertising()) {
        impl->was_advertising = true;
        furi_hal_bt_stop_advertising();
    }
    if(!ble_central_connect(light->addr, light->addr_type, timeout_ms)) {
        t->last_error = "Connect failed";
        return false;
    }
    impl->connected = true;
    uint16_t value_handle = 0;
    if(!ble_central_find_characteristic_128(
           GOVEE_SERVICE_UUID_LE, GOVEE_CONTROL_UUID_LE, &value_handle, NULL, 5000)) {
        t->last_error = "No Govee service";
        ble_central_disconnect();
        impl->connected = false;
        return false;
    }
    impl->control_handle = value_handle;
    return true;
}

static bool native_is_connected(GoveeTransport* t) {
    NativeImpl* impl = t->impl;
    return impl->connected && ble_central_is_connected();
}

static bool native_write(GoveeTransport* t, const uint8_t* frame, bool with_response) {
    NativeImpl* impl = t->impl;
    if(!native_is_connected(t)) {
        t->last_error = "Not connected";
        return false;
    }
    if(!ble_central_write(impl->control_handle, frame, GOVEE_FRAME_LEN, with_response, 2000)) {
        t->last_error = "Write failed";
        return false;
    }
    return true;
}

static void native_disconnect(GoveeTransport* t) {
    NativeImpl* impl = t->impl;
    if(impl->connected) {
        ble_central_disconnect();
        impl->connected = false;
    }
}

static void native_free(GoveeTransport* t) {
    NativeImpl* impl = t->impl;
    native_disconnect(t);
    if(ble_central_is_scanning()) ble_central_scan_stop();
    for(int i = 0; i < 10 && !ble_central_deinit(); i++) furi_delay_ms(100);
    if(impl->was_advertising) furi_hal_bt_start_advertising();
    furi_mutex_free(impl->mutex);
    furi_record_close(RECORD_BT);
    free(impl);
    free(t);
}

static const GoveeTransportVtable native_vt = {
    .scan = native_scan,
    .connect = native_connect,
    .is_connected = native_is_connected,
    .write = native_write,
    .disconnect = native_disconnect,
    .free = native_free,
    /* PR #4436's central API exposes writes only; no notification path, so
     * no reads. The census falls back to advertisement-only cataloguing. */
    .subscribe = NULL,
    .read = NULL,
};

bool govee_transport_native_available(const char** why) {
    if(!furi_hal_bt_is_central_supported()) {
        if(why) *why = "Needs Full BLE stack";
        return false;
    }
    return true;
}

GoveeTransport* govee_transport_native_alloc(void) {
    const char* why;
    if(!govee_transport_native_available(&why)) return NULL;
    GoveeTransport* t = malloc(sizeof(GoveeTransport));
    NativeImpl* impl = malloc(sizeof(NativeImpl));
    memset(impl, 0, sizeof(*impl));
    impl->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    impl->bt = furi_record_open(RECORD_BT);
    /* Drop any phone connection so the controller is ours. */
    bt_disconnect(impl->bt);
    ble_central_init();
    t->vt = &native_vt;
    t->kind = GoveeTransportNative;
    t->last_error = NULL;
    t->impl = impl;
    return t;
}

#else /* no furi_ble/central.h in this SDK */

bool govee_transport_native_available(const char** why) {
    if(why) *why = "Firmware lacks BLE central";
    return false;
}

GoveeTransport* govee_transport_native_alloc(void) {
    return NULL;
}

#endif
