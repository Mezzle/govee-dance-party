/*
 * Govee Party for ESP32: one image, three jobs.
 *
 *  1. BRIDGE for the Flipper Zero app. Plug the Flipper into the Grove port /
 *     UART and the Flipper does the driving over a tiny line protocol. The
 *     same protocol is accepted on the USB serial port, so a PC can drive it
 *     too.
 *
 *       > PING                        < PONG govee-bridge 3
 *       > SCAN <ms>                   < ADV <mac> <addrtype> <rssi> <pactType> <pactCode> <flags> <enc> <name>
 *                                     < SCAN_DONE          (pact columns are "-" without manufacturer data;
 *                                                           flags is hex, enc is 1 when flags bit 6 is set)
 *       > STOP                        (abort a scan)
 *       > CONN <mac> <addrtype>       < OK | ERR <reason>
 *       > W <40 hex chars> [R]        < OK | ERR <reason>  (R = write with response)
 *       > N                           < OK | ERR <reason>  (subscribe to the status characteristic 2b10)
 *       > R <40 hex chars> [ms]       < RX <40 hex chars> | ERR timeout   (write a read frame, wait for its AA reply)
 *                                     < NOTIFY <40 hex chars>   (unsolicited frames while subscribed)
 *       > DISC                        < OK
 *       > BGC                         < BGC <hex> | ERR <reason>   (read the BGC-info characteristic 2b12: encryptVersion)
 *       > SVC                         < SVC <service uuid> <char uuid> <props> per characteristic, then SVC_DONE
 *       > CENSUS [PROBE]              < CENSUS <json summary> per light, then CENSUS_DONE
 *       > DUMP                        dump and clear the LittleFS frame log on the USB port
 *
 *     Version 4 added BGC and SVC; everything a version 3 app sends is unchanged.
 *
 *     Wiring (Flipper GPIO header -> ESP32):
 *       Flipper 13 (TX)  -> BRIDGE_RX_PIN   (M5Stack Grove SDA, white)
 *       Flipper 14 (RX)  <- BRIDGE_TX_PIN   (M5Stack Grove SCL, yellow)
 *       Flipper 1  (5V)  -> 5V              (Grove V, red; the app switches it on)
 *       Flipper GND      -> GND             (Grove G, black)
 *
 *  2. STANDALONE with the board's own button, no Flipper:
 *       short press   Dance Party: every Govee light in range -> on, 100%,
 *                     colour pops, blinking scene, mic-driven music mode.
 *       double press  Rave: toggle a streamed show hopping between lights.
 *       triple press  Census: catalogue what every light in range speaks.
 *       long press    Govee B-Gone: every Govee light in range -> off.
 *       hold at boot  Lurk: keep scanning and switching lights off, forever.
 *
 *  3. CENSUS: for each light, record the advertisement, connect, subscribe,
 *     read power / brightness / mode / firmware / hardware / IC count / light
 *     count / pairing secret / segment page, optionally probe the four colour
 *     dialects, restore power and move on. One JSON line per frame plus one
 *     summary per light on the USB port, in the Govee-Bluetooth bench rig's
 *     log shape; buffered to LittleFS while no USB host is attached.
 *
 *  LED: blue idle, cyan scanning, green connected, red flash on error,
 *       purple breathing while lurking, yellow during a census, colour-synced
 *       during rave. While a button job runs the Flipper gets "ERR busy"
 *       (PING still works).
 *
 *  Boards: M5Stack ATOM Lite / ATOMS3 Lite are the intended targets (pins
 *  below). Any ESP32 with Bluetooth works for the bridge; the official Flipper
 *  WiFi devboard is an ESP32-S2 and has no Bluetooth. Library: NimBLE-Arduino 2.x.
 */

#include <NimBLEDevice.h>
#include <LittleFS.h>
#include "govee_frames.h"

/* ------------------------------------------------------------- board pins */
#if defined(ARDUINO_M5STACK_ATOMS3)
#define BRIDGE_RX_PIN 2
#define BRIDGE_TX_PIN 1
#define BUTTON_PIN 41
#define LED_PIN 35
#elif defined(ARDUINO_M5STACK_ATOM) || defined(ARDUINO_M5Stack_ATOM)
#define BRIDGE_RX_PIN 26
#define BRIDGE_TX_PIN 32
#define BUTTON_PIN 39
#define LED_PIN 27
#elif defined(ARDUINO_M5STACK_CORE) || defined(ARDUINO_M5Stack_Core_ESP32)
#define BRIDGE_RX_PIN 21
#define BRIDGE_TX_PIN 22
#define BUTTON_PIN 39 /* button A */
#elif defined(ARDUINO_M5STACK_CORE2) || defined(ARDUINO_M5STACK_Core2)
#define BRIDGE_RX_PIN 32
#define BRIDGE_TX_PIN 33
#elif defined(ARDUINO_M5STACK_CORES3)
#define BRIDGE_RX_PIN 2
#define BRIDGE_TX_PIN 1
#elif defined(ARDUINO_M5STACK_STICKC_PLUS) || defined(ARDUINO_M5STACK_STICKC_PLUS2) || \
    defined(ARDUINO_M5Stick_C_PLUS) || defined(ARDUINO_M5Stick_C)
#define BRIDGE_RX_PIN 32
#define BRIDGE_TX_PIN 33
#define BUTTON_PIN 37 /* button A */
#elif defined(ARDUINO_M5STACK_NANOC6)
#define BRIDGE_RX_PIN 2
#define BRIDGE_TX_PIN 1
#define BUTTON_PIN 9
#elif CONFIG_IDF_TARGET_ESP32S3
#define BRIDGE_RX_PIN 44
#define BRIDGE_TX_PIN 43
#elif CONFIG_IDF_TARGET_ESP32C3
#define BRIDGE_RX_PIN 20
#define BRIDGE_TX_PIN 21
#elif CONFIG_IDF_TARGET_ESP32C6
#define BRIDGE_RX_PIN 17
#define BRIDGE_TX_PIN 16
#else /* classic ESP32: UART2 default pins */
#define BRIDGE_RX_PIN 16
#define BRIDGE_TX_PIN 17
#endif

#ifndef BUTTON_PIN
#define BUTTON_PIN 0 /* BOOT button on most dev boards */
#endif
#if !defined(LED_PIN) && defined(RGB_BUILTIN)
#define LED_PIN RGB_BUILTIN
#endif

#define BRIDGE_BAUD 115200
#define BRIDGE_VERSION "4"
#define SKETCH_VERSION "0.2" /* evidence "software" field */
#define SCAN_MS 5000
#define CONNECT_TIMEOUT_MS 6000
#define MAX_LIGHTS 16
#define LONG_PRESS_MS 1200
#define DOUBLE_PRESS_MS 350
#define READ_TIMEOUT_MS 1500
#define NOTIFY_RING 8
/* After this many unanswered core reads (power, brightness, firmware) the
 * light is silent and the rest of the battery is skipped. */
#define CENSUS_SILENT_AFTER 3
#define EVIDENCE_SCHEMA "govee-evidence/1"

