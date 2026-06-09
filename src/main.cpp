#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <TM1637Display.h>
#include <SPI.h>
#include "DucoCC1101.h"
#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
// [DUCO] RF state
// ─────────────────────────────────────────────────────────────────────────────
static DucoCC1101    duco;
static volatile bool packetReady   = false;
static uint8_t       networkId[4]  = DUCO_NETWORK_ID;

static unsigned long joinScheduledAt = 0;  // millis() when countdown started, 0 = none
static bool          joinSent        = false;
static unsigned long joinLastSentAt  = 0;  // millis() of most recent sendJoinPacket()
static bool          joinSucceeded   = false;

IRAM_ATTR void onCC1101Interrupt() { packetReady = true; }

// ─────────────────────────────────────────────────────────────────────────────
// [POWER] meter state
// ─────────────────────────────────────────────────────────────────────────────
static float    g_watts       = 0.0f;
static float    g_total_kwh   = 0.0f;
static uint32_t g_lastPulseMs = 0;
static bool     g_isFlashing  = false;

// ─────────────────────────────────────────────────────────────────────────────
// [DISPLAY] TM1637 state
// ─────────────────────────────────────────────────────────────────────────────
static TM1637Display tm1637(PIN_TM1637_CLK, PIN_TM1637_DIO);
static bool g_displayOn = true;

// ─────────────────────────────────────────────────────────────────────────────
// [MQTT] / WiFi
// ─────────────────────────────────────────────────────────────────────────────
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

// ─────────────────────────────────────────────────────────────────────────────
// Millis-based timers (all in loop())
// ─────────────────────────────────────────────────────────────────────────────
static unsigned long tMqttRetry    = 0;
static unsigned long tSlowTick     = 0;
static unsigned long tAdcPoll      = 0;
static unsigned long tWattsPublish = 0;
static unsigned long tKwhPublish   = 0;
static unsigned long tPowerLog     = 0;
static unsigned long tPulseLog     = 0;
static unsigned long tDisplay      = 0;

// forward declaration (defined after pollADC)
static void updateDisplay();

// ─────────────────────────────────────────────────────────────────────────────
// Duco helpers
// ─────────────────────────────────────────────────────────────────────────────
static void drainDucoLog() {
    uint8_t n = duco.getNumberOfLogMessages();
    for (uint8_t i = 0; i < n; i++)
        Serial.printf("[LIB] %u - %s\n", i, duco.logMessages[i]);
}

static const char* rawModeToStr(uint8_t raw, bool permanent) {
    if (!permanent) {
        switch (raw) {
            case 0: return "auto";
            case 4: return "low";
            case 5: return "medium";
            case 6: return "high";
            case 7: return "nothome";
        }
    } else {
        switch (raw) {
            case 4: return "low_perm";
            case 5: return "medium_perm";
            case 6: return "high_perm";
        }
    }
    return "unknown";
}

static void publishMode() {
    const char* s = rawModeToStr(duco.getCurrentVentilationMode(),
                                 duco.getCurrentPermanentMode());
    mqtt.publish("duco/mode", s, /*retain=*/true);
    Serial.printf("[DUCO] mode published: %s\n", s);
}

