## ADDED Requirements

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
