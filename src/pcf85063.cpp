#include "pcf85063.h"

bool PCF85063::begin(TwoWire &wire)
{
    _wire = &wire;
    _wire->beginTransmission(ADDR);
    return _wire->endTransmission() == 0;
}

bool PCF85063::read(RtcTime &t)
{
    if (!_wire)
        return false;
    _wire->beginTransmission(ADDR);
    _wire->write(REG_SECONDS);
    if (_wire->endTransmission(false) != 0)
        return false;
    if (_wire->requestFrom((int)ADDR, 7) != 7)
        return false;

    uint8_t sec = _wire->read();
    uint8_t min = _wire->read();
    uint8_t hour = _wire->read();
    uint8_t day = _wire->read();
    _wire->read(); // weekday，本專案不用
    uint8_t mon = _wire->read();
    uint8_t year = _wire->read();

    // VL bit (秒的 bit7)=1 代表曾掉電、時間不可靠。
    if (sec & 0x80)
        return false;

    t.second = bcd2bin(sec & 0x7F);
    t.minute = bcd2bin(min & 0x7F);
    t.hour = bcd2bin(hour & 0x3F);
    t.day = bcd2bin(day & 0x3F);
    t.month = bcd2bin(mon & 0x1F);
    t.year = 2000 + bcd2bin(year);
    return true;
}

bool PCF85063::set(const RtcTime &t)
{
    if (!_wire)
        return false;
    _wire->beginTransmission(ADDR);
    _wire->write(REG_SECONDS);
    _wire->write(bin2bcd(t.second) & 0x7F); // 寫 0 到 VL，標記時間有效
    _wire->write(bin2bcd(t.minute));
    _wire->write(bin2bcd(t.hour));
    _wire->write(bin2bcd(t.day));
    _wire->write(0); // weekday 設 0，不使用
    _wire->write(bin2bcd(t.month));
    _wire->write(bin2bcd((uint8_t)(t.year % 100)));
    return _wire->endTransmission() == 0;
}
