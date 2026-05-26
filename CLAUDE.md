# CLAUDE.md

ESPresso：把 Waveshare ESP32-S3 板當桌面狀態顯示器（Mac CPU/RAM、時鐘、行事曆）。
完整背景、硬體規格、腳位、架構見 **README.md**；規格/設計/任務見 **openspec/**。

## 板子（務必記住，曾誤判一整輪）

**Waveshare ESP32-S3-Touch-LCD-1.83** — **ST7789P 標準 SPI，240×284**。
不是 AMOLED、不是 QSPI、不是 ST77916/SH8601、不是 360×360。

腳位：顯示 SPI `DC=4 CS=5 SCK=6 MOSI=7 RST=38 BL=40`；I2C `SDA=15 SCL=14`（AXP2101 0x34 / CST816D / QMI8658 0x6B / PCF85063 0x51 / ES8311 0x18）。

## 工具鏈（不要亂改）

- PlatformIO CLI：`~/.platformio/penv/bin/pio`
- `platformio.ini` platform 釘在 pioarduino `53.03.13`（arduino-esp32 **3.1.3**）。
  **不要升級**：core 3.2+ 的 I2C 驅動有 `ESP_ERR_INVALID_STATE` 回歸，會讓 AXP2101/I2C 全寫不進去。
- 函式庫：Arduino_GFX、lvgl@8.x、XPowersLib。

## 常用指令

```bash
# 編譯
~/.platformio/penv/bin/pio run
# 燒錄：一定要明確指定埠（platformio.ini 的 glob 不會被 esptool 展開）
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.usbmodem101
```

## 環境坑（macOS）

- **沒有 `timeout` 指令**（那是 GNU coreutils）。要限時讀序列就用 Python/pyserial。
- ESP32-S3 原生 USB CDC：開機 banner 只在前幾秒印，且 `loop()` 沒 print 時序列是空的（**不代表當機**）。用 pyserial toggle DTR/RTS 觸發 reset 後再讀（範例見 README）。
- 開機掛 USB CDC 已開（`-DARDUINO_USB_CDC_ON_BOOT=1`）。
- 燒錄/讀序列需要實體板子，且看畫面要靠使用者回報。

## 流程

- 用 OpenSpec 工作流（explore / propose / apply / archive）。目前進行中的 change：`desktop-status-display`。
- 顯示驅動站在 Arduino_GFX 上；不自己刻驅動。
- 韌體保持「笨」——資料收集都在 Mac 端 agent（Python），韌體只渲染。
