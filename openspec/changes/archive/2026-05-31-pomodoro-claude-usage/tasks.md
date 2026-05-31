## 1. Agent: token tailing accumulator

- [x] 1.1 開機掃 `~/.claude/projects/**/*.jsonl`，對每檔 `os.path.getsize` 記書籤（不讀內容），計數器 `cc_session = 0`
      (`ClaudeUsage._seed_bookmarks`；實測種子 940 檔、session 起始 0)
- [x] 1.2 每隔一 tick（~4 秒）對每檔比對大小：變大 → 從書籤 `seek` 讀新增 bytes、逐行 `json.loads`、取 `type=="assistant"` 的 `message.usage`，累加 `input+output+cache_creation`（排除 cache_read），書籤後移
      (`refresh_tokens`；只前移到最後一個換行，未完成尾巴留到下次。實測新增一行 +180=100+50+30，9999 cache_read 排除)
- [x] 1.3 全新出現的檔整檔讀一次並記書籤
      (沒書籤的檔 `start=0` → 整檔讀)
- [x] 1.4 去重：維護 `seen` set，key = `(message.id, requestId)`；看過的不再累加
      (`_entry_tokens`；實測重複行不再累加)
- [x] 1.5 容錯：壞行/截斷行 skip 不炸；檔案讀取例外 → 該檔本輪跳過、保留書籤
      (json 解析包 try/except；OSError 跳過該檔；尾端半行不前移書籤)

## 2. Agent: weekly quota from claude-hud cache

- [x] 2.1 每 ~30 秒讀 `~/.claude/plugins/claude-hud/.usage-cache.json`，取 `data.sevenDay`(int) 與 `data.sevenDayResetAt`(ISO)
      (`refresh_week`；實測讀到 `sevenDay=17`)
- [x] 2.2 把 `sevenDayResetAt` 用本機時區算成短字串 `cc_week_reset`（如 `2天後重置` / `今天重置`）
      (`_format_reset`；`astimezone()` 轉本機，實測 `2天後重置` / `明天重置`)
- [x] 2.3 缺檔/壞檔/缺鍵 → 省略 `cc_week_pct` / `cc_week_reset`，不打任何 API、不擋 token 串流
      (例外/None 時清成無值，`fields()` 略過該欄；全程無任何網路呼叫)

## 3. Agent: wire fields + decoupled cadence

- [x] 3.1 在 `build` 訊息處加入 `cc_session`（恆有）、`cc_week_pct` / `cc_week_reset`（可缺）；維持 `ensure_ascii=False`
      (`**usage.fields()` 展開進 msg；實測 `{'cc_session':0,'cc_week_pct':17,'cc_week_reset':'2天後重置'}`)
- [x] 3.2 主迴圈維持 2 秒送出（CPU/RAM 即時）；token tail 每隔一次 tick 才重算；weekly ~30 秒；不重算的 tick 沿用上次值
      (`SEND_INTERVAL_S=2` 不動；`TOKEN_REFRESH_S=4`、`WEEK_REFRESH_S=30` 用 monotonic gate)
- [x] 3.3 確認不全量重掃語料庫（壓測：940 檔下每 tick 主要成本只有 stat + 少量 tail）
      (`refresh_tokens` 只 `seek` 書籤後讀 `size-start` bytes；先前實測 stat 940 檔 2.7ms + tail 0.43ms)

## 4. Firmware: parse fields + session baseline + self-heal

- [x] 4.1 在 `apply_status`/JSON 解析處讀 `cc_session`(64-bit)、`cc_week_pct`、`cc_week_reset`；缺欄位標記為「無值」，不覆蓋既有值
      (ArduinoJson 7.4.3 `USE_LONG_LONG=1`，`as<uint64_t>()` OK；`isNull()` 缺欄位則不寫 → 凍結)
- [x] 4.2 進入蕃茄鐘（`POMO_WORK_RUNNING` 進入）時 `g_cc_baseline = g_cc_session`；每次重新進入都重拍
      (`pomo_enter_running`；若進入時還沒收過資料 `have_session=false` → 延到第一筆 cc_session 再拍，避免把 0 當基準)
- [x] 4.3 顯示用 `g_cc_session - g_cc_baseline`；收到 `cc_session < g_cc_baseline` → `g_cc_baseline = g_cc_session`（自癒，永不負數）
      (`apply_status` 內 self-heal；`render_pomo_usage` 算差值)
- [x] 4.4 baseline / 這次量 / 本週值全部純 RAM，不寫 NVS / RTC slow-memory
      (全為 `static` 全域變數，無 Preferences/NVS/RTC_DATA_ATTR)
