// ESPresso — 桌面狀態顯示器 (Waveshare ESP32-S3-Touch-LCD-1.83)
//
// 板子：1.83" LCD，240x284，ST7789P 標準 SPI，CST816D 觸控，
//       AXP2101 PMU、ES8311/ES7210 音訊、QMI8658 IMU、PCF85063 RTC。
//
// 顯示 SPI：DC=4 CS=5 SCK=6 MOSI=7 RST=38   背光：BL=40 (active-high, LEDC PWM)
// I2C：SDA=15 SCL=14 (AXP2101 0x34 / 觸控 / IMU / RTC 0x51 共用)
//
// 架構：韌體只負責「渲染」。資料 (cpu/ram/events/time) 由 Mac 端 agent
// 透過 USB CDC 以 newline-delimited JSON 餵進來。時鐘走板上 PCF85063 RTC，
// 由協定的 time 欄位 seed 一次後 free-run；agent 掛掉時鐘照走 (新鮮度指示)。

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include "SensorQMI8658.hpp"
#include "pcf85063.h"
#include "secrets.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
// 音訊：ES8311 codec register init (audio-driver lib) + ESP-IDF legacy i2s.h DMA。
//        蕃茄鐘結束時 pomo_beep() 推三段 1kHz LUT-正弦。詳見 audio_init() 上方註解。
//        為什麼用 legacy i2s.h 不用新版 i2s_std.h：arduino-esp32 開了 PSRAM,
//        新版 GDMA 在 i2s_new_channel 把 handle 配到 PSRAM 又規定 ISR context
//        必須在 internal SRAM → INVALID_ARG (gdma_register_tx_event_callbacks)。
//        legacy API 不走 GDMA 那條檢查路徑,跑得過。
#include "AudioBoard.h"
#include "driver/i2s.h"

// --- 顯示 SPI 腳位 (ST7789P) ---
#define LCD_DC 4
#define LCD_CS 5
#define LCD_SCK 6
#define LCD_MOSI 7
#define LCD_RST 38
#define LCD_BL 40
#define LCD_WIDTH 240
#define LCD_HEIGHT 284

// --- I2C ---
#define PIN_I2C_SDA 15
#define PIN_I2C_SCL 14

// --- 音訊 (Waveshare 官方 BSP esp32_s3_touch_lcd_1_83.h) ---
// ES8311 codec 在 I2C 0x18 (與其他 device 共用 SDA=15/SCL=14)，I2S 走獨立 5 條線。
// PA_EN active-high：idle 拉 LOW 省電/避雜訊，beep 期間才拉 HIGH。
#define PIN_I2S_MCLK 16
#define PIN_I2S_BCLK 9
#define PIN_I2S_LRCK 45
#define PIN_I2S_DOUT 8
#define PIN_I2S_DIN  10        // 板上 mic；目前用不到但 lib API 要傳
#define PIN_AUDIO_PA 46

// --- 觸控 CST816D (兼 deep-sleep ext0 喚醒源 + 運行時 tap 來源；INT active-low) ---
#define TP_INT 13           // RTC-capable GPIO，可當 deep-sleep ext0 喚醒
#define TP_RST 39
#define CST816_ADDR 0x15    // CST816D I2C 位址 (Waveshare 板/官方資料一致)

// --- 行為參數 ---
static constexpr uint32_t STALE_TIMEOUT_MS = 8000;    // 超過此時間沒收到訊息 → 標記資料過期 (變灰)
static constexpr int RTC_RESYNC_DRIFT_SEC = 2;        // RTC 與收到的時間差超過此值才重設
static constexpr int MAX_EVENTS = 5;                  // 最多顯示幾筆行事曆事件
// 背光 (LEDC PWM, 8-bit)
static constexpr uint8_t BL_FULL = 255;
static constexpr uint8_t BL_DIM = 40;
static constexpr int NIGHT_START_HOUR = 23;           // 夜間調暗：23:00 起
static constexpr int NIGHT_END_HOUR = 7;              //          07:00 止
static constexpr uint32_t IDLE_DIM_MS = 120000;       // 無資料 > 2 分鐘 → 調暗
static constexpr uint32_t IDLE_BLANK_MS = 600000;     // 無資料 > 10 分鐘 → 熄背光
static constexpr uint32_t BL_LEDC_FREQ = 5000;
static constexpr uint8_t BL_LEDC_RES = 8;
// 電量提醒門檻 (未充電時)：≤WARN 紅色恆亮、≤CRIT 紅色每秒閃爍升級警告
static constexpr int BATT_WARN_PCT = 20;
static constexpr int BATT_CRIT_PCT = 10;

// 校正/除錯用：開為 1 時把 QMI8658 三軸重力 (g) 每秒印到序列 + 螢幕底部疊圖。
// 朝向門檻已校正寫死 (見下方常數 + README「IMU 朝向校正」)，平時關閉；
// 之後要加橫躺等新姿勢、或重新校正時再開回 1。
#define IMU_CALIB_LOG 0

// --- 朝向偵測門檻 (實機校正 2026-05-29, QMI8658, 單位 g) ---
//   立著(active):  x≈+1.01 y≈0    z≈0    → 用 ax 判定 (upright)
//   朝下(sleep):   x≈0     y≈0    z≈-1.00 → 用 az 判定 (facedown，最高優先)
//   左倒(pomo):    x≈0     y≈-0.97 z≈0   → 用 ay 判定 (landscape)
// 各軸獨立做門檻+遲滯；分類器優先序：facedown > landscape > upright > 保持前態。
static constexpr float FACEDOWN_ENTER_G  = -0.80f;  // az 低於此 → 朝下
static constexpr float FACEDOWN_EXIT_G   = -0.55f;  // az 高於此 → 非朝下 (遲滯帶: -0.80~-0.55)
static constexpr float LANDSCAPE_ENTER_G = -0.80f;  // ay 低於此 → 橫躺 (左倒)
static constexpr float LANDSCAPE_EXIT_G  = -0.55f;  // ay 高於此 → 非橫躺
static constexpr float UPRIGHT_ENTER_G   = +0.80f;  // ax 高於此 → 立著
static constexpr float UPRIGHT_EXIT_G    = +0.55f;  // ax 低於此 → 非立著
static constexpr uint32_t FACEDOWN_HOLD_MS  = 3000; // 持續朝下達此時間才入睡 (防誤觸)
static constexpr uint32_t LANDSCAPE_HOLD_MS = 500;  // 持續橫躺達此時間才進蕃茄鐘
static constexpr uint32_t UPRIGHT_HOLD_MS   = 500;  // 持續立著達此時間才從蕃茄鐘退出 (防晃動誤退)
static constexpr uint64_t SLEEP_POLL_US = 1500000;  // deep-sleep 每 1.5s timer 喚醒輪詢朝向

// --- 蕃茄鐘行為參數 ---
static constexpr uint32_t POMO_WORK_MS         = 25UL * 60UL * 1000UL;  // 單次 work session 長度 (經典 25 分鐘)
static constexpr uint32_t POMO_BREAK_MS        = 5UL * 60UL * 1000UL;   // 休息長度 (經典 5 分鐘短休息)
// 過場時長：DONE 是「中繼站」(下一段是 BREAK)，BREAK_DONE 是「完整 cycle 終點」。
// 不同長度本身也是訊號分化：短過場 = 還沒結束、長過場 = 真結束 + 慶祝。
// 注意：時間計時從 beep 結束才起算 (見 pomo_enter_done / pomo_break_done)，
//       所以這裡是「使用者實際看到過場字的時間」，不含 beep 阻塞期。
static constexpr uint32_t POMO_WORK_TO_BREAK_HOLD_MS = 1500;            // 工作完短過場 → 接 BREAK
static constexpr uint32_t POMO_CYCLE_DONE_HOLD_MS    = 3000;            // 整個 cycle 完成 → 回 dashboard (= POMO_FADE_DURATION_MS, 動畫剛好播完)
static constexpr uint32_t POMO_CANCEL_GRACE_MS = 3000;                  // 進入後 3s 內立回視為誤觸
// (舊的 POMO_DONE_TIMEOUT_MS 已移除：work-DONE 不再「停 90s 等使用者」, 改成 1.5s 短過場後自動進 BREAK)
// 完成提示：sine ease 淡入淡出 (BL_FULL→DIM→FULL)，1 個 cycle 1500ms，總共 2 個 cycle = 3s
static constexpr uint32_t POMO_FADE_CYCLE_MS    = 1500;                 // 一次完整 fade (full→dim→full)
static constexpr uint32_t POMO_FADE_DURATION_MS = 3000;                 // 整段完成動畫長度 (= 2 × cycle, 對齊 POMO_CYCLE_DONE_HOLD_MS)
static constexpr uint8_t  POMO_PULSE_DIM        = 80;                   // fade 谷底亮度 (不到 0 避免黑屏感)

// --- 網路 ---
static constexpr uint16_t TCP_PORT = 3333;            // host-link 協定的 TCP 線路埠
static constexpr char MDNS_HOST[] = "espresso";       // → espresso.local

XPowersPMU pmu;
PCF85063 rtc;
SensorQMI8658 imu;
bool g_rtc_ok = false;
bool g_pmu_ok = false;
static bool g_audio_ok = false;     // ES8311 codec + I2S init 結果；false → pomo_beep() no-op
bool g_imu_ok = false;
bool g_touch_ok = false;             // boot 期 I2C probe 成功 → 運行時讀 CST816D
static bool g_tp_int_prev_high = true; // INT 上次取樣 (true=高=無觸)；初值見 setup_touch()

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, GFX_NOT_DEFINED /* MISO */);
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, LCD_RST, 0 /* rotation */, true /* IPS */, LCD_WIDTH, LCD_HEIGHT);

// --- LVGL 繪圖緩衝 (放 PSRAM；1/7 螢幕高度) ---
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lv_buf = nullptr;
static lv_disp_drv_t disp_drv;

// 含中文字的 LVGL 字型 (src/font_cjk18.c，由 lv_font_conv 從 Arial Unicode 產生)
extern const lv_font_t font_cjk18;

// --- UI 物件 (頂部狀態列 + 三張卡片：時鐘 / 系統 / 行事曆) ---
static lv_obj_t *card_clock, *card_stats, *card_events;
static lv_obj_t *lbl_clock;
static lv_obj_t *lbl_date;            // 頂部狀態列左：日期 + 星期
static lv_obj_t *status_box;          // 頂部狀態列右：連線 icon + 電池 (flex row)
static lv_obj_t *lbl_status;          // 連線狀態 icon (WiFi/USB/離線)
static lv_obj_t *lbl_batt;            // 電池 icon+%；偵測不到電池就隱藏 (flex 自動回收空間)
static lv_obj_t *bar_cpu, *bar_ram;
static lv_obj_t *lbl_cpu_val, *lbl_ram_val;
static lv_obj_t *next_marker;         // 下一件事左側的 accent 直條
// 每列拆兩個 label：時間前綴 (分色) + 標題 (純文字)。標題獨立成 label 後不走
// recolor，使用者標題裡的 '#' 原樣顯示、不再被當成 LVGL 顏色指令而遭刪除。
static lv_obj_t *lbl_ev_time[MAX_EVENTS];
static lv_obj_t *lbl_ev_title[MAX_EVENTS];
#if IMU_CALIB_LOG
static lv_obj_t *lbl_debug = nullptr;  // 校正用：螢幕上即時顯示三軸重力 (免接 USB 也能讀)
#endif

// 蕃茄鐘 fullscreen view (隱藏在 dashboard 之上；進入 POMO_* 模式時 show)
static lv_obj_t *pomo_view = nullptr;     // 全屏容器，覆蓋三卡
static lv_obj_t *pomo_arc  = nullptr;     // depleting wedge (Time Timer 風)：滿圓→空，背景餘光感知用
static lv_obj_t *pomo_lbl_time = nullptr; // 「MM:SS」大字 (疊在 wedge 中央)
static lv_obj_t *pomo_lbl_icon = nullptr; // 狀態 icon：跑步中隱藏；PAUSED=‖、DONE=✓ (只在「異常/終態」現身)
static lv_obj_t *pomo_bar_week = nullptr;    // 本週配額進度條 (底部)
static lv_obj_t *pomo_lbl_week = nullptr;    // 本週「18%  reset in 1d 23h」/「—」(bar 上方)
static lv_obj_t *pomo_lbl_session = nullptr; // 本次「usage 34.4K」(圓內，時間下方)

