# ESPresso — 桌面狀態顯示器

把 Waveshare ESP32-S3 開發板當成桌面常亮小螢幕，顯示 Mac 的 CPU/RAM 使用率、時鐘、以及行事曆代辦事項。

- **狀態**：Stage 0 完成（螢幕點亮 / 顏色文字正常）。後續 Stage 1+ 見 `openspec/changes/desktop-status-display/tasks.md`。
- 規格與設計：`openspec/`（proposal / design / specs / tasks）。

---

## 硬體

**板子：Waveshare ESP32-S3-Touch-LCD-1.83**（小方形手錶造型）

| 元件 | 型號 | 介面 / 位址 |
|------|------|------------|
| MCU | ESP32-S3R8 | 16MB Flash, 8MB PSRAM |
| 顯示 | **ST7789P**，1.83吋，**240×284**，IPS | **標準 SPI** |
| 觸控 | CST816D | I2C，RST=GPIO39, INT=GPIO13 |
| 電源管理 | AXP2101 PMU | I2C 0x34（XPowersLib） |
| IMU | QMI8658（6 軸） | I2C 0x6B |
| RTC | PCF85063 | I2C 0x51 |
| 音訊 | ES8311 codec + ES7210 AEC + 喇叭 + 麥克風 | I2C 0x18 / I2S |

### 腳位

```
顯示 ST7789P (SPI):
  DC   = GPIO4      CS  = GPIO5
  SCK  = GPIO6      MOSI= GPIO7
  RST  = GPIO38     BL  = GPIO40 (背光, active-high)

I2C 匯流排 (共用): SDA = GPIO15, SCL = GPIO14
  掃到位址: 0x15(CST816) 0x18(ES8311) 0x34(AXP2101)
            0x40 0x51(RTC) 0x6B(IMU) 0x7E

按鈕: BOOT = GPIO0,  PWR = GPIO41
```

> ⚠️ 此板早期曾被誤判為 AMOLED 1.8 / LCD 1.85C（QSPI/ST77916/360×360）——**那是錯的**。實機是 ST7789P 標準 SPI、240×284。

---

## 架構

ESP32 讀不到 Mac 的系統資訊與行事曆，因此分成兩半：

```
┌──────────── macOS ────────────┐        ┌──── ESP32-S3 (1.83 LCD) ────┐
│  agent.py (Python)             │  USB   │  韌體 = 顯示用戶端          │
│  • psutil  → CPU% / RAM%       │ 序列   │  • ST7789 (LVGL) 渲染       │
│  • osascript → 行事曆事件      │ ─────▶ │  • NTP 自管時鐘             │
│  • 打包 JSON → pyserial        │ JSON行 │  • 解析 JSON 更新畫面       │
└────────────────────────────────┘        └─────────────────────────────┘
        大腦                                       臉
```

- 傳輸：USB 序列（CDC），一行一個 JSON。協議見 `openspec/.../specs/host-link-protocol/`。
- 時鐘由韌體 NTP 自管，agent 掛掉時鐘仍走，並當資料新鮮度指標。

---

## 開發環境

- **PlatformIO**（CLI：`~/.platformio/penv/bin/pio`）
- Platform：**pioarduino**，釘在 `53.03.13`（= arduino-esp32 **core 3.1.3**）
  - 官方 `espressif32@6.x`（core 2.0.x）缺 `esp32-hal-periman.h`，Arduino_GFX 編不過 → 需 core 3.x。
  - 但 core **3.2.0+** 的新 I2C 驅動有回歸 bug（`i2c_master_transmit` 一律 `ESP_ERR_INVALID_STATE`），會讓 AXP2101 等 I2C 裝置寫不進去 → **必須釘 3.1.3**。
- 函式庫：`moononournation/GFX Library for Arduino`、`lvgl@8.x`、`lewisxhe/XPowersLib`。

### 建置 / 燒錄 / 看序列

```bash
# 編譯
~/.platformio/penv/bin/pio run

# 燒錄（注意：要給明確的埠，platformio.ini 的 glob 不會被 esptool 展開）
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.usbmodem101

# 看序列輸出（macOS 沒有 timeout 指令；ESP32-S3 原生 USB CDC 開機那幾秒才印，
# 故用 pyserial 觸發 reset 後再讀）
~/.platformio/penv/bin/python - <<'PY'
import serial, time
p = serial.Serial('/dev/cu.usbmodem101', 115200, timeout=0.2)
p.setDTR(False); p.setRTS(True); time.sleep(0.1)
p.setRTS(False); time.sleep(0.1); p.reset_input_buffer()
p.setRTS(True); time.sleep(0.1); p.setRTS(False)
end = time.time() + 8
while time.time() < end:
    line = p.readline()
    if line: print(line.decode('utf-8','replace').rstrip())
p.close()
PY
```

---

## Mac 端 agent（Stage 3 起）

只需 Python 3 + 兩個套件，其餘靠 macOS 內建工具：

```bash
pip install psutil pyserial
```

- CPU/RAM：`psutil`
- 行事曆：`osascript` 讀本機「行事曆.app」（**零 OAuth**；首次會跳權限授權視窗）
- 寫序列：`pyserial`，埠用 `/dev/cu.usbmodem*`
