# mac-data-agent Specification

## Purpose

The Mac data agent is a lightweight Python process that samples the host's CPU/RAM usage and upcoming calendar events, then streams them as newline-delimited JSON over USB serial to the ESP32 firmware, with a minimal dependency footprint and optional launchd auto-start.
## Requirements
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

### Requirement: Minimal-footprint install and optional auto-start
The agent SHALL depend only on Python 3 plus the `psutil` and `pyserial` pip packages, plus a one-time compile of the bundled `calbridge` Swift helper using the Xcode command line tools (`swiftc`/`codesign`), using built-in macOS frameworks otherwise. The agent SHALL provide an optional launchd configuration to run automatically in the background at login.

#### Scenario: Dependencies are limited
- **WHEN** a user sets up the agent
- **THEN** the only required pip installs are Python 3, `psutil`, and `pyserial`, plus building the bundled `calbridge` helper with the Xcode command line tools (no cloud, OAuth, or third-party services)

#### Scenario: Optional background auto-start
- **WHEN** the user installs the provided launchd configuration
- **THEN** the agent starts automatically in the background at login

### Requirement: Collect Claude Code token usage by tailing transcripts

The agent SHALL maintain a cumulative token counter (per the `claude-code-usage` capability) by reading Claude Code transcript files incrementally. At startup the agent SHALL record the current byte length of each transcript under `~/.claude/projects/` as a bookmark without parsing their contents, and seed the counter at zero. On each refresh the agent SHALL read only the bytes appended past each bookmark, parse the new lines, add the token metric of new `assistant` entries (deduplicated by `(message.id, requestId)`), and advance the bookmarks; newly appeared files SHALL be read in full. The agent SHALL NOT re-scan the entire transcript corpus on each refresh.

#### Scenario: Incremental tail, not full re-scan
- **WHEN** the agent refreshes its token counter after the user has had one new exchange
- **THEN** it reads only the newly appended bytes of the affected transcript(s) and adds their token metric, without re-reading unchanged files

#### Scenario: New project file appears
- **WHEN** a transcript file that did not exist at startup appears
- **THEN** the agent reads it in full once and bookmarks its end

### Requirement: Collect weekly quota from claude-hud cache

The agent SHALL read the weekly quota percentage and reset hint from the claude-hud plugin cache (`~/.claude/plugins/claude-hud/.usage-cache.json`, `data.sevenDay` and `data.sevenDayResetAt`) rather than calling any Anthropic API. The agent SHALL preformat the reset timestamp into a short human-readable hint string in the host's local timezone. When the cache is missing or unparseable the agent SHALL omit the weekly fields and continue running.

#### Scenario: Weekly fields populated from cache
- **WHEN** the claude-hud cache reports `sevenDay=16` and a `sevenDayResetAt` two days out
- **THEN** the agent emits `cc_week_pct=16` and a `cc_week_reset` hint such as `2天後重置`

#### Scenario: Cache missing
- **WHEN** the claude-hud cache file does not exist
- **THEN** the agent omits `cc_week_pct` and `cc_week_reset` and keeps streaming the other fields

### Requirement: Include usage fields in host-link messages at a decoupled cadence

The agent SHALL include the Claude Code usage fields (`cc_session`, and when available `cc_week_pct`/`cc_week_reset`) in the host-link status messages defined by `host-link-protocol`. The agent MAY refresh the token counter less often than the base send interval (re-tailing approximately every other tick) and SHALL refresh the weekly quota infrequently (approximately every 30 seconds); between refreshes it SHALL reuse the last computed values. The base CPU/RAM/time send cadence SHALL be unaffected.

#### Scenario: Usage piggybacks on existing messages
- **WHEN** the agent sends a periodic status message
- **THEN** the message also carries the latest known `cc_session` (and weekly fields when available)

#### Scenario: Weekly refreshed less often than sent
- **WHEN** several status messages are sent within a 30-second window
- **THEN** they reuse the same `cc_week_pct`/`cc_week_reset` without re-reading the cache each time

### Requirement: Fake sender supports usage fields

The bundled `fake_sender.py` test utility SHALL be able to inject synthetic `cc_session`, `cc_week_pct`, and `cc_week_reset` values (including their absence) so the firmware usage panels, the `—` no-value state, and the completion screen can be exercised without consuming real Claude Code tokens.

#### Scenario: Drive the UI without real usage
- **WHEN** the developer runs the fake sender with synthetic usage values
- **THEN** the firmware renders the corresponding weekly bar and session counter without any real Claude Code activity

