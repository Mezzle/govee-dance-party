/*
 * Govee Party for ESP32: one image, two jobs.
 *
 *  1. BRIDGE for the Flipper Zero app. Plug the Flipper into the Grove port /
 *     UART and the Flipper does the driving over a tiny line protocol:
 *
 *       > PING                        < PONG govee-bridge 2
 *       > SCAN <ms>                   < ADV <mac> <addrtype> <rssi> <name> ...
 *                                     < SCAN_DONE
 *       > STOP                        (abort a scan)
 *       > CONN <mac> <addrtype>       < OK | ERR <reason>
 *       > W <40 hex chars> [R]        < OK | ERR <reason>  (R = write with response)
 *       > DISC                        < OK
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
 *       long press    Govee B-Gone: every Govee light in range -> off.
 *       hold at boot  Lurk: keep scanning and switching lights off, forever.
 *
 *  LED: blue idle, cyan scanning, green connected, red flash on error,
 *       purple breathing while lurking, colour-synced during rave.
 *       While a button job runs the Flipper gets "ERR busy" (PING still works).
 *
 *  Boards: M5Stack ATOM Lite / ATOMS3 Lite are the intended targets (pins
 *  below). Any ESP32 with Bluetooth works for the bridge; the official Flipper
 *  WiFi devboard is an ESP32-S2 and has no Bluetooth. Library: NimBLE-Arduino 2.x.
 */

#include <NimBLEDevice.h>
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
#define BRIDGE_VERSION "2"
#define SCAN_MS 5000
#define CONNECT_TIMEOUT_MS 6000
#define MAX_LIGHTS 16
#define LONG_PRESS_MS 1200
#define DOUBLE_PRESS_MS 350

static const NimBLEUUID GOVEE_SERVICE("00010203-0405-0607-0809-0a0b0c0d1910");
static const NimBLEUUID GOVEE_CONTROL("00010203-0405-0607-0809-0a0b0c0d2b11");

enum Press { PressNone, PressShort, PressDouble, PressLong };

struct Light {
    NimBLEAddress addr;
    char model[6];
    GoveeDialect dialect;
    int rssi;
};

HardwareSerial flipperSerial(1);
static Light lights[MAX_LIGHTS];
static size_t lightCount = 0;
static NimBLEClient* client = nullptr;
static NimBLERemoteCharacteristic* control = nullptr;
static volatile bool scanning = false;
static bool bridgeScan = false; /* scan was requested by the Flipper: stream ADV lines */
static bool bridgeSession = false; /* Flipper owns the connection */
static bool busy = false; /* a button job is running */
static bool lurking = false;
static bool raving = false;

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
    flipperSerial.print(s);
    flipperSerial.print('\n');
}

static bool buttonDown() {
    return digitalRead(BUTTON_PIN) == LOW;
}

/* ---------------------------------------------------------------- scan */

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if(!dev->haveName()) return;
        char model[6];
        if(!goveeParseName(dev->getName().c_str(), model)) return;
        if(bridgeScan) {
            String line = "ADV ";
            line += dev->getAddress().toString().c_str();
            line += ' ';
            line += (int)dev->getAddress().getType();
            line += ' ';
            line += dev->getRSSI();
            line += ' ';
            line += dev->getName().c_str();
            reply(line);
        }
        for(size_t i = 0; i < lightCount; i++) {
            if(lights[i].addr == dev->getAddress()) {
                lights[i].rssi = dev->getRSSI();
                return;
            }
        }
        if(lightCount >= MAX_LIGHTS) return;
        Light& l = lights[lightCount++];
        l.addr = dev->getAddress();
        memcpy(l.model, model, 6);
        l.dialect = goveeDialect(model);
        l.rssi = dev->getRSSI();
        Serial.printf("found %s %s rssi %d\n", dev->getName().c_str(), l.addr.toString().c_str(), l.rssi);
        led(0, 60, 60);
    }
    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        scanning = false;
        if(bridgeScan) reply("SCAN_DONE");
        bridgeScan = false;
        ledIdle();
    }
} scanCallbacks;

