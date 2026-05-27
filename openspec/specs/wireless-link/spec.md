# wireless-link Specification

## Purpose

The wireless link lets the ESP32-S3 board receive status messages and firmware updates over WiFi instead of (or in addition to) USB serial: the firmware joins a provisioned network, advertises itself via mDNS, ingests host-link protocol messages over TCP through the same parser/renderer as serial, and supports password-protected OTA updates. The Mac data agent gains a network transport that discovers the board and reconnects automatically.

## Requirements

### Requirement: Connect to WiFi using provisioned credentials
The firmware SHALL connect to a WiFi network as a station using credentials provided at build time via a gitignored `include/secrets.h` (with a committed `include/secrets.h.example` template). The connection SHALL be established without blocking the UI render loop, and the firmware SHALL keep rendering (clock and cards) before and during connection. The firmware SHALL attempt to reconnect automatically if the connection drops.

#### Scenario: Joins the configured network
- **WHEN** the board powers on with valid credentials in `secrets.h`
- **THEN** it connects to the configured WiFi network as a station and logs its assigned IP

#### Scenario: UI renders before WiFi is up
- **WHEN** WiFi has not yet connected
- **THEN** the display already shows the clock and (empty/stale) cards rather than blocking

#### Scenario: Reconnects after a drop
- **WHEN** the WiFi connection is lost
- **THEN** the firmware attempts to reconnect automatically without a reboot, while the RTC clock keeps running

### Requirement: Advertise the board via mDNS
The firmware SHALL advertise itself on the local network via mDNS under the hostname `espresso.local` so the agent can reach it without a hardcoded IP address.

#### Scenario: Resolvable by name
- **WHEN** the board is connected to WiFi
- **THEN** it responds to mDNS for `espresso.local` on the local network

### Requirement: Ingest protocol messages over TCP
The firmware SHALL run a TCP line-server on a fixed port that accepts a client connection and reads newline-delimited JSON, feeding each complete line into the **same** parser and renderer used for the serial transport. Malformed lines SHALL be ignored without disrupting the last good state, identically to the serial path. The firmware SHALL continue to accept messages on the serial transport as well (both sources feed one parser).

#### Scenario: Network line is rendered
- **WHEN** a connected TCP client writes a valid newline-delimited JSON status message
- **THEN** the firmware parses and applies it exactly as if it had arrived over serial

#### Scenario: Either transport works
- **WHEN** a status message arrives over serial OR over the TCP connection
- **THEN** the firmware applies it through the same parsing/render path and updates the last-message timestamp

#### Scenario: Client reconnects
- **WHEN** the TCP client disconnects and a new connection is opened
- **THEN** the firmware accepts the new connection and resumes ingesting without a reboot

### Requirement: Support OTA firmware updates
The firmware SHALL support over-the-air firmware updates (ArduinoOTA) reachable at `espresso.local`, protected by a password supplied via `secrets.h`. OTA handling SHALL be polled non-blockingly within the main loop so it does not stall rendering.

#### Scenario: Wireless flash succeeds
- **WHEN** a developer uploads firmware over the network to `espresso.local` with the correct OTA password
- **THEN** the firmware accepts and applies the update without a USB connection

#### Scenario: Wrong password is rejected
- **WHEN** an OTA upload is attempted with an incorrect or missing password
- **THEN** the firmware rejects the update

### Requirement: Agent streams over the network with discovery and reconnect
The agent SHALL support a network transport that discovers the board at `espresso.local` (with a manual host/IP override available), opens a TCP connection, and writes the same host-link protocol messages it would write to serial. The agent SHALL select between serial and network transports, and SHALL auto-reconnect on the network without exiting. No new pip dependencies SHALL be required (standard-library sockets).

#### Scenario: Sends over the network
- **WHEN** the agent is in network mode and the board is reachable at `espresso.local`
- **THEN** it connects over TCP and writes one newline-delimited JSON status message per interval

#### Scenario: Manual host override
- **WHEN** mDNS resolution of `espresso.local` is unavailable and the user supplies a host/IP override
- **THEN** the agent connects to the supplied address instead

#### Scenario: Reconnects without exiting
- **WHEN** the TCP connection to the board fails (board reboot, WiFi drop, Mac sleep/wake)
- **THEN** the agent retries the connection and resumes streaming once the board is reachable again, without exiting