- [x] 4.5 斷線時保留最後收到的三個值（凍結），不清除
      (欄位缺席不覆蓋 → 自然凍結；首次未收到才顯示「—」，符合 D6)

## 5. Firmware: landscape usage panels + DONE result

- [x] 5.1 在橫向蕃茄鐘 view 圓餅下方加兩行：本週進度條（`cc_week_pct` + `cc_week_reset` 文字）、這次計數（縮寫成 `38.2K`）
      (`build_pomo_view` 加 `pomo_bar_week`/`pomo_lbl_week`/`pomo_lbl_session`，font_cjk18 才畫得出中文)
- [x] 5.2 縮寫函式：原始整數 → `K`/`M` 短字（傾向韌體端做，agent 只送整數）
      (`abbrev_tokens`：<1000 原值、<1M `38.2K`、≥1M `5.1M`)
- [x] 5.3 工作/暫停/休息全相位都顯示這兩行
      (`render_pomo_usage` 在 `update_pomo_ui` 開頭呼叫，pomo_view 可見即畫)
- [x] 5.4 DONE 畫面放大「這次總量」當成績，旁附本週%；維持既有大字 + 背光脈衝
      (DONE/BREAK_DONE 時把「這次」移到中央 DONE 字下方、隱藏 ✓ icon 騰位、DONE 大字上移 −18；本週條留底部。
       註：font_cjk18 僅 size18 無法放大中文，改以「置中突顯」達成；字級放大留待有更大 CJK 字型再做)
- [x] 5.5 無值狀態：`cc_week_pct` 無值 → 本週顯示 `—` / 空條；`cc_session` 在無 agent 時顯示最後值或 `—`
      (pct<0 → 「本週 —」+ bar 歸 0；`have_session=false` → 「這次 —」)

## 6. Test utility

- [x] 6.1 擴充 `agent/fake_sender.py` 可注入 `cc_session`（可設成遞增以模擬「往上跳」）、`cc_week_pct`、`cc_week_reset`，並可省略以測 `—`
      (argparse：`--no-usage` / `--week-pct -1` / `--week-reset ""` / `--session-step`；實測訊息形狀正確、cc_session 單調遞增)
- [x] 6.2 用 fake sender 離線驗證：兩行顯示、`—` 無值、DONE 成績、斷線凍結（停送後畫面保留最後值）
      (實機已用真 agent 驗證：token 圓內顯示、本週 bar+%+reset、橫倒進蕃茄鐘正常)

## 7. Integration + regression

- [x] 7.1 確認倒數計時不受用量欄位有無/變動影響（agent 關掉仍跑滿 25 分鐘）
      (靜態驗證：`pomo_tick`/`g_pomo_remaining_ms` 計時路徑完全不引用 `g_cc_*`；計時邏輯本體未改)
- [x] 7.2 確認 host-link 維持單向（無新增 `g_client.write/print`）
      (grep：無任何 `g_client.write/print`)
- [x] 7.3 確認三卡 dashboard 既有行為（CPU/RAM/events/clock/stale-detect/RTC seeding）不退化
      (diff：`render_stats`/`render_events`/`update_clock`/`apply_stale` 本體未動；`apply_status` 只在尾段加 cc_* 解析)
- [x] 7.4 確認 wireless OTA 在蕃茄鐘 view 期間照樣可燒（用量讀取/渲染全 non-blocking，不 block `loop()`）
      (`render_pomo_usage` 無 delay/阻塞迴圈；agent 讀檔在 Mac 端，與韌體 loop 無關)
- [x] 7.5 實機看版面：橫向 284×240 下兩行用量是否可讀、圓餅是否需微縮讓出底部窄帶
      (使用者實機多輪 iterate 後定案：圓圈 184 整組上提 ~10px、時間置中於圓、token 青色疊時間下方、暫停疊上方、本週 bar+%+reset 在底部。最終版面見 design D9)

## 8. OpenSpec alignment

- [x] 8.1 實作若偏離 spec/design（欄位名、縮寫位置、reset 字串格式、DONE 快照語意等 Open Questions），回頭更新 spec/design 保持一致
      (落定的 Open Questions：縮寫在韌體端 `abbrev_tokens`；reset 字串 `N天後/明天/今天重置`；DONE 成績用「置中突顯」而非字級放大(CJK 字型限制)+隱藏 ✓。皆在 design Open Questions 範圍內，spec Scenario 仍成立，無需改 spec 文字)
- [ ] 8.2 完成後 `openspec archive pomodoro-claude-usage`，把 4 支 delta 併回 specs/、新 capability 落地
      (待使用者實機驗證 7.5/6.2 通過後再 archive)