/* Census probe writes (red 02, green 0B, blue 0D, magenta 15) change the light's colour
 * and mode. Off by default; set to 1 here, or send "CENSUS PROBE". */
#ifndef CENSUS_PROBE_WRITES
#define CENSUS_PROBE_WRITES 0
#endif
/* Print the 8-byte pairing secret in the summary. Off: only its presence. */
#ifndef CENSUS_PRINT_SECRET
#define CENSUS_PRINT_SECRET 0
#endif
#define LOG_PATH "/census.jsonl"
#define LOG_OLD_PATH "/census.old.jsonl"
#define LOG_MAX_BYTES (96 * 1024) /* per file; two files are kept */

/* USB host detection only exists on native-USB boards running the CDC
 * console. With a UART-to-USB bridge chip (ATOM Lite) nothing can tell, so
 * the log is always stored as well and DUMP fetches it. */
#if ARDUINO_USB_CDC_ON_BOOT && (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6)
#define HOST_DETECT 1
#else
#define HOST_DETECT 0
#endif

static const NimBLEUUID GOVEE_SERVICE(GOVEE_SERVICE_UUID_STR);
static const NimBLEUUID GOVEE_CONTROL(GOVEE_CONTROL_UUID_STR);
static const NimBLEUUID GOVEE_STATUS(GOVEE_STATUS_UUID_STR);
/* BGC-info: byte 1 (lasswellt, H1270) or byte 0 (decompiled app) carries
 * encryptVersion 0 / 1 / 2. The census records the raw bytes. */
static const NimBLEUUID GOVEE_BGC("00010203-0405-0607-0809-0a0b0c0d2b12");
/* H6101 / H6104 use the generic UART service instead (SPEC 3.1). */
static const NimBLEUUID GOVEE_ALT_SERVICE("0000ffe0-0000-1000-8000-00805f9b34fb");

enum Press { PressNone, PressShort, PressDouble, PressTriple, PressLong };

struct Light {
    NimBLEAddress addr;
    char name[32];
    char model[6];
    GoveeDialect dialect;
    int rssi;
    GoveeAdvInfo adv;
    uint8_t segments; /* from AA 40 once read, else 0 = unknown */
};

enum Restored { RestoredNone, RestoredPower, RestoredFull };

struct CensusResult {
    bool connected = false, subscribed = false, silent = false, aborted = false;
    bool powerKnown = false, powerOn = false;
    bool brightnessKnown = false;
    uint8_t brightness = 0;
    bool submodeKnown = false;
    uint8_t submode = 0, modeBody[8] = {0};
    char firmware[12] = "", hardware[12] = "";
    bool icKnown = false;
    uint16_t icCount = 0;
    uint8_t segments = 0;
    bool lightCountKnown = false;
    uint8_t lightCountValue = 0;
    bool secretPresent = false;
    uint8_t secret[8] = {0};
    bool segmentPage = false;
    uint8_t pageBefore[20] = {0};
    bool probed = false;
    uint8_t echoed = 0; /* bit (1 << GoveeDialect) */
    uint8_t acks = 0; /* 33 xx application acks on the notify characteristic */
    uint8_t reads = 0, replies = 0, badFrames = 0;
    Restored restored = RestoredNone;
    const char* restoreNote = nullptr;
    /* GATT evidence, gathered on every connection including encrypted ones */
    bool gattKnown = false;
    bool hasService = false, hasControl = false, hasStatus = false, hasBgc = false, hasAlt = false;
    char bgc[41] = ""; /* raw 2b12 bytes as hex, "" when unreadable */
    uint8_t oddNotifies = 0; /* notifications that were not 20 bytes */
};

HardwareSerial flipperSerial(1);
static Stream* replyTo = &flipperSerial; /* where the current command came from */
static Light lights[MAX_LIGHTS];
static size_t lightCount = 0;
static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* control = nullptr;
static NimBLERemoteCharacteristic* status = nullptr;
static bool subscribed = false;
/* Whether 2b11 takes write requests. The H6006 (and H6004 per egold555)
 * offer write-without-response only, refuse a request and drop the link. */
static bool controlWithResponse = true;
static bool controlWithoutResponse = false;
static volatile bool scanning = false;
static bool bridgeScan = false; /* scan was requested by the Flipper: stream ADV lines */
static bool bridgeSession = false; /* Flipper owns the connection */
static bool busy = false; /* a button job is running */
static bool lurking = false;
static bool raving = false;
static bool censusToBridge = false; /* CENSUS came over the line protocol: echo summaries */
static File logFile;
static bool logStoring = false;

/* ----------------------------------------------------------------- LED */

static void led(uint8_t r, uint8_t g, uint8_t b) {
#ifdef LED_PIN
    rgbLedWrite(LED_PIN, r, g, b);
#else
    (void)r; (void)g; (void)b;
#endif
}
static void ledIdle() { led(0, 0, 12); }
static void ledError() { led(80, 0, 0); delay(150); ledIdle(); }

/* -------------------------------------------------------------- serial */

static void reply(const String& s) {
    replyTo->print(s);
    replyTo->print('\n');
}

static bool buttonDown() {
    return digitalRead(BUTTON_PIN) == LOW;
}

static bool hostAttached() {
#if HOST_DETECT
    return (bool)Serial;
#else
    return true;
#endif
}

static void hex40(const uint8_t f[20], char out[41]) {
    for(int i = 0; i < 20; i++) snprintf(out + i * 2, 3, "%02x", f[i]);
}

