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
# USB 燒錄：一定要明確指定埠（platformio.ini 的 glob 不會被 esptool 展開）
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.usbmodem101
# OTA 無線燒錄（免 USB、免停 agent）：用「IP」不要用 espresso.local
#   (espota/gethostbyname 不做 mDNS 解析；IP 從 `dns-sd -G v4 espresso.local` 或序列日誌取)
python3 ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py \
    -i <板子IP> -p 3232 --auth="<OTA_PASS>" \
    -f .pio/build/waveshare-s3-amoled/firmware.bin -r
```

## 無線傳輸（WiFi/OTA，已實作）

- 韌體連 WiFi (STA)、mDNS `espresso.local`、TCP line-server `:3333`（吃和序列同一份 newline-JSON）、ArduinoOTA `:3232`。憑證在 gitignore 的 `include/secrets.h`。
- **WiFi 只支援 2.4GHz**（ESP32-S3 無 5G）；SSID 別填 `-5G` 那條。
- **TCP 是單一 client**：同時只能跑「一支」agent。跑兩支會互相把對方踢掉 → broken-pipe 重連風暴（看起來像畫面閃爍/斷續）。
- `WiFi.setSleep(false)` 必須保留：開 modem-sleep 會週期漏接 ARP/SYN → 間歇 `No route to host` + 區網延遲飆到上百 ms。
- **OTA 用 IP 不用 `.local`**（見上）。OTA 期間 `loop()` 要還活著才收得到 → 若 loop 卡死，得先實體重開板子。
- **朝下休眠會擋 OTA**：接電池後板子朝下趴平 3 秒進 deep-sleep（省電），睡著時 `loop()` 不跑 → OTA 連不到。**要燒先讓板子立著醒著**（立起來或敲螢幕都會喚醒）。朝向校正值與 `FACEDOWN_*` 門檻見 README「IMU 朝向校正」＋「朝下休眠」。

## 環境坑（macOS）

- **沒有 `timeout` 指令**（那是 GNU coreutils）。要限時讀序列就用 Python/pyserial。
- ESP32-S3 原生 USB CDC：開機 banner 只在前幾秒印，且 `loop()` 沒 print 時序列是空的（**不代表當機**）。用 pyserial toggle DTR/RTS 觸發 reset 後再讀（範例見 README）。
- 開機掛 USB CDC 已開（`-DARDUINO_USB_CDC_ON_BOOT=1`）。
- 燒錄/讀序列需要實體板子，且看畫面要靠使用者回報。
- **這個 Claude Bash 環境解析不了 `.local`**（mDNS 多播被沙箱擋）：測板子連線一律用 **IP**，不要用 `espresso.local`（`socket.getaddrinfo` 會 gaierror）。IP 用 `dns-sd -G v4 espresso.local` 拿。但 `ping`/IP unicast 是通的。
- **loop 卡死的徵兆**：USB CDC 埠（`/dev/cu.usbmodem*`）消失但板子仍 `ping` 得到 = `loop()` task 卡住（不是整顆重開）；WiFi/lwIP 在另一個 task 照答 ping。需實體重插/RST 才能救回 USB。

## 流程

- 用 OpenSpec 工作流（explore / propose / apply / archive）。目前無進行中的 change（desktop-status-display、wireless-transport 皆已 archive）。
- 顯示驅動站在 Arduino_GFX 上；不自己刻驅動。
- 韌體保持「笨」——資料收集都在 Mac 端 agent（Python），韌體只渲染。
