# Duco RF Gateway + Power Monitor
<img width="1536" height="2048" alt="IMG_3045" src="https://github.com/user-attachments/assets/d4b622b8-64a8-4c61-8cb4-507fd8248ca3" />


An ESP8266 (Wemos D1 Mini) firmware that tries to **join and control a Duco
Silent ventilation unit over its 868 MHz RF link** (CC1101 radio), while
**monitoring household power** by counting the pulse LED on the electricity meter
and reporting everything to **Home Assistant over MQTT**. A TM1637 4-digit
display shows live wattage.

> **Status: the ventilation half is abandoned.** The CC1101 sniffer reliably
> *receives* Duco speed/mode packets, but the firmware never managed to complete
> the RF **pairing/join** handshake with the Ducobox — even with physical access
> to the mainboard and the pairing button pressed. The power-meter + MQTT half
> works. Heating control was solved a different way (an RF switch with a
> parameter mode), so this line of work was dropped. Kept as a reference.

## What works vs what doesn't

| Feature | State |
|---------|-------|
| CC1101 868 MHz packet **sniffing** (Duco mode/speed) | ✅ works |
| Power-meter pulse counting → watts / kWh | ✅ works |
| MQTT publish + Home Assistant auto-discovery | ✅ works |
| TM1637 live-wattage display | ✅ works |
| OTA reflash | ✅ works |
| Duco RF **join / pairing** (to *send* commands) | ❌ never completed |

## Hardware

| Item | Detail |
|------|--------|
| MCU | Wemos D1 Mini (ESP8266 @ 160 MHz) |
| Radio | CC1101 868 MHz transceiver (SPI) |
| Display | TM1637 4-digit 7-segment |
| Power sense | Phototransistor over the meter's impulse LED, into A0 (ADC) |

### Pin map (D1 Mini)

| Signal | GPIO | D-pin |
|--------|------|-------|
| SPI MOSI | 13 | D7 |
| SPI MISO | 12 | D6 |
| SPI SCLK | 14 | D5 |
| CC1101 CSN | 15 | D8 |
| CC1101 GDO0 (packet-ready IRQ) | 4 | D2 |
| CC1101 GDO2 (status) | 5 | D1 |
| TM1637 CLK | 0 | D3 |
| TM1637 DIO | 2 | D4 |
| Phototransistor (ADC) | A0 | — |

## How it works

- **CC1101 / Duco RF.** Radio tuned to **868.326 MHz**, GFSK, 38.38 kBaud,
  variable-length packets with CRC — the Duco air parameters. A 3-slot
  inbox/outbox message queue with ACK retry (300 ms window, up to 3 retries)
  mirrors the Duco protocol. The driver and protocol state machine live in
  `lib/Duco/` (based on
  [arnemauer/Ducobox-ESPEasy-Plugin](https://github.com/arnemauer/Ducobox-ESPEasy-Plugin)).
- **Join attempt.** On boot (after `AUTO_JOIN_DELAY_S`) or on the `duco/join`
  MQTT topic, the firmware runs a multi-stage join handshake and re-sends it
  every `AUTO_JOIN_RETRY_S`. The Ducobox never acknowledged the join — see
  [Why pairing failed](#why-pairing-failed).
- **Power meter.** The phototransistor signal is read on A0 with hysteresis
  (rising > 0.7, falling < 0.3 of full scale). Each LED pulse is one meter
  impulse; instantaneous watts = `3600 / interval_seconds` (1 imp/Wh meter). No
  pulse for 120 s reports 0 W.
- **Display.** Live watts on the TM1637 (kW abbreviation past 9999 W), a dash
  pattern when idle, `nAn` when WiFi is down.
- **MQTT / Home Assistant.** Publishes and subscribes (see below), and pushes
  Home Assistant MQTT-discovery configs so 5 entities appear automatically.

## MQTT topics

**Published**

| Topic | Payload | Cadence |
|-------|---------|---------|
| `power/watts` | instantaneous watts | ~5 s |
| `power/total_kwh` | cumulative energy | ~60 s |
| `duco/mode` | current ventilation mode | on change |
| `duco/joined` | join state + addr/networkId (retained) | on join |

**Subscribed**

| Topic | Action |
|-------|--------|
| `duco/set` | set ventilation mode |
| `duco/join` | trigger an RF join attempt |
| `display/set` | turn the TM1637 on/off |

Home Assistant discovery (`homeassistant/*`) registers: Ventilation Mode
(select), Duco Status (binary_sensor), Power Usage (sensor), Total Energy
(sensor), Display Power (switch).

## Configuration

- **`include/config.h`** — pins, power-meter thresholds, Duco device address /
  network ID, radio TX power, MQTT host/port, static IP.
- **`include/secrets.h`** — WiFi + OTA credentials. **Not committed.** Copy the
  template and fill in your own:

  ```bash
  cp include/secrets.example.h include/secrets.h
  ```

First-boot Duco fields: leave `DUCO_DEVICE_ADDRESS = 0`; on a successful join the
firmware prints + publishes the assigned address and network ID, which you then
paste into `config.h` and reflash. (In practice the join never succeeded here.)

## Build & flash (PlatformIO)

```bash
# USB
pio run -e d1_mini -t upload
pio device monitor -b 115200

# OTA (set --auth in platformio.ini to your OTA password first)
pio run -e d1_mini_ota -t upload
```

> The OTA password in `platformio.ini` is a placeholder (`YOUR_OTA_PASSWORD`).
> Set it to match `OTA_PASSWORD` in `secrets.h` for OTA to authenticate.

## Why pairing failed

The join handshake is fully implemented (multi-stage, with ACK retry) and the
radio clearly hears the Ducobox, so RX/modulation/frequency are correct. The
likely gap: the Ducobox only accepts a join while *it* is in pairing mode, and
that window has to line up with the gateway's join burst (`AUTO_JOIN_DELAY_S`,
default 5 s) — timing that was never reliably hit. Sending commands needs a
completed join, so control stayed out of reach. The project was shelved once an
RF-switch parameter-mode workaround solved the actual goal (de-heating the room).

## Known quirks

- `lib/Duco/` and `lib/DucoCC1101/` are duplicate copies of the same driver.
- Static IP `10.0.0.119` and MQTT broker `10.0.0.163` are hardcoded for a
  private LAN — change them for your network.
- `config.h` comment says "1 imp/Wh"; confirm your meter's impulse constant and
  adjust the watt formula if it differs.
- OTA password must match between `secrets.h` (firmware) and `platformio.ini`
  (uploader).

## Credits

Duco RF driver/protocol adapted from
[arnemauer/Ducobox-ESPEasy-Plugin](https://github.com/arnemauer/Ducobox-ESPEasy-Plugin).

## License

No license specified — personal/reference project.
