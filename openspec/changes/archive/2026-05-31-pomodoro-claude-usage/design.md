## Context

ESPresso 韌體是笨渲染器：Mac agent 每 2 秒推一行 newline-JSON（`time`/`cpu`/`ram`/`events`），韌體解析後渲染並用 PCF85063 自走時鐘。蕃茄鐘（已實作）橫躺進入、`millis()` 計時、明文不依賴 agent。本變更要在蕃茄鐘畫面疊上兩個 Claude Code 用量數字，且不能破壞上述任何一條既有契約。

關鍵既有事實（已查證本機）：

- **token 資料來源**：`~/.claude/projects/**/*.jsonl`，每個 `type:"assistant"` 行有 `message.usage = { input_tokens, output_tokens, cache_creation_input_tokens, cache_read_input_tokens }`，並帶 `message.id` 與 `requestId`。本機現況 **940 檔 / 421MB**。
- **本週%資料來源**：使用者已裝 claude-hud plugin，它把訂閱用量寫進 `~/.claude/plugins/claude-hud/.usage-cache.json`：`data.sevenDay`（0–100 整數）、`data.sevenDayResetAt`（ISO Z）。這份是 claude-hud 已經付過 Keychain + OAuth + token 換新代價的成果。
- **host-link 單向**：韌體從不回寫 agent（grep `g_client` 只有 read/accept/stop）。schema 明訂「不認得的欄位忽略」。
- **credential 在 macOS Keychain**（`Claude Code-credentials`），無明文檔。

Stakeholder：唯一使用者（作者，桌前）。約束來自硬體（240×284 小螢幕、橫向 284×240）、agent 跑在 launchd 背景（無 GUI 可按授權框）、韌體哲學（盡量笨、單向、不依賴 agent 計時）。

## Goals / Non-Goals

**Goals:**
- 蕃茄鐘畫面顯示「本週配額%」與「這次 token 量」，全程（工作/暫停/休息）可見，DONE 放大這次成績
- 用量資料全部本機、零授權、launchd 背景可跑
- 讀檔成本可忽略（不全量重掃 421MB）
- 倒數計時與 agent 獨立性、host-link 單向性、三卡 dashboard 行為皆不退化

**Non-Goals（本次明確排除）:**
- 自己打 Anthropic usage API（授權/換 token/私有 endpoint 太脆，見 D2）
- 顯示金額（USD）——本次只算 token，省掉 model→價格表
- 5 小時視窗——claude-hud cache 雖有 `fiveHour`，本次只用 `sevenDay`
- 「這次」只算工作不算休息——本次工作+休息全算（D5）
- 斷線時的陳舊提示（變灰/小點）——本次凍結最後值即可，不加 staleness UI（見 D6）
- 把用量寫進 NVS / 反向推給 Mac / 多裝置加總
- 本週進度條的「自訂目標分母」——直接用 claude-hud 的%當條，不引入自設目標

## Decisions

### D1: token 指標 = `input + output + cache_creation`，排除 cache_read

**選擇**：每個 assistant 回合的 `input_tokens + output_tokens + cache_creation_input_tokens` 累加；**丟掉** `cache_read_input_tokens`。

**為什麼**：cache_read 通常是 output 的 ~100 倍（實測一行：output 211 vs cache_read 18,999），且是「重複讀快取」的便宜 token，計入會讓數字灌水到失去意義。排除後數字代表「真正新做的工」（新輸入 + 新寫入快取的內容 + 生成），穩定、有感。model 不分（因為算 token 不算錢，免維護價格表）。

### D2: 本週%讀 claude-hud cache 檔，不自己打 API

**選擇**：agent 讀 `~/.claude/plugins/claude-hud/.usage-cache.json` 的 `data.sevenDay` 與 `data.sevenDayResetAt`。

**為什麼不自己打 API**：(1) credential 在 Keychain，非 Claude 程式去拿會跳存取授權框，launchd 背景無人可按 → 靜默失敗；(2) usage endpoint 私有未公開，Anthropic 可隨時改；(3) OAuth token 幾小時過期，得重做 Claude Code 的換新流程。claude-hud 已經把這三件事做完並寫成一個使用者自己的檔，agent 只 `json.load` → 零授權、零網路、背景可跑。

