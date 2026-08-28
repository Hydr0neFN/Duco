[English](README.md) · **繁體中文**

# Duco RF Gateway + Power Monitor
<img width="1536" height="2048" alt="IMG_3045" src="https://github.com/user-attachments/assets/d4b622b8-64a8-4c61-8cb4-507fd8248ca3" />


一個 ESP8266 (Wemos D1 Mini) 韌體，嘗試**透過其 868 MHz RF 射頻連結（CC1101 無線電模組）加入並控制 Duco Silent 通風單元**，同時藉由計算電表上的脈衝 LED 來**監控家庭用電**，並**透過 MQTT 將所有數據回報至 Home Assistant**。TM1637 4 位數顯示器則顯示即時瓦數。

> **狀態：通風控制部分已放棄。** CC1101 監聽器能穩定**接收** Duco 的風速/模式封包，但韌體始終未能與 Ducobox 完成 RF **配對/加入（pairing/join）**信號交換 —— 即使直接接觸主機板並按下了配對按鈕也是如此。電表 + MQTT 部分運作正常。暖氣控制已透過另一種方式解決（具備參數模式的 RF 開關），因此這條研發路線已終止。保留作為參考資料。

## 運作正常與無法運作的功能

| Feature | State |
|---------|-------|
| CC1101 868 MHz 封包**監聽**（Duco 模式/風速） | ✅ 正常運作 |
| 電表脈衝計數 → 瓦特 / kWh | ✅ 正常運作 |
| MQTT 發布 + Home Assistant 自動探索 | ✅ 正常運作 |
| TM1637 即時瓦數顯示 | ✅ 正常運作 |
| OTA 重新燒錄 | ✅ 正常運作 |
| Duco RF **加入 / 配對**（用於*發送*命令） | ❌ 未曾成功完成 |

## 硬體

| Item | Detail |
|------|--------|
| MCU | Wemos D1 Mini (ESP8266 @ 160 MHz) |
| 無線電模組 | CC1101 868 MHz 收發器 (SPI) |
| 顯示器 | TM1637 4 位數 7 段顯示器 |
| 電力感測 | 置於電表脈衝 LED 上的光電電晶體，接入 A0 (ADC) |

### 腳位對應表 (D1 Mini)

| Signal | GPIO | D-pin |
|--------|------|-------|
| SPI MOSI | 13 | D7 |
| SPI MISO | 12 | D6 |
| SPI SCLK | 14 | D5 |
| CC1101 CSN | 15 | D8 |
| CC1101 GDO0 (封包就緒 IRQ) | 4 | D2 |
| CC1101 GDO2 (狀態) | 5 | D1 |
| TM1637 CLK | 0 | D3 |
| TM1637 DIO | 2 | D4 |
| 光電電晶體 (ADC) | A0 | — |

## 運作原理
<img width="2160" height="3840" alt="5A009B2B-4272-4DF9-BD02-02433DA3A89B" src="https://github.com/user-attachments/assets/6d2d32ec-ccfc-4392-aaab-bbf4dcfaea32" />

