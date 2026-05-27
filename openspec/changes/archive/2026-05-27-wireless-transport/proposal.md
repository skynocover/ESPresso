## Why

The board currently receives data only over USB serial, which tethers it to the Mac by a cable and causes a flashing nuisance: because the agent holds `/dev/cu.usbmodem*`, esptool collides with it ("chip stopped responding") unless the agent is stopped first. The goal is an **untethered wireless status display** — the board sits anywhere, powered by any USB charger, and gets its data over WiFi while the Mac is on the same network. The clock already free-runs on the RTC, so when the Mac is unreachable the display simply greys stale data and keeps ticking.

## What Changes

- Firmware connects as a **WiFi station** using credentials from a gitignored `include/secrets.h` (with a committed `secrets.h.example` template).
- Firmware advertises itself via **mDNS as `espresso.local`** so the agent can find it without a hardcoded IP.
- Firmware runs a **TCP line-server** that feeds incoming newline-delimited JSON into the **same parser/renderer** already used for serial. The existing serial reader is **kept** as a debug/fallback path (both sources feed one parser).
- Firmware supports **OTA updates** (ArduinoOTA), so firmware can be flashed over WiFi — eliminating USB port contention entirely (no need to stop the agent, eventually no USB at all).
- The Mac agent (`agent.py`) gains a **network mode**: discover `espresso.local`, open a TCP connection, and write the same JSON lines it currently writes to serial; auto-reconnect on drop. Serial remains selectable for debugging.
- The **host-link protocol is unchanged** — identical newline-delimited JSON, now carried over a TCP stream instead of (or in addition to) the serial stream.
- The Swift `calbridge` calendar helper is **left untouched** — the signed reader stays isolated from the agent.

Non-goals (explicitly deferred): making the board fully standalone (it does **not** fetch calendar/CPU data itself — the Mac remains the data source); Bluetooth (the ESP32-S3 has no Bluetooth Classic, and BLE GATT streaming is not worth it here); switching the clock back to NTP (the RTC already works and needs no network); authentication/encryption of the LAN link (local-trust only in v1).

## Capabilities

### New Capabilities
- `wireless-link`: WiFi station connectivity and credential provisioning (`secrets.h`), mDNS discovery (`espresso.local`), the firmware-side TCP line-server that ingests protocol messages over the network (alongside serial), OTA firmware updates, and the agent's network transport mode (discover, connect, reconnect).

### Modified Capabilities
- `mac-data-agent`: the "Stream status over USB serial" requirement is generalized so the agent streams the host-link protocol over **either serial or the network**, selecting/auto-discovering the transport, rather than being serial-only.

## Impact

- **Firmware**: `platformio.ini` needs no new libraries — `WiFi.h`, `ESPmDNS.h`, `ArduinoOTA.h`, and `WiFiServer` ship with arduino-esp32 (still pinned to 3.1.3). New `include/secrets.h` (gitignored) + `include/secrets.h.example`. `src/main.cpp` gains WiFi connect, mDNS, a non-blocking `WiFiServer` client read loop feeding the existing line parser, and the ArduinoOTA handler in `loop()`.
- **Mac agent**: `agent.py` gains a network sender (socket to `espresso.local:<port>`, reconnect) and a transport selector (`--net` / `--serial` / auto); the serial path is retained. No new pip dependencies.
- **Protocol**: no change to `host-link-protocol`.
- **Power/operation**: board stays USB-powered (any charger); WiFi increases draw. The Mac must be powered on and on the same LAN for live data; otherwise the display greys stale values and the RTC clock keeps running.
- **Security**: the TCP server is unauthenticated on the local network (local-trust assumption); noted as a risk for a future hardening change.
