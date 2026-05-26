#include <Arduino.h>

void setup()
{
    Serial.begin(115200);

    // 等 USB CDC 列舉完成(最多 3 秒)
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 3000)
    {
        delay(10);
    }

    Serial.println();
    Serial.println("=================================");
    Serial.println("  ESP32-S3 AMOLED — Hello World");
    Serial.println("=================================");
    Serial.printf("Chip model     : %s\n", ESP.getChipModel());
    Serial.printf("CPU freq       : %d MHz\n", getCpuFrequencyMhz());
    Serial.printf("Free heap      : %u bytes\n", ESP.getFreeHeap());
    Serial.printf("PSRAM size     : %u bytes\n", ESP.getPsramSize());
    Serial.printf("Flash size     : %u bytes\n", ESP.getFlashChipSize());
    Serial.println();
}

void loop()
{
    static uint32_t i = 0;
    Serial.printf("Tick %lu  (uptime %lu ms)\n", i++, millis());
    delay(1000);
}