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
- 函式庫：Arduino_GFX、lvgl@8.x、XPowersLib、pschatzmann/arduino-audio-driver（ES8311）。
- **IDF C macro 在 C++ 編不過**：例如 `I2S_STD_CLK_DEFAULT_CONFIG` 的 designator 順序跟 struct decl 不一致 → C 沒事、C++ 報 `designator order does not match declaration order`。**用這類 macro 命中 designator order 錯誤時改成手動 `cfg.field = ...` 一行行賦值**（IDF header 寫的是 C 慣例）。

## 音訊（蕃茄鐘結束聲）

- **必用 legacy `driver/i2s.h`，不用新版 `driver/i2s_std.h`**：本專案開了 PSRAM (`qio_opi`)，新版 `i2s_new_channel()` 會把 handle 配到 PSRAM；接著 `gdma_register_tx_event_callbacks()` 強制檢查 ISR user_data 必須在 internal SRAM → 一律回 `ESP_ERR_INVALID_ARG`（IDF log: "user context not in internal RAM"）。Legacy DMA 路徑沒這條檢查。
- **音調頻率必須整除 sample rate**：1kHz @ 16kHz = 16 sample/cycle 乾淨；880Hz @ 16kHz = 18.18/cycle 會 audible aliasing 變雜訊。預先建一個 cycle 的 LUT 跑，不要 sinf() 即時算（float 累積誤差）。
- **`i2s_write()` 不等播完才 return**：把 buffer 交給 DMA 就回。段間一定要 `delay(~80ms)` 補 DMA 排空時間（4 buf × 256 frame / 16kHz ≈ 64ms），不然下一段 PCM 跟前段尾巴 overlap → 聲音糊掉。
- **ES8311 不在 AXP 任何軌**：吃常開 3.3V，不像 LCD 要先 `enableALDO3()`。喇叭 PA 由 **GPIO 46** 直接控（HIGH=開）。idle 拉 LOW 省電 + 避底噪。
- I2S 腳位：`MCLK=16 BCLK=9 LRCK=45 DOUT=8 DIN=10 PA=46`（Waveshare 官方 BSP `esp32_s3_touch_lcd_1_83.h`）。

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
- **拔插 USB 抓不到完整 boot log**：host 重新 enumerate 要 ~1.5–2s，這段時間韌體 setup 早期的 `Serial.println` 因 `setTxTimeoutMs(0)` 沒人讀就丟，所以 `pio device monitor` 只接得到 `[OK] UI ready` 之後（WiFi 那段）的訊息。要看 `[OK]/[WARN] AXP2101/RTC/IMU/audio` 那段，只有兩條路：(1) monitor 先 `Connected!` 後**手按板上 RESET 鍵**（host 不斷線）；(2) pyserial DTR/RTS reset 同個 session 直接讀。
- 開機掛 USB CDC 已開（`-DARDUINO_USB_CDC_ON_BOOT=1`）。
- 燒錄/讀序列需要實體板子，且看畫面要靠使用者回報。
- **這個 Claude Bash 環境解析不了 `.local`**（mDNS 多播被沙箱擋）：測板子連線一律用 **IP**，不要用 `espresso.local`（`socket.getaddrinfo` 會 gaierror）。IP 用 `dns-sd -G v4 espresso.local` 拿。但 `ping`/IP unicast 是通的。
- **loop 卡死的徵兆**：USB CDC 埠（`/dev/cu.usbmodem*`）消失但板子仍 `ping` 得到 = `loop()` task 卡住（不是整顆重開）；WiFi/lwIP 在另一個 task 照答 ping。需實體重插/RST 才能救回 USB。

## 韌體渲染與資料路徑（已踩過）

- **LVGL 8.x `LV_LABEL_LONG_DOT` 不是「單行截斷」**：實際行為是「**先 wrap、再在最後一行加 ...**」。Label 設了 width 但沒鎖 height 時，長中文標題會展開兩行 → 把下一個 label 擠掉，看起來像「不同筆截斷長度不同」其實是 y 位置全跑掉。**修法**：`lv_obj_set_height(lbl, font.line_height)` 鎖死一行高，wrap 沒地方放就退化成「真的單行截尾 + ...」。`main.cpp:402-414` 是這個修法的範本。
- **「等待校時」≠ RTC 壞**：`update_clock()` 只在 `g_rtc_ok=true` 且 `rtc.read()` 回 false 時印這串，後者只有 VL bit=1（RTC 從沒被 set 過）會觸發。**冷開機 VL=1 是常態**（PCF85063 走 ALDO3 沒備援），開機後等 agent 第一筆成功 parse 才會清。看到「等待校時」遲遲不消失第一個假設是「**沒人送資料／資料每行都 parse fail**」，不是 RTC 硬體問題。驗活：`echo '{"time":"...","cpu":1,"ram":1,"events":[]}' | nc -w1 <IP> 3333`，立刻顯示 = 韌體完全 OK。（`g_rtc_ok=false` 的狀態畫面是「`--:--` + 空白日期」，不是「等待校時」。）
- **host-link 行緩衝 ≥1024**（`feed_byte()`）：cc 三欄 ~75B + 5 筆事件 × CJK 標題（UTF-8 每字 3B）→ 單行很容易超過 512。超過時 `feed_byte` 把 buf 丟掉 → JSON 殘缺 → `deserializeJson` 失敗 → `apply_status` 沒跑 → `g_last_msg_ms` 沒更新。**級聯症狀**：8s → stale（全變灰），2min → 背光降到 40/255，10min → 背光熄。看起來像 agent 掛了、板子斷線，實際 agent 一直在送、是韌體每行都 drop。**任何新欄位要加進 host-link JSON 前先估行長**。
- **症狀疊加 / 同一根因多重表現**：buffer 512 太小同時造成「中文標題顯示舊」「背光週期性熄」「agent 看起來掛了」三個現象。Debug 時看到多症狀**先找共同上游**（資料流哪裡斷？parse 哪裡 fail？），不要每個分頭修。

## 流程

- 用 OpenSpec 工作流（explore / propose / apply / archive）。目前無進行中的 change（desktop-status-display、wireless-transport 皆已 archive）。
- 顯示驅動站在 Arduino_GFX 上；不自己刻驅動。
- 韌體保持「笨」——資料收集都在 Mac 端 agent（Python），韌體只渲染。
