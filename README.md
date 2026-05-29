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

### IMU 朝向校正（QMI8658，實機量測 2026-05-29，單位 g）

朝向偵測（朝下休眠/立起喚醒）用加速度計的重力向量判定。各姿勢實測值：

| 姿勢 | x | y | z | 重力主軸 |
|------|----|----|----|---------|
| **立著**（直立，平常看的姿勢／active） | **+1.01** | ~0 | ~0 | +X |
| **朝下趴平**（螢幕朝下／sleep） | ~0 | ~0 | **−1.00** | **−Z** |
| **橫躺**（量測時的側放姿勢） | ~0 | **−0.97** | ~0 | −Y |
| 朝上平放（推論，未量） | ~0 | ~0 | +1.0 | +Z |

- 目前**只用 Z 軸**判朝下：`az ≤ −0.80g` → 朝下（睡）、`az ≥ −0.55g` → 非朝下（遲滯帶），常數在 `src/main.cpp` 的 `FACEDOWN_ENTER_G / FACEDOWN_EXIT_G`。
- 之後若要做「橫躺/橫向模式」等新姿勢，用上表的軸/正負當基準（橫躺 = −Y、朝上 = +Z）。
- 重新校正：把 `src/main.cpp` 的 `IMU_CALIB_LOG` 改 1，螢幕底部與序列會即時顯示 `x/y/z`（單位 0.01g）。

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

- 傳輸：**USB 序列（CDC）或 WiFi（TCP）**，協定相同——一行一個 JSON。韌體兩條來源餵同一個解析器，agent 端二選一。協議見 `openspec/.../specs/host-link-protocol/`，無線細節見下方「無線傳輸」。
- 時鐘走板上 **PCF85063 RTC**：agent 在每筆 JSON 帶現在時間，韌體 seed RTC 一次後 RTC 自己 free-run。agent 掛掉時鐘仍走，並當資料新鮮度指標。**時鐘本身不需要 WiFi/NTP。**

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
| 傳輸 | 網路 (`socket` → `espresso.local:3333`) 或序列 (`pyserial` → `/dev/cu.usbmodem*`)，見「無線傳輸」 |

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
python3 agent.py                       # 自動探索：能解析 espresso.local 就走網路，否則走序列
python3 agent.py --net                 # 強制走網路 (espresso.local:3333)
python3 agent.py --net --host 192.168.1.42   # mDNS 不通時手動指定 IP
python3 agent.py --serial              # 強制走序列，自動找 /dev/cu.usbmodem*
python3 agent.py --serial --port /dev/cu.usbmodemXXXX

# 還沒接板子、只想驗證韌體 UI 會更新：用假資料產生器
python3 fake_sender.py
```

板子重開 / WiFi 掉線 / Mac 睡醒 / 序列被拔，agent 都會自動重連、不會結束。

### 開機自動啟動（選用，launchd）

編輯 `agent/com.espresso.agent.plist`，把 `__PYTHON__`（建議用 venv 內的 `python3`）與 `__AGENT__`（`agent.py` 絕對路徑）換成實際路徑，然後：

```bash
cp agent/com.espresso.agent.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.espresso.agent.plist
# 停用：launchctl unload ~/Library/LaunchAgents/com.espresso.agent.plist
```

> 建議先在終端機手動跑一次完成行事曆授權，再交給 launchd（背景啟動時授權視窗可能不會即時出現）。

## 無線傳輸（WiFi）

讓板子擺脫資料線：插任意 USB 充電器供電，資料走 WiFi。Mac 開著且同網段時即時更新；Mac 關機/離網時畫面把過期資料變灰、RTC 時鐘照走。序列線路保留作為除錯/救援/首次燒錄用。

### 1. 設定 WiFi / OTA 憑證

韌體的 SSID、密碼、OTA 密碼放在 **gitignore 的 `include/secrets.h`**（明碼存 flash，個人桌面裝置可接受，勿放共用祕密）：

```bash
cp include/secrets.h.example include/secrets.h
# 編輯 include/secrets.h 填入 WIFI_SSID / WIFI_PASS / OTA_PASS
```

> ⚠️ **ESP32-S3 只支援 2.4GHz WiFi**。`WIFI_SSID` 要填 2.4G 那條（HITRON 之類雙頻路由常把 5G 命名成 `xxx-5G`，別填到它），否則會卡在 `4WAY_HANDSHAKE_TIMEOUT`/連不上。

開機後韌體以 station 模式連線（非阻塞，UI 先渲染、連線期間照常顯示），掉線自動重連。連上後：
- 從序列印出取得的 IP（`[wifi] connected, IP=...`）。
- 以 mDNS 宣告 **`espresso.local`**，agent 不需記 IP。
- 開 TCP 線路伺服器於 **port 3333**，吃和序列完全相同的 newline-JSON。

### 2. 選傳輸（agent 端）

`agent.py` 預設**自動探索**：能解析 `espresso.local` 就走網路，否則走序列。也可強制：

```bash
python3 agent.py --net                       # 走網路
python3 agent.py --net --host <板子IP>       # guest/AP isolation 等 mDNS 不通時用 IP
python3 agent.py --serial                    # 走序列
```

> mDNS 不通時，從序列日誌（上面那條 `[wifi] connected, IP=`）拿 IP，丟給 `--host`。
>
> ⚠️ **同時只能跑一支 agent**。韌體 TCP server 是單一 client，新連線會踢掉舊的；跑兩支（例如一支網路一支序列、或兩個視窗）會互踢成 broken-pipe 重連風暴，畫面變閃爍/斷續。

### 3. OTA 燒錄（免 USB 資料線，免停 agent）

WiFi 證明可用後，韌體可直接無線燒錄，徹底解決「agent 佔著序列埠 → esptool 撞埠」的老問題：

```bash
# upload-port 給 mDNS 名稱（或 IP），PlatformIO 走 espota 協定上傳
~/.platformio/penv/bin/pio run -t upload \
    --upload-port espresso.local --upload-flags "--auth=<你的 OTA_PASS>"
