## 1. Stage 0 — Bring up display (Waveshare LVGL demo)

- [x] 1.1 Board identified as **Waveshare ESP32-S3-Touch-LCD-1.83** (ST7789P SPI, 240×284 — NOT the AMOLED/QSPI board originally assumed). `platformio.ini`: pioarduino platform pinned to `53.03.13` (arduino-esp32 3.1.3 — newer cores have an I2C `ESP_ERR_INVALID_STATE` regression). lib_deps: Arduino_GFX, lvgl, XPowersLib.
- [x] 1.2 Add `include/lv_conf.h` (LVGL 8.x config); build flags `-DLV_CONF_INCLUDE_SIMPLE -I include`. PSRAM draw buffers wired in Stage 1 firmware.
- [x] 1.3 Stage 0 `src/main.cpp`: init ST7789P over Arduino_ESP32SPI (DC=4 CS=5 SCK=6 MOSI=7 RST=38, BL=40), AXP2101 ALDO3 power rail, render color test + text via Arduino_GFX.
- [x] 1.4 Flashed; AMOLED→LCD lights up and renders solid colors + text correctly. ✅ Stage 0 hardware bring-up complete.

## 2. Stage 1 — RTC clock in firmware (seeded over USB, no NTP/WiFi)

- [x] 2.1 PCF85063 RTC driver (I2C 0x51): read/set time; seed from the protocol `time` field, re-sync only on drift
- [x] 2.2 Render a clock widget that updates every second from the RTC, independent of any serial data
- [x] 2.3 Verify the clock keeps running (free-runs on RTC) with no agent connected

## 3. Stage 2 — Serial data pipeline (with fake data)

- [x] 3.1 Implement line-buffered USB CDC reader that splits on `\n`
- [x] 3.2 Parse each line as JSON; ignore malformed/truncated lines, keep last good state
- [x] 3.3 Map `cpu`/`ram`/`events` fields onto LVGL CPU/RAM arcs and an events list
- [x] 3.4 Write a throwaway sender that emits fake JSON every 2s; confirm the UI updates (`agent/fake_sender.py`)
- [x] 3.5 Record a "last message received" timestamp on each successful parse

## 4. Stage 3 — Real Mac agent

- [x] 4.1 Create `agent/` with `agent.py`; document Python 3 + `psutil` + `pyserial` setup
- [x] 4.2 Sample CPU% and RAM% via `psutil` as integers 0–100
- [x] 4.3 Read upcoming events via native **EventKit** (PyObjC), up to N soonest as `{t,title}` — replaced osascript (>150s, unusable). API verified <1ms; populates once Calendar TCC is granted interactively.
- [x] 4.4 Handle empty/no-readable-calendar and not-yet-granted permission gracefully (empty list)
- [x] 4.5 Discover the port via `/dev/cu.usbmodem*` glob and write one JSON line per 2s
- [x] 4.6 Reopen the port and resume on write failure (board unplug/replug) without exiting
- [x] 4.7 End-to-end test: real CPU/RAM and calendar events (incl. Chinese titles) render on the board

## 5. Stage 4 — Polish & always-on hardening

- [x] 5.1 Stale-data detection: gray out CPU/RAM/events after a configurable timeout; clear on resume
- [x] 5.2 Backlight PWM control on GPIO40 (LEDC) with adjustable brightness
- [x] 5.3 Scheduled backlight dimming/blanking (night hours + prolonged inactivity)
- [x] 5.4 Lay out the 240×284 UI (clock, CPU/RAM gauges, events list) and style it (note: original task said 368×448, a leftover from the AMOLED misID; actual panel is 240×284)
- [x] 5.5 Provide an optional launchd plist to auto-start the agent at login, with setup notes (`agent/com.espresso.agent.plist`)
- [x] 5.6 Write a short README covering wiring, setup, permissions, and running (`README.md` + `agent/`)
