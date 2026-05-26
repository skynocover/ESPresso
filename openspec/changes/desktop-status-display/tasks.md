## 1. Stage 0 — Bring up display (Waveshare LVGL demo)

- [x] 1.1 Board identified as **Waveshare ESP32-S3-Touch-LCD-1.83** (ST7789P SPI, 240×284 — NOT the AMOLED/QSPI board originally assumed). `platformio.ini`: pioarduino platform pinned to `53.03.13` (arduino-esp32 3.1.3 — newer cores have an I2C `ESP_ERR_INVALID_STATE` regression). lib_deps: Arduino_GFX, lvgl, XPowersLib.
- [x] 1.2 Add `include/lv_conf.h` (LVGL 8.x config); build flags `-DLV_CONF_INCLUDE_SIMPLE -I include`. PSRAM draw buffers wired in Stage 1 firmware.
- [x] 1.3 Stage 0 `src/main.cpp`: init ST7789P over Arduino_ESP32SPI (DC=4 CS=5 SCK=6 MOSI=7 RST=38, BL=40), AXP2101 ALDO3 power rail, render color test + text via Arduino_GFX.
- [x] 1.4 Flashed; AMOLED→LCD lights up and renders solid colors + text correctly. ✅ Stage 0 hardware bring-up complete.

## 2. Stage 1 — NTP clock in firmware

- [ ] 2.1 Connect to time source and sync via NTP (WiFi credentials config) and set timezone
- [ ] 2.2 Render a clock widget that updates every second independent of any serial data
- [ ] 2.3 Verify the clock keeps running with no agent connected

## 3. Stage 2 — Serial data pipeline (with fake data)

- [ ] 3.1 Implement line-buffered USB CDC reader that splits on `\n`
- [ ] 3.2 Parse each line as JSON; ignore malformed/truncated lines, keep last good state
- [ ] 3.3 Map `cpu`/`ram`/`events` fields onto LVGL CPU/RAM arcs and an events list
- [ ] 3.4 Write a throwaway sender that emits fake JSON every 2s; confirm the UI updates
- [ ] 3.5 Record a "last message received" timestamp on each successful parse

## 4. Stage 3 — Real Mac agent

- [ ] 4.1 Create `agent/` with `agent.py`; document Python 3 + `psutil` + `pyserial` setup
- [ ] 4.2 Sample CPU% and RAM% via `psutil` as integers 0–100
- [ ] 4.3 Read upcoming events from Calendar.app via `osascript`; return up to N soonest as `{t,title}`
- [ ] 4.4 Handle empty/no-readable-calendar and not-yet-granted permission gracefully (empty list)
- [ ] 4.5 Discover the port via `/dev/cu.usbmodem*` glob and write one JSON line per 2s
- [ ] 4.6 Reopen the port and resume on write failure (board unplug/replug) without exiting
- [ ] 4.7 End-to-end test: real CPU/RAM and calendar events render on the board

## 5. Stage 4 — Polish & always-on hardening

- [ ] 5.1 Stale-data detection: gray out CPU/RAM/events after a configurable timeout; clear on resume
- [ ] 5.2 Backlight PWM control on GPIO40 (LEDC) with adjustable brightness
- [ ] 5.3 Scheduled backlight dimming/blanking (night hours or prolonged inactivity)
- [ ] 5.4 Lay out the 368×448 UI (clock, CPU/RAM gauges, events list) and style it
- [ ] 5.5 Provide an optional launchd plist to auto-start the agent at login, with setup notes
- [ ] 5.6 Write a short README covering wiring, setup, permissions, and running