// 行事曆內排版 (卡片內座標)
static constexpr int EV_X = 8;        // 時間前綴左縮排 (留 next_marker 的位置)
static constexpr int EV_TIME_W = 96;  // 時間前綴欄寬 (足夠放 "今天 14:30")；標題接其右
static constexpr int EV_Y0 = 4;       // 第一筆事件 y (拿掉標題列後可從卡片頂端開始)
static constexpr int EV_STEP = 21;    // 每行 y 間距 (≈font_cjk18 行高 22，不再重疊)

// 卡片底色 / 強調色 (COL_CARD_BG 是時鐘冒號 recolor 用的純整數；其餘為 lv_color_t)
#define COL_CARD_BG 0x16181c
#define COL_CARD lv_color_hex(COL_CARD_BG)
#define COL_BORDER lv_color_hex(0x262a30)  // 卡片細邊框，提供層次
#define COL_TRACK lv_color_hex(0x2a2d33)
#define COL_ACCENT lv_color_hex(0x4f9dff)  // 強調藍 (CPU 條 / 下一件事)
#define COL_CPU COL_ACCENT
#define COL_RAM lv_color_hex(0x57d977)
#define COL_WARN lv_color_hex(0xffb454)    // CPU/RAM > 80%
#define COL_CRIT lv_color_hex(0xff5f56)    // CPU/RAM > 90%
#define COL_CRIT_DIM lv_color_hex(0x7a2420) // 暗紅：低電量閃爍的「暗」相位 (紅↔暗紅脈動)
#define COL_MUTED lv_color_hex(0x9aa0a6)
#define COL_STALE lv_color_hex(0x555a60)
#define COL_STALE_BAR lv_color_hex(0x3a3d42)

// 由 y/m/d 算星期 (0=日…6=六)，Sakamoto 演算法
static int day_of_week(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static const char *WEEKDAY_ZH[] = {"週日", "週一", "週二", "週三", "週四", "週五", "週六"};

// --- 狀態 ---
static uint32_t g_last_msg_ms = 0;   // 最後一次成功解析訊息的時間 (millis)
static bool g_has_msg = false;       // 是否曾收到任何訊息
static bool g_is_stale = true;       // 目前是否處於過期狀態 (開機尚無資料即視為過期)
static String g_line;                // USB CDC 逐行緩衝
static String g_net_line;            // TCP 逐行緩衝 (與序列分開，共用解析路徑)
static uint8_t g_cur_bl = BL_FULL;   // 目前背光值
static char g_last_src = 0;          // 最後一筆成功訊息的來源 ('U'=序列 / 'N'=TCP，給狀態 icon 用)
static bool g_colon_on = true;       // 時鐘冒號閃爍狀態 (每秒翻轉)
// 三態朝向 (含遲滯)。優先序：FACEDOWN > LANDSCAPE > UPRIGHT；boundary 時保持前態。
enum Orientation : uint8_t { ORI_UPRIGHT = 0, ORI_LANDSCAPE = 1, ORI_FACEDOWN = 2 };
static Orientation g_orientation = ORI_UPRIGHT;
static uint32_t g_orientation_since = 0; // 進入目前姿勢的 millis() (用來算 hold)

// 應用模式：dashboard (三卡) 與蕃茄鐘各相位。POMO_* 純 RAM、不持久化。
enum AppMode : uint8_t {
    MODE_DASHBOARD = 0,
    MODE_POMO_RUNNING,        // 工作中倒數 (POMO_WORK_MS)
    MODE_POMO_PAUSED,         // 工作暫停 (tap 切換)
    MODE_POMO_DONE,           // 工作結束短過場 (POMO_WORK_TO_BREAK_HOLD_MS); 結束後自動進 BREAK
    MODE_POMO_BREAK_RUNNING,  // 休息中倒數 (POMO_BREAK_MS); tap = 跳過, tilt up = 退 cycle
    MODE_POMO_BREAK_DONE      // 休息結束短過場; 結束後自動回 dashboard
};
// 「目前不在 dashboard」= 蕃茄鐘任一相位。enum 把 DASHBOARD 固定在 0,所有 POMO_* 都 >0,
// 用 helper 包起來讓呼叫端讀起來像意圖 (而不是依賴 enum order 的雜訊)。
static inline bool is_pomo_mode(AppMode m) { return m != MODE_DASHBOARD; }
static AppMode  g_mode = MODE_DASHBOARD;
static uint32_t g_pomo_remaining_ms = 0;  // running 時被 millis() 推進；paused 凍結
static uint32_t g_pomo_last_tick_ms = 0;  // 最後一次推進的 millis() (用差值更新 remaining)
static uint16_t g_pomo_cycle_count   = 0; // 完成的完整 work+break cycle 計數;log 用,未持久化 (重開歸零)
static uint32_t g_pomo_entered_ms = 0;    // 進入 RUNNING 的 millis() (3s 寬限基準)
static uint32_t g_pomo_done_at_ms = 0;    // 進入 DONE 的 millis() (脈衝相位 + 60s timeout)
static uint16_t g_pomo_last_secs_shown = 0xFFFF; // UI 上次顯示的秒數 (避免每 tick 重畫)

// Claude Code 用量 (host-link optional 欄位 → 蕃茄鐘畫面)。純 RAM，不持久化。
// 缺欄位時不覆蓋既有值 → agent 斷線自然「凍結最後值」(見 design D6)。
static uint64_t g_cc_session = 0;          // agent 端單調累積 token (絕對值無意義，只看差值)
static bool     g_cc_have_session = false; // 是否曾收到 cc_session (沒收過 → 顯示「—」)
static uint64_t g_cc_baseline = 0;         // 進蕃茄鐘那刻的快照 (這次 = session − baseline)
static bool     g_cc_baseline_set = false; // 基準線是否已用「真實收到的值」拍過
static int      g_cc_week_pct = -1;        // 0-100；-1 = 無值
static String   g_cc_week_reset;           // 預先格式化的重置提示；空 = 無值
// 用量兩行的「上次顯示」快取 (diff 用，避免每秒重畫整條窄帶)
static int      g_cc_shown_pct = -2;
static String   g_cc_shown_reset = "\x01"; // 不可能的初值 → 第一次必重畫
static char     g_cc_shown_sess[16] = "";

// 最近一次資料快取：stale 切換時要能用原值重繪 (改顏色)，故存起來。
// 初值 -1 = 尚未收到任何值 → 第一筆 (即使是 0%) 也會被視為「變了」而確實寫進量表。
static int g_cpu = -1, g_ram = -1;
static String g_ev_t[MAX_EVENTS];    // 事件時間前綴 (含日期，如 "今天 14:30")
static String g_ev_title[MAX_EVENTS];
static int g_ev_count = 0;

// --- 網路狀態 ---
static WiFiServer g_server(TCP_PORT);
static WiFiClient g_client;          // 單一連線；新連線進來就取代舊的
static bool g_net_started = false;   // mDNS/OTA/TCP server 是否已啟動 (WiFi 首次連上後)

// ---------- LVGL flush ----------
static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
#if LV_COLOR_16_SWAP
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif
    lv_disp_flush_ready(drv);
}

// ---------- UI 建立 ----------
// 建一張卡片 (圓角深色容器，無捲動)
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, COL_BORDER, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

// 建一條 CPU/RAM 橫條 (caption + bar + value)；row 為卡片內 y 偏移
static void make_stat_row(lv_obj_t *card, lv_obj_t *&bar, lv_obj_t *&val,
                          int row_y, const char *cap, lv_color_t color)
{
    lv_obj_t *capl = lv_label_create(card);
    lv_obj_set_style_text_font(capl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(capl, COL_MUTED, 0);
    lv_label_set_text(capl, cap);
    lv_obj_set_pos(capl, 0, row_y + 1);

    bar = lv_bar_create(card);
    lv_obj_set_size(bar, 120, 10);
    lv_obj_set_pos(bar, 40, row_y + 3);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);

    // 數值靠右對齊 → 9% / 100% 切換時右緣不跳動
    val = lv_label_create(card);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(val, lv_color_white(), 0);
    lv_obj_set_width(val, 44);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(val, "--%");
    lv_obj_set_pos(val, LCD_WIDTH - 16 - 16 - 44, row_y);  // 貼卡片右內緣
}

