## ADDED Requirements

### Requirement: Collect CPU and RAM usage
The agent SHALL sample the Mac's overall CPU utilization and used-memory percentage using `psutil`, expressing each as an integer percent in the range 0–100.

#### Scenario: Sampling produces in-range values
- **WHEN** the agent takes a sample
- **THEN** it produces a `cpu` and a `ram` integer each between 0 and 100 inclusive

### Requirement: Collect upcoming calendar events from Calendar.app
The agent SHALL read upcoming events from the local macOS Calendar.app via AppleScript (`osascript`), returning a bounded number of the soonest future events with their start time and title. The agent SHALL NOT require OAuth, Google API credentials, or any cloud registration.

#### Scenario: Upcoming events are returned
- **WHEN** the local Calendar.app contains future events and access is granted
- **THEN** the agent returns up to the configured number of soonest events, each with an `HH:MM` start time and a title

#### Scenario: No readable calendar or no events
- **WHEN** Calendar.app has no upcoming events, or no calendar is readable
- **THEN** the agent returns an empty events list and continues running

#### Scenario: Calendar access not yet granted
- **WHEN** macOS has not granted the agent access to Calendar
- **THEN** the agent treats events as empty and continues running without crashing

### Requirement: Stream status over USB serial
The agent SHALL open the board's serial port (discovered by globbing `/dev/cu.usbmodem*`) and write one newline-delimited JSON status message per interval conforming to the host-link protocol.

#### Scenario: Writes valid protocol messages
- **WHEN** the agent has a fresh sample and an open port
- **THEN** it writes a single-line JSON status message followed by a newline

#### Scenario: Port disappears and returns
- **WHEN** writing to the serial port fails (e.g. the board is unplugged)
- **THEN** the agent attempts to reopen the port and resumes streaming once it is available again, without exiting

### Requirement: Minimal-footprint install and optional auto-start
The agent SHALL depend only on Python 3 plus the `psutil` and `pyserial` pip packages, using built-in macOS tooling otherwise. The agent SHALL provide an optional launchd configuration to run automatically in the background at login.

#### Scenario: Dependencies are limited
- **WHEN** a user sets up the agent
- **THEN** the only required installs are Python 3, `psutil`, and `pyserial`

#### Scenario: Optional background auto-start
- **WHEN** the user installs the provided launchd configuration
- **THEN** the agent starts automatically in the background at login