static bool startScan(uint32_t ms, bool forBridge) {
    if(client && client->isConnected()) client->disconnect();
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

static const char* connectLight(const NimBLEAddress& addr) {
    if(!client) {
        client = NimBLEDevice::createClient();
        client->setConnectTimeout(CONNECT_TIMEOUT_MS);
        client->setConnectionParams(12, 24, 0, 200);
    }
    if(client->isConnected()) client->disconnect();
    if(!client->connect(addr, false, false)) return "connect failed";
    NimBLERemoteService* svc = client->getService(GOVEE_SERVICE);
    control = svc ? svc->getCharacteristic(GOVEE_CONTROL) : nullptr;
    if(!control) {
        client->disconnect();
        return "no govee service";
    }
    led(0, 40, 0);
    return nullptr;
}

static void disconnectLight() {
    if(client && client->isConnected()) client->disconnect();
    control = nullptr;
}

static bool send(const uint8_t frame[20], bool withResponse = true) {
    if(!control || !client->isConnected()) return false;
    bool ok = control->writeValue(frame, 20, withResponse);
    delay(60);
    return ok;
}

/* -------------------------------------------------------- bridge commands */

static int hexval(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void bridgeWrite(const String& hex, bool withResponse) {
    if(!bridgeSession || !client || !client->isConnected() || !control) {
        reply("ERR disconnected");
        return;
    }
    uint8_t frame[20];
    if(hex.length() != 40) {
        reply("ERR bad length");
        return;
    }
    for(int i = 0; i < 20; i++) {
        int hi = hexval(hex[i * 2]), lo = hexval(hex[i * 2 + 1]);
        if(hi < 0 || lo < 0) {
            reply("ERR bad hex");
            return;
        }
        frame[i] = (uint8_t)((hi << 4) | lo);
    }
    if(control->writeValue(frame, 20, withResponse)) {
        reply("OK");
    } else {
        reply(client->isConnected() ? "ERR write failed" : "ERR disconnected");
    }
}

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
    } else if(line == "DISC") {
        disconnectLight();
        bridgeSession = false;
        ledIdle();
        reply("OK");
    } else {
        reply("ERR unknown command");
    }
}

static void serviceSerial() {
    static String buf;
    while(flipperSerial.available()) {
        char c = (char)flipperSerial.read();
        if(c == '\n' || c == '\r') {
            if(buf.length()) handleLine(buf);
            buf = "";
        } else if(buf.length() < 120) {
            buf += c;
        }
    }
}

/* ------------------------------------------------------- standalone jobs */

static void party(const Light& l) {
    uint8_t f[20];
    goveePower(f, true);
    send(f);
    goveeBrightness(f, 100, l.dialect);
    send(f);
    static const uint8_t pops[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 0, 255}};
    for(auto& c : pops) {
        goveeRgb(f, c[0], c[1], c[2], l.dialect);
        led(c[0] / 4, c[1] / 4, c[2] / 4);
        send(f);
        delay(90);
    }
    goveeScene(f, 0x08); /* blinking, as a fallback */
    send(f);
    goveeMusic(f, 0x00, 255, 0, 255); /* energic: the light's mic takes over */
    send(f);
}

static void bgone(const Light& l) {
    (void)l;
    uint8_t f[20];
    goveePower(f, false);
    send(f);
    send(f);
}

static void sweep(void (*visit)(const Light&)) {
    size_t hits = 0;
    for(size_t i = 0; i < lightCount; i++) {
        serviceSerial();
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

/* Rave: hop between lights, streaming a rainbow/strobe show to each for a
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
        const Light& l = lights[idx % lightCount];
        idx++;
        if(connectLight(l.addr)) {
            ledError();
            continue;
        }
        goveePower(f, true);
        send(f);
        goveeBrightness(f, 100, l.dialect);
        send(f);
        unsigned long until = millis() + 6000;
        uint32_t tick = 0;
        while(millis() < until && raving) {
            if((tick / 40) & 1) { /* alternate: 40 frames rainbow, 40 frames strobe */
                hsvToRgb(hue, 255, 255, r, g, b);
                hue = (hue + 14) % 360;
            } else {
                r = g = b = (tick & 1) ? 255 : 1;
            }
            goveeRgb(f, r, g, b, l.dialect);
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
    unsigned long released = millis();
    while(millis() - released < DOUBLE_PRESS_MS) {
        if(buttonDown()) {
            while(buttonDown()) delay(10);
            return PressDouble;
        }
        delay(10);
    }
    return PressShort;
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
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    delay(50);
    lurking = buttonDown();
    while(buttonDown()) delay(10);
    ledIdle();
    reply("READY govee-bridge " BRIDGE_VERSION);
    Serial.println(lurking ? "govee party: LURK mode" : "govee party: ready");
}

void loop() {
    serviceSerial();

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
