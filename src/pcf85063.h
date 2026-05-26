// PCF85063 RTC — 最小 I2C 驅動 (板上即時時鐘，位址 0x51)
//
// 用途：host-link 協定的 time 欄位餵進來後，把時間寫進 RTC 一次；
// 之後 RTC 靠自己的石英晶振 free-run，agent 掛掉時鐘照走 (新鮮度指示)。
//
// 暫存器 (PCF85063A/TP)：0x04 秒(含 VL bit) / 0x05 分 / 0x06 時(24h) /
// 0x07 日 / 0x08 星期 / 0x09 月 / 0x0A 年(00-99，代表 2000-2099)。皆 BCD。

#pragma once
#include <Arduino.h>
#include <Wire.h>

struct RtcTime
{
    uint16_t year;  // 完整西元年，如 2026
    uint8_t month;  // 1-12
    uint8_t day;    // 1-31
    uint8_t hour;   // 0-23
    uint8_t minute; // 0-59
    uint8_t second; // 0-59
};

class PCF85063
{
public:
    static constexpr uint8_t ADDR = 0x51;
    static constexpr uint8_t REG_CTRL1 = 0x00;
    static constexpr uint8_t REG_SECONDS = 0x04;

    // 確認晶片在匯流排上 (回應位址即視為存在)。
    bool begin(TwoWire &wire = Wire);

    // 讀目前時間；失敗回 false。
    bool read(RtcTime &t);

    // 設定時間 (24 小時制，BCD 寫入)；失敗回 false。
    bool set(const RtcTime &t);

private:
    TwoWire *_wire = nullptr;
    static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
    static uint8_t bcd2bin(uint8_t v) { return (uint8_t)(((v >> 4) * 10) + (v & 0x0F)); }
};