```

密碼錯誤或未帶會被韌體拒絕。OTA 處理在 `loop()` 內非阻塞輪詢，不卡畫面。

> ⚠️ 若 `--upload-port espresso.local` 失敗（`Host ... Not Found`），改用 **IP**：espota 底層 `gethostbyname` 不做 mDNS 解析。直接呼叫工具也行：
> ```bash
> python3 ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py \
>     -i <板子IP> -p 3232 --auth="<OTA_PASS>" \
>     -f .pio/build/waveshare-s3-amoled/firmware.bin -r
> ```
> IP 用 `dns-sd -G v4 espresso.local` 或序列日誌取。

### 安全性（local-trust）

v1 的 TCP 線路**未認證/未加密**——同網段任何人都能往 port 3333 推假資料。這是「本地信任」假設下的取捨（個人桌面、家用網段）；OTA 則有密碼保護擋區網內流氓上傳。要更硬的話，未來可加共用 token 認證，屬另一個 change。

---

## 朝下休眠 / 喚醒（motion-sleep，無線電池版）

接上鋰電池後，可拿著走的省電行為（韌體 `src/main.cpp`）：

- **朝下趴平（螢幕朝下）持續 3 秒 → 進 deep-sleep**：關背光、AXP2101 關顯示電軌（ALDO3），待機降到 µA 級。RTC 走電池備援軌不斷電，時鐘照走。
- **立起來 → 自動醒**（約 1–2 秒）：deep-sleep 由 RTC timer 每 1.5s 喚醒，只開 I2C 讀重力；仍朝下就立刻回睡，已立起才完整開機（顯示＋WiFi＋等 agent 重連 ~3–5s）。時鐘喚醒後立即正確。
- **敲螢幕 → 立即醒**：觸控 CST816 INT（GPIO13）接 deep-sleep ext0 喚醒（active-low）。
- 朝向判定只用加速度 Z 軸，門檻/遲滯見 `FACEDOWN_ENTER_G / FACEDOWN_EXIT_G`，校正值見上方「IMU 朝向校正」。

> ⚠️ **OTA 與休眠**：板子朝下睡著時 `loop()` 沒在跑 → **OTA 連不到**（timer 喚醒不開 OTA）。要無線燒錄，先讓板子**立著、醒著**（loop 在跑才收得到）。卡死就實體 RST / USB。

## 燒錄指令

```bash
# USB 序列燒錄（首次/救援；platformio.ini 的 glob 不會被 esptool 展開，要給明確埠）
pio run -t upload --upload-port /dev/cu.usbmodem101 && pio device monitor

# OTA 無線燒錄（見「無線傳輸 §3」）
pio run -t upload --upload-port espresso.local --upload-flags "--auth=<OTA_PASS>"
```