## 1. Stage 0 — WiFi station + mDNS

- [ ] 1.1 Add `include/secrets.h.example` (WIFI_SSID / WIFI_PASS / OTA_PASS) and gitignore `include/secrets.h`; include `secrets.h` from firmware
- [ ] 1.2 Connect WiFi as STA from `secrets.h` non-blocking in `setup()`; build the UI first so it renders before/while connecting; auto-reconnect on drop
- [ ] 1.3 Advertise mDNS as `espresso.local` once connected; log assigned IP over serial
- [ ] 1.4 Verify on hardware: board joins WiFi, logs IP, and resolves at `espresso.local`

## 2. Stage 1 — TCP transport (same protocol)

- [ ] 2.1 Refactor the serial line buffer + JSON apply into one shared `feed_byte()`/`apply_line()` path
- [ ] 2.2 Add a non-blocking `WiFiServer` TCP line-server (fixed port) feeding the shared parser; single client, accept/replace on new connect; keep serial reader feeding the same path
- [ ] 2.3 Agent: network sender — resolve `espresso.local` (with `--host` override), open TCP, write the same newline-JSON; stdlib sockets only
- [ ] 2.4 Agent: transport selector (`--net` / `--serial` / auto-discover default)
- [ ] 2.5 Verify on hardware: CPU/RAM/calendar render over WiFi with the cable supplying power only (agent in network mode, serial port free)

## 3. Stage 2 — OTA (flash over WiFi)

- [ ] 3.1 Add ArduinoOTA with password from `secrets.h`; call `ArduinoOTA.handle()` non-blocking in `loop()`
- [ ] 3.2 Document/verify `pio run -t upload` over `espota` to `espresso.local`; confirm wrong/missing password is rejected
- [ ] 3.3 Verify on hardware: a wireless flash succeeds with the USB cable carrying only power

## 4. Stage 3 — Hardening & docs

- [ ] 4.1 Agent: auto-reconnect on TCP failure (board reboot, WiFi drop, Mac sleep/wake) without exiting
- [ ] 4.2 Firmware: tolerate client reconnect and half-open sockets; confirm stale-graying + free-running RTC clock during Mac outage
- [ ] 4.3 Update README: WiFi setup (`secrets.h`), mDNS name, transport selection, OTA flashing, and the local-trust/security note
