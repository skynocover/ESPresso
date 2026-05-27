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
#include "pcf85063.h"
#include "secrets.h"

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

// --- 網路 ---
static constexpr uint16_t TCP_PORT = 3333;            // host-link 協定的 TCP 線路埠
static constexpr char MDNS_HOST[] = "espresso";       // → espresso.local

XPowersPMU pmu;
PCF85063 rtc;
bool g_rtc_ok = false;

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

// --- UI 物件 (三張卡片：時鐘 / 系統 / 行事曆) ---
static lv_obj_t *card_clock, *card_stats, *card_events;
static lv_obj_t *lbl_clock;
static lv_obj_t *lbl_date;
static lv_obj_t *bar_cpu, *bar_ram;
static lv_obj_t *lbl_cpu_val, *lbl_ram_val;
static lv_obj_t *lbl_events_title;
static lv_obj_t *lbl_event[MAX_EVENTS];

// 卡片底色 / 強調色
#define COL_CARD lv_color_hex(0x16181c)
#define COL_TRACK lv_color_hex(0x2a2d33)
#define COL_CPU lv_color_hex(0x4f9dff)
#define COL_RAM lv_color_hex(0x57d977)
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
    lv_obj_set_style_border_width(c, 0, 0);
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

    val = lv_label_create(card);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(val, lv_color_white(), 0);
    lv_label_set_text(val, "--%");
    lv_obj_set_pos(val, 168, row_y);
}

static void build_ui()
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // === 卡片 1：時鐘 + 日期 ===
    card_clock = make_card(scr, 8, 6, LCD_WIDTH - 16, 76);
    lbl_clock = lv_label_create(card_clock);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_clock, lv_color_white(), 0);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_MID, 0, -2);

    lbl_date = lv_label_create(card_clock);
    lv_obj_set_style_text_font(lbl_date, &font_cjk18, 0);
    lv_obj_set_style_text_color(lbl_date, COL_MUTED, 0);
    lv_label_set_text(lbl_date, "");
    lv_obj_align(lbl_date, LV_ALIGN_BOTTOM_MID, 0, 0);

    // === 卡片 2：CPU / RAM 橫條 ===
    card_stats = make_card(scr, 8, 88, LCD_WIDTH - 16, 56);
    make_stat_row(card_stats, bar_cpu, lbl_cpu_val, 0, "CPU", COL_CPU);
    make_stat_row(card_stats, bar_ram, lbl_ram_val, 20, "RAM", COL_RAM);

    // === 卡片 3：行事曆 ===
    card_events = make_card(scr, 8, 150, LCD_WIDTH - 16, 128);
    lbl_events_title = lv_label_create(card_events);
    lv_obj_set_style_text_font(lbl_events_title, &font_cjk18, 0);
    lv_obj_set_style_text_color(lbl_events_title, COL_MUTED, 0);
    lv_label_set_text(lbl_events_title, "即將到來");
    lv_obj_set_pos(lbl_events_title, 0, 0);

    for (int i = 0; i < MAX_EVENTS; i++)
    {
        lbl_event[i] = lv_label_create(card_events);
        lv_obj_set_style_text_font(lbl_event[i], &font_cjk18, 0);
        lv_obj_set_style_text_color(lbl_event[i], lv_color_white(), 0);
        lv_label_set_long_mode(lbl_event[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_event[i], LCD_WIDTH - 32);
        lv_obj_set_pos(lbl_event[i], 0, 22 + i * 18);
        lv_label_set_text(lbl_event[i], "");
    }
}

// ---------- 過期 (stale) 視覺：CPU/RAM/事件變灰 ----------
static void apply_stale(bool stale)
{
    lv_color_t txt = stale ? COL_STALE : lv_color_white();
    lv_obj_set_style_text_color(lbl_cpu_val, txt, 0);
    lv_obj_set_style_text_color(lbl_ram_val, txt, 0);
    lv_obj_set_style_bg_color(bar_cpu, stale ? COL_STALE_BAR : COL_CPU, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar_ram, stale ? COL_STALE_BAR : COL_RAM, LV_PART_INDICATOR);
    for (int i = 0; i < MAX_EVENTS; i++)
        lv_obj_set_style_text_color(lbl_event[i], txt, 0);
}

// ---------- 套用一筆 JSON 狀態 ----------
// time 欄位 seed RTC；cpu/ram 進量表；events 進清單。未知欄位忽略。
static void apply_status(JsonDocument &doc)
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

    // cpu / ram
    if (doc["cpu"].is<int>())
    {
        int cpu = constrain(doc["cpu"].as<int>(), 0, 100);
        lv_bar_set_value(bar_cpu, cpu, LV_ANIM_OFF);
        lv_label_set_text_fmt(lbl_cpu_val, "%d%%", cpu);
    }
    if (doc["ram"].is<int>())
    {
        int ram = constrain(doc["ram"].as<int>(), 0, 100);
        lv_bar_set_value(bar_ram, ram, LV_ANIM_OFF);
        lv_label_set_text_fmt(lbl_ram_val, "%d%%", ram);
    }

    // events
    if (doc["events"].is<JsonArray>())
    {
        JsonArray evs = doc["events"].as<JsonArray>();
        int i = 0;
        for (JsonObject ev : evs)
        {
            if (i >= MAX_EVENTS)
                break;
            const char *t = ev["t"] | "";
            const char *title = ev["title"] | "";
            lv_label_set_text_fmt(lbl_event[i], "%s  %s", t, title);
            i++;
        }
        if (i == 0)
        {
            lv_label_set_text(lbl_event[0], "沒有即將到來的活動");
            i = 1;
        }
        for (; i < MAX_EVENTS; i++)
            lv_label_set_text(lbl_event[i], "");
    }

    g_last_msg_ms = millis();
    g_has_msg = true;
    if (g_is_stale)
    {
        g_is_stale = false;
        apply_stale(false);
    }
}

// ---------- 共用解析路徑：序列與 TCP 都把 byte 餵進來，整行才解析 ----------
// 解析一整行 JSON (忽略壞行，保留上次狀態)，然後清空緩衝。
static void apply_line(String &buf)
{
    if (buf.length() > 0)
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, buf);
        if (!err)
            apply_status(doc);
    }
    buf = "";
}

// 餵一個 byte 進指定的行緩衝；遇 \n 觸發解析，過長視為壞行丟棄。
static void feed_byte(char c, String &buf)
{
    if (c == '\n')
        apply_line(buf);
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
        feed_byte((char)Serial.read(), g_line);
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
            feed_byte((char)buf[i], g_net_line);
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
    lv_label_set_text_fmt(lbl_clock, "%02d:%02d", t.hour, t.minute);
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

void setup()
{
    Serial.begin(115200);
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
        pmu.setALDO3Voltage(3300);
        pmu.enableALDO3();
        Serial.println("[OK] AXP2101 ALDO3 on");
    }
    else
        Serial.println("[WARN] AXP2101 begin 失敗");
    delay(100);

    // 2) RTC
    g_rtc_ok = rtc.begin(Wire);
    Serial.printf("[%s] PCF85063 RTC\n", g_rtc_ok ? "OK" : "WARN");

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
        // 過期偵測
        bool stale = !g_has_msg || (now - g_last_msg_ms > STALE_TIMEOUT_MS);
        if (stale != g_is_stale)
        {
            g_is_stale = stale;
            apply_stale(stale);
        }
        update_backlight();
    }
    delay(5);
}