// ─────────────────────────────────────────────────────────────────────────────
// HA MQTT auto-discovery
// ─────────────────────────────────────────────────────────────────────────────
static void publishDiscovery() {
    // Shared device block (short HA keys)
    static const char DEV[] =
        "\"dev\":{\"ids\":[\"duco-gateway\"],"
        "\"name\":\"Duco Gateway\","
        "\"mdl\":\"D1 Mini + CC1101\","
        "\"mf\":\"DIY\"}";

    char buf[512];

    // Ventilation mode — select entity
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Duco Ventilation Mode\","
        "\"uniq_id\":\"dg_mode\","
        "\"cmd_t\":\"duco/set\","
        "\"stat_t\":\"duco/mode\","
        "\"options\":[\"auto\",\"low\",\"medium\",\"high\"],"
        "\"avty_t\":\"duco/status\","
        "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
        "%s}", DEV);
    if (!mqtt.publish("homeassistant/select/duco_gateway/mode/config", buf, true))
        Serial.println("[MQTT] WARN: select discovery truncated — increase buffer");

    // Gateway online/offline — binary_sensor
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Duco Status\","
        "\"uniq_id\":\"dg_conn\","
        "\"stat_t\":\"duco/status\","
        "\"pl_on\":\"online\",\"pl_off\":\"offline\","
        "\"dev_cla\":\"connectivity\","
        "%s}", DEV);
    mqtt.publish("homeassistant/binary_sensor/duco_gateway/status/config", buf, true);

    // Instantaneous power — sensor
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Power Usage\","
        "\"uniq_id\":\"dg_power\","
        "\"stat_t\":\"power/watts\","
        "\"unit_of_meas\":\"W\","
        "\"dev_cla\":\"power\","
        "\"stat_cla\":\"measurement\","
        "\"avty_t\":\"duco/status\","
        "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
        "%s}", DEV);
    mqtt.publish("homeassistant/sensor/duco_gateway/power/config", buf, true);

    // Cumulative energy — sensor
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Total Energy\","
        "\"uniq_id\":\"dg_energy\","
        "\"stat_t\":\"power/total_kwh\","
        "\"unit_of_meas\":\"kWh\","
        "\"dev_cla\":\"energy\","
        "\"stat_cla\":\"total_increasing\","
        "\"avty_t\":\"duco/status\","
        "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
        "%s}", DEV);
    mqtt.publish("homeassistant/sensor/duco_gateway/energy/config", buf, true);

    // Display power switch
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Display Power\","
        "\"uniq_id\":\"dg_disp\","
        "\"cmd_t\":\"display/set\","
        "\"stat_t\":\"display/state\","
        "\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
        "\"ic\":\"mdi:monitor-shimmer\","
        "%s}", DEV);
    mqtt.publish("homeassistant/switch/duco_gateway/display/config", buf, true);

    Serial.println("[MQTT] HA discovery published (5 entities)");
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT callback
// ─────────────────────────────────────────────────────────────────────────────
static void mqttCallback(char* topic, byte* payload, unsigned int len) {
    char buf[16] = {};
    memcpy(buf, payload, min(len, (unsigned int)(sizeof(buf) - 1)));

    if (strcmp(topic, "duco/set") == 0) {
        uint8_t mode = 0xFF;
        if      (strcasecmp(buf, "auto")   == 0) mode = 0x00;
        else if (strcasecmp(buf, "low")    == 0) mode = 0x04;
        else if (strcasecmp(buf, "medium") == 0) mode = 0x05;
        else if (strcasecmp(buf, "high")   == 0) mode = 0x06;

        if (mode != 0xFF) {
            Serial.printf("[MQTT] set mode -> %s\n", buf);
            duco.requestVentilationMode(mode, /*permanent=*/false, /*pct=*/0, /*btn=*/1);
        } else {
            Serial.printf("[MQTT] unknown mode '%s'\n", buf);
        }

    } else if (strcmp(topic, "duco/join") == 0) {
        if (strcasecmp(buf, "start") == 0) {
            Serial.println("[DUCO] join triggered via MQTT");
            duco.sendJoinPacket();
            joinSent      = true;
            joinLastSentAt = millis();
            joinSucceeded  = false;
        }

    } else if (strcmp(topic, "display/set") == 0) {
        g_displayOn = (strcasecmp(buf, "ON") == 0);
        mqtt.publish("display/state", g_displayOn ? "ON" : "OFF", true);
        Serial.printf("[DISP] display %s\n", g_displayOn ? "ON" : "OFF");
        updateDisplay();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WiFi — blocking only in setup(), guarded by 15 s timeout
// ─────────────────────────────────────────────────────────────────────────────
static void connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return;

    WiFi.mode(WIFI_STA);
    WiFi.config(
        IPAddress(10,  0,   0, 119),   // static IP
        IPAddress(10,  0,   0,   1),   // gateway
        IPAddress(255, 255, 255, 0),   // subnet
        IPAddress(10,  0,   0,   1)    // DNS
    );
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("[WiFi] connecting");
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
        delay(250);
        Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("\n[WiFi] up: %s\n", WiFi.localIP().toString().c_str());
    else
        Serial.println("\n[WiFi] timeout — will retry in loop");
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT connect + publish LWT / discovery
// ─────────────────────────────────────────────────────────────────────────────
static bool mqttConnect() {
    if (mqtt.connected()) return true;

    Serial.print("[MQTT] connecting...");
    bool ok = mqtt.connect(
        MQTT_CLIENT_ID,
        *MQTT_USER ? MQTT_USER : nullptr,   // nullptr = no auth field sent
        *MQTT_PASS ? MQTT_PASS : nullptr,
        "duco/status", /*qos*/0, /*retain*/true, "offline"
    );
    if (ok) {
        mqtt.publish("duco/status", "online", true);
        mqtt.subscribe("duco/set");
        mqtt.subscribe("duco/join");
        mqtt.subscribe("display/set");
        mqtt.publish("display/state", g_displayOn ? "ON" : "OFF", true);
        publishDiscovery();
        Serial.println(" OK");
    } else {
        Serial.printf(" failed rc=%d\n", mqtt.state());
    }
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// OTA
// ─────────────────────────────────────────────────────────────────────────────
static void setupOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]()  { Serial.println("[OTA] start"); });
    ArduinoOTA.onEnd([]()    { Serial.println("[OTA] done");  });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        Serial.printf("[OTA] %u%%\r", p * 100 / t);
    });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("[OTA] error %u\n", e);
    });
    ArduinoOTA.begin();
    Serial.printf("[OTA] listening  hostname=%s  port=8266\n", OTA_HOSTNAME);
}

