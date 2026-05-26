## ADDED Requirements

### Requirement: Collect CPU and RAM usage
The agent SHALL sample the Mac's overall CPU utilization and used-memory percentage using `psutil`, expressing each as an integer percent in the range 0–100.

#### Scenario: Sampling produces in-range values
- **WHEN** the agent takes a sample
- **THEN** it produces a `cpu` and a `ram` integer each between 0 and 100 inclusive

### Requirement: Collect upcoming calendar events from EventKit
The agent SHALL read upcoming events from the local macOS calendar database via the native EventKit framework, invoked through a small code-signed Swift helper (`calbridge/eventbridge`) that the agent calls as a subprocess and whose JSON output it parses. The helper SHALL return a bounded number of the soonest future events with their start time and title. The agent SHALL NOT require OAuth, Google API credentials, or any cloud registration.

(Notes: (1) An earlier design used Calendar.app AppleScript via `osascript`, but its `whose`-filter event query proved unusably slow — over 150 seconds across a typical set of calendars. EventKit's predicate query returns in milliseconds. (2) A bare/unbundled Python process cannot obtain the macOS 14+ Calendars TCC grant — it is silently denied with no prompt — so EventKit is reached through a Swift binary that embeds the `NSCalendars*UsageDescription` strings and is ad-hoc code-signed, which is what makes the authorization prompt appear.)

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
- **THEN** it writes a single-line JSON status message (including the host's current local `time`, `cpu`, `ram`, and `events`) followed by a newline

#### Scenario: Port disappears and returns
- **WHEN** writing to the serial port fails (e.g. the board is unplugged)
- **THEN** the agent attempts to reopen the port and resumes streaming once it is available again, without exiting

### Requirement: Minimal-footprint install and optional auto-start
The agent SHALL depend only on Python 3 plus the `psutil` and `pyserial` pip packages, plus a one-time compile of the bundled `calbridge` Swift helper using the Xcode command line tools (`swiftc`/`codesign`), using built-in macOS frameworks otherwise. The agent SHALL provide an optional launchd configuration to run automatically in the background at login.

#### Scenario: Dependencies are limited
- **WHEN** a user sets up the agent
- **THEN** the only required pip installs are Python 3, `psutil`, and `pyserial`, plus building the bundled `calbridge` helper with the Xcode command line tools (no cloud, OAuth, or third-party services)

#### Scenario: Optional background auto-start
- **WHEN** the user installs the provided launchd configuration
- **THEN** the agent starts automatically in the background at login