static bool parseHex40(const String& hex, uint8_t frame[20]) {
    if(hex.length() != 40) return false;
    for(int i = 0; i < 20; i++) {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        auto val = [](char c) -> int {
            if(c >= '0' && c <= '9') return c - '0';
            if(c >= 'a' && c <= 'f') return c - 'a' + 10;
            if(c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = val(hi), l = val(lo);
        if(h < 0 || l < 0) return false;
        frame[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

/* ------------------------------------------------------------ frame log */

/* The census log goes to the USB port live and, while no host is attached
 * (or always, where that cannot be told), to LittleFS. Bounded: when the
 * file passes LOG_MAX_BYTES it becomes the .old file and a new one starts. */
static void logOpen() {
    logStoring = !hostAttached() || !HOST_DETECT;
    if(!logStoring) return;
    if(LittleFS.exists(LOG_PATH)) {
        File f = LittleFS.open(LOG_PATH, "r");
        size_t sz = f ? f.size() : 0;
        if(f) f.close();
        if(sz > LOG_MAX_BYTES) {
            LittleFS.remove(LOG_OLD_PATH);
            LittleFS.rename(LOG_PATH, LOG_OLD_PATH);
        }
    }
    logFile = LittleFS.open(LOG_PATH, "a");
    if(!logFile) logStoring = false;
}

static void logClose() {
    if(logFile) logFile.close();
    logStoring = false;
}

static void logLine(const char* line) {
    Serial.println(line);
    if(logStoring && logFile) {
        logFile.println(line);
        if(logFile.size() > LOG_MAX_BYTES) {
            logFile.close();
            LittleFS.remove(LOG_OLD_PATH);
            LittleFS.rename(LOG_PATH, LOG_OLD_PATH);
            logFile = LittleFS.open(LOG_PATH, "a");
        }
    }
}

static void dumpStoredLog() {
    bool any = false;
    for(const char* path : {LOG_OLD_PATH, LOG_PATH}) {
        if(!LittleFS.exists(path)) continue;
        File f = LittleFS.open(path, "r");
        if(!f) continue;
        if(!any) Serial.println("{\"summary\":\"dump\",\"note\":\"stored census log follows\"}");
        any = true;
        while(f.available()) Serial.write(f.read());
        f.close();
        LittleFS.remove(path);
    }
    if(any) Serial.println("{\"summary\":\"dump\",\"note\":\"end of stored log\"}");
}

static void uptimeIso(char out[24]) {
    unsigned long ms = millis();
    snprintf(out, 24, "T+%lu.%03lu", ms / 1000, ms % 1000);
}

/* {"at","direction","frame","note","decoded","address"[,"redacted"]}:
 * tools/govee_rig.py's shape. Pairing-secret bytes (AA B1 01 .. replies,
 * 33 B2 writes) are blanked before they reach the log. */
static void logFrame(const char* direction, const uint8_t frame[20], const char* note, const char* address) {
    char at[24], hex[41], decoded[48], line[240];
    uint8_t f[20];
    memcpy(f, frame, 20);
    bool redacted = goveeFrameRedact(f);
    uptimeIso(at);
    hex40(f, hex);
    goveeDescribe(f, direction[0] == 'r', decoded, sizeof(decoded));
    snprintf(
        line,
        sizeof(line),
        "{\"at\":\"%s\",\"direction\":\"%s\",\"frame\":\"%s\",\"note\":\"%s\",\"decoded\":\"%s\",\"address\":\"%s\"%s}",
        at, direction, hex, note, decoded, address, redacted ? ",\"redacted\":true" : "");
    logLine(line);
}

/* ---------------------------------------------------------------- scan */

static void advLine(const NimBLEAdvertisedDevice* dev, const GoveeAdvInfo& adv) {
    char line[128];
    char pact[24];
    if(adv.valid) {
        snprintf(pact, sizeof(pact), "%u %u %02x %d", adv.pactType, adv.pactCode, adv.flags, adv.encrypted ? 1 : 0);
    } else {
        snprintf(pact, sizeof(pact), "- - - 0");
    }
    snprintf(
        line,
        sizeof(line),
        "ADV %s %d %d %s %s",
        dev->getAddress().toString().c_str(),
        (int)dev->getAddress().getType(),
        dev->getRSSI(),
        pact,
        dev->getName().c_str());
    reply(line);
}

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if(!dev->haveName()) return;
        char model[6];
        if(!goveeParseName(dev->getName().c_str(), model)) return;
        GoveeAdvInfo adv;
        if(dev->haveManufacturerData()) {
            std::string m = dev->getManufacturerData();
            goveeParseMfr((const uint8_t*)m.data(), m.size(), adv);
        }
        if(bridgeScan) advLine(dev, adv);
        for(size_t i = 0; i < lightCount; i++) {
            if(lights[i].addr == dev->getAddress()) {
                lights[i].rssi = dev->getRSSI();
                if(adv.valid && !lights[i].adv.valid) {
                    lights[i].adv = adv;
                    lights[i].dialect = goveeDialectForLight(lights[i].model, adv);
                }
                return;
            }
        }
        if(lightCount >= MAX_LIGHTS) return;
        Light& l = lights[lightCount++];
        l.addr = dev->getAddress();
        strlcpy(l.name, dev->getName().c_str(), sizeof(l.name));
        memcpy(l.model, model, 6);
        l.adv = adv;
        l.dialect = goveeDialectForLight(model, adv);
        l.rssi = dev->getRSSI();
        l.segments = 0;
        Serial.printf(
            "found %s %s rssi %d %s pact %u/%u flags %02x%s\n",
            l.name,
            l.addr.toString().c_str(),
            l.rssi,
            goveeDialectName(l.dialect),
            adv.pactType,
            adv.pactCode,
            adv.flags,
            adv.encrypted ? " ENCRYPTED" : "");
        led(0, 60, 60);
    }
    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        scanning = false;
        if(bridgeScan) reply("SCAN_DONE");
        bridgeScan = false;
        ledIdle();
    }
} scanCallbacks;

static void disconnectLight();

static bool startScan(uint32_t ms, bool forBridge) {
    disconnectLight();
    lightCount = 0;
    bridgeScan = forBridge;
    led(0, 20, 20);
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallbacks, false);
    scan->setActiveScan(true);
    scan->setInterval(45);
    scan->setWindow(30);
    scan->setDuplicateFilter(false);
    scan->clearResults();
    scanning = true;
    if(!scan->start(ms, false, true)) {
        scanning = false;
        bridgeScan = false;
        ledIdle();
        return false;
    }
    return true;
}

static void scanBlocking() {
    if(!startScan(SCAN_MS, false)) return;
    while(scanning) {
        delay(20);
    }
    Serial.printf("scan done: %u light(s)\n", (unsigned)lightCount);
}

/* ------------------------------------------------------------- connect */

/* Notifications land here from the NimBLE host task; a small ring hands
 * them to the main loop. */
static uint8_t ring[NOTIFY_RING][20];
static volatile uint8_t ringHead = 0, ringTail = 0;
static portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint8_t oddNotifyCount = 0; /* notifications that were not 20 bytes */

static void onNotify(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
    (void)c; (void)isNotify;
    if(len != 20) {
        if(oddNotifyCount < 255) oddNotifyCount++;
        return;
    }
    portENTER_CRITICAL(&ringMux);
    uint8_t next = (ringHead + 1) % NOTIFY_RING;
    if(next != ringTail) {
        memcpy(ring[ringHead], data, 20);
        ringHead = next;
    }
    portEXIT_CRITICAL(&ringMux);
}

static bool ringPop(uint8_t out[20]) {
    bool got = false;
    portENTER_CRITICAL(&ringMux);
    if(ringTail != ringHead) {
        memcpy(out, ring[ringTail], 20);
        ringTail = (ringTail + 1) % NOTIFY_RING;
        got = true;
    }
    portEXIT_CRITICAL(&ringMux);
    return got;
}

static void ringClear() {
    portENTER_CRITICAL(&ringMux);
    ringTail = ringHead;
    portEXIT_CRITICAL(&ringMux);
}

static const char* connectLight(const NimBLEAddress& addr) {
    if(!client) {
        client = NimBLEDevice::createClient();
        client->setConnectTimeout(CONNECT_TIMEOUT_MS);
        client->setConnectionParams(12, 24, 0, 200);
    }
    if(client->isConnected()) client->disconnect();
    subscribed = false;
    status = nullptr;
    ringClear();
    oddNotifyCount = 0;
    if(!client->connect(addr, false, false)) return "connect failed";
    NimBLERemoteService* svc = client->getService(GOVEE_SERVICE);
    control = svc ? svc->getCharacteristic(GOVEE_CONTROL) : nullptr;
    status = svc ? svc->getCharacteristic(GOVEE_STATUS) : nullptr;
    if(!control) {
        client->disconnect();
        return "no govee service";
    }
    controlWithResponse = control->canWrite();
    controlWithoutResponse = control->canWriteNoResponse();
    if(!controlWithResponse && !controlWithoutResponse) {
        client->disconnect();
        control = nullptr;
        status = nullptr;
        return "control not writable";
    }
    led(0, 40, 0);
    return nullptr;
}

static void disconnectLight() {
    if(client && client->isConnected()) client->disconnect();
    control = nullptr;
    status = nullptr;
    subscribed = false;
}

static bool connected() {
    return control && client && client->isConnected();
}

static const char* subscribeStatus() {
    if(!connected()) return "disconnected";
    if(subscribed) return nullptr;
    if(!status) return "no status characteristic";
    if(!status->canNotify()) return "status cannot notify";
    if(!status->subscribe(true, onNotify, true)) return "subscribe failed";
    subscribed = true;
    return nullptr;
}

static bool send(const uint8_t frame[20], bool withResponse = true) {
    if(!connected()) return false;
    bool ok = control->writeValue(frame, 20, controlWithResponse && (withResponse || !controlWithoutResponse));
    delay(60);
    return ok;
}

/* Drains unsolicited frames: NOTIFY lines for a bridge session, log lines
 * during a census. */
static const char* logAddress = "";
static bool logging = false;
static CensusResult* logResult = nullptr; /* the light under census, for ack / bad-frame counts */

/* A frame that was not the reply to a pending read: a 33 xx ack (byte 2 == 0
 * means accepted, SPEC 3.3), an EE push, or a late reply. */
static void noteStray(const uint8_t f[20]) {
    if(!goveeVerify(f)) {
        if(logging) logFrame("rx", f, "bad_checksum", logAddress);
        if(logResult && logResult->badFrames < 255) logResult->badFrames++;
    } else if(f[0] == GOVEE_TYPE_WRITE) {
        if(logging) logFrame("rx", f, f[2] == 0 ? "ack" : "ack_error", logAddress);
        if(logResult && f[2] == 0 && logResult->acks < 255) logResult->acks++;
    } else if(logging) {
        logFrame("rx", f, "unsolicited", logAddress);
    }
    if(bridgeSession) {
        char hex[41];
        hex40(f, hex);
        reply(String("NOTIFY ") + hex);
    }
}

static void drainNotifies() {
    uint8_t f[20];
    while(ringPop(f)) noteStray(f);
}

/* Writes a read request and waits for the matching AA reply. Other frames
 * that arrive meanwhile are passed to drainNotifies' consumers. */
static bool readFrame(const uint8_t req[20], uint8_t rep[20], uint32_t timeoutMs) {
    if(!connected()) return false;
    const char* err = subscribeStatus();
    if(err) return false;
    if(logging) logFrame("tx", req, "query", logAddress);
    if(!control->writeValue(req, 20, controlWithResponse)) return false;
    unsigned long until = millis() + timeoutMs;
    while((long)(until - millis()) > 0) {
        uint8_t f[20];
        while(ringPop(f)) {
            if(goveeReadMatches(req, f)) {
                if(!goveeVerify(f)) {
                    /* A corrupt reply proves the light answered, not what it said. */
                    if(logging) logFrame("rx", f, "bad_checksum", logAddress);
                    if(logResult && logResult->badFrames < 255) logResult->badFrames++;
                    return false;
                }
                memcpy(rep, f, 20);
                if(logging) logFrame("rx", rep, "reply", logAddress);
                return true;
            }
            noteStray(f);
        }
        delay(5);
    }
    if(logging) logFrame("rx", req, "timeout", logAddress);
    return false;
}

/* ----------------------------------------------------------- GATT facts */

/* Reads the BGC-info characteristic (2b12) as hex. False when absent or unreadable. */
static bool readBgc(char hex[41]) {
    hex[0] = 0;
    if(!client || !client->isConnected()) return false;
    NimBLERemoteService* svc = client->getService(GOVEE_SERVICE);
    NimBLERemoteCharacteristic* c = svc ? svc->getCharacteristic(GOVEE_BGC) : nullptr;
    if(!c || !c->canRead()) return false;
    NimBLEAttValue v = c->readValue();
    size_t n = v.size() > 20 ? 20 : v.size();
    if(n == 0) return false;
    for(size_t i = 0; i < n; i++) snprintf(hex + i * 2, 3, "%02x", v.data()[i]);
    return true;
}

/* One "SVC <service> <characteristic> <props>" line per characteristic. */
static void listServices() {
    if(!client || !client->isConnected()) return;
    const auto& services = client->getServices(true);
    for(auto* svc : services) {
        const auto& chars = svc->getCharacteristics(true);
        for(auto* c : chars) {
            String props;
            if(c->canRead()) props += 'r';
            if(c->canWrite()) props += 'w';
            if(c->canWriteNoResponse()) props += 'W';
            if(c->canNotify()) props += 'n';
            if(c->canIndicate()) props += 'i';
            reply(String("SVC ") + svc->getUUID().toString().c_str() + " " + c->getUUID().toString().c_str() + " " + (props.length() ? props : String("-")));
        }
    }
}

/* Which of the known Govee characteristics this light exposes. */
static void gatherGatt(CensusResult& r) {
    if(!client || !client->isConnected()) return;
    r.gattKnown = true;
    NimBLERemoteService* svc = client->getService(GOVEE_SERVICE);
    r.hasService = svc != nullptr;
    r.hasControl = svc && svc->getCharacteristic(GOVEE_CONTROL);
    r.hasStatus = svc && svc->getCharacteristic(GOVEE_STATUS);
    r.hasBgc = svc && svc->getCharacteristic(GOVEE_BGC);
    r.hasAlt = client->getService(GOVEE_ALT_SERVICE) != nullptr;
    if(r.hasBgc) readBgc(r.bgc);
}

/* -------------------------------------------------------- bridge commands */

static void bridgeWrite(const String& hex, bool withResponse) {
    if(!bridgeSession || !connected()) {
        reply("ERR disconnected");
        return;
    }
    uint8_t frame[20];
    if(hex.length() != 40) {
        reply("ERR bad length");
        return;
    }
    if(!parseHex40(hex, frame)) {
        reply("ERR bad hex");
        return;
    }
    /* Honour the requested procedure when supported; otherwise use the other. */
    if(control->writeValue(frame, 20, controlWithResponse && (withResponse || !controlWithoutResponse))) {
        reply("OK");
    } else {
        reply(client->isConnected() ? "ERR write failed" : "ERR disconnected");
    }
}

static void bridgeRead(const String& args) {
    if(!bridgeSession || !connected()) {
        reply("ERR disconnected");
        return;
    }
    int sp = args.indexOf(' ');
    String hex = sp > 0 ? args.substring(0, sp) : args;
    uint32_t timeout = sp > 0 ? (uint32_t)args.substring(sp + 1).toInt() : READ_TIMEOUT_MS;
    if(timeout == 0) timeout = READ_TIMEOUT_MS;
    uint8_t req[20], rep[20];
    if(!parseHex40(hex, req)) {
        reply("ERR bad hex");
        return;
    }
    const char* err = subscribeStatus();
    if(err) {
        reply(String("ERR ") + err);
        return;
    }
    if(readFrame(req, rep, timeout)) {
        char out[41];
        hex40(rep, out);
        reply(String("RX ") + out);
    } else {
        reply(client->isConnected() ? "ERR timeout" : "ERR disconnected");
    }
}

static void census(bool probe);

static void handleLine(String line) {
    line.trim();
    if(line.length() == 0) return;
    if(line == "PING") {
        reply("PONG govee-bridge " BRIDGE_VERSION);
        return;
    }
    if(busy) {
        reply("ERR busy");
        return;
    }
    if(line.startsWith("SCAN")) {
        uint32_t ms = line.length() > 5 ? line.substring(5).toInt() : SCAN_MS;
        if(ms == 0) ms = SCAN_MS;
        bridgeSession = false;
        if(!startScan(ms, true)) reply("ERR scan start failed");
    } else if(line == "STOP") {
        if(scanning) NimBLEDevice::getScan()->stop();
        reply("OK");
    } else if(line.startsWith("CONN ")) {
        int sp = line.indexOf(' ', 5);
        String mac = sp > 0 ? line.substring(5, sp) : line.substring(5);
        int type = sp > 0 ? line.substring(sp + 1).toInt() : 0;
        const char* err = connectLight(NimBLEAddress(std::string(mac.c_str()), type));
        if(err) {
            ledError();
            reply(String("ERR ") + err);
        } else {
            bridgeSession = true;
            reply("OK");
        }
    } else if(line.startsWith("W ")) {
        bool withResponse = line.endsWith(" R");
        String hex = withResponse ? line.substring(2, line.length() - 2) : line.substring(2);
        hex.trim();
        bridgeWrite(hex, withResponse);
    } else if(line == "N") {
        if(!bridgeSession || !connected()) {
            reply("ERR disconnected");
        } else {
            const char* err = subscribeStatus();
            reply(err ? String("ERR ") + err : String("OK"));
        }
    } else if(line.startsWith("R ")) {
        bridgeRead(line.substring(2));
    } else if(line == "BGC") {
        if(!bridgeSession || !connected()) {
            reply("ERR disconnected");
        } else {
            char hex[41];
            if(readBgc(hex)) reply(String("BGC ") + hex);
            else reply("ERR no bgc");
        }
    } else if(line == "SVC") {
        if(!bridgeSession || !connected()) {
            reply("ERR disconnected");
        } else {
            listServices();
            reply("SVC_DONE");
        }
    } else if(line == "DISC") {
        disconnectLight();
        bridgeSession = false;
        ledIdle();
        reply("OK");
    } else if(line == "CENSUS" || line == "CENSUS PROBE") {
        busy = true;
        censusToBridge = true;
        Serial.println("census (bridge)");
        scanBlocking();
        census(line.endsWith("PROBE") || CENSUS_PROBE_WRITES);
        censusToBridge = false;
        busy = false;
        reply("CENSUS_DONE");
    } else if(line == "DUMP") {
        dumpStoredLog();
        reply("OK");
    } else {
        reply("ERR unknown command");
    }
}

static void serviceOne(Stream& port, String& buf) {
    while(port.available()) {
        char c = (char)port.read();
        if(c == '\n' || c == '\r') {
            if(buf.length()) {
                replyTo = &port;
                handleLine(buf);
            }
            buf = "";
        } else if(buf.length() < 120) {
            buf += c;
        }
    }
}

static void serviceSerial() {
    static String flipperBuf, usbBuf;
    serviceOne(flipperSerial, flipperBuf);
    serviceOne(Serial, usbBuf);
    drainNotifies();
}

/* ------------------------------------------------------- standalone jobs */

/* Reads AA 40 once per light so the whole-strip mask fits the segment count
 * (FF 7F is only right for 15). 0 stays 0 when the light does not answer. */
static void learnSegments(Light& l) {
    if(l.segments || (l.dialect != GoveeRgbic && l.dialect != GoveeDreamcolor)) return;
    uint8_t q[20], a[20];
    goveeReadIcCount(q);
    if(readFrame(q, a, 800) && a[4]) l.segments = a[4];
}

static void musicFrame(uint8_t f[20], uint8_t sub) {
    if(sub == GOVEE_SUBMODE_MUSIC_MODERN) {
        goveeMusicModern(f, GOVEE_MUSIC13_DEFAULT, GOVEE_MUSIC_SENSITIVITY_DEFAULT);
    } else if(sub == GOVEE_SUBMODE_MUSIC_DREAMCOLOR) {
        goveeMusicDreamcolor(f, GOVEE_MUSIC11_ENERGETIC, GOVEE_MUSIC_SENSITIVITY_DEFAULT);
    } else {
        goveeMusic(f, GOVEE_MUSIC_ENERGIC, 255, 0, 255);
    }
}

/* The music sub-mode byte is per family (SPEC 8.2): 0x13 modern RGBIC, 0x11
 * dreamcolor, 0x01 Telink-era. Try the family's best guess first and stop at
 * the first one AA 05 01 echoes; if reads fail, write all three, least
 * likely first. */
static void partyMusic(const Light& l) {
    uint8_t cands[3];
    if(l.dialect == GoveeRgbic) {
        cands[0] = GOVEE_SUBMODE_MUSIC_MODERN; cands[1] = GOVEE_SUBMODE_MUSIC_DREAMCOLOR; cands[2] = GOVEE_SUBMODE_MUSIC_LEGACY;
    } else if(l.dialect == GoveeDreamcolor) {
        cands[0] = GOVEE_SUBMODE_MUSIC_DREAMCOLOR; cands[1] = GOVEE_SUBMODE_MUSIC_MODERN; cands[2] = GOVEE_SUBMODE_MUSIC_LEGACY;
    } else {
        cands[0] = GOVEE_SUBMODE_MUSIC_LEGACY; cands[1] = GOVEE_SUBMODE_MUSIC_MODERN; cands[2] = GOVEE_SUBMODE_MUSIC_DREAMCOLOR;
    }
    bool canRead = subscribeStatus() == nullptr;
    uint8_t f[20], q[20], a[20];
    int first = canRead ? 0 : 2, step = canRead ? 1 : -1;
    for(int i = first; i >= 0 && i < 3; i += step) {
        musicFrame(f, cands[i]);
        send(f);
        if(!canRead) continue;
        goveeReadMode(q);
        if(readFrame(q, a, 1000) && a[2] == cands[i]) {
            Serial.printf("%s: music sub-mode 0x%02x\n", l.name, cands[i]);
            return;
        }
    }
}

static void party(Light& l) {
    uint8_t f[20];
    goveePower(f, true);
    send(f);
    goveeBrightness(f, 100, l.dialect);
    send(f);
    learnSegments(l);
    static const uint8_t pops[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 0, 255}};
    for(auto& c : pops) {
        goveeRgbSeg(f, c[0], c[1], c[2], l.dialect, l.segments);
        led(c[0] / 4, c[1] / 4, c[2] / 4);
        send(f);
        delay(90);
    }
    goveeScene(f, GOVEE_SCENE_BLINKING); /* blinking, as a fallback */
    send(f);
    partyMusic(l); /* the light's mic takes over */
}