**代價**：軟相依 claude-hud。缺檔/壞檔/格式變 → 本週欄位省略，韌體顯示 `—`。可接受：使用者本來就在用它，且讀不到通常代表「最近沒用 Claude、數字本來也沒變」。

**為什麼本週用%不用 token**：訂閱週配額上限是不公開的，「token / 上限」需要瞎猜分母；`sevenDay` 本身就是百分比，直接當進度條、且天然對齊訂閱重置窗。

### D3: 這次 = 單調累積計數器 + 板子端基準線相減

**選擇**：agent 維護一個「自開機以來只增的 token 累積數」`cc_session`，每行送出。韌體在**進入蕃茄鐘那刻**把當下的 `cc_session` 拍成 `baseline`，畫面顯示 `cc_session − baseline`。

**為什麼**：host-link 單向，Mac 不知道蕃茄鐘何時開始。但韌體知道。只要 agent 一直送一個單調成長的數，韌體自己減基準線就得到「這段期間長了多少」。**不需要反向通道**，維持 host-link 單向契約。絕對值無意義，只有差值有意義。

**累積計數器的種子**：開機時對所有檔 `stat` 記下目前大小當「書籤」（不讀內容），計數從 0 起。之後每隔一 tick 只讀書籤之後新增的 bytes、解析新行、累加、書籤後移；出現全新檔則整檔讀（還很小）。**明文不全量重掃**那 421MB。

### D4: 板子端自癒——累積值倒退就重設基準線

**選擇**：韌體若收到 `cc_session < baseline`，把 `baseline` 重設為當前 `cc_session`（並可把已顯示的這次量歸零或保留，取小）。

**為什麼**：累積計數器理論單調，但 agent 中途重啟（計數歸 0）、檔案被輪替/刪除時會突然變小。沒有自癒就會顯示負數或暴量。自癒把「計數器倒退」當成「新的起點」，最壞只是那一次 session 少算一點。韌體照樣笨（一個 if）。

**為什麼不在 agent 端硬保證單調**：agent 跨重啟要保證單調得持久化或全量重算（又回到掃 421MB）。把韌性放在板子端（一行 if）比放在 agent 端便宜得多。

### D5: 這次涵蓋工作+休息全程，基準線在進入時拍

**選擇**：基準線在橫躺進入蕃茄鐘（`POMO_WORK_RUNNING` 進入）那刻拍一次；直到立回直立退出蕃茄鐘前，這次量一路累加，跨越 work→DONE→break 各相位都不重拍。每次重新進入蕃茄鐘 = 重拍基準線（這次歸零）。

**為什麼全算**：語意單純——「我倒下板子這整段餵了 Claude 多少」。休息時仍可能用 Claude，分段凍結反而要多一套狀態。3 秒寬限內誤觸取消時，基準線白拍無害（沒顯示多久）。

### D6: 斷線時凍結最後值，不加 staleness 提示

**選擇**：agent 斷線/無新訊息時，三個用量欄位保留**最後一次收到的值**不動（本週%、這次量都凍結）。本次**不**加變灰、小點等「資料停了」的視覺提示。

**為什麼**：使用者選擇保持畫面單純。後果是畫面看不出資料是否停更——已知並接受。若日後誤判困擾再加低調提示（design 留此伏筆，spec 不訂 staleness 行為）。

**注意**：這跟既有三卡 dashboard 的 stale-detect 是兩套——dashboard 的陳舊處理不變；用量面板只在蕃茄鐘 view 出現，採凍結。

### D7: 送出節奏拆開

**選擇**：保持 agent 主迴圈每 2 秒送一行（dashboard CPU/RAM 即時）；token tail 每 **10 秒**重算；claude-hud cache 每 ~30 秒讀一次。三欄位的值在不重算的 tick 沿用上次結果。

**為什麼**：讀檔雖便宜（實測每 tick：stat 940 檔 2.7ms + tail 8KB 0.43ms + claude-hud 0.1ms），但 dashboard 即時感值得保留。token 改 10 秒的關鍵理由：**Claude 一個回合結束才把 usage 寫進 JSONL**，而一回合動輒數十秒到數分鐘，5s/10s 對「實時感」毫無差別，10s 還省一半 stat。拆開 = 各取所需。

### D8: 三欄位的線路型別與格式