static void build_ui()
{
    lv_obj_t *scr = lv_scr_act();
    // 極深的同色系背景 (非純黑) → 卡片邊緣更柔和，少一點「貼在黑洞上」的廉價感
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d0e10), 0);

    // === 頂部狀態列 (浮在背景上，非卡片)：日期左、連線 icon + 電池右 ===
    // 把「日期/連線/電池」這些 meta 資訊集中成一條，時鐘卡才能純當大字主角。
    lbl_date = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_date, &font_cjk18, 0);
    lv_obj_set_style_text_color(lbl_date, COL_MUTED, 0);
    lv_label_set_text(lbl_date, "");
    lv_obj_align(lbl_date, LV_ALIGN_TOP_LEFT, 12, 6);

    // 右側 icon 群：flex row，靠右對齊。電池隱藏時 flex 會把空間收回，連線 icon 順勢靠右。
    status_box = lv_obj_create(scr);
    lv_obj_remove_style_all(status_box);   // 透明、無邊框/內距
    lv_obj_set_size(status_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_box, 8, 0);
    lv_obj_clear_flag(status_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(status_box, LV_ALIGN_TOP_RIGHT, -12, 6);

    // 連線狀態 icon：WiFi / USB / 離線 — 比「整片變灰」更直接 (先建 → 在電池左邊)
    lbl_status = lv_label_create(status_box);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_status, COL_STALE, 0);
    lv_label_set_text(lbl_status, LV_SYMBOL_WARNING);

    // 電池 icon+%：預設隱藏，update_battery() 偵測到電池才顯示 (目前未接 → 不出現)
    lbl_batt = lv_label_create(status_box);
    lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_batt, COL_MUTED, 0);
    lv_label_set_text(lbl_batt, "");
    lv_obj_add_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN);

    // === 卡片 1：時鐘 (hero，獨佔一卡置中) ===
    card_clock = make_card(scr, 8, 28, LCD_WIDTH - 16, 58);
    lbl_clock = lv_label_create(card_clock);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_clock, lv_color_white(), 0);
    lv_label_set_recolor(lbl_clock, true);   // 讓冒號可單獨變色 (閃爍)
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER, 0, 0);

    // === 卡片 2：CPU / RAM 橫條 ===
    card_stats = make_card(scr, 8, 90, LCD_WIDTH - 16, 56);
    make_stat_row(card_stats, bar_cpu, lbl_cpu_val, 0, "CPU", COL_CPU);
    make_stat_row(card_stats, bar_ram, lbl_ram_val, 20, "RAM", COL_RAM);

    // === 卡片 3：行事曆 (拿掉「即將到來」標題列 → 多一行空間、行距放正常不再重疊；
    //     事件列本身 (accent 條 + 時間前綴) 已自帶語意，標題列屬裝飾，省去更乾淨) ===
    card_events = make_card(scr, 8, 150, LCD_WIDTH - 16, 128);

    // 下一件事左側的 accent 直條 (跟著第一筆事件)
    next_marker = lv_obj_create(card_events);
    lv_obj_set_size(next_marker, 3, font_cjk18.line_height - 2);
    lv_obj_set_pos(next_marker, 0, EV_Y0 + 1);
    lv_obj_set_style_bg_color(next_marker, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(next_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(next_marker, 0, 0);
    lv_obj_set_style_radius(next_marker, 2, 0);
    lv_obj_clear_flag(next_marker, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < MAX_EVENTS; i++)
    {
        int y = EV_Y0 + i * EV_STEP;

        // 時間前綴 (顏色由 render_events 依 stale / 下一件事決定)
        lbl_ev_time[i] = lv_label_create(card_events);
        lv_obj_set_style_text_font(lbl_ev_time[i], &font_cjk18, 0);
        lv_label_set_long_mode(lbl_ev_time[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl_ev_time[i], EV_TIME_W);
        lv_obj_set_pos(lbl_ev_time[i], EV_X, y);
        lv_label_set_text(lbl_ev_time[i], "");

        // 標題：純文字 (不開 recolor)，使用者輸入的 '#' 原樣安全
        lbl_ev_title[i] = lv_label_create(card_events);
        lv_obj_set_style_text_font(lbl_ev_title[i], &font_cjk18, 0);
        lv_obj_set_style_text_color(lbl_ev_title[i], lv_color_white(), 0);
        lv_label_set_long_mode(lbl_ev_title[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_ev_title[i], LCD_WIDTH - 32 - EV_X - EV_TIME_W);
        lv_obj_set_pos(lbl_ev_title[i], EV_X + EV_TIME_W, y);
        lv_label_set_text(lbl_ev_title[i], "");
    }

#if IMU_CALIB_LOG
    // 校正疊圖：壓在最底，蓋住行事曆卡下緣一點點，顯示即時重力向量。
    // 校正完關掉 IMU_CALIB_LOG 後整段不編入，不影響正式 UI。
    lbl_debug = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_debug, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_debug, lv_color_hex(0xffd166), 0);
    lv_obj_set_style_bg_color(lbl_debug, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lbl_debug, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(lbl_debug, 3, 0);
    lv_label_set_text(lbl_debug, "imu --");
    lv_obj_align(lbl_debug, LV_ALIGN_BOTTOM_MID, 0, -2);
#endif
}

// 負載門檻配色：>=90 紅、>=80 琥珀，否則用基礎色 (CPU 藍 / RAM 綠)
static lv_color_t load_color(int v, lv_color_t base)
{
    if (v >= 90)
        return COL_CRIT;
    if (v >= 80)
        return COL_WARN;
    return base;
}

// 依目前快取值 + stale 狀態重繪 CPU/RAM 橫條 (顏色)
static void render_stats()
{
    bool s = g_is_stale;
    lv_obj_set_style_bg_color(bar_cpu, s ? COL_STALE_BAR : load_color(g_cpu, COL_CPU), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar_ram, s ? COL_STALE_BAR : load_color(g_ram, COL_RAM), LV_PART_INDICATOR);
    lv_color_t txt = s ? COL_STALE : lv_color_white();
    lv_obj_set_style_text_color(lbl_cpu_val, txt, 0);
    lv_obj_set_style_text_color(lbl_ram_val, txt, 0);
}

// 依事件快取重繪清單：時間前綴分色 (第一筆=下一件事用 accent 藍)，標題另一 label。
static void render_events()
{
    bool s = g_is_stale;

    if (g_ev_count == 0)
    {
        lv_obj_add_flag(next_marker, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_ev_time[0], "");
        lv_label_set_text(lbl_ev_title[0], "沒有即將到來的活動");
        // placeholder 也要跟著 stale 變灰 (其餘卡片都變灰時不該獨亮)
        lv_obj_set_style_text_color(lbl_ev_title[0], s ? COL_STALE : COL_MUTED, 0);
        for (int i = 1; i < MAX_EVENTS; i++)
        {
            lv_label_set_text(lbl_ev_time[i], "");
            lv_label_set_text(lbl_ev_title[i], "");
        }
        return;
    }

    // accent 直條：非 stale 才亮藍，stale 時轉灰
    lv_obj_clear_flag(next_marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(next_marker, s ? COL_STALE : COL_ACCENT, 0);

    for (int i = 0; i < MAX_EVENTS; i++)
    {
        if (i >= g_ev_count)
        {
            lv_label_set_text(lbl_ev_time[i], "");
            lv_label_set_text(lbl_ev_title[i], "");
            continue;
        }
        // 時間前綴色：stale→灰；下一件事 (i==0)→accent 藍；其餘→muted
        lv_color_t tc = s ? COL_STALE : (i == 0 ? COL_ACCENT : COL_MUTED);
        lv_obj_set_style_text_color(lbl_ev_time[i], tc, 0);
        lv_obj_set_style_text_color(lbl_ev_title[i], s ? COL_STALE : lv_color_white(), 0);
        lv_label_set_text(lbl_ev_time[i], g_ev_t[i].c_str());
        lv_label_set_text(lbl_ev_title[i], g_ev_title[i].c_str());
    }
}

// 連線狀態 icon：WiFi (網路新鮮) / USB (序列新鮮) / 離線 (過期)
static void update_status_icon()
{
    const char *sym;
    lv_color_t c;
    if (g_is_stale || !g_has_msg)
    {
        sym = LV_SYMBOL_WARNING;
        c = COL_STALE;
    }
    else if (g_last_src == 'N')
    {
        sym = LV_SYMBOL_WIFI;
        c = COL_ACCENT;
    }
    else
    {
        sym = LV_SYMBOL_USB;
        c = COL_RAM;
    }
    lv_label_set_text(lbl_status, sym);
    lv_obj_set_style_text_color(lbl_status, c, 0);
}

// 電池：AXP2101 偵測到電池才顯示 icon+%；目前未接 → 隱藏 (flex 自動回收空間)。
// 之後裝上 LiPo 不必改韌體就會自己出現。充電時轉綠並顯示閃電；低電量轉琥珀/紅。
static void update_battery()
{
    int pct = -1;
    if (g_pmu_ok && pmu.isBatteryConnect())
        pct = pmu.getBatteryPercent();   // 無 gauge/讀取失敗回 -1

    if (pct < 0)
    {
        if (!lv_obj_has_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    bool chg = pmu.isCharging();

    const char *sym;
    if (pct > 80)
        sym = LV_SYMBOL_BATTERY_FULL;
    else if (pct > 60)
        sym = LV_SYMBOL_BATTERY_3;
    else if (pct > 40)
        sym = LV_SYMBOL_BATTERY_2;
    else if (pct > 20)
        sym = LV_SYMBOL_BATTERY_1;
    else
        sym = LV_SYMBOL_BATTERY_EMPTY;

    // 配色：充電→綠；未充電下 ≤CRIT 每秒閃 (紅↔暗紅) 升級警告、≤WARN 恆紅、≤40 琥珀、其餘 muted。
    // update_battery() 每秒被呼叫一次 → 用 static toggle 做閃爍 (不另開 timer)。
    // blink 只在 ≤CRIT 相位翻轉，其餘狀態一律歸零 (離開警戒後不殘留半個相位)。
    static bool blink = false;
    lv_color_t c;
    if (chg)
        c = COL_RAM;
    else if (pct <= BATT_CRIT_PCT)
    {
        blink = !blink;
        c = blink ? COL_CRIT : COL_CRIT_DIM;   // 紅↔暗紅脈動：醒目提示「快沒電」
    }
    else if (pct <= BATT_WARN_PCT)
        c = COL_CRIT;
    else if (pct <= 40)
        c = COL_WARN;
    else
        c = COL_MUTED;
    if (chg || pct > BATT_CRIT_PCT)
        blink = false;

    lv_obj_clear_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(lbl_batt, c, 0);
    // 充電時用閃電蓋掉電量階梯 icon (一眼看出在充)，否則顯示對應電量階梯
    lv_label_set_text_fmt(lbl_batt, "%s %d%%", chg ? LV_SYMBOL_CHARGE : sym, pct);
}

// stale 狀態切換 → 重繪受影響元件 (時鐘照走，不變灰)
static void apply_stale(bool stale)
{
    g_is_stale = stale;
    render_stats();
    render_events();
    update_status_icon();
}

// ---------- 套用一筆 JSON 狀態 ----------
// time 欄位 seed RTC；cpu/ram 進量表；events 進清單。未知欄位忽略。
static void apply_status(JsonDocument &doc, char src)
{
    // time → RTC (僅在 RTC 尚未設定，或漂移超過閾值時才重設)
    const char *tstr = doc["time"] | (const char *)nullptr;
    if (tstr && g_rtc_ok)
    {
        RtcTime nt;
        if (sscanf(tstr, "%hu-%hhu-%hhu %hhu:%hhu:%hhu",
                   &nt.year, &nt.month, &nt.day, &nt.hour, &nt.minute, &nt.second) == 6)
        {
            RtcTime cur;
            bool need_set = true;
            if (g_has_msg && rtc.read(cur))
            {
                long drift = (long)cur.hour * 3600 + cur.minute * 60 + cur.second -
                             ((long)nt.hour * 3600 + nt.minute * 60 + nt.second);
                if (cur.year == nt.year && cur.month == nt.month && cur.day == nt.day &&
                    labs(drift) <= RTC_RESYNC_DRIFT_SEC)
                    need_set = false;
            }
            if (need_set)
                rtc.set(nt);
        }
    }

    // cpu / ram → 存快取 + 更新數值文字 (顏色由 render_stats 依門檻決定)。
    // 和快取比對，值真的變了才寫量表/文字並標記重繪 — 閒置桌面值多半不動，
    // 省去每 2s 一輪無意義的 bar/label 重設與 SPI 重刷 (與下方 events 同樣的 diff 策略)。
    bool stats_changed = false;
    if (doc["cpu"].is<int>())
    {
        int v = constrain(doc["cpu"].as<int>(), 0, 100);
        if (v != g_cpu)
        {
            g_cpu = v;
            lv_bar_set_value(bar_cpu, g_cpu, LV_ANIM_OFF);
            lv_label_set_text_fmt(lbl_cpu_val, "%d%%", g_cpu);
            stats_changed = true;
        }
    }
    if (doc["ram"].is<int>())
    {
        int v = constrain(doc["ram"].as<int>(), 0, 100);
        if (v != g_ram)
        {
            g_ram = v;
            lv_bar_set_value(bar_ram, g_ram, LV_ANIM_OFF);
            lv_label_set_text_fmt(lbl_ram_val, "%d%%", g_ram);
            stats_changed = true;
        }
    }

    // events → 存快取 (t 已含日期前綴，如 "今天 14:30")。agent 每 2s 都送一份，但內容
    // 多半 5 分鐘才換一次 → 先和快取比對，真的變了才標記重繪，省去每筆的字串重設與重畫。
    // 註：events 缺欄位或非陣列時，as<JsonArray>() 回傳 null array、迭代零筆 → i=0，
    //     會把 g_ev_count 歸零、清掉舊事件，不會卡著上一份事件不放 (spec: events 必帶)。
    bool events_changed = false;
    {
        JsonArray evs = doc["events"].as<JsonArray>();
        int i = 0;
        for (JsonObject ev : evs)
        {
            if (i >= MAX_EVENTS)
                break;
            const char *t = ev["t"] | "";
            const char *title = ev["title"] | "";
            if (g_ev_t[i] != t || g_ev_title[i] != title)
            {
                g_ev_t[i] = t;
                g_ev_title[i] = title;
                events_changed = true;
            }
            i++;
        }
        if (g_ev_count != i)
        {
            g_ev_count = i;
            events_changed = true;
        }
    }

    // Claude Code 用量 (optional 欄位)。缺欄位 → 不覆蓋既有值，斷線時自然凍結最後值。
    if (!doc["cc_session"].isNull())
    {
        g_cc_session = doc["cc_session"].as<uint64_t>();
        g_cc_have_session = true;
        if (is_pomo_mode(g_mode))
        {
            // 進蕃茄鐘時若還沒收到資料 → 基準線在第一筆 cc_session 抵達才拍 (避免把 0 當基準)
            if (!g_cc_baseline_set)
            {
                g_cc_baseline = g_cc_session;
                g_cc_baseline_set = true;
            }
            // 自癒：累積值倒退 (agent 重開/檔輪替) → 重設基準線，永不負數/暴量
            else if (g_cc_session < g_cc_baseline)
            {
                g_cc_baseline = g_cc_session;
            }
        }
    }
    if (!doc["cc_week_pct"].isNull())
        g_cc_week_pct = constrain(doc["cc_week_pct"].as<int>(), 0, 100);
    if (!doc["cc_week_reset"].isNull())
        g_cc_week_reset = doc["cc_week_reset"].as<const char *>();

    g_last_msg_ms = millis();
    char prev_src = g_last_src;
    g_last_src = src;
    g_has_msg = true;

    // 套用資料 + (必要時) 解除 stale。剛從 stale 恢復 → 交給 apply_stale 全部重繪
    // (它是 g_is_stale 的唯一 owner，省得在這裡再手刻一份同樣的三卡重繪)；
    // 仍新鮮時只重繪真的變了的部分 (CPU/RAM 門檻配色、事件、來源 icon)。
    if (g_is_stale)
    {
        apply_stale(false);
    }
    else
    {
        if (stats_changed)
            render_stats();
        if (events_changed)
            render_events();
        if (g_last_src != prev_src)
            update_status_icon();
    }
}

// ---------- 共用解析路徑：序列與 TCP 都把 byte 餵進來，整行才解析 ----------
// 解析一整行 JSON (忽略壞行，保留上次狀態)，然後清空緩衝。
static void apply_line(String &buf, char src)
{
    if (buf.length() > 0)
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, buf);
        if (!err)
            apply_status(doc, src);
    }
    buf = "";
}

// 餵一個 byte 進指定的行緩衝；遇 \n 觸發解析，過長視為壞行丟棄。
// src 隨 byte 一路傳進 apply_status，來源屬於「該筆訊息」而非全域，免旁路全域競態。
static void feed_byte(char c, String &buf, char src)
{
    if (c == '\n')
        apply_line(buf, src);
    else if (c != '\r')
    {
        // 防爆衝。原本 512：加 cc_session/cc_week_pct/cc_week_reset (~75B) 後，
        // 5 筆 CJK 標題事件 (UTF-8 每字 3B) 很容易超過 → 行被截一半 → JSON parse 失敗，
        // 畫面卡上一份事件 (常見症狀：「芒種」之類的中文標題顯示舊或亂)。
        // 1024 對單行 JSON 有 2× 餘裕，PSRAM 充足、零成本。
        if (buf.length() < 1024)
            buf += c;
        else
            buf = ""; // 過長視為壞行，丟棄
    }
}

// ---------- 逐行讀 USB CDC (debug/fallback 路徑) ----------
static void poll_serial()
{
    while (Serial.available())
        feed_byte((char)Serial.read(), g_line, 'U');
}

// ---------- 逐行讀 TCP 連線 (與序列共用 feed_byte → apply_status) ----------
// 單一 client：新連線進來就取代舊的；容忍斷線/半開 socket。
// 有界讀取：一次最多搬 256 bytes，且 read() 回 <=0 就 break，
// 避免半開 socket 下 available()>0 但讀不到造成 loop 空轉卡死 (USB CDC 會被餓死)。
static void poll_tcp()
{
    WiFiClient nc = g_server.accept();
    if (nc)
    {
        g_client.stop(); // 丟棄舊連線
        g_client = nc;
        g_net_line = "";
        Serial.println("[tcp] client connected");
    }
    if (!g_client.connected())
        return;
    uint8_t buf[256];
    int avail;
    while ((avail = g_client.available()) > 0)
    {
        int want = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
        int got = g_client.read(buf, want);
        if (got <= 0)
            break; // 讀不到 (半開/race) → 收手，下圈再來，絕不空轉
        for (int i = 0; i < got; i++)
            feed_byte((char)buf[i], g_net_line, 'N');
    }
}

// ---------- 啟動 mDNS / OTA / TCP server (WiFi 首次連上後呼叫一次) ----------
static void start_network_services()
{
    Serial.print("[wifi] connected, IP=");
    Serial.println(WiFi.localIP());

    // ArduinoOTA.begin() 內部會以 hostname 起 mDNS → espresso.local 可解析。
    ArduinoOTA.setHostname(MDNS_HOST);
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([]()
                       { Serial.println("[ota] start"); });
    ArduinoOTA.onEnd([]()
                     { Serial.println("[ota] end"); });
    ArduinoOTA.onError([](ota_error_t e)
                       { Serial.printf("[ota] error %u\n", e); });
    ArduinoOTA.begin();

    // 對外宣告 host-link TCP 線路服務
    MDNS.addService("espresso-link", "tcp", TCP_PORT);
    Serial.printf("[mdns] %s.local  [tcp] line-server on :%u\n", MDNS_HOST, TCP_PORT);

    g_server.begin();
    g_net_started = true;
}

// ---------- QMI8658 初始化 (只開 accelerometer)；完整開機與 timer-poll 快路徑共用 ----------
static bool imu_init()
{
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL))
        return false;
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_1000Hz,
                            SensorQMI8658::LPF_MODE_0);
    imu.enableAccelerometer();
    return true;
}

// ---------- 讀 QMI8658 加速度 (單位 g)；用於朝向偵測與校正 ----------
static bool imu_read_accel(float &x, float &y, float &z)
{
    if (!g_imu_ok || !imu.getDataReady())
        return false;
    return imu.getAccelerometer(x, y, z);
}

// ---------- 朝向分類 ----------
// 三態 + 各軸獨立遲滯 + 優先序 (facedown > landscape > upright > 保持前態)。
// 與舊版單一 g_facedown bool 等價的精神：在 enter/exit 兩值之間 latch 前態。
// 註：facedown 用 az、landscape 用 ay、upright 用 ax — 三軸彼此不互斥，
//      所以要顯式優先序而不是 if/else 鏈：邊界角度同時滿足兩個 candidate 時，朝下永遠勝。
static Orientation classify_orientation(Orientation prev, float ax, float ay, float az)
{
    bool is_facedown =
        (az <= FACEDOWN_ENTER_G) ||
        (prev == ORI_FACEDOWN && az <= FACEDOWN_EXIT_G);
    if (is_facedown)
        return ORI_FACEDOWN;

    bool is_landscape =
        (ay <= LANDSCAPE_ENTER_G) ||
        (prev == ORI_LANDSCAPE && ay <= LANDSCAPE_EXIT_G);
    if (is_landscape)
        return ORI_LANDSCAPE;

    bool is_upright =
        (ax >= UPRIGHT_ENTER_G) ||
        (prev == ORI_UPRIGHT && ax >= UPRIGHT_EXIT_G);
    if (is_upright)
        return ORI_UPRIGHT;

    return prev;   // 三個 hysteresis band 都沒到 → 保持前態 (不抖)
}

// 前向宣告 — Pomodoro / view 切換在下方定義，但被 IMU tick 與 tap 呼叫
static void pomo_enter_running();
static void pomo_exit_to_dashboard();
static void pomo_toggle_pause();
static void pomo_dismiss_done();
static void pomo_enter_break();      // work-DONE → BREAK 自動轉場
static void pomo_break_done();       // break-RUNNING → BREAK_DONE
static void show_pomo_view();
static void show_dashboard_view();
static void update_pomo_ui();
static void enter_sleep_from_active();
static bool audio_init();
static void pomo_beep();             // 工作結束「叮叮叮」
static void pomo_break_beep();       // 休息結束「鐺——」(低音長聲)

// 觸控 tap 事件 → 由 mode 決定語意
static void handle_tap()
{
    switch (g_mode)
    {
    case MODE_POMO_RUNNING:
    case MODE_POMO_PAUSED:
        pomo_toggle_pause();
        break;
    case MODE_POMO_DONE:
        // 工作完過場期間 tap = 跳過休息直接收工 (有人「不想休息再繼續」)
    case MODE_POMO_BREAK_RUNNING:
        // 休息中 tap = 跳過休息回 dashboard (偷工/想做別的事)
    case MODE_POMO_BREAK_DONE:
        // 休息完短過場 tap = 立刻回 dashboard,不等 1.5s
        pomo_dismiss_done();
        break;
    default:
        // dashboard 沒有 tap 語意；先吃掉不動作 (未來可擴充)
        break;
    }
}

// ---------- 朝向 tick：facedown 全域最高優先；landscape 進蕃茄鐘；upright 退出 ----------
// 由 loop() 每秒一次的 IMU 區段呼叫，傳入剛讀到的三軸 g。
static void orientation_tick(float ax, float ay, float az, uint32_t now)
{
    Orientation next = classify_orientation(g_orientation, ax, ay, az);
    if (next != g_orientation)
    {
        g_orientation = next;
        g_orientation_since = now;
    }
    uint32_t held = now - g_orientation_since;

    // (1) facedown 永遠最高優先 — 不論前景在 dashboard / pomo 哪個相位，
    //     達 FACEDOWN_HOLD_MS 一律入睡，並由 enter_sleep_from_active() 清空 pomo state。
    if (g_orientation == ORI_FACEDOWN && held >= FACEDOWN_HOLD_MS)
    {
        enter_sleep_from_active();   // 不返回
        return;
    }

    // (2) landscape → 進 / 維持蕃茄鐘
    if (g_orientation == ORI_LANDSCAPE && held >= LANDSCAPE_HOLD_MS)
    {
        if (g_mode == MODE_DASHBOARD)
            pomo_enter_running();
        return;
    }

    // (3) upright → 從蕃茄鐘退出 (3s 寬限只在 RUNNING 階段適用；其他狀態直接退)
    if (g_orientation == ORI_UPRIGHT && held >= UPRIGHT_HOLD_MS)
    {
        if (is_pomo_mode(g_mode))
            pomo_exit_to_dashboard();
    }
}

// ---------- 蕃茄鐘狀態轉移 ----------
// arc 整圈重設：範圍 = 該階段時長 (秒)、value=滿 (從滿開始倒)、indicator=該階段顏色、opa=不透。
// 用一個 helper 統一 work/break 進場流程,避免兩處重複 (色票/樣式集中,要改只改一處)。
//   WORK  = 暗紅 0xd9544f (專注/警戒)
//   BREAK = 森林綠 0x5fb86d (放鬆,但跟 DONE 的鮮綠 0x57d977 區隔)
static constexpr uint32_t POMO_ARC_COL_WORK  = 0xd9544f;
static constexpr uint32_t POMO_ARC_COL_BREAK = 0x5fb86d;
static constexpr uint32_t POMO_ARC_COL_DONE  = 0x57d977;
static void pomo_arc_reset(uint32_t duration_ms, uint32_t color_hex)
{
    const int32_t secs = (int32_t)(duration_ms / 1000);
    lv_arc_set_range(pomo_arc, 0, secs);
    lv_arc_set_value(pomo_arc, secs);
    lv_obj_set_style_arc_color(pomo_arc, lv_color_hex(color_hex), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(pomo_arc, LV_OPA_COVER, LV_PART_INDICATOR);
}

static void pomo_enter_running()
{
    uint32_t now = millis();
    g_mode = MODE_POMO_RUNNING;
    g_pomo_remaining_ms = POMO_WORK_MS;
    g_pomo_last_tick_ms = now;
    g_pomo_entered_ms = now;
    g_pomo_last_secs_shown = 0xFFFF;     // 強制下一次 update_pomo_ui() 重畫
    // 這次用量基準線：拍下當前累積值；若還沒收過資料 (have_session=false) 則延到第一筆抵達再拍。
    g_cc_baseline = g_cc_session;
    g_cc_baseline_set = g_cc_have_session;
    g_cc_shown_pct = -2;                 // 強制用量兩行重畫
    g_cc_shown_reset = "\x01";
    g_cc_shown_sess[0] = '\0';
    Serial.println("[pomo] enter RUNNING");
    show_pomo_view();
    pomo_arc_reset(POMO_WORK_MS, POMO_ARC_COL_WORK);   // 清掉前一輪 BREAK/DONE 留下的綠色
    update_pomo_ui();
}

static void pomo_exit_to_dashboard()
{
    // 規格 D8：3s 寬限 = 「視為誤觸」；本實作對 UI 而言效果相同 (回 dashboard)，
    // 但分支保留以供日後若加「次數統計」時區分。
    // 寬限以「使用者立回 +X 的瞬間」 (g_orientation_since) 計，不用 millis() 當下值 —
    // 否則 UPRIGHT_HOLD_MS 延後會吃掉一段寬限，使用者剛擦邊就不算了 (不符合直覺)。
    bool within_grace = (g_mode == MODE_POMO_RUNNING) &&
                        (g_orientation_since - g_pomo_entered_ms < POMO_CANCEL_GRACE_MS);
    if (within_grace)
        Serial.println("[pomo] cancel (within grace)");
    else
        Serial.println("[pomo] exit → dashboard (discard progress)");
    g_mode = MODE_DASHBOARD;
    g_pomo_remaining_ms = 0;
    show_dashboard_view();
}

static void pomo_toggle_pause()
{
    uint32_t now = millis();
    if (g_mode == MODE_POMO_RUNNING)
    {
        // 凍結 remaining：先 flush 從 last_tick 到現在的差值
        uint32_t delta = now - g_pomo_last_tick_ms;
        g_pomo_remaining_ms = (delta >= g_pomo_remaining_ms) ? 0 : (g_pomo_remaining_ms - delta);
        g_mode = MODE_POMO_PAUSED;
        Serial.printf("[pomo] PAUSED at %lu ms\n", (unsigned long)g_pomo_remaining_ms);
    }
    else if (g_mode == MODE_POMO_PAUSED)
    {
        g_pomo_last_tick_ms = now;
        g_mode = MODE_POMO_RUNNING;
        Serial.printf("[pomo] RESUMED at %lu ms\n", (unsigned long)g_pomo_remaining_ms);
    }
    g_pomo_last_secs_shown = 0xFFFF;
    update_pomo_ui();
}

static void pomo_dismiss_done()
{
    Serial.println("[pomo] DONE dismissed by tap");
    g_mode = MODE_DASHBOARD;
    g_pomo_remaining_ms = 0;
    show_dashboard_view();
}

// === 音訊：蕃茄鐘結束「叮叮叮」三聲 ============================================
//
// 分工：
//   audio-driver lib (pschatzmann/arduino-audio-driver) 寫 ES8311 I2C 暫存器，
//     讓 codec 從 I2S 收 PCM、推進 DAC + 喇叭路徑；同時管 PA_EN (GPIO 46)。
//   ESP-IDF legacy driver/i2s.h 跑 I2S DMA,把合成的正弦 PCM 送到 codec。
//   Sample rate 固定 16 kHz：1 kHz tone 整除 → 16 sample/cycle,零 aliasing。
//   選 legacy 而非新版 i2s_std 的原因見 audio_init() 內註解。
//
// 電源：板子的 ES8311 + 喇叭功放沒接 AXP2101 任何軌 (吃常開 3.3V)，所以
//   不用像 LCD 那樣先 enableALDOx。idle 時把 PA_EN 拉低省電 + 避免雜訊。
//
// 時機：pomo_enter_done() 呼叫 pomo_beep()。整段 ~600ms 是「同步阻塞」的
//   — 蕃茄鐘 DONE state 本來就有 60s timeout 在等，這點 latency 可忽略。

static audio_driver::DriverPins g_audio_pins;
static audio_driver::AudioBoard g_audio(audio_driver::AudioDriverES8311, g_audio_pins);
static constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
// (g_audio_ok 在最上方早期 globals 區宣告)

static bool audio_init()
{
    using namespace audio_driver;
    // 共用 setup() 早期已 Wire.begin(15,14) + setClock(400000) 的 I2C bus
    // → 把已開的 Wire 給 codec driver,不重複 begin。傳明確 SCL/SDA + active=true,
    // 預設的 addI2C(fn, Wire) 會把 scl/sda=-1 + set_active=false,某些 driver 路徑會吃癟。
    g_audio_pins.addI2C(PinFunction::CODEC, PIN_I2C_SCL, PIN_I2C_SDA, 0, 400000, Wire, true);
    g_audio_pins.addI2S(PinFunction::CODEC, PIN_I2S_MCLK, PIN_I2S_BCLK,
                        PIN_I2S_LRCK, PIN_I2S_DOUT, PIN_I2S_DIN);
    g_audio_pins.addPin(PinFunction::PA, PIN_AUDIO_PA, PinLogic::Output);

    CodecConfig cfg;
    cfg.input_device  = ADC_INPUT_NONE;     // 不收 mic
    cfg.output_device = DAC_OUTPUT_ALL;     // 推喇叭
    cfg.i2s.bits = BIT_LENGTH_16BITS;
    cfg.i2s.rate = RATE_16K;                // 16000 Hz — 1kHz 整除 (16 sample/cycle, 零 aliasing)
    cfg.i2s.fmt  = I2S_NORMAL;
    cfg.i2s.mode = MODE_MASTER;             // ESP 推 BCLK/LRCK, codec 當 slave
    if (!g_audio.begin(cfg))
    {
        Serial.println("[WARN] ES8311 codec init 失敗 → 蕃茄鐘將無聲");
        // begin() 中途失敗有可能 lib 已先把 PA 拉高,保險起見直接控 GPIO 強制 LOW
        // (不能依賴 g_audio.setPAPower,因為 begin 失敗後 driver state 不確定)。
        pinMode(PIN_AUDIO_PA, OUTPUT);
        digitalWrite(PIN_AUDIO_PA, LOW);
        return false;
    }
    g_audio.setVolume(60);                  // 0..100; 太大會吵
    g_audio.setPAPower(false);              // idle 關功放,避省電/雜訊

    // Legacy I2S driver: i2s_driver_install + i2s_set_pin。
    // 不用新版 i2s_std: arduino-esp32 開了 PSRAM (qio_opi), 新版 i2s_new_channel 把 handle
    // 配到 PSRAM,接著 gdma_register_tx_event_callbacks 規定 ISR user_data 必須在 internal
    // SRAM → ESP_ERR_INVALID_ARG。Legacy 走舊 DMA 路徑沒這條檢查。
    i2s_config_t i2s_cfg = {};
    i2s_cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_cfg.sample_rate = 16000;
    i2s_cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;     // STEREO interleaved
    i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_cfg.dma_buf_count = 4;
    i2s_cfg.dma_buf_len = 256;                               // 4×256 frame × 4 byte = 4 KB DMA,~64ms
    i2s_cfg.use_apll = false;
    i2s_cfg.tx_desc_auto_clear = true;                       // underrun 自動填 0,避雜訊
    i2s_cfg.fixed_mclk = 0;
    esp_err_t e = i2s_driver_install(I2S_PORT, &i2s_cfg, 0, nullptr);
    if (e != ESP_OK)
    {
        Serial.printf("[WARN] i2s_driver_install 失敗 err=0x%x\n", e);
        g_audio.setPAPower(false);          // codec 已 begin,PA 可能被 lib 預設拉高 → 強制關
        return false;
    }
    i2s_pin_config_t pins = {};
    pins.mck_io_num   = PIN_I2S_MCLK;
    pins.bck_io_num   = PIN_I2S_BCLK;
    pins.ws_io_num    = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    e = i2s_set_pin(I2S_PORT, &pins);
    if (e != ESP_OK)
    {
        Serial.printf("[WARN] i2s_set_pin 失敗 err=0x%x\n", e);
        i2s_driver_uninstall(I2S_PORT);
        g_audio.setPAPower(false);          // 同上,失敗 path 一律確保 PA 關閉,避免常開耗電/底噪
        return false;
    }
    Serial.println("[OK] audio (ES8311 + I2S)");
    return true;
}

// 推 1 段 freq_hz 正弦波 (LUT-based)。持續 ms 毫秒。amplitude 是 int16 PCM 振幅 (0..32767)。
// STEREO：每 frame 寫 L+R 同值。
//
// **freq_hz 必須整除 16000** (否則 phase 累積誤差 → audible aliasing 雜訊)。
// 合法常用音調: 250/500/800/1000/1600/2000 Hz (samples/cycle = 64/32/20/16/10/8)。
// 若不整除 → fallback 1000Hz + 警告 log。
static void audio_play_beep(uint32_t freq_hz, uint32_t ms, int16_t amplitude)
{
    static constexpr uint32_t RATE = 16000;
    static constexpr size_t MAX_SPC = 64;       // 上限 = 支援到 250Hz; stack LUT 128 bytes
    if (freq_hz == 0 || RATE % freq_hz != 0 || (RATE / freq_hz) > MAX_SPC)
    {
        Serial.printf("[WARN] beep freq %u Hz 不合法 (要整除 %u 且 ≥250Hz), fallback 1000Hz\n",
                      (unsigned)freq_hz, (unsigned)RATE);
        freq_hz = 1000;
    }
    const size_t spc = RATE / freq_hz;          // samples per cycle

    // single-cycle LUT 在 stack 上即時建; sinf() 呼叫 ≤64 次, 約 10us, 忽略不計
    int16_t lut[MAX_SPC];
    for (size_t i = 0; i < spc; i++)
    {
        const float a = (float)i * (2.0f * 3.14159265f / (float)spc);
        lut[i] = (int16_t)(sinf(a) * 32767.0f);
    }

    static constexpr size_t FRAMES = 256;
    int16_t buf[FRAMES * 2];                    // STEREO
    const uint32_t total_frames = (RATE * ms) / 1000;
    size_t phase = 0;
    for (uint32_t produced = 0; produced < total_frames;)
    {
        const size_t n = (total_frames - produced) < FRAMES ? (total_frames - produced) : FRAMES;
        for (size_t i = 0; i < n; i++)
        {
            const int16_t v = (int16_t)(((int32_t)lut[phase] * amplitude) >> 15);
            buf[2 * i]     = v;
            buf[2 * i + 1] = v;
            phase++;
            if (phase >= spc) phase = 0;
        }
        size_t written = 0;
        i2s_write(I2S_PORT, buf, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        produced += n;
    }
    delay(80);  // 等 DMA buffer 排空 (~64ms 容量) 再 return
}

// 結束「叮叮叮」三聲：1kHz × 200ms × 3，中間 200ms 靜音。
// audio_play_beep 結尾已內含 80ms 等 DMA 排空 + 此處 delay(120) = 200ms 間隔。
// PA 切換：上電 → 等 30ms settle → 播聲 → 全部播完關 PA (避免常開耗電/底噪)。
static void pomo_beep()
{
    if (!g_audio_ok)
        return;
    g_audio.setPAPower(true);
    delay(30);
    for (int i = 0; i < 3; i++)
    {
        audio_play_beep(1000, 200, 8000);   // 1kHz × 200ms × 3, amp 8000 = -12dBFS — 「該休息了」軟提示
        if (i < 2)
            delay(120);
    }
    g_audio.setPAPower(false);
}

// 休息結束：1 聲 500Hz × 600ms 低音長聲。比工作結束的「叮叮叮」更沉、更短一段,
// 語意上「拉回專注」(work→break 是放鬆 → 軟; break→work 是收心 → 沉)。
static void pomo_break_beep()
{
    if (!g_audio_ok)
        return;
    g_audio.setPAPower(true);
    delay(30);
    audio_play_beep(500, 600, 8000);   // 1× 500Hz × 600ms, amp 8000 = -12dBFS
    g_audio.setPAPower(false);
}

static void pomo_enter_done()
{
    g_mode = MODE_POMO_DONE;
    g_pomo_remaining_ms = 0;
    g_pomo_last_secs_shown = 0xFFFF;
    Serial.println("[pomo] WORK DONE → 短過場後自動進 BREAK");
    update_pomo_ui();
    // 強制 flush 一次,讓 "BREAK" 字在 beep 阻塞期間就先畫到 panel。
    // 不做的話 lv_timer_handler 要等 beep 返回後下一圈 loop 才跑 → 過場字看不到。
    lv_timer_handler();
    pomo_beep();
    // beep 結束才起算過場時間,否則 1.5s 視窗有 ~1.1s 被 beep 吃掉 → 過場字實質不可見。
    g_pomo_done_at_ms = millis();
    // DONE state 由 pomo_tick 在 POMO_WORK_TO_BREAK_HOLD_MS 後自動 → pomo_enter_break()
}

// 工作結束後自動進入休息：換 arc 顏色為綠系、reset 倒數到 BREAK 時長,繼續用同一個 view。
static void pomo_enter_break()
{
    uint32_t now = millis();
    g_mode = MODE_POMO_BREAK_RUNNING;
    g_pomo_remaining_ms = POMO_BREAK_MS;
    g_pomo_last_tick_ms = now;
    g_pomo_last_secs_shown = 0xFFFF;
    Serial.println("[pomo] enter BREAK");
    pomo_arc_reset(POMO_BREAK_MS, POMO_ARC_COL_BREAK);
    update_pomo_ui();
}

// 休息結束:長過場 + 低音 chime, 接著 pomo_tick 在 POMO_CYCLE_DONE_HOLD_MS 後自動回 dashboard。
// 過場時長 = 背光 sine 動畫長度 (POMO_FADE_DURATION_MS=3000ms),動畫剛好播完整個週期。
static void pomo_break_done()
{
    g_mode = MODE_POMO_BREAK_DONE;
    g_pomo_remaining_ms = 0;
    g_pomo_last_secs_shown = 0xFFFF;
    Serial.println("[pomo] BREAK DONE → 長過場 + 慶祝動畫後回 dashboard");
    update_pomo_ui();
    // 同 pomo_enter_done: flush "DONE" 字到 panel,避免 beep 期間使用者看不到。
    lv_timer_handler();
    pomo_break_beep();
    // beep 結束後才起算,讓 3s 過場 + 背光動畫從 "使用者實際看到 DONE 字" 那一刻開始。
    g_pomo_done_at_ms = millis();
}

// 每圈呼叫：推進 millis() 計時、處理階段轉移
//   RUNNING / BREAK_RUNNING → 倒數;歸零 → 進對應 DONE
//   DONE        → 等 POMO_WORK_TO_BREAK_HOLD_MS (1.5s) 後 → BREAK
//   BREAK_DONE  → 等 POMO_CYCLE_DONE_HOLD_MS (3s, 含背光動畫) 後 → DASHBOARD
static void pomo_tick(uint32_t now)
{
    if (g_mode == MODE_POMO_RUNNING || g_mode == MODE_POMO_BREAK_RUNNING)
    {
        uint32_t delta = now - g_pomo_last_tick_ms;
        if (delta > 0)
        {
            g_pomo_last_tick_ms = now;
            if (delta >= g_pomo_remaining_ms)
            {
                g_pomo_remaining_ms = 0;
                if (g_mode == MODE_POMO_RUNNING)
                    pomo_enter_done();
                else
                    pomo_break_done();
            }
            else
            {
                g_pomo_remaining_ms -= delta;
            }
        }
    }
    else if (g_mode == MODE_POMO_DONE)
    {
        // 工作完短過場 → 自動進休息
        if (now - g_pomo_done_at_ms >= POMO_WORK_TO_BREAK_HOLD_MS)
            pomo_enter_break();
    }
    else if (g_mode == MODE_POMO_BREAK_DONE)
    {
        // 休息完長過場 (含 3s 背光慶祝動畫) 結束。
        // 自動續輪 (傳統 Pomodoro):
        //   - 還橫躺著 → 直接進下一輪 work,不繞 dashboard。繞 dashboard 會看到 portrait 閃一下又轉回 landscape,
        //     視覺抖動;且 IMU 1s 取樣才會偵測到 landscape, 中間有 ~1s 顯示空窗。
        //   - 已起身/翻面 → 回 dashboard (orientation_tick 的 upright/facedown path 應該已經處理,但保險走一次)
        // 想結束 cycle: 立起來 (UPRIGHT) → orientation_tick 退回 dashboard;朝下趴平 → 入睡。
        if (now - g_pomo_done_at_ms >= POMO_CYCLE_DONE_HOLD_MS)
        {
            g_pomo_cycle_count++;
            if (g_orientation == ORI_LANDSCAPE)
            {
                Serial.printf("[pomo] cycle #%u 完成 → 自動續下一輪 (still landscape)\n",
                              (unsigned)g_pomo_cycle_count);
                pomo_enter_running();
            }
            else
            {
                Serial.printf("[pomo] cycle #%u 完成 → dashboard (orientation 已變)\n",
                              (unsigned)g_pomo_cycle_count);
                g_mode = MODE_DASHBOARD;
                g_pomo_remaining_ms = 0;
                show_dashboard_view();
            }
        }
    }
}

// ---------- 每秒從 RTC 更新時鐘 ----------
static void update_clock()
{
    if (!g_rtc_ok)
        return;
    RtcTime t;
    if (!rtc.read(t))
    {
        lv_label_set_text(lbl_clock, "--:--");
        lv_label_set_text(lbl_date, "等待校時...");
        return;
    }
    // 冒號每秒在白↔卡片底色間切換 → 看似閃爍；冒號永遠佔位故寬度不抖。
    g_colon_on = !g_colon_on;
    unsigned ccol = g_colon_on ? 0xFFFFFF : COL_CARD_BG;
    lv_label_set_text_fmt(lbl_clock, "%02d#%06X :#%02d", t.hour, ccol, t.minute);
    const char *wd = WEEKDAY_ZH[day_of_week(t.year, t.month, t.day)];
    lv_label_set_text_fmt(lbl_date, "%d月%d日 %s", t.month, t.day, wd);
}

// ---------- 背光：夜間 + 閒置調暗/熄滅 (GPIO40 LEDC PWM) ----------
static void update_backlight()
{
    uint8_t target = BL_FULL;

    // 夜間時段 → 調暗 (需要 RTC 有效時間)
    if (g_rtc_ok)
    {
        RtcTime t;
        if (rtc.read(t))
        {
            bool night = (NIGHT_START_HOUR <= NIGHT_END_HOUR)
                             ? (t.hour >= NIGHT_START_HOUR && t.hour < NIGHT_END_HOUR)
                             : (t.hour >= NIGHT_START_HOUR || t.hour < NIGHT_END_HOUR);
            if (night)
                target = BL_DIM;
        }
    }

    // 長時間沒資料 → 調暗 / 熄滅
    if (g_has_msg)
    {
        uint32_t idle = millis() - g_last_msg_ms;
        if (idle > IDLE_BLANK_MS)
            target = 0;
        else if (idle > IDLE_DIM_MS && target > BL_DIM)
            target = BL_DIM;
    }

    if (target != g_cur_bl)
    {
        g_cur_bl = target;
        ledcWrite(LCD_BL, target);
    }
}

// ---------- Deep-sleep：設定雙喚醒源後進睡 (不返回) ----------
// 喚醒源：(1) RTC timer 週期輪詢朝向；(2) 觸控 ext0 (CST816 INT, active-low) 立即喚醒。
// 不依賴 QMI8658 INT — 該腳官方未拉到可用 GPIO。
static void arm_wakeups_and_sleep()
{
    esp_sleep_enable_timer_wakeup(SLEEP_POLL_US);
    rtc_gpio_pullup_en((gpio_num_t)TP_INT);     // CST816 INT 開漏 → 需上拉，平時為高
    rtc_gpio_pulldown_dis((gpio_num_t)TP_INT);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)TP_INT, 0);  // 觸控拉低 → 喚醒
    esp_deep_sleep_start();
}

// 清醒狀態下判定要睡時呼叫：清空 pomo 進度 + 關背光 + 關顯示電軌降待機電流，然後進睡。
static void enter_sleep_from_active()
{
    Serial.println("[sleep] face-down → deep sleep");
    // 規格：facedown 是全域最高優先；睡前清空 pomo 變數，醒回來一定從 dashboard 起頭 (不恢復)。
    g_mode = MODE_DASHBOARD;
    g_pomo_remaining_ms = 0;
    g_pomo_entered_ms = 0;
    g_pomo_done_at_ms = 0;
    Serial.flush();
    ledcWrite(LCD_BL, 0);                 // 關背光 (耗電大宗)
    if (g_pmu_ok)
        pmu.disableALDO3();               // 關顯示電軌；RTC 走電池備援軌不受影響
    arm_wakeups_and_sleep();
}

// ---------- 觸控 CST816D：boot init + runtime INT-driven 讀 ----------
// boot：拉 RST 釋放、I2C probe。失敗 → 不擋啟動，運行時 poll_touch 整段 no-op。
static void setup_touch()
{
    // RST 拉低 ~10ms 後放高 (官方手冊建議)；之後等控制器 boot 50ms。
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);
    delay(10);
    digitalWrite(TP_RST, HIGH);
    delay(50);

    // 用 INPUT_PULLUP — INT 是 open-drain，平時靠上拉維持高，觸控按下控制器拉低。
    pinMode(TP_INT, INPUT_PULLUP);

    // I2C probe：只看是否 ACK，不嘗試讀任何 register (避免不同批次 firmware 差異)。
    Wire.beginTransmission(CST816_ADDR);
    uint8_t err = Wire.endTransmission();
    g_touch_ok = (err == 0);

    // 初始化 prev_high 為「當下 INT 真實值」— 若是觸控 ext0 喚醒，INT 還壓在低，
    // 必須記成 false，這樣使用者放手 → INT 回高 (上升沿，被忽略)，下一次按才算 tap，
    // 不會把「叫醒的那一下」當 runtime tap 餵給 UI (規格要求)。
    g_tp_int_prev_high = (digitalRead(TP_INT) == HIGH);

    Serial.printf("[%s] CST816D touch (addr 0x%02X, err=%u)\n",
                  g_touch_ok ? "OK" : "WARN", CST816_ADDR, err);
}

