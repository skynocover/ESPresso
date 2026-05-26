## ADDED Requirements

### Requirement: Newline-delimited JSON message framing
The agent and firmware SHALL exchange data as UTF-8 JSON objects, one object per line, terminated by `\n`. The firmware SHALL parse input line-by-line and SHALL ignore any line that is not valid JSON without disrupting the last known good state.

#### Scenario: Well-formed line is applied
- **WHEN** the firmware reads a complete line containing a valid JSON status object
- **THEN** it parses the object and updates the displayed values

#### Scenario: Malformed line is ignored
- **WHEN** the firmware reads a line that fails JSON parsing or is truncated
- **THEN** it discards that line and retains the previously displayed values without crashing

### Requirement: Status message schema
A status message SHALL contain a `cpu` integer (0–100, percent), a `ram` integer (0–100, percent), and an `events` array. Each event object SHALL contain a `t` string (start time, `HH:MM`, 24-hour) and a `title` string. The `events` array MAY be empty. Unknown fields SHALL be ignored by the firmware to allow forward-compatible additions.

#### Scenario: Full status message
- **WHEN** the agent sends `{"cpu":42,"ram":68,"events":[{"t":"14:30","title":"Standup"}]}`
- **THEN** the firmware displays CPU 42%, RAM 68%, and one upcoming event "14:30 Standup"

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