- `cc_session`：整數，自 agent 開機以來的累積 token（D1 指標、全專案）。可能很大 → 韌體用 **64-bit** 存（或 agent 每日歸零，由 D4 自癒吸收）。
- `cc_week_pct`：整數 0–100，本週配額已用%（claude-hud `sevenDay`）。讀不到 → 欄位省略。
- `cc_week_reset`：字串，agent 依 `sevenDayResetAt` **預先格式化**好的精確重置倒數（對齊 claude-hud HUD：`reset in 1d 23h` / `reset in 3h` / `reset soon`）。時區換算、語言都在 Mac 端定，韌體只畫（守「韌體笨」）。讀不到 → 欄位省略。

三者皆 optional。`cc_session` 正常一定有（token tail 不依賴 claude-hud）；`cc_week_*` 依賴 claude-hud cache 可缺。

### D9: 版面——倒數仍是主角，用量在下方窄帶

橫向 284×240。圓餅倒數 + 大字 MM:SS 維持視覺主角；下方讓出一條窄帶放兩行：

**最終版面（實機多輪 iterate 後定案）**：本次 token 疊在圓內時間下方、本週在底部，圓圈整組略上提讓構圖平衡：

```
┌──────────────────────────────────────────────┐
│  ‖                  ← 暫停/BREAK (時間上方,圓內) │
│            ╭────────────────╮                 │
│           │      23:55       │  ← 時間 (置中於圓)│
│           │      34.4K       │  ← 本次 token (青)│
│            ╰────────────────╯                 │
│   ▓▓▓▓▓░░░░░░░  18%  reset in 1d 23h           │
└──────────────────────────────────────────────┘
```

- 圓圈 184、置中於畫面但整組上提 ~10px（時間置中於圓圈內）。`MM:SS` 白、`34.4K` 青色疊其下；暫停 ‖ / 休息 `BREAK` 疊其上。
- 底部：本週進度條（綠）+ `18%  reset in 1d 23h`（去掉「本週」字與 cache_read 式贅字，只留 %＋精確 d/h 倒數）。
- **DONE**：時間變「DONE/BREAK」、token 留原位 → 自然成「成績」（不需另搬位置）。
- 缺值（`—`）、暫停、休息相位都照顯示；數字縮寫（`34.4K`/`5.1M`）由韌體 `abbrev_tokens` 做（agent 只送原始整數，較好 debug）。

## Risks / Trade-offs

- **累積計數器跨 agent 重啟非單調** → D4 板子端自癒（counter < baseline 重設）吸收，不在 agent 端扛
- **claude-hud cache 格式/路徑變動** → 容錯：讀不到或缺鍵 → 本週欄位省略 → 韌體 `—`；不讓它擋住 token 串流
- **橫向小螢幕版面密度** → 兩行用量字級需小但可讀；圓餅可能要微縮讓出底部窄帶，實機調
- **斷線凍結會誤導**（D6 已知接受）→ 留伏筆，不在本次 spec 訂 staleness
- **隱私**：TCP line-server 區網無驗證，同網段理論上讀得到 token 量；敏感度低，本次不處理，列為 capability 註記
- **讀檔 I/O**：實測可忽略（檔尾在 page cache、純讀不磨 SSD）；唯一會痛的是全量重掃——D3 明文不做
- **OTA / loop 存活**：用量讀取與渲染全 non-blocking，不得 block `loop()`（沿用既有風格）

## Migration Plan

協定為**向後相容的加欄位**（既有韌體收到新欄位本來就忽略）。部署 = agent 更新（純讀檔，無新相依）+ 韌體 OTA 一次。Rollback = 燒回上一版 firmware.bin、還原 agent.py；舊 agent 不送新欄位時，新韌體用量面板顯示 `—`，蕃茄鐘其餘行為不變。

無資料遷移——用量不持久化，掉電/睡眠後從 dashboard 起頭、基準線重拍。

## Open Questions

1. **數字縮寫在韌體還是 agent**？傾向 agent 送原始整數、韌體縮寫成 `38.2K`（較好 debug、agent 端不必猜顯示寬度）。實作時定。
2. **`cc_week_reset` 字串語言/格式**：`2天後重置` vs `2d` vs `06/02 重置`？實機看小螢幕寬度定。
3. **這次量在 DONE 後若使用者停在休息相位繼續用 Claude**，DONE 成績是「工作結束當下」的快照還是持續更新到退出？傾向 DONE 顯示「進入 DONE 當下」的快照（成績定格），休息相位的小字行才持續跳。實作時確認。