// ─────────────────────────────────────────────────────────────────────────────
// [POWER] ADC poll — call every 10 ms
//
// Phototransistor on A0 (0–1023, normalized to 0.0–1.0).
// Meter rate: 10 imp/Wh (10000 imp/kWh)  →  1 pulse = 0.1 Wh = 0.0001 kWh
// Power formula: W = 360 / interval_sec
// ─────────────────────────────────────────────────────────────────────────────
static void pollADC() {
    float    x   = analogRead(A0) / 1023.0f;
    uint32_t now = millis();

    if (x > POWER_PULSE_HIGH && !g_isFlashing) {
        if (g_lastPulseMs > 0) {
            float interval_sec = (now - g_lastPulseMs) / 1000.0f;
            if (interval_sec > 0.05f) {
                g_watts = 360.0f / interval_sec;    // 10 imp/Wh (10000 imp/kWh)
                if (now - tPulseLog >= 30000) {
                    tPulseLog = now;
                    Serial.printf("[POWER] pulse  interval=%.2fs  %.1fW\n",
                                  interval_sec, g_watts);
                }
            }
        }
        g_lastPulseMs  = now;
        g_isFlashing   = true;
        g_total_kwh   += 0.0001f;   // 10 imp/Wh → 0.1 Wh = 0.0001 kWh per pulse
    }

    if (x < POWER_PULSE_LOW) g_isFlashing = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// [DISPLAY] update — mirrors ESPHome TM1637 lambda
// ─────────────────────────────────────────────────────────────────────────────
static void updateDisplay() {
    // Segment encodings — prefixed D_ to avoid clash with TM1637Display.h macros
    static const uint8_t DIGITS[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
    };
    static const uint8_t D_DASH  = 0x40;  // '-'
    static const uint8_t D_n     = 0x54;  // 'n'
    static const uint8_t D_LETA  = 0x77;  // 'A'
    static const uint8_t D_LETE  = 0x79;  // 'E'
    static const uint8_t D_r     = 0x50;  // 'r'
    static const uint8_t D_BLANK = 0x00;  // ' '
    static const uint8_t D_DOT   = 0x80;  // decimal point (OR into digit byte)

    if (!g_displayOn) {
        tm1637.setBrightness(0, false);
        uint8_t blank[4] = {D_BLANK, D_BLANK, D_BLANK, D_BLANK};
        tm1637.setSegments(blank, 4, 0);
        return;
    }
    tm1637.setBrightness(7, true);

    if (WiFi.status() != WL_CONNECTED) {
        // "nAn "
        uint8_t segs[4] = {D_n, D_LETA, D_n, D_BLANK};
        tm1637.setSegments(segs, 4, 0);
        return;
    }

    // 3-digit display: pos0, pos1, pos2 are digits; pos3 is dot-only.
    uint8_t segs[4] = {D_BLANK, D_BLANK, D_BLANK, D_BLANK};
    int w = (int)g_watts;

    if (w == 0) {
        // "---" — three dashes
        segs[0] = D_DASH; segs[1] = D_DASH; segs[2] = D_DASH;

    } else if (w >= 10000) {
        // "Er " — overflow
        segs[0] = D_LETE; segs[1] = D_r;

    } else if (w >= 1000) {
        // "X.Y" — kW with 1 decimal, e.g. 1234 W → "1.2"
        int h = (w + 50) / 100;
        if (h > 99) h = 99;
        segs[0] = DIGITS[h / 10] | D_DOT;
        segs[1] = DIGITS[h % 10];

    } else if (w >= 100) {
        // "XYZ" — three digits across pos0, pos1, pos2
        segs[0] = DIGITS[w / 100];
        segs[1] = DIGITS[(w / 10) % 10];
        segs[2] = DIGITS[w % 10];

    } else {
        // " XY" — 1–99 W right-aligned in pos1, pos2
        segs[1] = (w >= 10) ? DIGITS[w / 10] : D_BLANK;
        segs[2] = DIGITS[w % 10];
    }

    tm1637.setSegments(segs, 4, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n[BOOT] Duco RF Gateway + Power Monitor  v1.0");

    // WiFi (blocks up to 15 s)
    connectWifi();

    // MQTT — 512-byte buffer covers all discovery payloads
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(512);
    mqtt.setCallback(mqttCallback);
    mqttConnect();

    // OTA
    setupOTA();

    // CC1101 / Duco
    duco.init();
    duco.setLogRFMessages(true);
    duco.setGatewayAddress(DUCO_DEVICE_ADDRESS);
    duco.setNetworkId(networkId);
    duco.setRadioPower(DUCO_RADIO_POWER);
    duco.setTemperature(210);   // 21.0 °C reported to Ducobox
    duco.initReceive();

    if (duco.getDucoDeviceState() == ducoDeviceState_initialised) {
        Serial.println("[DUCO] CC1101 ready @ 868 MHz");
        if (DUCO_DEVICE_ADDRESS != 0) {
            duco.sendSubscribeMessage();
            Serial.println("[DUCO] subscribe sent — waiting for Ducobox reply");
        } else {
#if AUTO_JOIN_DELAY_S > 0
            joinScheduledAt = millis();
            Serial.printf("[DUCO] auto-join in %d s — put Ducobox in pairing mode NOW\n",
                          AUTO_JOIN_DELAY_S);
#else
            Serial.println("[DUCO] no address — publish 'start' to duco/join");
#endif
        }
    } else {
        Serial.printf("[DUCO] CC1101 init FAILED (state=0x%02X) — check wiring & 3.3 V\n",
                      duco.getDucoDeviceState());
    }

    pinMode(PIN_GDO0, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_GDO0), onCC1101Interrupt, RISING);
    pinMode(PIN_GDO2, INPUT);

    // TM1637 display — show "----" until first power reading
    tm1637.setBrightness(7, true);
    uint8_t init_segs[4] = {0x40, 0x40, 0x40, 0x00};  // 3 dashes, pos3 blank (dead)
    tm1637.setSegments(init_segs, 4, 0);
    Serial.println("[DISP] TM1637 ready");

    Serial.println("[BOOT] ready\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// loop  — fully non-blocking, millis()-gated
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // ── WiFi watchdog ────────────────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) connectWifi();

    // ── MQTT reconnect (5 s backoff) ─────────────────────────────────────────
    if (!mqtt.connected() && now - tMqttRetry >= 5000) {
        tMqttRetry = now;
        mqttConnect();
    }
    mqtt.loop();

    // ── OTA ──────────────────────────────────────────────────────────────────
    ArduinoOTA.handle();

    // ── ADC power meter (every 10 ms) ────────────────────────────────────────
    if (now - tAdcPoll >= 10) {
        tAdcPoll = now;
        pollADC();
    }

    // ── Publish watts (every 5 s) ────────────────────────────────────────────
    if (now - tWattsPublish >= 5000) {
        tWattsPublish = now;
        // zero out if meter is idle
        if (g_lastPulseMs > 0 && now - g_lastPulseMs > POWER_TIMEOUT_MS)
            g_watts = 0.0f;
        if (mqtt.connected()) {
            char buf[12];
            snprintf(buf, sizeof(buf), "%.1f", g_watts);
            mqtt.publish("power/watts", buf);
        }
        if (now - tPowerLog >= 30000) {
            tPowerLog = now;
            Serial.printf("[POWER] watts=%.1f\n", g_watts);
        }
    }

    // ── Update display (every 5 s) ───────────────────────────────────────────
    if (now - tDisplay >= 5000) {
        tDisplay = now;
        updateDisplay();
    }

    // ── Publish total kWh (every 60 s, retain=true) ──────────────────────────
    if (now - tKwhPublish >= 60000) {
        tKwhPublish = now;
        if (mqtt.connected()) {
            char buf[12];
            snprintf(buf, sizeof(buf), "%.4f", g_total_kwh);
            mqtt.publish("power/total_kwh", buf, /*retain=*/true);
            Serial.printf("[POWER] total=%.4f kWh\n", g_total_kwh);
        }
    }

    // ── CC1101 packet received (ISR flag) ────────────────────────────────────
    if (packetReady) {
        packetReady = false;
        if (duco.checkForNewPacket()) {
            duco.processNewMessages();
            drainDucoLog();
            if (mqtt.connected()) publishMode();
        }
    }

    // ── Duco ACK handling — must be called frequently ────────────────────────
    duco.checkForAck();
    drainDucoLog();

    // ── Duco slow tick (100 ms) ───────────────────────────────────────────────
    if (now - tSlowTick >= 100) {
        tSlowTick = now;

        duco.checkAndResetRxFifoOverflow();

        // Auto-join: fire after countdown expires (first attempt)
        if (joinScheduledAt > 0 && !joinSent &&
            now - joinScheduledAt >= (unsigned long)AUTO_JOIN_DELAY_S * 1000UL) {
            Serial.println("[DUCO] sending join packet (attempt 1)");
            duco.sendJoinPacket();
            drainDucoLog();
            joinSent      = true;
            joinLastSentAt = now;
        }

        // Retry join every AUTO_JOIN_RETRY_S until Ducobox responds
        if (joinSent && !joinSucceeded &&
            now - joinLastSentAt >= (unsigned long)AUTO_JOIN_RETRY_S * 1000UL) {
            Serial.println("[DUCO] join retry — resending join packet");
            duco.sendJoinPacket();
            drainDucoLog();
            joinLastSentAt = now;
        }

        // Successful join detection
        if (duco.pollNewDeviceAddress()) {
            joinSucceeded = true;
            uint8_t  addr = duco.getDeviceAddress();
            uint8_t* nid  = duco.getnetworkID();
            Serial.printf("[DUCO] JOIN OK — addr=%u  nid=%02X%02X%02X%02X\n",
                          addr, nid[0], nid[1], nid[2], nid[3]);
            Serial.println("[DUCO] → copy these into config.h and reflash:");
            Serial.printf ("[DUCO]   #define DUCO_DEVICE_ADDRESS  %u\n", addr);
            Serial.printf ("[DUCO]   #define DUCO_NETWORK_ID  { 0x%02X, 0x%02X, 0x%02X, 0x%02X }\n",
                          nid[0], nid[1], nid[2], nid[3]);
            char msg[48];
            snprintf(msg, sizeof(msg), "addr=%u nid=%02X%02X%02X%02X",
                     addr, nid[0], nid[1], nid[2], nid[3]);
            if (mqtt.connected()) mqtt.publish("duco/joined", msg, /*retain=*/true);
        }
    }
}
