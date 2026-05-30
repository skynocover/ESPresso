## Context

ESPresso 韌體至今是「笨渲染器」：所有狀態（CPU/RAM/事件）由 Mac agent 透過 host-link 推送，韌體只渲染並保持本地時鐘 (PCF85063 + LVGL)。已實作的能力剛好把蕃茄鐘需要的東西都備齊了，但全都用在別處：

- **QMI8658 加速度計**：已啟用，但目前**只用 Z 軸**判朝下睡眠 (`FACEDOWN_ENTER_G = −0.80g`, `FACEDOWN_EXIT_G = −0.55g`, hold 3s)。橫躺 −Y 軸已實機校正 (~−0.97g) 但沒接 UI。
- **CST816D 觸控**：只當 `esp_sleep_enable_ext0_wakeup` 的喚醒源 (TP_INT=GPIO13 active-low)，`loop()` 內**沒有讀**過任何觸控事件，I2C 也沒對它做 init/poll。
- **PCF85063 RTC + millis()**：時鐘已運作，可作為計時與壁鐘基準。
- **LCD 240×284 ST7789P + LVGL 8.x**：三卡 UI 直立佈局，沒有切頁、沒有橫向旋轉、沒有全屏接管的先例。
- **ES8311 + 喇叭**：板載但韌體**從未碰過** I2S/codec。
- **兩顆實體鈕**：BOOT=GPIO0 (strapping 腳，可讀)；PWR=GPIO41 是 AXP2101 PowerKey (XPowersLib `isPekeyShortPress/LongPressIrq`，長按關機)。**本次都不動。**

Stakeholder：唯一使用者 (作者本人，桌面前)。約束來自硬體 (240×284 小螢幕、手錶造型不耐邊緣施力) 和韌體哲學 (盡量笨、保留 OTA/wireless link 可用)。

## Goals / Non-Goals

**Goals:**
- 橫躺 (左倒) → 全屏蕃茄鐘倒數，零摩擦進入
- 點觸控 = 開始/暫停；立回直立 = 退出；朝下 = 全域 panic 重置 + 睡眠
- 計時不依賴 Mac agent；TCP 斷線、agent 沒開、WiFi 沒連都照走
- 維持既有 facedown 睡眠/喚醒、wireless OTA、三卡狀態 UI 行為不退化

**Non-Goals (本次明確排除):**
- 音訊提示 (ES8311 / I2S / 喇叭) — 提示先用螢幕大字＋背光脈衝代替
- Mac agent 反向通知 / TCP 反向通道 — host-link 維持單向 agent→板子
- 完整 25/5/25/5/25/15 番茄循環 — 第一版只做單次 25 分鐘 work session
- 多種時長 / 自訂時長 / 設定 UI — 25 分鐘寫死，未來再說
- 累積統計 / 紀錄存 NVS — 完成只在當下提示，不留紀錄
- 觸控手勢 (滑動切頁、雙擊等) — 只用 single tap，long-press 留給「未來重置」備案但先不做
- 動實體按鈕 BOOT/PWR

## Decisions

### D1: 計時在裝置上跑，不依賴 Mac agent

**選擇**: `millis()` 跑倒數，PCF85063 RTC 作為壁鐘秒進位的對齊基準；完全不靠 host-link 傳 `remaining`。

**為什麼不讓 agent 驅動**: host-link 是單一 TCP client (CLAUDE.md 明文)，agent 不在/掉線/重連風暴時計時會崩。蕃茄鐘的爽點 = 一倒板子就專注，這個體驗不能掛在另一台機器在線上。

**代價**: 韌體第一次擁有自己的有意義狀態 (work-running、剩餘秒數)。可接受 — 蕃茄鐘範圍小、生命週期短 (≤25 分鐘)、不持久化。

### D2: 朝下 = 全域最高優先 + 清空進度

**選擇**: 既有 facedown 偵測升級成「不論前景在哪個狀態 (狀態卡 / 蕃茄鐘 work / 蕃茄鐘 paused)，朝下 3s 都進 deep-sleep 並把蕃茄鐘進度歸零」。喚醒回來一定是狀態卡 (+X)，**不**恢復蕃茄鐘。

**為什麼**: 使用者明確要求；另外它順便提供 panic exit (UI 卡了趴一下就好)。維持既有 `FACEDOWN_HOLD_MS = 3000` 寬限就足以擋手晃過 −Z 的誤觸發。

**替代方案**: 朝下時保留 paused 狀態待下次醒來繼續。否決理由 — 增加狀態複雜度、語意不直覺、跟「panic exit」衝突。

### D3: 橫躺 = 左倒一側 (−Y) 觸發，自動開始 + 3 秒寬限取消

**選擇**: 偵測 ay ≤ −0.80g (與 facedown Z 用同套門檻精神) 並 hold 短時間 (~500 ms) 後進入蕃茄鐘，立即啟動 25:00 倒數。進入後 3 秒內若立回直立 (+X ≥ +0.80g)，**不計入**這次番茄，當作誤觸。

