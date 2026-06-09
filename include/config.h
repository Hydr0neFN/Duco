#pragma once

// ── WiFi / OTA secrets ───────────────────────────────────────────────────────
// Real values live in include/secrets.h, which is gitignored.
// Copy include/secrets.example.h → include/secrets.h and fill in your network.
#include "secrets.h"

// ── Static IP ────────────────────────────────────────────────────────────────
#define STATIC_IP      "10.0.0.119"
#define STATIC_GW      "10.0.0.1"
#define STATIC_SUBNET  "255.255.255.0"
#define STATIC_DNS     "10.0.0.1"

// ── MQTT broker ──────────────────────────────────────────────────────────────
#define MQTT_HOST      "10.0.0.163"
#define MQTT_PORT      1883
#define MQTT_USER      ""       // leave empty if not required
#define MQTT_PASS      ""
#define MQTT_CLIENT_ID "duco-gateway"

// ── Duco RF ───────────────────────────────────────────────────────────────────
// First boot: leave DUCO_DEVICE_ADDRESS = 0.
// The firmware will auto-join after AUTO_JOIN_DELAY_S seconds.
// After joining, addr + networkId are printed to Serial AND published to
// duco/joined — copy those values here and reflash.
// Set AUTO_JOIN_DELAY_S = 0 to disable auto-join (trigger via duco/join MQTT).
#define AUTO_JOIN_DELAY_S    5
#define AUTO_JOIN_RETRY_S   30   // resend join every N s until Ducobox responds
#define DUCO_DEVICE_ADDRESS  0
#define DUCO_NETWORK_ID      { 0x00, 0x00, 0x00, 0x00 }

// TX power — choose one:
//   0x8D =  0.6 dBm  (safe for nearby Ducobox)
//   0x81 =  5.0 dBm
//   0xC1 = 10.3 dBm  (long range)
//   0x0F = -20  dBm  (lowest)
#define DUCO_RADIO_POWER  0xC1

// ── Pin map (Wemos D1 Mini) ───────────────────────────────────────────────────
// SPI: MOSI=D7/GPIO13, MISO=D6/GPIO12, SCLK=D5/GPIO14, CSN=D8/GPIO15
#define PIN_GDO0  4   // D2 — CC1101 packet-ready interrupt
#define PIN_GDO2  5   // D1 — CC1101 GDO2 status (input)

// ── Power meter ───────────────────────────────────────────────────────────────
// Phototransistor on A0.  Meter: 1 imp/Wh (1000 imp/kWh) → formula: 3600/interval_s = W
// Thresholds match ESPHome: normalized 0.0–1.0 (raw 0–1023 / 1023).
#define POWER_PULSE_HIGH   0.7f    // rising threshold  → pulse detected
#define POWER_PULSE_LOW    0.3f    // falling threshold → pulse over
#define POWER_TIMEOUT_MS   120000  // no pulse for this long → report 0 W

// ── TM1637 display ───────────────────────────────────────────────────────────
#define PIN_TM1637_CLK  0   // D3
#define PIN_TM1637_DIO  2   // D4

// ── OTA ──────────────────────────────────────────────────────────────────────
#define OTA_HOSTNAME "duco-gateway"
// OTA_PASSWORD is defined in secrets.h (gitignored)
