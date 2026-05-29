## Why

ESPresso 目前是 USB 供電的長亮桌面顯示器。要讓它能「拿著走」變成無線裝置，必須解決兩件事：靠電池供電時要能顯示電量並在低電量時提醒；以及在不看的時候真正省電——光關背光不夠（WiFi 連線待機仍吃數十 mA），得進 deep-sleep。配合一個直覺的開關手勢（朝下趴平＝睡、立起來＝醒）與點擊喚醒，才能在可接受的電池續航下保持「隨手一瞄就有資訊」的體驗。

## What Changes

- **電池電量提醒**：`update_battery()` 已會顯示 AXP2101 的電量 icon+% 與充電狀態；新增低電量門檻提醒（如 ≤20%／≤10% 圖示閃紅，可選用 ES8311 發出提示音），以及充飽提示。
- **朝下深度休眠**：當 QMI8658 偵測到裝置持續「朝下趴平」達設定秒數（含遲滯防抖）→ 進入 `esp_deep_sleep`，由 AXP2101 關閉顯示電軌（ALDO3）降低待機電流。
- **立起來/觸控喚醒（方案 B）**：deep-sleep 設定雙喚醒源——RTC timer（每 1–2s）＋觸控 ext0（TP_INT=GPIO13）。timer 醒來只初始化 I2C 快速讀重力判朝向：仍朝下→立刻回 deep-sleep（醒窗數十 ms、不開顯示/WiFi）；已立起→完整開機（顯示＋WiFi＋等 agent 重連）。觸控醒則直接完整開機。
  - 採輪詢喚醒而非 IMU 中斷，因為官方 `pin_config.h` 未定義 QMI8658 INT、官方 08 範例亦用 polling，無法確定 INT 有拉到可用的 RTC GPIO。
- **觸控喚醒/調亮（active 時）**：裝置清醒時點一下螢幕（CST816）→ 回亮背光並重置現有 idle dim 計時。不做分頁/切卡（240×284 小螢幕，三張卡同屏即可）。
- PCF85063 RTC 經電池不斷電，deep-sleep 醒來讀 RTC seed 時鐘立即正確；CPU/RAM/行事曆顯示 stale，待 agent 重連（~3–5s）恢復。

## Capabilities

### New Capabilities
- `battery-status`: 從 AXP2101 讀取電池電量／充電狀態並在 UI 呈現，低電量與充飽時提醒（視覺，選配音效）。
- `motion-sleep`: 以 QMI8658 重力向量做朝向偵測，朝下進 deep-sleep、立起或觸控喚醒（timer 輪詢 + ext0 雙喚醒），並在睡眠時管理電源軌與喚醒後狀態還原。

### Modified Capabilities
- `display-firmware`: 既有「Backlight dimming and power saving」需求擴充——新增清醒狀態下的觸控喚醒/調亮，且背光控制需與 deep-sleep 進出協調。

## Impact

- 韌體 `src/main.cpp`（單檔 LVGL + Arduino_GFX + XPowersLib）：新增 IMU（QMI8658）讀取、deep-sleep 進出與喚醒源設定、觸控喚醒處理、低電量提醒；`update_battery()` 擴充門檻邏輯。
- 依賴：需要 QMI8658 驅動函式庫（SensorLib / Arduino_DriveBus，官方範例使用）；ES8311 音效為選配，若做需 I2S/codec 初始化。
- 硬體：需實際接上鋰電池至板上 1.2mm 座；重力軸向正負需實機校正。
- AXP2101：新增 sleep 時關閉顯示電軌、喚醒後重新供電的電源管理。
- `mac-data-agent`：deep-sleep 造成 TCP 頻繁斷線；README 述 agent 已支援自動重連，需驗證重連節奏無副作用（預期無 spec 變更）。
- 「笨臉」原則維持：朝向判定／喚醒／電量提醒皆為本地行為，資料邏輯仍在 Mac agent。
