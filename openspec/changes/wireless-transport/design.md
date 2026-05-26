## Context

The desktop-status-display change (now archived) made the board a "dumb renderer": the Mac agent collects CPU/RAM (psutil) and calendar (signed Swift `calbridge`/EventKit) and streams newline-delimited JSON over **USB serial**; the firmware parses each line and renders three cards, keeping time on the onboard PCF85063 RTC. The host-link protocol is already transport-neutral (one JSON object per `\n`-terminated line).

Two limits motivate this change: the board is cable-tethered to the Mac, and flashing requires stopping the agent first (it holds `/dev/cu.usbmodem*`, colliding with esptool). The board is an ESP32-S3 (WiFi + BLE only — **no Bluetooth Classic**), 8MB PSRAM, on arduino-esp32 **3.1.3** (pinned; newer cores break I2C). WiFi/mDNS/OTA all ship in that core.

## Goals / Non-Goals

**Goals:**
- Carry the existing protocol over WiFi so the board needs no data cable — just power from any USB source.
- Find the board by name (`espresso.local`), not a memorized IP.
- Allow OTA firmware flashing, removing USB port contention.
- Keep the firmware a dumb renderer and the protocol byte-for-byte unchanged.
- Degrade gracefully when the Mac is off/unreachable (grey stale data; RTC clock keeps running) — already implemented.

**Non-Goals:**
- The board fetching its own data (calendar/weather/CPU) — the Mac stays the brain.
- Bluetooth/BLE transport.
- Switching the clock to NTP (RTC already works offline).
- Authenticated/encrypted transport (local-trust LAN only in v1).
- WiFi onboarding UX beyond editing a credentials file.

## Decisions

### Board is the TCP server; agent connects and pushes
The firmware runs a small `WiFiServer` on a fixed port; the agent opens a TCP socket to it and writes the same `\n`-delimited JSON lines. Chosen over (a) **agent-as-HTTP-server, board polls** — adds polling latency and makes the board depend on the Mac's address; (b) **UDP broadcast** — lossy, no delivery signal; (c) **MQTT** — needs a broker; (d) **WebSocket** — extra framing/library for no benefit on a trusted LAN. A raw TCP stream maps 1:1 onto the existing line protocol: the firmware's read loop is the serial reader with a different byte source.

### Discovery via mDNS (`espresso.local`)
The firmware calls `MDNS.begin("espresso")` and advertises the service; the agent resolves `espresso.local`. Chosen over a hardcoded/static IP (brittle across networks/DHCP) and over a discovery handshake (overkill). The agent keeps a `--host` override for networks where mDNS is blocked.

### Dual ingest: serial AND TCP feed one parser
The line buffer + JSON apply logic is refactored so both the serial reader and the TCP client push bytes into the **same** function. Keeping serial costs almost nothing and preserves a wired debug/recovery path (and first-time bring-up before WiFi credentials exist). The agent picks one transport; the firmware accepts whichever arrives.

### WiFi credentials in a gitignored `secrets.h`
`include/secrets.h` (gitignored) holds SSID/password/OTA-password; `include/secrets.h.example` is committed as a template. Chosen over a captive-portal/WiFiManager flow for v1 simplicity (one fewer library, no provisioning UI); a portal can be a later change. Credentials live in flash plaintext — acceptable for a personal desk device.

### OTA via ArduinoOTA, password-protected
`ArduinoOTA` is handled in `loop()`; PlatformIO uploads to `espresso.local` via the espota protocol. This removes USB entirely after the first flash and fully resolves the port-contention nuisance. An OTA password (from `secrets.h`) guards against rogue LAN uploads.

### Non-blocking integration with the LVGL loop
`loop()` already calls `lv_timer_handler()` every few ms. WiFi connect is kicked off in `setup()` with reconnection left to the WiFi stack; the TCP `accept()`/read and `ArduinoOTA.handle()` are polled non-blockingly each loop so the UI stays smooth. No data over any transport simply means the existing stale-detection greys the values.

## Risks / Trade-offs

- **WiFi drops / Mac sleeps → stale display** → existing stale-graying + free-running RTC clock already cover this; agent auto-reconnects; firmware keeps last good state.
- **mDNS not resolvable on some networks (guest/AP isolation)** → agent supports a `--host <ip>` override; document finding the IP from serial logs.
- **Unauthenticated TCP on the LAN (anyone could push fake data)** → local-trust assumption for v1; documented; a future change can add a shared token.
- **OTA abuse on the LAN** → require an OTA password from `secrets.h`.
- **WiFi connect blocking the boot/UI** → connect asynchronously; never busy-wait in `loop()`; UI renders (clock + empty cards) before WiFi is up.
- **Multiple/stale TCP clients** → accept a single client at a time; drop/replace on new connect; tolerate half-open sockets via the line reader.
- **Credentials/OTA password in flash plaintext** → gitignored, personal-device scope; not a shared/production secret.
- **Higher power draw with WiFi on** → acceptable (USB-powered); optionally enable modem-sleep later.

## Migration Plan

Phased so each stage is independently verifiable on hardware; serial stays working throughout, so there's always a fallback.
0. WiFi STA connect from `secrets.h` + mDNS `espresso.local`; confirm the board joins and is pingable/resolvable (serial logs the IP).
1. TCP line-server feeding the shared parser; add agent `--net` mode (resolve `espresso.local`, connect, send); confirm CPU/RAM/calendar render over WiFi with the cable carrying only power.
2. OTA: add ArduinoOTA + password; confirm `pio run -t upload` over `espota` to `espresso.local` works.
3. Hardening: agent reconnect on drop; board handles Mac sleep/reboot and its own reconnect; verify stale-graying and RTC clock during outages.

Rollback: serial transport remains; reflash the prior firmware over USB. Removing WiFi is deleting `secrets.h` and the WiFi/OTA code paths.

## Open Questions

- Fixed TCP port choice (e.g., 3333) — any conflict concerns on the user's LAN?
- Should serial be removed once WiFi is proven, or kept permanently as debug? (Leaning: keep.)
- OTA password storage/rotation — fine in `secrets.h` for now?
- Eventually add a shared-token auth for the TCP link, or leave local-trust?