**為什麼一側不兩側**: 校正表只實測 −Y；雙側都接會多一條狀態分支但功能等價，違反 YAGNI。

**為什麼自動開始**: 進入手勢已經是「我要專注」的明確意圖，再要求點觸控啟動會減損零摩擦的爽度。3 秒寬限解掉誤觸風險。

**替代方案**: 進入後停在 25:00 待命、點觸控才開始。否決理由 — 橫躺後手通常已離開板子，再要拿回去點等於兩段動作。

### D4: 觸控只用 single tap，不做手勢/長按

**選擇**: CST816D 在 `loop()` 內用「INT pin 拉低就讀一次 I2C」的方式 (避免 polling 浪費 CPU)。讀到 touch-down 視為 tap 事件；不做 long-press、不解析 gesture 寄存器、不做雙擊。Tap 語意：work-running → paused；paused → work-running。

**為什麼最小**: 第一版控制詞彙需求只有一個 (開始/暫停)。多事件意味著更多 UI affordance、更多誤觸路徑。退出/重置已由 IMU 手勢處理 (立回 / 朝下)。

**橫向座標映射**: 蕃茄鐘畫面 LVGL 旋轉到 landscape (`lv_disp_set_rotation(LV_DISP_ROT_90)` 或繪製時手動算)。CST816D 回的原始座標是直立 (0..240, 0..284)，UI 旋轉後座標也要對應轉。**簡化**：tap 不在意座標 (整片螢幕都是 tap target)，**只要判斷有 touch-down 事件**就好，於是可以省掉座標映射這條複雜度。長按/手勢若未來要加，再處理 mapping。

### D5: 觸控的 wake/run 雙身分

CST816D 的 INT 同時是 (a) deep-sleep 喚醒源 (RTC GPIO ext0)，(b) 運行時 tap 通知。**策略**：
- 啟動時 (`init`) 把 CST816D 從 reset 拉出、做最小 I2C poke 確認在線；不需要設手勢 (預設讀座標即可)。
- `loop()` 內：以 INT 為主 (高→低代表有 touch event)，發生時讀一次 I2C 拿狀態，不需固定 polling。
- 進 deep-sleep 前不需要關 CST816D；既有路徑已保留 ext0。

### D6: 完成提示只用螢幕＋背光，**不啟用 ES8311**

**選擇**: 倒數歸零時，螢幕進入完成畫面 (大字 "DONE" 或類似)，背光做幾次脈衝 (PWM 上下)；持續到使用者立回 / 點觸控 / 朝下。

**為什麼**: ES8311 韌體完全沒碰過——需要 I2S 設定、codec init、PMU 開 LDO 給 codec/喇叭、音檔來源 (合成方波? PROGMEM PCM?)。為了一個叮聲做這些不划算，且容易拖累本次範圍。背光脈衝是 GPIO40 PWM，幾行就完成。

**替代方案**: 加 ES8311 叮聲。明確列為未來工作，不在本次。

### D7: UI 切換 = 全屏接管，背景三卡暫停渲染

**選擇**: 進入蕃茄鐘時建立新 LVGL screen (或隱藏三張卡並蓋上倒數標籤)，host-link 仍持續接收 (CPU/RAM/events 持續更新到變數)，但**不**呼叫對應的 `lv_label_set_text`。退出時把最新值補上。

**為什麼**: 渲染是顯示熱點，背景跑沒意義；但資料管線停掉會讓退出時看到陳舊值。中間方案 = 資料收、UI 不畫。

### D8: 狀態機

```
   ┌───────────── 立回 +X (ax ≥ +0.80g) ─────────────┐
   │                                                  │
   ▼                                                  │
┌─────────┐  橫躺 −Y          ┌────────────────────┐  │
│ STATUS  │ ─────────────────▶│ POMO_WORK_RUNNING  │ ─┤
│ (+X)    │ (ay ≤ −0.80g)     │ countdown ⏱        │  │
│ 三卡UI  │                   │ tap → PAUSED       │  │
└─────────┘                   └────────────────────┘  │
   ▲                            │   ▲                 │
   │                            │   │ tap             │
   │                            ▼   │                 │
   │ wake (timer/touch)       ┌────────────────────┐  │
   │                          │ POMO_PAUSED        │ ─┤
   │                          │ 倒數凍結            │  │
   │                          └────────────────────┘  │
   │                                                  │
   │                          ┌────────────────────┐  │
   │                          │ POMO_DONE          │ ─┤
   │                          │ 螢幕脈衝 + 大字     │  │
   │                          └────────────────────┘  │
   │                                                  │
   │                ┌───────────────────────────┐     │
   └─────────────── │ DEEP_SLEEP                │ ◀───┘
                    │ + clear pomo state         │  朝下 −Z (az ≤ −0.80g)
                    └───────────────────────────┘  hold 3s  [全域最高優先]
```

