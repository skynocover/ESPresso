# ESPresso — 桌面狀態顯示器

把 Waveshare ESP32-S3 開發板當成桌面常亮小螢幕，顯示 Mac 的 CPU/RAM 使用率、時鐘、以及行事曆代辦事項。

- **狀態**：✅ 實機運作中。三張卡片 UI（時鐘+中文日期 / CPU·RAM 橫條 / 中文行事曆事件）、RTC 時鐘、過期變灰、背光調暗，搭配 Mac agent（psutil + EventKit/CalBridge）端到端驗證完成。進度見 `openspec/changes/desktop-status-display/tasks.md`。
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
│  • psutil   → CPU% / RAM%      │ 序列   │  • ST7789 (LVGL) 渲染       │
│  • CalBridge(Swift)→ 行事曆    │ ─────▶ │  • PCF85063 RTC 自管時鐘    │
│  • 含本機時間 → 打包 JSON      │ JSON行 │  • 解析 JSON 更新畫面       │
│  • pyserial 寫埠               │        │  • 過期變灰 / 背光調暗      │
└────────────────────────────────┘        └─────────────────────────────┘
        大腦                                       臉
```

- 傳輸：USB 序列（CDC），一行一個 JSON。協議見 `openspec/.../specs/host-link-protocol/`。
- 時鐘走板上 **PCF85063 RTC**：agent 在每筆 JSON 帶現在時間，韌體 seed RTC 一次後 RTC 自己 free-run。agent 掛掉時鐘仍走，並當資料新鮮度指標。**不需要 WiFi/NTP。**

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

## Mac 端 agent（`agent/`）

只需 Python 3 + 兩個 pip 套件，行事曆走自帶的簽名 Swift 小程式（**零 OAuth、零雲端註冊**）。
macOS 的 Python 多半是 externally-managed（PEP 668），用虛擬環境最省事：

```bash
cd agent
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt        # psutil / pyserial
sh calbridge/build.sh                  # 編譯+簽名行事曆讀取器 (需 Xcode CLT)
```

| 資料 | 來源 |
|------|------|
| CPU / RAM | `psutil`（整數 0–100%）|
| 行事曆事件 | 原生 **EventKit**，經 `calbridge/eventbridge`（簽名 Swift），未來 7 天最多 5 筆 |
| 現在時間 | 隨每筆 JSON 一起送，用來 seed 板上 RTC |
| 寫序列 | `pyserial`，埠自動找 `/dev/cu.usbmodem*` |

> **為何要一支 Swift 小程式？** 兩個坑疊在一起：
> 1. `osascript` 讀「行事曆.app」的 `whose`-filter 在多行事曆下實測 **>150 秒**，不堪用。
> 2. EventKit 雖快（數毫秒），但 **macOS 14+ 的 TCC 只對「有簽章 + 有用途說明字串」的執行檔跳授權視窗**；裸跑的 Python（含 pyobjc）會被**靜默拒絕、連視窗都不跳**。
>
> 解法：把 EventKit 讀取包成 `calbridge/eventbridge` —— 嵌 `NSCalendars*UsageDescription`、ad-hoc 簽名的 Swift 執行檔。agent 用 subprocess 呼叫它、收 JSON。

### 行事曆權限（一次性）

編好之後，**先手動跑一次** helper 觸發授權視窗，按**允許**：

```bash
cd agent && ./calbridge/eventbridge        # 跳出「允許存取行事曆」→ 按允許，會印出事件 JSON
```

之後 agent 就讀得到。沒授權的話事件清單會是空的（時鐘 / CPU / RAM 照常）。

- 若沒跳視窗或誤按拒絕：`tccutil reset Calendar` 後再跑一次 `./calbridge/eventbridge`。
- 每次重新 `build.sh` 後 cdhash 會變，需要再授權一次。

### 執行

```bash
# 先把韌體燒進板子，接著：
python3 agent.py                       # 自動找 /dev/cu.usbmodem*
python3 agent.py --port /dev/cu.usbmodemXXXX

# 還沒接板子、只想驗證韌體 UI 會更新：用假資料產生器
python3 fake_sender.py
```

板子被拔掉再插回，agent 會自動重連、不會結束。

### 開機自動啟動（選用，launchd）

編輯 `agent/com.espresso.agent.plist`，把 `__PYTHON__`（建議用 venv 內的 `python3`）與 `__AGENT__`（`agent.py` 絕對路徑）換成實際路徑，然後：

```bash
cp agent/com.espresso.agent.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.espresso.agent.plist
# 停用：launchctl unload ~/Library/LaunchAgents/com.espresso.agent.plist
```

> 建議先在終端機手動跑一次完成行事曆授權，再交給 launchd（背景啟動時授權視窗可能不會即時出現）。

## 燒錄指令

```
pio run -t upload && pio device monitor
```