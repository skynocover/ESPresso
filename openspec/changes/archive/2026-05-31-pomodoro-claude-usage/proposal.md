## Why

蕃茄鐘現在是一塊純倒數的畫面。作者專注時段幾乎都在用 Claude Code，但「我這週燒了多少額度」「這一輪蕃茄鐘餵了 Claude 多少」這兩個數字平常看不到——要嘛切到 statusline、要嘛去翻 dashboard。把它們放進蕃茄鐘畫面，等於把「專注」跟「我剛剛用掉多少」綁在同一個餘光裡：本週是配額焦慮的安心計（一週才動十幾趴），這次是即時回饋（按完一輪、幾秒後數字往上跳）。

資料全部來自本機、零授權成本：token 量在 `~/.claude/projects/**/*.jsonl` 的 `usage` 區塊，本週配額%則直接讀使用者已裝的 claude-hud plugin 寫好的 cache 檔。韌體維持笨渲染，所有複雜度留在 Mac agent。

## What Changes

- **新增 `claude-code-usage` 能力**：定義「用量」的語意——token 指標（`input + output + cache_creation`，**排除** cache_read）、全專案加總、跨重啟自癒的累積計數器、本週配額%的來源（claude-hud cache）與去重規則
- **Mac agent 抓兩種用量**：(a) tail JSONL 只讀新增 bytes 累加 token；(b) 讀 claude-hud `.usage-cache.json` 拿 `sevenDay`% 與重置時間。打包進現有 host-link JSON 的三個新欄位
- **host-link 協定加三個 optional 欄位**：`cc_session`（累積 token 計數）、`cc_week_pct`（本週配額 0–100）、`cc_week_reset`（agent 預先格式化的重置提示字串）。韌體本來就忽略不認得的欄位，向前相容
- **蕃茄鐘畫面顯示兩行用量**：`本週 ▓▓░ 16% ·2天後重置` + `這次 ▸ 38.2K`，工作/暫停/休息全程顯示；工作結束的 DONE 畫面把「這次總量」放大當成績
- **這次 = 基準線相減**：板子進入蕃茄鐘那刻把 `cc_session` 拍成基準線，之後顯示 `cc_session − 基準線`；不需要反向通道。收到的累積值小於基準線（agent 重開）時自動重設基準線
- **送出節奏拆開**：dashboard 的 CPU/RAM 維持 2 秒即時；token 每隔一次 tick（~4 秒）才重讀；本週% ~30 秒
- **不動計時**：蕃茄鐘倒數仍由 `millis()` 驅動、不依賴 agent。agent 不在/斷線時用量面板顯示最後一次收到的值（凍結），倒數照跑

## Capabilities

### New Capabilities
- `claude-code-usage`：Claude Code 用量的領域語意——token 指標定義、全專案加總、跨重啟自癒的單調累積計數器、本週配額%來源（claude-hud cache）、`message.id + requestId` 去重、區網無驗證的隱私註記

### Modified Capabilities
- `mac-data-agent`：新增「收集 Claude Code 用量並串流」——開機記檔案書籤、每隔一 tick tail 新增 bytes 累加 token、讀 claude-hud cache 拿本週%、把三欄位塞進 host-link 訊息；明文不全量重掃語料庫
- `host-link-protocol`：訊息 schema 加三個 optional 欄位（`cc_session` / `cc_week_pct` / `cc_week_reset`），維持「不認得的欄位忽略」的向前相容
- `pomodoro-timer`：新增「蕃茄鐘期間顯示 Claude 用量面板」需求——進入時拍基準線、全程顯示、DONE 放大成績、累積值倒退時自癒；用量面板不影響倒數計時（仍不依賴 agent）
- `display-firmware`：橫向蕃茄鐘 view 多渲染兩行用量（本週進度條 + 這次計數）；agent 斷線/無資料時凍結顯示最後值

## Impact

- 程式碼：`agent/agent.py` — 加 JSONL tail 累加器、claude-hud cache 讀取、三欄位打包、節奏拆開；`src/main.cpp` — 解析三欄位、蕃茄鐘進入時拍基準線、自癒邏輯、橫向 view 多兩行渲染、DONE 成績
- 工具：`agent/fake_sender.py` — 擴充可注入假的用量欄位，讓 UI/版面可離線調校（不燒真 token）
- 相依：不新增韌體函式庫；agent 不新增 pip 套件（純標準庫讀檔）；**軟相依** claude-hud plugin 提供本週%（缺檔則本週顯示 `—`）
- 規格：新增 1 支 capability、改 4 支 delta
- 風險：tail 計數器跨 agent 重啟/檔案輪替的單調性（用板子端自癒吸收）；橫向小螢幕版面密度（兩行擠在倒數圓餅下方）；claude-hud cache 格式變動（容錯成 `—`）
- 不影響：蕃茄鐘計時準度與 agent 獨立性、host-link 單向性（不開反向通道）、wireless OTA、實體按鈕、現有三卡 dashboard、facedown 睡眠
