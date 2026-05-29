## 1. 硬體準備與校正

- [x] 1.2 加 QMI8658 驅動函式庫（lewisxhe/SensorLib，對齊官方 08 範例）至 `platformio.ini`
- [x] 1.3 初始化 QMI8658（僅 accelerometer），序列印 raw 三軸重力值
- [x] 1.4 實機校正：記錄「立著」與「朝下趴平」對應的軸/正負與門檻，寫成常數＋量測註解

## 2. 電量顯示與提醒（battery-status）

- [x] 2.2 加低電量門檻常數（≤20% 警告、≤10% 危急）與 charging 時清除邏輯
- [x] 2.3 警告色/閃爍視覺（沿用既有 LVGL tick），危急再升級

## 4. 朝向偵測與入睡判定（motion-sleep）

- [x] 4.1 用 1.4 的校正把重力向量分類為 active / face-down，含進睡與醒來雙門檻遲滯
- [x] 4.2 加「持續朝下 ≥ N 秒」debounce 才觸發入睡
- [x] 4.3 入睡前流程：關背光 → AXP2101 關 ALDO3（顯示電軌）→ 進 deep-sleep

## 5. Deep-sleep 喚醒與狀態還原（motion-sleep）

- [x] 5.1 設定雙喚醒源：`esp_sleep_enable_timer_wakeup`(1.5s) + `esp_sleep_enable_ext0_wakeup`(GPIO13, active-low)
- [x] 5.2 開機分流：讀 `esp_sleep_get_wakeup_cause()`；EXT0/正常→完整開機，TIMER→輕量路徑
- [x] 5.3 輕量 timer 路徑：只開 I2C 讀重力；仍朝下→立刻 `esp_deep_sleep_start()`（不開 SPI/顯示/WiFi）
- [x] 5.4 timer 路徑判定已立起→走完整開機路徑（顯示＋WiFi＋host link）
- [x] 5.5 完整開機還原：重開 ALDO3、init 顯示、讀 PCF85063 seed 時鐘（立即正確）、重連 host link、資料顯示 stale 直到 agent 重連

## 6. 整合驗證與收尾

- [x] 6.3 端到端：立著正常顯示 → 朝下入睡 → 立起 ~1–2s 自動醒 → 觸控即時醒；時鐘喚醒後立即正確（實機驗證通過）
- [x] 6.5 更新 README / CLAUDE.md：朝下休眠/喚醒手勢、IMU 朝向校正值、`FACEDOWN_*` 常數位置、OTA 需板子醒著的坑

## 延後 / 後續（不在本次 archive 範圍）

以下項目本次未做或需更多實機條件，留作後續 change；對應的未實作行為已從本 change 的 spec delta 移除，歸檔後 specs 僅反映已交付內容：

- **§3 清醒時觸控喚醒/調亮**（CST816 讀觸控事件、點擊回亮背光/重置 idle）— 延後成獨立 change。
- **§2.4 ES8311 低電量提示音**（spec 列為選配 MAY）— 延後。
- **電池實機驗證**（原 §1.1 接鋰電池確認 AXP2101 偵測、§2.1 三狀態顯示、§6.4 放電過程提醒觸發/清除）— 顯示與提醒程式碼已完成，待接電池實測。
- **§6.1 待機電流量測 / timer 間隔・HOLD 調校**、**§6.2 頻繁 sleep/wake 下 agent 重連驗證** — 待接電池後量測微調。