// 運行時：INT 高→低 (falling edge) 視為 touch-down → emit 一個 tap。
// 不做 I2C polling — 只在偵測到下降沿時讀一次寄存器確認 (順便把 INT 清掉)。
// 寄存器讀失敗也照樣 emit tap：邊緣本身就是有觸控的證據，I2C 只是順手讀掉避免 latch。
static void poll_touch()
{
    if (!g_touch_ok)
        return;
    bool high_now = (digitalRead(TP_INT) == HIGH);
    if (g_tp_int_prev_high && !high_now)
    {
        // 邊緣偵測到 → 讀一次 0x01 (touch number)；不解析座標 (規格：tap 不在意座標)
        uint8_t n = 0;
        Wire.beginTransmission(CST816_ADDR);
        Wire.write((uint8_t)0x01);
        if (Wire.endTransmission(false) == 0 &&
            Wire.requestFrom((uint8_t)CST816_ADDR, (uint8_t)1) == 1)
            n = Wire.read();
        // n>0 通常為 1 (CST816D 只有單點)；n==0 可能是已釋放但 INT 還沒回高 — 仍視為一次 tap edge
        (void)n;
        handle_tap();
    }
    g_tp_int_prev_high = high_now;
}

// ---------- 蕃茄鐘 UI：fullscreen view 與 dashboard 切換 ----------
// 設計理念 (calm tech，桌邊 ambient object)：
//  - depleting wedge 是主視覺 (餘光就看到「還剩多少」，不用讀數字)
//  - MM:SS 疊在 wedge 中央當 secondary check
//  - 跑步中不顯示 PLAY icon (運轉中的數字 + 縮小的形狀本身就是「running」訊號)
//  - 只有 PAUSED / DONE 這類「異常或終態」才有 glyph，符合「正常狀態安靜、異常狀態大聲」
static void build_pomo_view()
{
    pomo_view = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(pomo_view);
    // 用 LV_PCT(100) → 旋轉時 LVGL 會用「目前」hor_res/ver_res 重算大小，自動填滿。
    lv_obj_set_size(pomo_view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(pomo_view, 0, 0);
    lv_obj_set_style_bg_color(pomo_view, lv_color_hex(0x0d0e10), 0);
    lv_obj_set_style_bg_opa(pomo_view, LV_OPA_COVER, 0);
    lv_obj_clear_flag(pomo_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pomo_view, LV_OBJ_FLAG_HIDDEN);

    // === Depleting wedge (Time Timer 風)：滿圓 → 隨剩餘時間縮小 ===
    // 把 arc width 拉到 = radius → 視覺上就是填滿的圓盤 (pie chart)，不是細環。
    // 細環 (Apple Watch 風) 在桌邊餘光下看不清；實心扇形才有「在消失」的感覺。
    pomo_arc = lv_arc_create(pomo_view);
    lv_obj_set_size(pomo_arc, 184, 184);                  // 一切 (暫停/時間/usage) 疊在圓內；圓圈整組上提，底部留更多 padding 更平衡
    lv_obj_align(pomo_arc, LV_ALIGN_CENTER, 0, -10);
    lv_arc_set_rotation(pomo_arc, 270);                   // 12 點鐘 (LVGL 0°=3 點鐘，270 偏移到頂)
    lv_arc_set_bg_angles(pomo_arc, 0, 360);               // 背景畫整圈
    lv_arc_set_range(pomo_arc, 0, POMO_WORK_MS / 1000);   // 範圍以秒計
    lv_arc_set_value(pomo_arc, POMO_WORK_MS / 1000);      // 起始 = 滿
    // 樣式：細外圈 (Apple Watch 風)：背景做整圈淡 track，indicator 是「剩餘」的紅色弧。
    // arc_width=12 → 外輪廓細圈，中間鏤空 → MM:SS 在正中央更顯眼。
    lv_obj_set_style_arc_width(pomo_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(pomo_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(pomo_arc, lv_color_hex(0x2a2d33), LV_PART_MAIN);      // track：淡灰、永遠看得到整圈
    lv_obj_set_style_arc_color(pomo_arc, lv_color_hex(0xd9544f), LV_PART_INDICATOR); // 剩餘：偏暗紅，不刺眼
    lv_obj_set_style_arc_rounded(pomo_arc, true, LV_PART_MAIN);                      // 細圈用圓頭比較柔
    lv_obj_set_style_arc_rounded(pomo_arc, true, LV_PART_INDICATOR);
    // 隱藏拖曳 knob 圓點，禁止互動 (這是顯示元件，不是控制元件)
    lv_obj_remove_style(pomo_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(pomo_arc, LV_OBJ_FLAG_CLICKABLE);

    // === MM:SS 疊在 wedge 中央 ===
    pomo_lbl_time = lv_label_create(pomo_view);
    lv_obj_set_style_text_font(pomo_lbl_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(pomo_lbl_time, lv_color_white(), 0);
    lv_label_set_text(pomo_lbl_time, "25:00");
    lv_obj_align(pomo_lbl_time, LV_ALIGN_CENTER, 0, -10);  // 置中於圓圈 (跟著圓圈一起上提)
    lv_obj_move_foreground(pomo_lbl_time);   // 疊在 arc 之上

    // === 狀態 icon — 預設隱藏，只在 PAUSED / DONE 出現 ===
    pomo_lbl_icon = lv_label_create(pomo_view);
    lv_obj_set_style_text_font(pomo_lbl_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(pomo_lbl_icon, COL_WARN, 0);
    lv_label_set_text(pomo_lbl_icon, LV_SYMBOL_PAUSE);
    lv_obj_align(pomo_lbl_icon, LV_ALIGN_CENTER, 0, -56); // 時間上方 (圓內；= 圓心 -10 再上 46)
    lv_obj_add_flag(pomo_lbl_icon, LV_OBJ_FLAG_HIDDEN);   // 預設隱藏；update_pomo_ui() 視狀態 show
    lv_obj_move_foreground(pomo_lbl_icon);

    // === Claude 用量：本次 token 疊在「圓內、時間下方」；本週 bar+%+reset 在底部 (見 design D9；版面實機調) ===
    // 用 font_cjk18 才畫得出中文；數字/% 是 ASCII 在同字型內。
    // 本次 token (圓內、時間下方；青色，只放數字)
    pomo_lbl_session = lv_label_create(pomo_view);
    lv_obj_set_style_text_font(pomo_lbl_session, &font_cjk18, 0);
    lv_obj_set_style_text_color(pomo_lbl_session, lv_color_hex(0x6cc8e8), 0);
    lv_label_set_text(pomo_lbl_session, "—");
    lv_obj_align(pomo_lbl_session, LV_ALIGN_CENTER, 0, 32);  // 時間下方 (圓內；= 圓心 -10 再下 42)
    lv_obj_move_foreground(pomo_lbl_session);

    // 本週進度條 (底部，綠系跟完成色一致)
    pomo_bar_week = lv_bar_create(pomo_view);
    lv_obj_set_size(pomo_bar_week, 184, 6);
    lv_obj_align(pomo_bar_week, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_bar_set_range(pomo_bar_week, 0, 100);
    lv_bar_set_value(pomo_bar_week, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(pomo_bar_week, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pomo_bar_week, lv_color_hex(0x2a2d33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pomo_bar_week, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(pomo_bar_week, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(pomo_bar_week, lv_color_hex(0x57d977), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(pomo_bar_week, LV_OPA_COVER, LV_PART_INDICATOR);

    // 本週文字 (bar 上方；只剩 %+reset，無「本週」字)
    pomo_lbl_week = lv_label_create(pomo_view);
    lv_obj_set_style_text_font(pomo_lbl_week, &font_cjk18, 0);
    lv_obj_set_style_text_color(pomo_lbl_week, lv_color_hex(0xa9b0bb), 0);
    lv_label_set_text(pomo_lbl_week, "—");
    lv_obj_align(pomo_lbl_week, LV_ALIGN_BOTTOM_MID, 0, -12);
}

// 把 token 累積數縮寫成短字：12 → "12"、38200 → "38.2K"、5100000 → "5.1M"
static void abbrev_tokens(uint64_t n, char *out, size_t n_out)
{
    if (n < 1000)
        snprintf(out, n_out, "%u", (unsigned)n);
    else if (n < 1000000ULL)
        snprintf(out, n_out, "%.1fK", (double)n / 1000.0);
    else
        snprintf(out, n_out, "%.1fM", (double)n / 1000000.0);
}

// 蕃茄鐘畫面底部兩行 Claude 用量。值有 diff 才重畫 (避免每秒重刷窄帶)。
//   本週：bar + 「本週 16%  2天後重置」；無值 → 「本週 —」+ 空條
//   這次：「這次 38.2K」= cc_session − baseline；無資料 → 「這次 —」
//   emphasis (DONE)：把「這次」那行移到中央當成績 (字型受限無法放大，用置中突顯)
static void render_pomo_usage()
{
    // --- 本週：只剩 % + reset 文字 (無 bar、無「本週」字) ---
    if (g_cc_week_pct != g_cc_shown_pct || g_cc_week_reset != g_cc_shown_reset)
    {
        g_cc_shown_pct = g_cc_week_pct;
        g_cc_shown_reset = g_cc_week_reset;
        lv_bar_set_value(pomo_bar_week, g_cc_week_pct < 0 ? 0 : g_cc_week_pct, LV_ANIM_OFF);
        if (g_cc_week_pct < 0)
            lv_label_set_text(pomo_lbl_week, "—");
        else if (g_cc_week_reset.length())
            lv_label_set_text_fmt(pomo_lbl_week, "%d%%  %s",
                                  g_cc_week_pct, g_cc_week_reset.c_str());
        else
            lv_label_set_text_fmt(pomo_lbl_week, "%d%%", g_cc_week_pct);
    }

    // --- 這次 ---
    char num[16];
    if (!g_cc_have_session)
        snprintf(num, sizeof num, "—");
    else
    {
        uint64_t sess = g_cc_baseline_set ? (g_cc_session - g_cc_baseline) : 0;
        abbrev_tokens(sess, num, sizeof num);
    }
    // 固定在圓內時間下方 (位置在 build_pomo_view 設好，不隨相位移動)。
    // DONE 時上方時間變「DONE/BREAK」、token 留在原位 → 自然變成「成績」。
    if (strcmp(num, g_cc_shown_sess) != 0)
    {
        strncpy(g_cc_shown_sess, num, sizeof g_cc_shown_sess - 1);
        g_cc_shown_sess[sizeof g_cc_shown_sess - 1] = '\0';
        lv_label_set_text(pomo_lbl_session, num);
    }
}

// dashboard ↔ pomo 切換：show/hide 各組件。資料 model (g_cpu/g_ram/events) 在 pomo
// 期間仍由 host-link 寫入 (apply_status 沒被 mode 擋)，但「寫進 lv_label」這個動作
// 也持續發生 — render_stats/render_events 改寫的是隱藏物件，flush 不會畫 → 零成本。
// 退回 dashboard 時最新值已經在 label 上，show 出來即生效，不需再補一次。
static void show_pomo_view()
{
    if (!pomo_view)
        return;
    lv_obj_add_flag(lbl_date, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_stats, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_events, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(pomo_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(pomo_view);
    // 螢幕邏輯旋轉到 landscape (左側朝下，與 ay≤−0.80g 的物理姿勢一致)：
    // 設定後 lv_disp_get_hor_res() 回 284、ver_res 回 240；LV_PCT(100) 物件自動配合。
    // 用 ROT_270 (= 逆時針 90°)：實機驗證 ROT_90 上下顛倒，270 才對應左倒姿勢的閱讀方向。
    lv_disp_set_rotation(NULL, LV_DISP_ROT_270);
    lv_obj_invalidate(lv_scr_act());
}

static void show_dashboard_view()
{
    if (pomo_view)
        lv_obj_add_flag(pomo_view, LV_OBJ_FLAG_HIDDEN);
    lv_disp_set_rotation(NULL, LV_DISP_ROT_NONE);   // 回 portrait → 三卡座標恢復原意
    lv_obj_clear_flag(lbl_date, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(status_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(card_clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(card_stats, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(card_events, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_scr_act());
}

static void update_pomo_ui()
{
    if (!pomo_view || lv_obj_has_flag(pomo_view, LV_OBJ_FLAG_HIDDEN))
        return;

    // 底部兩行 Claude 用量：全相位都畫；DONE/BREAK_DONE 時把「這次」移中央當成績。
    render_pomo_usage();

    // 兩段「過場」畫面共用同一視覺 (wedge 空 + 鮮綠 + ✓)，只差中間字：
    //   WORK→BREAK 中繼站 (1.5s)   → "BREAK" 預告下一段
    //   整個 cycle 終點 (3s, 含背光 pulse) → "DONE"
    if (g_mode == MODE_POMO_DONE || g_mode == MODE_POMO_BREAK_DONE)
    {
        if (g_pomo_last_secs_shown != 0)
        {
            const char *done_text = (g_mode == MODE_POMO_DONE) ? "BREAK" : "DONE";
            lv_arc_set_value(pomo_arc, 0);
            lv_obj_set_style_arc_color(pomo_arc, lv_color_hex(POMO_ARC_COL_DONE), LV_PART_INDICATOR);
            lv_label_set_text(pomo_lbl_time, done_text);
            lv_obj_align(pomo_lbl_time, LV_ALIGN_CENTER, 0, -10);   // 跟圓圈一起上提；token 留下方當成績
            lv_obj_add_flag(pomo_lbl_icon, LV_OBJ_FLAG_HIDDEN);     // DONE 改用 render_pomo_usage 的成績佔中央
            g_pomo_last_secs_shown = 0;
        }
        return;
    }

    // running / paused：顯示 MM:SS；秒數變了才重畫 (避免每 loop 重設文字)
    uint32_t secs = (g_pomo_remaining_ms + 999) / 1000;  // 向上取整 → 25:00 顯示一秒、最後不會卡在 0:01
    if (secs != g_pomo_last_secs_shown)
    {
        g_pomo_last_secs_shown = (uint16_t)secs;
        uint32_t mm = secs / 60;
        uint32_t ss = secs % 60;
        lv_label_set_text_fmt(pomo_lbl_time, "%02u:%02u", (unsigned)mm, (unsigned)ss);
        lv_obj_align(pomo_lbl_time, LV_ALIGN_CENTER, 0, -10);
        // wedge 縮放：value=剩餘秒數 → indicator angle 自動算
        lv_arc_set_value(pomo_arc, (int32_t)secs);
    }

    // 狀態 icon (時間下方)：
    //   WORK RUNNING → 隱藏 (專注時不打擾)
    //   WORK PAUSED  → ‖ (黃) + wedge 半透 = 凍結感
    //   BREAK RUNNING → "BREAK" 字 (綠) = 明確標示「現在是休息」
    if (g_mode == MODE_POMO_PAUSED)
    {
        lv_obj_clear_flag(pomo_lbl_icon, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(pomo_lbl_icon, LV_SYMBOL_PAUSE);
        lv_obj_set_style_text_color(pomo_lbl_icon, COL_WARN, 0);
        lv_obj_set_style_arc_opa(pomo_arc, LV_OPA_50, LV_PART_INDICATOR);
    }
    else if (g_mode == MODE_POMO_BREAK_RUNNING)
    {
        lv_obj_clear_flag(pomo_lbl_icon, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(pomo_lbl_icon, "BREAK");
        lv_obj_set_style_text_color(pomo_lbl_icon, lv_color_hex(POMO_ARC_COL_BREAK), 0);
        lv_obj_set_style_arc_opa(pomo_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    }
    else
    {
        lv_obj_add_flag(pomo_lbl_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_opa(pomo_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    }
}

// 完成提示：POMO_FADE_DURATION_MS 內以 sine ease 在 BL_FULL ↔ POMO_PULSE_DIM 間平滑呼吸；
// 之後留 BL_FULL。sine ease 而非方波：方波讀作「警報/錯誤」，平滑曲線讀作「完成」(calm tech)。
// 回傳 true 代表本函式接管了背光 (update_backlight 該圈就跳過)。
// 只在 BREAK_DONE 觸發 (整個 cycle 的真正終點;WORK→BREAK 中間的 DONE 是中繼站不慶祝)。
// POMO_CYCLE_DONE_HOLD_MS 已對齊 POMO_FADE_DURATION_MS,動畫剛好播完才回 dashboard。
static bool update_pomo_done_backlight(uint32_t now)
{
    if (g_mode != MODE_POMO_BREAK_DONE)
        return false;
    uint32_t elapsed = now - g_pomo_done_at_ms;
    if (elapsed > POMO_FADE_DURATION_MS)
    {
        if (g_cur_bl != BL_FULL)
        {
            g_cur_bl = BL_FULL;
            ledcWrite(LCD_BL, BL_FULL);
        }
        return true;
    }
    // cosine 從 1 (peak=BL_FULL) → -1 (trough=DIM) → 1，週期 POMO_FADE_CYCLE_MS。
    // (1 + cos) / 2 ∈ [0, 1]：1 = 完整亮、0 = 谷底。
    uint32_t phase_ms = elapsed % POMO_FADE_CYCLE_MS;
    float phase = (float)phase_ms / (float)POMO_FADE_CYCLE_MS * 2.0f * (float)M_PI;
    float k = (1.0f + cosf(phase)) * 0.5f;
    uint8_t v = POMO_PULSE_DIM + (uint8_t)((BL_FULL - POMO_PULSE_DIM) * k + 0.5f);
    if (v != g_cur_bl)
    {
        g_cur_bl = v;
        ledcWrite(LCD_BL, v);
    }
    return true;
}

// deep-sleep 由 timer 喚醒時的快路徑：只開 I2C 讀一次重力，仍朝下就立刻回睡，
// 完全不碰顯示/WiFi (醒窗壓在數十 ms)；已立起則 return → 往下走完整開機。
// 觸控 (ext0) 或正常開機不進此路徑，直接完整開機。必須在任何昂貴初始化之前呼叫。
static void handle_wakeup_fast_path()
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER)
        return;
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    g_imu_ok = imu_init();
    if (!g_imu_ok)
        return;   // 讀不到 IMU → 保險起見完整開機
    float ax = 0, ay = 0, az = 0;
    bool got = false;
    for (int i = 0; i < 60 && !got; i++)   // 等一個樣本 (ODR 1kHz，最多 ~60ms)
    {
        got = imu_read_accel(ax, ay, az);
        delay(1);
    }
    if (got && az <= FACEDOWN_EXIT_G)      // 仍朝下 → 立刻回睡
        arm_wakeups_and_sleep();
    // 否則 (已立起 / 讀失敗) → return → 完整開機
}

void setup()
{
    Serial.begin(115200);
    // deep-sleep timer 喚醒的快路徑：在等序列/顯示/WiFi 等昂貴步驟前先判朝向，
    // 仍朝下就直接回睡 (才省得到電)。觸控/正常開機會直接 return 往下完整開機。
    handle_wakeup_fast_path();
    // 非阻塞：無頭 (OTA/網路) 運作時若沒有 host 在讀 USB CDC，
    // Serial.print 不可卡住 loop (否則 loop 卡死 → USB 餓死、只剩 WiFi 答 ping)。
    Serial.setTxTimeoutMs(0);
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 1500)
        delay(10);
    Serial.println("\n=== ESPresso (LCD 1.83 / ST7789P) ===");

    // 1) I2C + AXP2101 ALDO3 供電
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    delay(50);
    if (pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL))
    {
        g_pmu_ok = true;
        pmu.setALDO3Voltage(3300);
        pmu.enableALDO3();
        pmu.enableBattDetection();   // 開電池偵測 → getBatteryPercent()/isBatteryConnect() 才有效
        Serial.println("[OK] AXP2101 ALDO3 on");
    }
    else
        Serial.println("[WARN] AXP2101 begin 失敗");
    delay(100);

    // 2) RTC
    g_rtc_ok = rtc.begin(Wire);
    Serial.printf("[%s] PCF85063 RTC\n", g_rtc_ok ? "OK" : "WARN");

    // 2b) QMI8658 IMU：只用 accelerometer 做朝向偵測 (立著 / 朝下趴平 / 橫躺)。
    //     gyro 不需要 → 不啟用以省電/省時。位址 0x6B (QMI8658_L_SLAVE_ADDRESS)。
    g_imu_ok = imu_init();
    Serial.printf("[%s] QMI8658 IMU (accel)\n", g_imu_ok ? "OK" : "WARN");

    // 2c) CST816D 觸控：boot 時 RST 釋放 + I2C 在線檢查；運行時用 INT 下降沿觸發 tap。
    //     兼容既有 deep-sleep ext0 喚醒路徑 — 睡前不關 CST816D (沿用既有設計)。
    setup_touch();

    // 2d) 音訊：ES8311 codec + I2S。失敗 → g_audio_ok = false，pomo_beep() no-op，
    //     其餘功能照常 (顯示/觸控/網路 不受影響)。
    g_audio_ok = audio_init();

    // 3) 背光 PWM (LEDC, GPIO40)
    ledcAttach(LCD_BL, BL_LEDC_FREQ, BL_LEDC_RES);
    ledcWrite(LCD_BL, BL_FULL);

    // 4) 顯示
    if (!gfx->begin())
        Serial.println("[ERROR] gfx->begin() 失敗");

    // 5) LVGL
    lv_init();
    size_t buf_px = LCD_WIDTH * (LCD_HEIGHT / 7);
    lv_buf = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!lv_buf)
        lv_buf = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DEFAULT);
    lv_disp_draw_buf_init(&draw_buf, lv_buf, nullptr, buf_px);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    // 打開軟體旋轉：蕃茄鐘 view 進入時呼叫 lv_disp_set_rotation(NULL, LV_DISP_ROT_90)
    // → LVGL 在 render 階段把繪圖座標旋轉好，flush_cb 收到的 area 已是面板座標、無需改 driver。
    disp_drv.sw_rotate = 1;
    lv_disp_drv_register(&disp_drv);

    build_ui();
    build_pomo_view();   // 蕃茄鐘 fullscreen view 預設隱藏，landscape 才 show 出來
    apply_stale(true); // 開機尚無資料 → 先以過期樣式呈現
    Serial.println("[OK] UI ready");

    // 6) WiFi (非阻塞)：UI 已建好才連線，連線期間畫面照常渲染。
    //    setAutoReconnect 讓掉線後 WiFi stack 自動重連，無需重開機。
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_HOST);
    WiFi.setAutoReconnect(true);
    // 關閉 modem-sleep：省電模式下 radio 週期休眠會偶爾漏接 ARP/SYN，
    // 造成間歇的 "No route to host" 與區網內數百 ms 延遲。常開換穩定 (USB 供電可接受)。
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[wifi] connecting to \"%s\"...\n", WIFI_SSID);
}

void loop()
{
    poll_serial();

    // 網路：WiFi 首次連上後啟動服務；之後每圈非阻塞地處理 OTA 與 TCP。
    if (WiFi.status() == WL_CONNECTED)
    {
        if (!g_net_started)
            start_network_services();
        ArduinoOTA.handle();
        poll_tcp();
    }

    // 觸控：每圈讀一次 INT (digitalRead 廉價)；只在下降沿讀 I2C → 不浪費 bus
    poll_touch();

    uint32_t now = millis();

    // 蕃茄鐘 tick：每圈推進 (≥1ms 才有 delta)，比 1s 區塊更密 → 60s timeout / 倒數
    // 即時收 tap 暫停事件後也立即反映；遠輕於完整 1s 區塊的 RTC/IMU 工作。
    pomo_tick(now);

    // DONE 階段的 sine fade 背光：要每圈跑 (~5ms cadence) 才平滑，1s 取樣會退化成方波。
    // 不在 DONE 階段時函式內部 short-circuit 直接 return false，幾乎零成本。
    bool pomo_owns_bl = update_pomo_done_backlight(now);

    lv_timer_handler();

    static uint32_t last_tick = 0;
    if (now - last_tick >= 1000)
    {
        last_tick = now;
        update_clock();
        update_battery();
        // 朝向偵測：facedown 全域最高優先 (達 HOLD → deep sleep)，
        // 否則檢 landscape (進蕃茄鐘) / upright (退蕃茄鐘)。遲滯邏輯在 classify_orientation。
        if (g_imu_ok)
        {
            float ax, ay, az;
            if (imu_read_accel(ax, ay, az))
            {
#if IMU_CALIB_LOG
                // 校正/驗證：序列 + 螢幕底部疊圖顯示重力 + 目前朝向分類。
                static const char *ORI_NAME[] = {"UP", "LAND", "DOWN"};
                Serial.printf("[imu] accel g: x=%+.2f y=%+.2f z=%+.2f  ori=%s\n",
                              ax, ay, az, ORI_NAME[g_orientation]);
                if (lbl_debug)
                    lv_label_set_text_fmt(lbl_debug, "x%d y%d z%d /100g %s",
                                          (int)lroundf(ax * 100), (int)lroundf(ay * 100),
                                          (int)lroundf(az * 100), ORI_NAME[g_orientation]);
#endif
                orientation_tick(ax, ay, az, now);   // facedown 入睡時不返回
            }
        }
        // 蕃茄鐘 UI 每秒更新 MM:SS (秒數沒變時內部會 short-circuit，不重畫)
        if (is_pomo_mode(g_mode))
            update_pomo_ui();

        // 過期偵測
        bool stale = !g_has_msg || (now - g_last_msg_ms > STALE_TIMEOUT_MS);
        if (stale != g_is_stale)
            apply_stale(stale);   // apply_stale 自己會設 g_is_stale
        // 背光：DONE 階段已被上面每圈的 sine fade 接管 (pomo_owns_bl)；其餘走夜間/閒置邏輯。
        if (!pomo_owns_bl)
            update_backlight();
    }
    delay(5);
}
