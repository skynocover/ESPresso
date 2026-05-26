## Context

The board is a Waveshare ESP32-S3-Touch-LCD-1.83 (ESP32-S3R8, 16MB flash, 8MB PSRAM; **ST7789P SPI** LCD at **240×284**; CST816D I2C touch; AXP2101 PMU; onboard PCF85063 RTC, QMI8658 IMU, ES8311/ES7210 audio). Display SPI pins DC=4 CS=5 SCK=6 MOSI=7 RST=38, backlight GPIO40; shared I2C SDA=15 SCL=14. The repo currently holds a hello-world sketch and a `platformio.ini` with USB CDC enabled.

(Historical note: this board was initially misidentified as an AMOLED/QSPI panel; all QSPI/ST77916/360×360 references were wrong. The display is plain SPI ST7789P.)

The goal is an always-on desktop status display. Because the firmware cannot access macOS metrics or calendars, a Mac-side agent must supply that data. The board sits on the desk powered by USB, making USB serial the natural transport.

## Goals / Non-Goals

**Goals:**
- Glanceable always-on display: clock, CPU%, RAM%, next few calendar events.
- Keep firmware simple and stable; concentrate complexity in the Mac agent.
- No intrusive Mac install: Python 3 + two pip packages, otherwise built-in macOS tools.
- Survive agent outages gracefully (clock keeps running; stale data is visually indicated).
- Dim/blank the LCD backlight on a schedule for always-on power saving.

**Non-Goals:**
- Teams unread, Outlook unread, GPU usage, WiFi/wireless transport (all deferred).
- Touch-driven interaction beyond what burn-in mitigation needs (no app/menu system in v1).
- Multi-host support or running against more than one Mac.

## Decisions

### Split architecture: Mac agent (brain) + firmware (face)
The agent collects data and the firmware renders it. Adding future data sources usually means editing only the agent. Chosen over an "ESP32 does everything" approach because the board has no path to macOS metrics/credentials.

### Transport: USB serial (CDC), newline-delimited JSON
The board is USB-powered on the desk anyway; serial is the simplest reliable channel and needs zero network config. One JSON object per line keeps framing trivial and debuggable (you can watch it with a serial monitor). Considered WiFi (HTTP/MQTT/WebSocket) and ESPHome/Home Assistant; both add firmware/connectivity complexity that contradicts the "simple desktop gadget" goal. WiFi remains a clean future upgrade.

### Clock kept on the onboard PCF85063 RTC, seeded over USB (no NTP/WiFi)
The agent includes the current local time in each status message; the firmware writes it to the onboard PCF85063 RTC (I2C 0x51) once (and re-syncs only if drift exceeds a few seconds), and renders the clock from the RTC. The RTC free-runs on its own crystal, so the clock survives agent crashes and doubles as a freshness signal: if data fields go stale but the clock ticks, the user knows the agent (not the board) is the problem.

This replaces the original NTP design. NTP would have required WiFi credentials and a network stack purely for a clock, which contradicts the "simple USB gadget, no network config" goal — the data already flows over USB, so seeding the RTC over the same link costs only a small `time` field in the protocol plus a minimal PCF85063 driver. Considered (B) displaying agent-sent time with no RTC, but that freezes the clock when the agent stops and loses the freshness-signal property. WiFi/NTP remains a clean future upgrade if untethered operation is ever wanted.

### Display stack: Arduino_GFX (ST7789) + LVGL
Drive the ST7789P over `Arduino_ESP32SPI` + `Arduino_ST7789` (Arduino_GFX), with LVGL on top for arc/gauge and list widgets. No custom driver needed. Backlight on GPIO40 (PWM via LEDC).

### Toolchain: pioarduino platform pinned to arduino-esp32 3.1.3
`platformio.ini` uses the pioarduino platform release `53.03.13`. The official `espressif32@6.x` only ships arduino-esp32 2.0.x (missing `esp32-hal-periman.h`, so Arduino_GFX won't compile), requiring core 3.x. But core **3.2.0+** introduced an I2C regression (`i2c_master_transmit` returns `ESP_ERR_INVALID_STATE` every call), which blocks all I2C writes (AXP2101 etc.). 3.1.3 is the last working version — hence the pin.

### Calendar via local Calendar.app (AppleScript), not Google API
Read upcoming events from macOS Calendar.app using `osascript`. Assumes the user's Google Calendar is already subscribed there (the common case). This eliminates OAuth, app registration, and SDK dependencies. Trade-off: depends on the account being present in Calendar.app; if not, the user subscribes it once. The same AppleScript pattern later covers Outlook unread.

### Agent: Python with psutil + pyserial
`psutil` for CPU/RAM, `subprocess`→`osascript` for calendar, `pyserial` to write the port. Python is fastest to write/debug and the user confirmed Python is fine. Optional launchd plist auto-starts it in the background. Considered a compiled Swift binary (zero pip deps) but rejected for v1 due to slower iteration.

### Backlight dimming (power saving, not burn-in)
This is an LCD, so there is no AMOLED burn-in concern. For always-on use we instead manage the backlight (GPIO40 PWM): lower brightness at night or after prolonged no-data, and optionally blank entirely. Firmware-side and configurable.

## Risks / Trade-offs

- **Always-on power draw / backlight wear** → scheduled backlight dimming/blanking (GPIO40 PWM). No burn-in risk (LCD).
- **Agent stops or USB unplugged, board shows stale numbers** → firmware tracks last-message timestamp; if no message for N seconds, gray out / mark data stale while the NTP clock keeps running.
- **macOS serial port name varies / not exclusive** → agent globs `/dev/cu.usbmodem*`; document picking the right port and handle reconnect (reopen on write failure).
- **Calendar.app doesn't contain the Google account** → document one-time subscription; agent degrades gracefully (empty events list) if no calendar is readable.
- **Calendar permission prompt blocks first run** → document the one-time authorization; agent handles the denied/empty case without crashing.
- **Malformed/partial serial lines** → firmware parses line-by-line and silently drops lines that fail JSON parsing, keeping the last good state.
- **PSRAM/heap pressure from LVGL framebuffer** → 8MB PSRAM is ample; allocate LVGL draw buffers in PSRAM per Waveshare demo guidance.

## Migration Plan

Greenfield. Phased so each stage is independently verifiable:
0. Bring up Waveshare LVGL demo — confirm the panel lights and draws.
1. RTC clock in firmware, seeded over USB by the agent (no NTP/WiFi).
2. Data pipeline: agent emits fake JSON → firmware parses and shows CPU/RAM.
3. Real data: psutil CPU/RAM + AppleScript calendar.
4. Polish: anti-burn-in, stale-data handling, UI styling, launchd auto-start.

Rollback: the prior hello-world `main.cpp` is in git history; re-flashing it restores baseline. Removing the agent is deleting the `agent/` directory.

## Open Questions

- Exact UI layout for 368×448 (clock prominence vs. gauges vs. event list) — to be decided during stage 4.
- Dimming policy: fixed night hours, ambient/manual, or idle-based?
- Event source scope: all calendars or a chosen subset; how many events and how far ahead.