static void bgone(Light& l) {
    (void)l;
    uint8_t f[20];
    goveePower(f, false);
    send(f);
    send(f);
}

static void sweep(void (*visit)(Light&)) {
    size_t hits = 0;
    for(size_t i = 0; i < lightCount; i++) {
        serviceSerial();
        if(lights[i].adv.encrypted) {
            Serial.printf("%s: encrypted link, skipped\n", lights[i].name);
            continue;
        }
        if(connectLight(lights[i].addr)) {
            ledError();
            continue;
        }
        visit(lights[i]);
        disconnectLight();
        hits++;
        delay(150);
    }
    Serial.printf("sweep done: %u/%u\n", (unsigned)hits, (unsigned)lightCount);
    ledIdle();
}

/* Rave: hop between lights, streaming a rainbow/strobe/candy/aurora show to each for a
 * few seconds. Any button press ends it. */
static void rave() {
    uint16_t hue = 0;
    uint8_t r, g, b, f[20];
    size_t idx = 0;
    while(raving) {
        serviceSerial();
        if(lightCount == 0) {
            scanBlocking();
            if(lightCount == 0) {
                for(int i = 0; i < 20 && raving; i++) {
                    delay(100);
                    if(buttonDown()) raving = false;
                }
                continue;
            }
        }
        Light& l = lights[idx % lightCount];
        idx++;
        if(l.adv.encrypted || connectLight(l.addr)) {
            ledError();
            for(int i = 0; i < 20 && raving; i++) { /* keep the button live */
                delay(50);
                if(buttonDown()) {
                    raving = false;
                    while(buttonDown()) delay(10);
                }
            }
            continue;
        }
        goveePower(f, true);
        send(f);
        goveeBrightness(f, 100, l.dialect);
        send(f);
        learnSegments(l);
        unsigned long until = millis() + 6000;
        uint32_t tick = 0;
        while(millis() < until && raving) {
            /* Carry the show across visits so each light gets a different opening. */
            switch(((idx - 1) + tick / 40) % 4) {
            case 0:
                hsvToRgb(hue, 255, 255, r, g, b);
                hue = (hue + 14) % 360;
                break;
            case 1:
                r = g = b = (tick & 1) ? 255 : 1;
                break;
            case 2: /* candy */
                switch((tick / 4) % 3) {
                case 0: r = 255; g = 20; b = 147; break;
                case 1: r = 0; g = 255; b = 255; break;
                default: r = 255; g = 220; b = 0; break;
                }
                break;
            default: /* aurora */
                hsvToRgb(120 + tick % 120, 220,
                    80 + (tick % 32 < 16 ? tick % 32 : 31 - tick % 32) * 11, r, g, b);
                break;
            }
            goveeRgbSeg(f, r, g, b, l.dialect, l.segments);
            send(f, false);
            led(r / 3, g / 3, b / 3);
            delay(40);
            tick++;
            serviceSerial();
            if(buttonDown()) {
                raving = false;
                while(buttonDown()) delay(10);
            }
        }
        disconnectLight();
    }
    ledIdle();
}

