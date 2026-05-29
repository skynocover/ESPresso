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

// --- 觸控 CST816 (deep-sleep ext0 喚醒源用；INT active-low) ---
#define TP_INT 13   // RTC-capable GPIO，可當 deep-sleep ext0 喚醒
#define TP_RST 39

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
//   立著(active): x≈+1.01 y≈0 z≈0 ；朝下(sleep): x≈0 y≈0 z≈-1.00 ；橫躺: y≈-0.97
//   只有「朝下趴平」時 Z 強負 → 用 az 判定，配遲滯避免邊界抖動。
static constexpr float FACEDOWN_ENTER_G = -0.80f;   // az 低於此 → 朝下
static constexpr float FACEDOWN_EXIT_G = -0.55f;    // az 高於此 → 非朝下 (遲滯帶: -0.80~-0.55)
static constexpr uint32_t FACEDOWN_HOLD_MS = 3000;  // 持續朝下達此時間才入睡 (防誤觸)
static constexpr uint64_t SLEEP_POLL_US = 1500000;  // deep-sleep 每 1.5s timer 喚醒輪詢朝向

// --- 網路 ---
static constexpr uint16_t TCP_PORT = 3333;            // host-link 協定的 TCP 線路埠
static constexpr char MDNS_HOST[] = "espresso";       // → espresso.local

XPowersPMU pmu;
PCF85063 rtc;
SensorQMI8658 imu;
bool g_rtc_ok = false;
bool g_pmu_ok = false;
bool g_imu_ok = false;

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
static bool g_facedown = false;      // 目前是否判定為「朝下」(含遲滯)
static uint32_t g_facedown_since = 0; // 進入朝下的起始時間 (達 HOLD 才入睡)

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
        if (buf.length() < 512) // 防爆衝
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

// 清醒狀態下判定要睡時呼叫：關背光 + 關顯示電軌降待機電流，然後進睡。
static void enter_sleep_from_active()
{
    Serial.println("[sleep] face-down → deep sleep");
    Serial.flush();
    ledcWrite(LCD_BL, 0);                 // 關背光 (耗電大宗)
    if (g_pmu_ok)
        pmu.disableALDO3();               // 關顯示電軌；RTC 走電池備援軌不受影響
    arm_wakeups_and_sleep();
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

    // 2b) QMI8658 IMU：只用 accelerometer 做朝向偵測 (立著 vs 朝下趴平)。
    //     gyro 不需要 → 不啟用以省電/省時。位址 0x6B (QMI8658_L_SLAVE_ADDRESS)。
    g_imu_ok = imu_init();
    Serial.printf("[%s] QMI8658 IMU (accel)\n", g_imu_ok ? "OK" : "WARN");

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
    lv_disp_drv_register(&disp_drv);

    build_ui();
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

    lv_timer_handler();

    static uint32_t last_tick = 0;
    uint32_t now = millis();
    if (now - last_tick >= 1000)
    {
        last_tick = now;
        update_clock();
        update_battery();
        // 朝向偵測：朝下 (az≤ENTER) 持續達 HOLD → deep sleep；遲滯避免邊界抖動。
        if (g_imu_ok)
        {
            float ax, ay, az;
            if (imu_read_accel(ax, ay, az))
            {
#if IMU_CALIB_LOG
                // 校正/驗證：序列 + 螢幕底部疊圖顯示重力。LVGL printf 不支援 %f → 用整數 (0.01g)。
                Serial.printf("[imu] accel g: x=%+.2f y=%+.2f z=%+.2f\n", ax, ay, az);
                if (lbl_debug)
                    lv_label_set_text_fmt(lbl_debug, "x%d y%d z%d /100g",
                                          (int)lroundf(ax * 100), (int)lroundf(ay * 100),
                                          (int)lroundf(az * 100));
#endif
                if (!g_facedown && az <= FACEDOWN_ENTER_G)
                {
                    g_facedown = true;
                    g_facedown_since = now;
                }
                else if (g_facedown && az >= FACEDOWN_EXIT_G)
                    g_facedown = false;

                if (g_facedown && (now - g_facedown_since >= FACEDOWN_HOLD_MS))
                    enter_sleep_from_active();   // 不返回 (進 deep sleep)
            }
        }
        // 過期偵測
        bool stale = !g_has_msg || (now - g_last_msg_ms > STALE_TIMEOUT_MS);
        if (stale != g_is_stale)
            apply_stale(stale);   // apply_stale 自己會設 g_is_stale
        update_backlight();
    }
    delay(5);
}
