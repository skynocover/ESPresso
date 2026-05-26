# host-link-protocol Specification

## Purpose

The host-link protocol is the contract between the Mac data agent and the ESP32 firmware: newline-delimited UTF-8 JSON status messages carrying the host time, CPU%, RAM%, and upcoming events, sent at a regular cadence over USB serial.

## Requirements

### Requirement: Newline-delimited JSON message framing
The agent and firmware SHALL exchange data as UTF-8 JSON objects, one object per line, terminated by `\n`. The firmware SHALL parse input line-by-line and SHALL ignore any line that is not valid JSON without disrupting the last known good state.

#### Scenario: Well-formed line is applied
- **WHEN** the firmware reads a complete line containing a valid JSON status object
- **THEN** it parses the object and updates the displayed values

#### Scenario: Malformed line is ignored
- **WHEN** the firmware reads a line that fails JSON parsing or is truncated
- **THEN** it discards that line and retains the previously displayed values without crashing

### Requirement: Status message schema
A status message SHALL contain a `time` string (the host's current local time, `YYYY-MM-DD HH:MM:SS`, 24-hour), a `cpu` integer (0–100, percent), a `ram` integer (0–100, percent), and an `events` array. Each event object SHALL contain a `t` string (start time, `HH:MM`, 24-hour) and a `title` string. The `events` array MAY be empty. Unknown fields SHALL be ignored by the firmware to allow forward-compatible additions.

#### Scenario: Full status message
- **WHEN** the agent sends `{"time":"2026-05-26 14:29:58","cpu":42,"ram":68,"events":[{"t":"14:30","title":"Standup"}]}`
- **THEN** the firmware displays CPU 42%, RAM 68%, one upcoming event "14:30 Standup", and seeds its clock from the `time` field

### Requirement: Time seeds the firmware RTC
The agent SHALL include the host's current local time in each status message via the `time` field. The firmware SHALL use this field to set its onboard PCF85063 RTC when the RTC is unset or has drifted beyond a small threshold from the received time, and SHALL otherwise let the RTC free-run. The displayed clock SHALL be driven by the RTC, not directly by the latest `time` field, so the clock keeps advancing when no messages arrive.

#### Scenario: RTC seeded on first message
- **WHEN** the firmware receives its first valid status message containing a `time` field
- **THEN** it sets the RTC to that time and renders the clock from the RTC

#### Scenario: Clock free-runs without the agent
- **WHEN** no status message has arrived for an extended period
- **THEN** the displayed clock continues advancing from the free-running RTC

#### Scenario: Empty events
- **WHEN** the agent sends a message whose `events` array is empty
- **THEN** the firmware shows the CPU/RAM values and an empty/"no upcoming events" state

#### Scenario: Forward-compatible unknown field
- **WHEN** the agent sends a message containing an additional field the firmware does not recognize
- **THEN** the firmware applies the known fields and ignores the unknown field

### Requirement: Update cadence and freshness
The agent SHALL send a status message at a regular interval (default every 2 seconds). The firmware SHALL record the time of the last successfully parsed message so that staleness can be detected.

#### Scenario: Regular updates
- **WHEN** the agent is running normally
- **THEN** it emits a status message approximately every 2 seconds

#### Scenario: Freshness timestamp recorded
- **WHEN** the firmware successfully parses a status message
- **THEN** it updates its internal "last message received" timestamp
