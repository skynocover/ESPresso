## MODIFIED Requirements

### Requirement: Stream status over USB serial
When the serial transport is selected, the agent SHALL open the board's serial port (discovered by globbing `/dev/cu.usbmodem*`) and write one newline-delimited JSON status message per interval conforming to the host-link protocol. Serial is one of two supported transports; the network transport is defined by the `wireless-link` capability. The agent SHALL select which transport to use (serial, network, or auto-discovery).

#### Scenario: Writes valid protocol messages
- **WHEN** the agent has a fresh sample and an open serial port
- **THEN** it writes a single-line JSON status message (including the host's current local `time`, `cpu`, `ram`, and `events`) followed by a newline

#### Scenario: Port disappears and returns
- **WHEN** writing to the serial port fails (e.g. the board is unplugged)
- **THEN** the agent attempts to reopen the port and resumes streaming once it is available again, without exiting

#### Scenario: Transport is selectable
- **WHEN** the user runs the agent
- **THEN** the agent can stream over the serial transport or the network transport (per `wireless-link`), defaulting to discovering the board