/* -------------------------------------------------------------- census */

static volatile bool censusAbort = false;

static bool censusRead(const uint8_t q[20], uint8_t a[20], CensusResult& r) {
    if(censusAbort) {
        r.aborted = true;
        return false;
    }
    r.reads++;
    if(!readFrame(q, a, READ_TIMEOUT_MS)) return false;
    r.replies++;
    return true;
}

static void censusWrite(const uint8_t f[20], const char* note) {
    logFrame("tx", f, note, logAddress);
    send(f);
    delay(150);
}

/* Probe writes: the four colour dialects, each with its own colour, each
 * judged only by its own colour appearing where it was not before. Mirrors
 * govee_census.c in the Flipper app. */
static void censusProbe(CensusResult& r) {
    uint8_t q[20], a[20], f[20];
    r.probed = true;
    uint8_t segments = (r.icKnown && r.segments) ? r.segments : 0;
    if(r.powerKnown && !r.powerOn) {
        goveePower(f, true);
        censusWrite(f, "probe");
    }
    uint8_t page[20];
    bool havePage = r.segmentPage;
    if(havePage) memcpy(page, r.pageBefore, 20);
    uint8_t lastModeRgb[3] = {0, 0, 0};
    bool haveModeRgb = r.submodeKnown && goveeSubmodeIsColor(r.submode);
    if(haveModeRgb) memcpy(lastModeRgb, r.modeBody, 3);

    static const GoveeDialect order[] = {GoveeLegacy, GoveeDreamcolor, GoveeRgbww, GoveeRgbic};
    for(GoveeDialect d : order) {
        if(censusAbort) break;
        uint8_t want = goveeDialectSubmode(d), rgb[3];
        goveeProbeColor(d, rgb);
        bool stalePage = havePage && goveeReplySegmentPageHas(page, rgb[0], rgb[1], rgb[2]);
        bool staleMode = haveModeRgb && memcmp(lastModeRgb, rgb, 3) == 0;
        goveeRgbSeg(f, rgb[0], rgb[1], rgb[2], d, segments);
        censusWrite(f, "probe");
        bool echoed = false;
        goveeReadMode(q);
        if(censusRead(q, a, r) && a[2] == want) {
            if((d == GoveeLegacy || d == GoveeRgbww) && !staleMode) {
                echoed = a[3] == rgb[0] && a[4] == rgb[1] && a[5] == rgb[2];
            }
            haveModeRgb = goveeSubmodeIsColor(a[2]);
            memcpy(lastModeRgb, a + 3, 3);
        }
        goveeReadSegmentColors(q, 1);
        if(censusRead(q, a, r)) {
            if(goveeReplySegmentPageHas(a, rgb[0], rgb[1], rgb[2]) && !stalePage) echoed = true;
            memcpy(page, a, 20);
            havePage = true;
        }
        if(echoed) r.echoed |= (uint8_t)(1u << d);
        /* Back to the starting 02 / 0D state between dialects (same as govee_census.c). */
        bool restorable = r.submodeKnown && (r.submode == GOVEE_SUBMODE_COLOR_LEGACY || r.submode == GOVEE_SUBMODE_COLOR_RGBWW);
        if(d != order[3] && restorable) {
            uint8_t body[9];
            body[0] = r.submode;
            memcpy(body + 1, r.modeBody, 8);
            goveeBuild(f, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, body, sizeof(body));
            censusWrite(f, "restore");
            delay(600);
            memcpy(lastModeRgb, r.modeBody, 3);
            haveModeRgb = true;
        }
    }

    /* Restore: the pre-probe 02 / 0D body written back verbatim (the reply
     * mirrors the write layout; an RGB write would turn a 0D white, reported
     * as RGB 00 00 00 + kelvin on the H6006, into black). */
    if(r.submodeKnown && (r.submode == GOVEE_SUBMODE_COLOR_LEGACY || r.submode == GOVEE_SUBMODE_COLOR_RGBWW)) {
        GoveeDialect od = r.submode == GOVEE_SUBMODE_COLOR_LEGACY ? GoveeLegacy : GoveeRgbww;
        uint8_t body[9];
        body[0] = r.submode;
        memcpy(body + 1, r.modeBody, 8);
        goveeBuild(f, GOVEE_TYPE_WRITE, GOVEE_CMD_MODE, body, sizeof(body));
        censusWrite(f, "restore");
        r.restored = RestoredFull;
        r.restoreNote = (r.echoed & (1u << od)) ? nullptr : "colour rewritten in the reported dialect, which did not echo";
    } else if(r.submodeKnown && goveeSubmodeIsColor(r.submode)) {
        r.restored = RestoredPower;
        r.restoreNote = "pre-probe colour not readable from the mode reply (0B/15); left on the last probe colour";
    } else if(r.submodeKnown) {
        r.restored = RestoredPower;
        r.restoreNote = "pre-probe mode was not a static colour; not rewritten";
    } else {
        r.restored = RestoredPower;
        r.restoreNote = "pre-probe mode unknown; left on the last probe colour";
    }
    if(r.powerKnown) {
        goveePower(f, r.powerOn);
        censusWrite(f, "restore");
    } else {
        r.restored = RestoredNone;
        r.restoreNote = "pre-probe power unknown; nothing restored";
    }
}

