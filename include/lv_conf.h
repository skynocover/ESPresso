// LVGL 8.x 設定檔 (ESPresso / Waveshare ESP32-S3-Touch-AMOLED-1.8)
//
// 透過 build_flag -DLV_CONF_INCLUDE_SIMPLE，lvgl 會用 include path 找到本檔。
// 只覆寫關鍵項目，其餘交給 lv_conf_internal.h 的預設值。

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// --- 色彩：SH8601 為 RGB565；Arduino_GFX 推 bitmap 時需位元組互換 ---
// 若上板後顏色不對 (例如紅藍對調/雜訊)，把 LV_COLOR_16_SWAP 改 0 再試。
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1

// --- 記憶體：LVGL 內部物件池 (放內部 RAM，48KB 對此 UI 充足) ---
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)

// --- 時間基準：直接用 Arduino millis()，免自行呼叫 lv_tick_inc ---
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

// --- 預設重繪緩衝刷新時間 ---
#define LV_DISP_DEF_REFR_PERIOD 16 // ~60fps
#define LV_INDEV_DEF_READ_PERIOD 30

// --- 關閉 log/assert 的額外開銷 (開發階段可調) ---
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

// --- 字型：時鐘要大字，事件列表用中小字 ---
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

// --- 大字型支援：bpp=2 的 CJK 字型 (font_cjk18) bitmap index 超過 20-bit，需開此項改用 32-bit ---
#define LV_FONT_FMT_TXT_LARGE 1

// --- 用到的 widget (8.x 多半預設開啟，這裡明確標示需求) ---
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_LABEL 1
#define LV_USE_LIST 1

#endif // LV_CONF_H
