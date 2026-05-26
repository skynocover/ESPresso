## Why

The Waveshare ESP32-S3-Touch-LCD-1.83 board (ST7789P SPI LCD, 240×284) currently runs only a serial "hello world" sketch. We want to turn it into an always-on desktop info display showing a clock, the Mac's live CPU/RAM usage, and upcoming calendar events — a glanceable secondary screen that sits on the desk over USB.

The ESP32 cannot read macOS system stats or calendars directly, so the design splits responsibilities: a lightweight Mac-side agent collects data and streams it over USB serial, while the firmware is a "dumb" renderer. This keeps all OS/credential complexity on the Mac (easy to write and debug in Python) and the firmware simple and stable.

## What Changes

- Add ESP32-S3 firmware that drives the 1.83" ST7789P SPI LCD (240×284) via Arduino_GFX + LVGL (DC=4 CS=5 SCK=6 MOSI=7 RST=38, backlight GPIO40). Power rails managed via AXP2101 (XPowersLib).
- Firmware keeps its own time via NTP, so the clock keeps working even if the Mac agent stops (also serves as a data-freshness indicator).
- Firmware reads newline-delimited JSON from USB serial (CDC) and updates CPU/RAM gauges and an upcoming-events list.
- Add backlight dimming / scheduled blanking for always-on power saving (LCD via GPIO40 PWM). Note: no AMOLED burn-in concern on this panel.
- Add a Python Mac agent (`agent.py`) that samples CPU/RAM via `psutil` and reads upcoming events from the local Calendar.app via AppleScript, packaging them as JSON and writing to `/dev/cu.usbmodem*` via `pyserial`.
- Define a simple newline-delimited JSON protocol as the contract between agent and firmware.
- Provide an optional launchd setup so the agent auto-starts in the background.

Non-goals (explicitly deferred): Microsoft Teams unread (no clean local API), Outlook unread (same AppleScript approach can be added later), GPU usage (requires `sudo powermetrics`), and WiFi/wireless transport.

## Capabilities

### New Capabilities
- `host-link-protocol`: The newline-delimited JSON message format and serial transport contract between the Mac agent and the firmware (fields, units, cadence, framing, malformed-line handling).
- `mac-data-agent`: The macOS Python agent that collects CPU/RAM and calendar events and streams them over USB serial, including the optional launchd auto-start.
- `display-firmware`: The ESP32-S3 firmware that initializes the AMOLED/LVGL stack, keeps NTP time, parses incoming protocol messages, renders the UI, and applies anti-burn-in measures.

### Modified Capabilities
<!-- None: this is a greenfield project; the existing sketch is replaced. -->

## Impact

- **Firmware**: replaces `src/main.cpp`; `platformio.ini` uses the pioarduino platform pinned to `53.03.13` (arduino-esp32 3.1.3 — newer cores have an I2C `ESP_ERR_INVALID_STATE` regression). lib_deps: Arduino_GFX (ST7789), lvgl@8.x, XPowersLib (AXP2101). USB CDC already enabled (`ARDUINO_USB_CDC_ON_BOOT=1`).
- **Mac side**: new `agent/` directory with `agent.py`; requires Python 3 plus `psutil` and `pyserial` (pip). Uses built-in `osascript` for Calendar — no OAuth, no extra services.
- **Permissions**: first run prompts the user to grant Calendar access to the terminal/Python process (one-time authorization, not an install).
- **No** USB drivers, cloud apps, or OAuth registrations required.