/* Same battery and order as govee_census.c in the Flipper app. */
static void censusRun(bool probe, CensusResult& r) {
    uint8_t q[20], a[20];
    uint8_t coreTimeouts = 0;
    goveeReadPower(q);
    if(censusRead(q, a, r)) { r.powerKnown = true; r.powerOn = a[2] != 0; } else coreTimeouts++;
    goveeReadBrightness(q);
    if(censusRead(q, a, r)) { r.brightnessKnown = true; r.brightness = a[2]; } else coreTimeouts++;
    goveeReadFirmware(q);
    if(censusRead(q, a, r)) goveeReplyVersionString(a, r.firmware, sizeof(r.firmware)); else coreTimeouts++;
    if(coreTimeouts >= CENSUS_SILENT_AFTER && r.replies == 0) {
        r.silent = !r.aborted; /* locked, encrypted or foreign dialect: stop early */
        return;
    }
    goveeReadMode(q);
    if(censusRead(q, a, r)) { r.submodeKnown = true; r.submode = a[2]; memcpy(r.modeBody, a + 3, 8); }
    goveeReadHardware(q);
    if(censusRead(q, a, r)) goveeReplyVersionString(a, r.hardware, sizeof(r.hardware));
    goveeReadIcCount(q);
    if(censusRead(q, a, r)) { r.icKnown = true; r.icCount = (uint16_t)((a[2] << 8) | a[3]); r.segments = a[4]; }
    goveeReadLightCount(q);
    if(censusRead(q, a, r)) { r.lightCountKnown = true; r.lightCountValue = a[2]; }
    goveeReadSecret(q);
    if(censusRead(q, a, r) && a[2] == 0x01) { r.secretPresent = true; memcpy(r.secret, a + 3, 8); }
    goveeReadSegmentColors(q, 1);
    if(censusRead(q, a, r)) { r.segmentPage = true; memcpy(r.pageBefore, a, 20); }
    r.silent = r.replies == 0 && !r.aborted;
    if(probe && !r.silent && !r.aborted) censusProbe(r);
}