- **CC1101 / Duco RF。** 無線電頻率調諧至 **868.326 MHz**、GFSK、38.38 kBaud、具 CRC 的可變長度封包 —— 此為 Duco 的空中通訊參數。具備 ACK 重試機制（300 ms 視窗，最多重試 3 次）的 3 槽收件匣/寄件匣訊息佇列，重現了 Duco 通訊協定。驅動程式與通訊協定狀態機位於 `lib/Duco/`（基於 [arnemauer/Ducobox-ESPEasy-Plugin](https://github.com/arnemauer/Ducobox-ESPEasy-Plugin)）。
- **加入嘗試。** 開機時（經過 `AUTO_JOIN_DELAY_S` 後）或在收到 `duco/join` MQTT 主題時，韌體會執行多階段的加入信號交換，並每隔 `AUTO_JOIN_RETRY_S` 重新發送一次。Ducobox 從未確認此加入請求 —— 請參閱[為何配對失敗](#為何配對失敗)。
- **電表。** 光電電晶體訊號由 A0 讀取並具備遲滯（hysteresis）比較（上升 > 滿量程的 0.7，下降 < 滿量程的 0.3）。每個 LED 脈衝代表一次電表脈衝；即時瓦數 = `3600 / interval_seconds`（適用於 1 imp/Wh 電表）。若連續 120 秒無脈衝則回報 0 W。
- **顯示器。** 於 TM1637 上顯示即時瓦數（超過 9999 W 時縮寫為 kW），閒置時顯示破折號圖案，WiFi 斷線時顯示 `nAn`。
- **MQTT / Home Assistant。** 負責發布與訂閱（見下文），並推送 Home Assistant MQTT 自動探索設定，使 5 個實體能自動顯示。

## MQTT 主題

**已發布的主題**

| Topic | Payload | Cadence |
|-------|---------|---------|
| `power/watts` | 即時瓦數 | ~5 秒 |
| `power/total_kwh` | 累計用電量 | ~60 秒 |
| `duco/mode` | 目前通風模式 | 數值變更時 |
| `duco/joined` | 加入狀態 + addr/networkId（保留訊息） | 加入時 |

**已訂閱的主題**

| Topic | Action |
|-------|--------|
| `duco/set` | 設定通風模式 |
| `duco/join` | 觸發 RF 加入嘗試 |
| `display/set` | 開啟/關閉 TM1637 |

Home Assistant 探索（`homeassistant/*`）會註冊：Ventilation Mode（select）、Duco Status（binary_sensor）、Power Usage（sensor）、Total Energy（sensor）、Display Power（switch）。

## 設定

- **`include/config.h`** — 腳位、電表閾值、Duco 裝置位址 / 網路 ID、無線電發射功率、MQTT 主機/連接埠、靜態 IP。
- **`include/secrets.h`** — WiFi 與 OTA 憑證資訊。**未提交至版本控制。** 請複製範本並填入你自己的資料：

  ```bash
  cp include/secrets.example.h include/secrets.h
  ```

首次開機的 Duco 欄位：保留 `DUCO_DEVICE_ADDRESS = 0`；若成功加入，韌體會印出並發布獲分配的位址與網路 ID，隨後你可將其貼入 `config.h` 並重新燒錄。（在實際操作中，此處從未成功加入過。）

## 建置與燒錄 (PlatformIO)

```bash
# USB
pio run -e d1_mini -t upload
pio device monitor -b 115200

# OTA (set --auth in platformio.ini to your OTA password first)
pio run -e d1_mini_ota -t upload
```

> `platformio.ini` 中的 OTA 密碼為佔位符（`YOUR_OTA_PASSWORD`）。請將其設定為與 `secrets.h` 中的 `OTA_PASSWORD` 一致，以通過 OTA 驗證。

## 為何配對失敗

加入信號交換已完整實作（多階段且具備 ACK 重試機制），且無線電模組能清楚接收到 Ducobox 的訊號，因此 RX/調變/頻率皆正確無誤。可能的問題癥結在於：Ducobox 僅在其*自身*處於配對模式時才接受加入，而該時間視窗必須與閘道器的加入發送突波（`AUTO_JOIN_DELAY_S`，預設為 5 秒）精確重疊 —— 但此時序從未被可靠地命中。發送命令必須先完成加入流程，因此控制功能始終無法達成。在透過 RF 開關的參數模式變通方案解決了實際目標（為房間散熱）後，此專案便被擱置。

## 已知特性與注意事項

- `lib/Duco/` 與 `lib/DucoCC1101/` 是同一個驅動程式的重複副本。
- 靜態 IP `10.0.0.119` 與 MQTT 代理伺服器 `10.0.0.163` 是針對私人區域網路寫死的 —— 請依你的網路環境進行修改。
- `config.h` 註解寫著「1 imp/Wh」；請確認你電表的脈衝常數，若不相同請調整瓦數計算公式。
- `secrets.h`（韌體）與 `platformio.ini`（上傳工具）中的 OTA 密碼必須相符。

## 特別鳴謝

Duco RF 驅動程式/通訊協定改編自 [arnemauer/Ducobox-ESPEasy-Plugin](https://github.com/arnemauer/Ducobox-ESPEasy-Plugin)。

## 授權條款

未指定授權條款 —— 個人/參考專案。
