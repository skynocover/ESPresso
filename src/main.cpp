// ESPresso — 桌面狀態顯示器 (Waveshare ESP32-S3-Touch-LCD-1.83)
//
// Stage 0：純 Arduino_GFX 點亮測試。
// 板子：1.83" LCD，240x284，ST7789P 標準 SPI 驅動，CST816D 觸控，
//       AXP2101 PMU、ES8311/ES7210 音訊、QMI8658 IMU、PCF85063 RTC。
//
// 顯示 SPI：DC=4 CS=5 SCK=6 MOSI=7 RST=38   背光：BL=40 (active-high)
// I2C：SDA=15 SCL=14 (AXP2101 / 觸控 / IMU / RTC 共用)

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

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

XPowersPMU pmu;

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, GFX_NOT_DEFINED /* MISO */);

// ST7789：(bus, rst, rotation, ips, w, h, col_off1, row_off1, col_off2, row_off2)
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, LCD_RST, 0 /* rotation */, true /* IPS */, LCD_WIDTH, LCD_HEIGHT);

void setup()
{
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 2000)
        delay(10);

    Serial.println("\n=== ESPresso Stage0 (LCD 1.83 / ST7789P) ===");
    Serial.printf("PSRAM size : %u bytes\n", ESP.getPsramSize());

    // 1) I2C + AXP2101：開啟電源軌 (確保面板/系統供電)
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
        Serial.println("[WARN] AXP2101 begin 失敗 (面板若直接吃 3V3 仍可能正常)");
    delay(100);

    // 2) 背光開 (GPIO40, active-high)
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
    Serial.println("[OK] 背光 GPIO40 拉高");

    // 3) 初始化 ST7789 (建構子帶 RST=38，begin() 會做硬體 reset)
    if (!gfx->begin())
        Serial.println("[ERROR] gfx->begin() 失敗");
    else
        Serial.println("[OK] gfx->begin() 成功");
}

void loop()
{
    Serial.println("fill RED");
    gfx->fillScreen(RGB565_RED);
    delay(2000);
    Serial.println("fill GREEN");
    gfx->fillScreen(RGB565_GREEN);
    delay(2000);
    Serial.println("fill BLUE");
    gfx->fillScreen(RGB565_BLUE);
    delay(2000);

    gfx->fillScreen(RGB565_BLACK);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(3);
    gfx->setCursor(20, 120);
    gfx->println("ESPresso");
    delay(2000);
}
