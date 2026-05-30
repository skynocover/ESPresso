## Why

韌體目前是「笨渲染器」，所有狀態都來自 Mac agent。日常使用者想在桌前進入專注時段時，沒有原生的計時器；用手機/Mac 上的蕃茄鐘要切視窗、會被通知打斷。這塊小板子已經有 IMU、觸控、RTC、螢幕，做一個「橫躺即進入專注計時」的零摩擦手勢，硬體成本是零，使用門檻是零，而且**讓裝置第一次擁有自己的狀態**——不依賴 Mac 在線也能工作。

## What Changes

- **新增蕃茄鐘模式**：橫躺 (IMU −Y 軸) 觸發全屏 25 分鐘倒數，立回 +X 退出；自動開始，前 3 秒立回可取消不計
- **觸控升級為運行時輸入**：CST816D 從「只當 deep-sleep ext0 喚醒源」升級成「`loop()` 內也讀」，提供點按 = 開始/暫停的控制
- **朝下升級為全域最高優先**：朝下 (−Z) 3 秒任何狀態都進 deep-sleep，並清空蕃茄鐘進度
- **裝置自走計時**：以 `millis()` 計時，PCF85063 RTC 作為校時與壁鐘時間來源；不依賴 Mac agent、不開反向通道
- **完成提示**：螢幕大字＋背光脈衝；ES8311 音訊延後（不在本次範圍）
- **不動實體按鈕**：BOOT (GPIO0) 與 PWR (GPIO41/AXP2101 PowerKey) 都不接入控制流

## Capabilities

### New Capabilities
- `pomodoro-timer`：橫躺手勢進入/退出、25 分鐘倒數狀態機、觸控開始/暫停、完成提示、與 Mac agent 資料管線並存（背景仍接收但不渲染）
- `touch-input`：CST816D 在 `loop()` 內輪詢/INT 讀取，提供 tap / long-press 事件給 UI 層（不含手勢辨識；橫向座標映射）

### Modified Capabilities
- `motion-sleep`：擴充朝向分類加入「橫躺 landscape」，並把「朝下 → sleep」升級為全域最高優先（任一前景狀態都被中斷、並清空蕃茄鐘進度）
- `display-firmware`：在橫躺進入蕃茄鐘時，UI 由三卡狀態畫面全屏切換到倒數畫面；既有三卡渲染在橫躺期間暫停更新（資料仍接收）

## Impact

- 程式碼：`src/main.cpp` — 新增蕃茄鐘狀態機、CST816D 讀取、橫躺偵測；既有 facedown 流程升級為全域中斷
- 函式庫：不新增依賴（IMU/觸控/RTC/LVGL 都已在用）；**不**啟用 ES8311 I2S
- 規格：新增兩支 spec、改兩支 delta
- 風險：橫躺與朝下兩條 IMU 路徑的優先序需明確；橫躺自動開始可能誤觸發（用前 3 秒寬限解）
- 不影響：Mac agent、TCP host-link 協定、wireless OTA、實體按鈕、現有 facedown 校正常數
