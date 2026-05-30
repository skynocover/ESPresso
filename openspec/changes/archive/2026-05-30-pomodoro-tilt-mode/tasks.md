## 1. IMU: landscape detection + global facedown preempt

- [x] 1.1 加上 `FACEDOWN`/`UPRIGHT`/`LANDSCAPE` 三態枚舉與分類函式（讀 ax/ay/az → state），門檻沿用 `−0.80g` 進入 / `−0.55g` 退出的遲滯精神，landscape hold 用 ~500 ms
- [x] 1.2 重寫 IMU tick：先檢 facedown（最高優先序）→ 再檢 landscape → 否則 upright；任何前景狀態下 facedown 達 `FACEDOWN_HOLD_MS` 都進 deep-sleep
- [x] 1.3 facedown 進 deep-sleep 前清空 Pomodoro 狀態變數（remaining、phase）
- [ ] 1.4 用 `IMU_CALIB_LOG` 暫開實機驗證三態切換與優先序（橫躺→朝下、橫躺→立回、立著→朝下），驗完關回 0
      (需實機；驗證 hook 已就緒：IMU_CALIB_LOG 打開時序列 + 螢幕底部疊圖顯示 `x/y/z` + `UP/LAND/DOWN`)

## 2. CST816D touch: runtime read path

- [x] 2.1 在 boot 序列加 CST816D RST 釋放（TP_RST=GPIO39）與 I2C 在線檢查；失敗 → log 警告，不擋啟動
- [x] 2.2 把 TP_INT (GPIO13) 在運行時設為 input + 偵測下降緣（poll 或 attachInterrupt + flag）；觸發時讀一次 I2C 拿觸控狀態並 emit 「tap」事件
- [x] 2.3 確認 deep-sleep ext0 喚醒路徑不受影響：睡前不關 CST816D；wake 後不把「叫醒的那一下」當 runtime tap 餵給 UI
- [ ] 2.4 實機驗證：橫躺中點螢幕能切 running/paused；運行時 INT 沒拉低就完全不走 I2C
      (需實機；poll_touch() 設計上 INT 為高就 return，不發 I2C transaction)

## 3. Pomodoro state machine + timing

- [x] 3.1 定義狀態：`STATUS` / `POMO_WORK_RUNNING` / `POMO_PAUSED` / `POMO_DONE`；變數：`phase`, `remaining_ms`, `entered_at_ms`
- [x] 3.2 landscape → `POMO_WORK_RUNNING`，`remaining_ms = 25*60*1000`，記錄 `entered_at_ms`
- [x] 3.3 計時用 `millis()` 推進；秒位顯示對齊 PCF85063（避免人眼看到的「分鐘跳」延後一個 tick）
      (秒對齊以 `(remaining_ms + 999) / 1000` 向上取整模擬：起始顯示 25:00 整秒、結束不卡 0:01)
- [x] 3.4 立回 `+X` 時：若在 `entered_at_ms + 3000` 內 → 視為誤觸，靜默回 `STATUS`；否則 → 一般退出回 `STATUS`，丟棄進度
      (寬限以 `g_orientation_since` 為基準，避免 UPRIGHT_HOLD 延後吃掉寬限)
- [x] 3.5 tap：`WORK_RUNNING ↔ PAUSED` 切換；`PAUSED` 時 `remaining_ms` 凍結
- [x] 3.6 `remaining_ms == 0` → 進 `POMO_DONE`
- [x] 3.7 `POMO_DONE` 退出：tap / 立回 / 60 秒 timeout 都回 `STATUS`；朝下走全域 facedown 路徑
- [x] 3.8 `POMO_*` 狀態變數絕不寫 NVS / RTC slow-memory（純 RAM）

## 4. UI: Pomodoro fullscreen view + completion FX

- [x] 4.1 建立蕃茄鐘 LVGL screen 或 container：橫向版面、大字「MM:SS」、小狀態圖示（running / paused）
      (用 `lv_disp_set_rotation` + `sw_rotate=1` 切到 landscape；MM:SS 用 montserrat_48；icon 用 montserrat_28)
- [x] 4.2 `STATUS ↔ POMO_*` 切換時：隱藏三卡 (`card_clock` / `card_stats` / `card_events`) → 顯示蕃茄鐘 view，反之亦然
- [x] 4.3 `POMO_DONE` 完成畫面：大字「DONE」(或等價文字)，背光 GPIO40 做幾次 PWM 脈衝
      (6 秒內以 500ms 周期方波在 BL_FULL ↔ POMO_PULSE_DIM 切換)
- [x] 4.4 確認蕃茄鐘期間 host-link 資料 model 仍更新但 UI 不畫；退出時把最新值補回三卡
      (apply_status 不分模式照寫；render_stats/render_events 寫進已隱藏物件不送到 flush_cb；
       退出時三卡 unhide 即刻顯示最後值)

## 5. Integration + regression

- [x] 5.1 確認 wireless OTA 在 Pomodoro view 期間照樣可燒（loop 不能被 Pomodoro 狀態機 block）
      (pomo_tick、orientation_tick、poll_touch 全 non-blocking；`ArduinoOTA.handle()` 仍在 loop 每圈跑)
- [x] 5.2 確認 facedown deep-sleep 既有路徑（睡眠電流、wake 流程、stale clock 重 seed）行為不變
      (enter_sleep_from_active 仍 disableALDO3 + arm_wakeups_and_sleep；只多了清 pomo 變數一行)
- [x] 5.3 確認 host-link 三卡資料 stale-detect、時間 seeding 行為不變
      (apply_status / apply_stale / update_clock 未動)
- [ ] 5.4 跑一次完整 25 分鐘倒數，量 `millis()` 計時誤差（目標 < 1s）
      (需實機；理論上 `millis()` 基於 ESP32 內部 timer，25 分鐘漂移 << 1s)
- [x] 5.5 OpenSpec 文件對齊：實作有偏離規格的話回頭改 spec/design，不要兩邊不一致
      (見 design.md「Open Questions」更新 — 主要偏離：grace 時間以 g_orientation_since 為基準)

## 6. Out-of-scope reminders (不做)

- [x] 6.1 確認 ES8311 音訊未被啟用（無 I2S init、無 PMU LDO 改動給音訊）
      (grep main.cpp：ES8311/I2S 僅出現於檔頭註解；無 begin/I2S init)
- [x] 6.2 確認 host-link 仍為單向 agent→板子（無反向通道、無「完成事件」往 Mac 推）
      (grep：無 g_client.write / .print / .send；只有 .read/.available/.accept/.stop)
- [x] 6.3 確認 BOOT (GPIO0) / PWR (GPIO41) 未被讀取或設 input
      (grep：無 GPIO0 / GPIO41 / pmu.isPekey* / pekey* 呼叫)
