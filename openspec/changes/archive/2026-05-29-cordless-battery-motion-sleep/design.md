## Context

ESPresso 韌體（`src/main.cpp`，單檔 LVGL + Arduino_GFX + XPowersLib）目前是 USB 長亮、被動顯示。要轉成可攜無線裝置，需在電池供電下省電並保留「隨手一瞄即有資訊」的體感。

已查官方 repo `waveshareteam/ESP32-S3-Touch-LCD-1.83` 的 `pin_config.h`：只定義 LCD、I2C（SDA=15/SCL=14）、TP_RST=39、TP_INT=13；**QMI8658 INT 未定義**，官方 08 範例以 polling 讀 IMU。亦即 IMU 中斷沒有保證接到可用的 RTC GPIO。AXP2101=0x34、QMI8658=0x6B、PCF85063=0x51。`update_battery()` 已能讀電量/充電並繪 icon。CLAUDE.md 限制：core 釘 3.1.3、`WiFi.setSleep(false)` 不可關。

## Goals / Non-Goals

**Goals:**
- 電池電量顯示（已有）＋低電量/充飽提醒。
- 朝下趴平 → deep-sleep 真省電；立起來（~1–2s 內）或觸控 → 喚醒。
- 喚醒後時鐘立即正確、資料隨 agent 重連恢復。
- 維持「笨臉」：朝向/喚醒/提醒皆本地行為，資料邏輯仍在 Mac agent。

**Non-Goals:**
- IMU 硬體中斷喚醒（INT 腳未證實可用，列為未來升級）。
- 觸控分頁/切卡、手勢操作。
- 計步、語音、麥克風等其他 IMU/音訊功能。
- 加密/認證的 host-link（仍 local-trust）。

## Decisions

**1. 喚醒策略：RTC timer 輪詢 + 觸控 ext0 雙喚醒（方案 B），而非 IMU 中斷。**
- 為何：IMU INT 未在 `pin_config.h` 出現、官方範例用 polling，賭它接到 RTC GPIO 風險高。觸控 TP_INT=GPIO13 是 RTC-capable，可靠。
- 做法：`esp_sleep_enable_timer_wakeup(1–2s)` + `esp_sleep_enable_ext0_wakeup(GPIO13, level)`。timer 醒來只 `Wire.begin` + 讀 QMI8658 加速度一次，朝下→立刻 `esp_deep_sleep_start()` 再睡（不碰 SPI/顯示/WiFi，醒窗數十 ms）；立起→繼續完整 `setup()`。觸控醒（`esp_sleep_get_wakeup_cause()==EXT0`）→ 直接完整開機。
- 替代方案：light-sleep 可快醒、保 RAM，但與 `WiFi.setSleep(false)` 衝突且省電有限；deep-sleep + IMU INT 最佳但賭硬體。

**2. 朝向判定：accel 重力向量 + 遲滯。**
- 立著時某水平軸 ≈ ±1g、Z ≈ 0；朝下趴平時 Z ≈ ±1g。實機校正確定「朝下」對應哪一軸/正負（寫成常數，附量測註解）。
- 遲滯：用兩個門檻（進睡/醒來）+ 連續 N 次取樣一致，避免邊界抖動與拿起瞬間誤判。入睡再加「持續朝下 ≥ 設定秒數」debounce。

**3. 電源軌管理：sleep 前關 ALDO3（顯示電軌），喚醒後重開。**
- 既有 `setup()` 已 `pmu.setALDO3Voltage(3300)` 供顯示。sleep 前 `pmu.disableALDO3()`（或等效）關顯示電源；完整開機路徑重新供電。RTC 走電池備援軌、不可關。
- 注意 timer-poll 醒來只需 I2C（AXP2101/IMU 在主 3V3），不需開 ALDO3。

**4. 低電量提醒分層。**
- `update_battery()` 內加門檻：≤20% 警告色/閃爍、≤10% 升級警告（+ 選配 ES8311 短音）；charging 時清除。閃爍用既有 LVGL timer/tick。音效為選配旗標，預設可關（初期可只做視覺，避免先拉 I2S/codec 依賴）。

**5. IMU 驅動沿用官方範例用的函式庫（SensorLib / Arduino_DriveBus）。**
- 與官方 08 範例一致，降低踩坑風險；只用 accelerometer（gyro 不需要）以省電/省時。

## Risks / Trade-offs

- **[timer 輪詢省電不如純中斷]** → 醒窗壓到最短（不初始化顯示/WiFi/gyro），1–2s 間隔取捨；實機量平均電流再調間隔。
- **[喚醒延遲 1–2s 非瞬間]** → 可接受；觸控提供「想立刻看就敲一下」的即時路徑。
- **[deep-sleep 醒來 WiFi/agent 重連 ~3–5s 資料空窗]** → 時鐘即時正確、其餘顯示 stale（既有 stale 機制），agent 已自動重連。
- **[頻繁 sleep/wake 造成 agent TCP 重連風暴]** → 入睡 debounce 避免抖動；驗證 agent 重連節奏；單 client 限制下確保只跑一支 agent（README 已警示）。
- **[重力軸向判錯導致該睡不睡/該醒不醒]** → 必須實機校正 + 序列日誌印 raw accel 輔助；遲滯 + debounce。
- **[關 ALDO3 後喚醒顯示初始化順序]** → 完整開機路徑重做顯示 init；確認 ST7789 重新 reset/init 正常。
- **[ES8311 音效拉高複雜度]** → 設為選配、預設視覺-only，分階段。

## Migration Plan

1. 接電池，序列印 raw accel 校正朝向軸/門檻。
2. 先做電量提醒（純視覺）——風險最低、`update_battery()` 已有基礎。
3. 加 active 觸控喚醒/調亮（不涉 sleep）。
4. 加朝向偵測 + deep-sleep 進入 + timer/ext0 雙喚醒 + 完整開機還原。
5. sleep 電源軌管理（關 ALDO3）並實機量待機電流、調 timer 間隔。
6. （選配）ES8311 低電量提示音。
- 回退：deep-sleep / 觸控喚醒 / 提醒皆以編譯旗標或常數門檻控制，可單獨關閉退回現行長亮行為。

## Open Questions

- 「朝下」確切對應哪一軸正負？入睡 debounce 秒數與 timer 間隔的最省電組合？（實機校正/量測決定）
- 低電量門檻值（20/10% 是否合適）與是否預設啟用音效？
- 完整開機路徑是否需在 timer-poll 與 ext0/正常 boot 間共用，還是分流以縮短 poll 醒窗？（實作時依量測決定）