static void jsonStr(String& s, const char* key, const char* v) {
    if(s.length() > 1) s += ',';
    s += '"'; s += key; s += "\":\"";
    for(const char* p = v; *p; p++) {
        if((unsigned char)*p < 0x20) continue;
        if(*p == '"' || *p == '\\') s += '\\';
        s += *p;
    }
    s += '"';
}
static void jsonRaw(String& s, const char* key, const String& v) {
    if(s.length() > 1) s += ',';
    s += '"'; s += key; s += "\":"; s += v;
}
static void jsonBool(String& s, const char* key, bool v) { jsonRaw(s, key, v ? "true" : "false"); }

/* Same keys as govee_census_summary_json() in the Flipper app (govee-evidence/1). */
static String censusSummary(const Light& l, const CensusResult& r, const char* addr, bool printSecret) {
    String s = "{";
    char at[24];
    uptimeIso(at);
    jsonStr(s, "summary", "light");
    jsonStr(s, "schema", EVIDENCE_SCHEMA);
    jsonStr(s, "source", "esp32");
    jsonStr(s, "at", at);
    jsonStr(s, "software", "govee-party-esp32 " SKETCH_VERSION " bridge " BRIDGE_VERSION);
    jsonStr(s, "address", addr);
    jsonStr(s, "name", l.name);
    jsonStr(s, "model", l.model);
    jsonBool(s, "known_model", goveeModelKnown(l.model));
    jsonRaw(s, "rssi", String(l.rssi));
    if(l.adv.valid) {
        jsonStr(s, "pact", (String(l.adv.pactType) + "/" + String(l.adv.pactCode)).c_str());
        jsonRaw(s, "flags", String(l.adv.flags));
        jsonBool(s, "encrypted", l.adv.encrypted);
    } else {
        jsonRaw(s, "pact", "null");
        jsonRaw(s, "flags", "null");
        jsonRaw(s, "encrypted", "null");
    }
    jsonStr(s, "dialect", goveeDialectName(l.dialect));
    jsonStr(s, "dialect_source", goveeDialectSource(l.model, l.adv));
    jsonBool(s, "secret_family", goveeNeedsSecret(l.model));
    jsonBool(s, "connected", r.connected);
    jsonBool(s, "subscribed", r.subscribed);
    jsonBool(s, "silent", r.connected && r.silent);
    if(r.aborted) jsonBool(s, "aborted", true);
    if(r.gattKnown) {
        String g;
        if(r.hasService) g += "1910";
        if(r.hasControl) g += "/2b11";
        if(r.hasStatus) g += "/2b10";
        if(r.hasBgc) g += "/2b12";
        if(r.hasAlt) g += (g.length() ? "+" : "") + String("ffe0");
        jsonStr(s, "gatt", g.length() ? g.c_str() : "none");
        if(r.bgc[0]) jsonStr(s, "bgc", r.bgc);
    }
    jsonRaw(s, "reads", String(r.reads));
    jsonRaw(s, "replies", String(r.replies));
    if(r.badFrames) jsonRaw(s, "bad_frames", String(r.badFrames));
    if(r.acks) jsonRaw(s, "acks", String(r.acks));
    if(r.oddNotifies) jsonRaw(s, "odd_notifies", String(r.oddNotifies));
    if(r.powerKnown) jsonBool(s, "power", r.powerOn);
    if(r.brightnessKnown) jsonRaw(s, "brightness", String(r.brightness));
    if(r.submodeKnown) {
        char sm[24];
        snprintf(sm, sizeof(sm), "%02X %02X%02X%02X%02X", r.submode, r.modeBody[0], r.modeBody[1], r.modeBody[2], r.modeBody[3]);
        jsonStr(s, "submode", sm);
        jsonStr(s, "submode_name", goveeSubmodeName(r.submode));
    }
    if(r.firmware[0]) jsonStr(s, "firmware", r.firmware);
    if(r.hardware[0]) jsonStr(s, "hardware", r.hardware);
    if(r.icKnown) {
        jsonRaw(s, "ic_count", String(r.icCount));
        jsonRaw(s, "segments", String(r.segments));
    }
    if(r.lightCountKnown) jsonRaw(s, "light_count", String(r.lightCountValue));
    jsonBool(s, "secret_present", r.secretPresent);
    if(r.secretPresent && printSecret) {
        char hex[17];
        for(int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", r.secret[i]);
        jsonStr(s, "secret", hex);
    }
    jsonBool(s, "segment_page", r.segmentPage);
    jsonBool(s, "probed", r.probed);
    if(r.probed) {
        String e;
        for(int d = 0; d < GoveeDialectCount; d++) {
            if(!(r.echoed & (1u << d))) continue;
            char b[4];
            snprintf(b, sizeof(b), "%s%02X", e.length() ? "," : "", goveeDialectSubmode((GoveeDialect)d));
            e += b;
        }
        jsonStr(s, "echoed", e.c_str());
        jsonStr(s, "restored", r.restored == RestoredFull ? "full" : r.restored == RestoredPower ? "power" : "none");
        if(r.restoreNote) jsonStr(s, "restore_note", r.restoreNote);
    }
    s += '}';
    return s;
}

static void census(bool probe) {
    led(60, 40, 0);
    logOpen();
    logging = true;
    censusAbort = false;
    {
        String head = String("{\"summary\":\"session\",\"schema\":\"" EVIDENCE_SCHEMA "\",\"source\":\"esp32\",\"software\":\"govee-party-esp32 " SKETCH_VERSION " bridge " BRIDGE_VERSION "\",\"board\":\"" ARDUINO_BOARD "\",\"lights\":") +
                      String(lightCount) + ",\"probe\":" + (probe ? "true" : "false") + "}";
        logLine(head.c_str());
    }
    for(size_t i = 0; i < lightCount && !censusAbort; i++) {
        Light& l = lights[i];
        std::string addrStr = l.addr.toString();
        logAddress = addrStr.c_str();
        CensusResult r;
        logResult = &r;
        Serial.printf("census %u/%u %s\n", (unsigned)i + 1, (unsigned)lightCount, l.name);
        if(connectLight(l.addr) == nullptr) {
            r.connected = true;
            gatherGatt(r);
            if(l.adv.encrypted) {
                /* AES-wrapped link: plaintext frames are useless. The GATT
                 * table and the 2b12 bytes are the evidence; no frames sent. */
            } else if(subscribeStatus() == nullptr) {
                r.subscribed = true;
                censusRun(probe, r);
                if(r.icKnown && r.segments) l.segments = r.segments;
            }
            r.oddNotifies = oddNotifyCount;
            disconnectLight();
        } else {
            ledError();
            led(60, 40, 0);
        }
        logResult = nullptr;
        String summary = censusSummary(l, r, addrStr.c_str(), CENSUS_PRINT_SECRET);
        logLine(summary.c_str());
        if(censusToBridge) reply("CENSUS " + summary);
        logAddress = "";
        /* A button press between lights stops the census; PING still answers. */
        serviceSerial();
        if(buttonDown()) {
            censusAbort = true;
            while(buttonDown()) delay(10);
        }
        delay(150);
    }
    logging = false;
    logClose();
    Serial.printf("census done: %u light(s)%s\n", (unsigned)lightCount, censusAbort ? " (stopped)" : "");
    ledIdle();
}

/* -------------------------------------------------------------- button */

static Press readButton() {
    if(!buttonDown()) return PressNone;
    unsigned long t0 = millis();
    while(buttonDown()) {
        if(millis() - t0 > LONG_PRESS_MS) {
            led(60, 20, 0);
            while(buttonDown()) delay(10);
            return PressLong;
        }
        delay(10);
    }
    int clicks = 1;
    while(clicks < 3) {
        unsigned long released = millis();
        bool again = false;
        while(millis() - released < DOUBLE_PRESS_MS) {
            if(buttonDown()) {
                while(buttonDown()) delay(10);
                again = true;
                break;
            }
            delay(10);
        }
        if(!again) break;
        clicks++;
    }
    return clicks >= 3 ? PressTriple : clicks == 2 ? PressDouble : PressShort;
}

static void lurkCycle() {
    scanBlocking();
    if(lightCount) sweep(bgone);
    for(int i = 0; i < 100; i++) { /* breathe purple for ten seconds */
        uint8_t v = (uint8_t)(i < 50 ? i : 100 - i);
        led(v / 2, 0, v);
        delay(100);
        serviceSerial();
        if(buttonDown()) { /* a press leaves lurk mode */
            while(buttonDown()) delay(10);
            lurking = false;
            ledIdle();
            return;
        }
    }
}

/* ---------------------------------------------------------------- main */

void setup() {
    Serial.begin(115200);
    flipperSerial.begin(BRIDGE_BAUD, SERIAL_8N1, BRIDGE_RX_PIN, BRIDGE_TX_PIN);
    // Classic ESP32 GPIO34-39 have no internal pull-up; the M5Stack
    // buttons on these pins use the board's external pull-up instead.
#if CONFIG_IDF_TARGET_ESP32 && BUTTON_PIN >= 34 && BUTTON_PIN <= 39
    pinMode(BUTTON_PIN, INPUT);
#else
    pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif
    if(!LittleFS.begin(true)) Serial.println("LittleFS unavailable: census log will not be stored");
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    delay(50);
    lurking = buttonDown();
    while(buttonDown()) delay(10);
    ledIdle();
    replyTo = &flipperSerial;
    reply("READY govee-bridge " BRIDGE_VERSION);
    Serial.println(lurking ? "govee party: LURK mode" : "govee party: ready");
#if HOST_DETECT
    if(hostAttached()) dumpStoredLog();
#endif
}

void loop() {
    serviceSerial();

#if HOST_DETECT
    /* A USB host just arrived: hand over whatever was buffered. */
    static bool hostWas = false;
    bool hostNow = hostAttached();
    if(hostNow && !hostWas && !busy) dumpStoredLog();
    hostWas = hostNow;
#endif

    if(lurking) {
        busy = true;
        lurkCycle();
        busy = false;
        return;
    }

    /* While the Flipper is scanning or holds a connection, the button only
     * flashes: the Flipper is in charge. */
    if(bridgeSession || (scanning && bridgeScan)) {
        if(buttonDown()) {
            ledError();
            while(buttonDown()) delay(10);
        }
        delay(5);
        return;
    }

    Press p = readButton();
    if(p == PressNone) {
        delay(10);
        return;
    }
    busy = true;
    switch(p) {
    case PressShort:
        Serial.println("dance party");
        scanBlocking();
        sweep(party);
        break;
    case PressDouble:
        Serial.println("rave");
        raving = true;
        rave();
        break;
    case PressTriple:
        Serial.println("census");
        scanBlocking();
        census(CENSUS_PROBE_WRITES);
        break;
    case PressLong:
        Serial.println("b-gone");
        scanBlocking();
        sweep(bgone);
        break;
    default:
        break;
    }
    busy = false;
}