關鍵性質：
- **朝下優先序高於所有橫躺/直立判斷**：每個 IMU tick 先檢 `az`，再檢 `ay/ax`。
- **進入 POMO_WORK_RUNNING 後 3 秒內立回 +X**：視為誤觸，回 STATUS，**不**計入這次番茄 (本次沒有「次數」概念，這條規格效果其實只是「不顯示 DONE」)。
- **POMO_DONE 退出條件**：立回 +X (回 STATUS)、朝下 (DEEP_SLEEP)、tap (回 STATUS)。
- **狀態切換 debounce**：IMU 各分類用 hold ~500 ms (朝下沿用 3000 ms)，避免角度晃動。

## Risks / Trade-offs

- **橫躺與朝下優先序錯亂** → 規格明訂「朝下永遠最先檢」，並用既有 3000 ms hold 擋掉中間角度的誤判
- **CST816D init 在現有 boot path 加新 I2C device，可能跟 AXP2101/RTC 搶 bus** → 既有 I2C 已多 device 共存無問題；CST816D 加入只多一個位址，但要在 AXP 開觸控 LDO 之後 init (boot 順序要驗)
- **橫向 UI 在 240×284 小螢幕的視覺密度** → 第一版只放「MM:SS」大數字 + 小狀態指示 (running/paused 圖示)，不放多餘元素
- **觸控 tap 跟 deep-sleep ext0 喚醒共用 INT**：tap 在運行時誤把板子叫醒？只在睡眠時 ext0 才生效，運行時觸控不會「重新喚醒」，所以沒衝突
- **OTA 期間 loop() 要還活著**：蕃茄鐘狀態機不能阻塞 loop()，沿用既有 non-blocking 風格 (millis 判斷而非 delay)
- **WiFi modem-sleep 已明文關閉**：不變
- **倒數準度**：用 `millis()` 在 25 分鐘內漂移 < 1 秒可接受 (RTC 校時不需要)；若漂移太大再用 PCF85063 對齊

## Migration Plan

無 schema/協定變動。部署 = 韌體 OTA 一次。Rollback = 燒回上一版 firmware.bin (上一次 commit 的 build artifact)。

無資料遷移 — 蕃茄鐘狀態不持久化 (D1/D2)，掉電/睡眠後一律從 STATUS 起頭。

## Open Questions

1. **POMO_DONE 持續多久**？無限到使用者反應，還是 60 秒自動回 STATUS？暫定 60 秒，實作時微調。→ **實作採 60 s** (`POMO_DONE_TIMEOUT_MS`)。
2. **是否在橫躺進入時把 host-link 資料 freeze 顯示**（退出時補新值）？D7 已決定「資料收 UI 不畫」，但「進入瞬間的快照要不要保留」這個細節留實作判斷。→ **實作不 freeze**：apply_status 照常寫 `lv_label`，只是物件被 hidden 不送 flush；退 dashboard 後是最新值。
3. **AXP2101 LDO 對 CST816D 觸控供電的旗標**——既有韌體開機已能用觸控當喚醒源，代表 LDO 已開；要確認 deep-sleep wake 後 LDO 持續供電不需要再切換。實作期 spike 一下。→ **保持現狀**：`enter_sleep_from_active` 只關 ALDO3 (顯示)，觸控供電軌不動；既有 ext0 喚醒路徑驗證過可運作。

## Implementation Notes (deltas vs. spec/design)

實作時與規格/設計有以下小幅調整，spec 文字本身未受影響（行為仍滿足規格 Scenario）：

- **3 s 寬限基準改用 `g_orientation_since`**（使用者立回 +X 的瞬間）而非 `pomo_exit_to_dashboard()` 觸發當下的 `millis()`。原因：為了過濾邊界抖動，立回 +X 仍有 `UPRIGHT_HOLD_MS = 500 ms` 的 hold；若用退出當下時間做基準，500 ms hold 會吃掉一段寬限，使用者擦邊的 2.6 s 立回就不算「誤觸」了。改用 orientation_since 後語意更貼近「使用者按意圖立回的瞬間」。
- **MM:SS 秒位向上取整**（`(remaining_ms + 999) / 1000`）：剛進入 RUNNING 時顯示 "25:00" 整秒，最後一秒顯示 "0:01" 而不是 "0:00" 卡一秒，視覺更自然。
- **landscape 旋轉用 LVGL `lv_disp_set_rotation` + `sw_rotate = 1`**：dashboard ↔ pomo 切換時整片螢幕邏輯旋轉；pomo_view 用 `LV_PCT(100)` 自動配合旋轉後尺寸 (284×240)。dashboard 物件以 hidden 形式留在 portrait 座標系，回 dashboard 時座標即恢復。
- **觸控 wake 防誤觸採「初始化時取 INT 真實值」**：`setup_touch()` 把 `g_tp_int_prev_high = digitalRead(TP_INT)`。若是 touch ext0 喚醒，INT 還壓在低 → prev=false → 不會把「叫醒的那一下」當 falling-edge tap (規格 `touch-input` 的 "No spurious tap on wake")。
