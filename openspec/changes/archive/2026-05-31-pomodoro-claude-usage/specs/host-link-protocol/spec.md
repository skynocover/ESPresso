## ADDED Requirements

### Requirement: Claude Code usage fields

The protocol SHALL define three additional optional fields a status message may carry to convey Claude Code usage, per the `claude-code-usage` capability:

- `cc_session`: an integer cumulative token counter (the monotonic counter from `claude-code-usage`). When present it SHALL be a non-negative integer whose differences are the unit of meaning. It is normally always present.
- `cc_week_pct`: an integer 0–100, the weekly subscription quota percentage. It MAY be absent when the source is unavailable.
- `cc_week_reset`: a short human-readable, agent-preformatted reset hint string (e.g. `2天後重置`). It MAY be absent when the source is unavailable.

These fields are optional additions under the existing "unknown fields are ignored" rule, so older firmware remains compatible. The firmware SHALL apply whichever of these fields are present and SHALL treat absent fields as "no value" without disrupting other fields.

#### Scenario: Message with usage fields
- **WHEN** the agent sends `{"time":"2026-05-31 14:00:00","cpu":12,"ram":40,"events":[],"cc_session":382000,"cc_week_pct":16,"cc_week_reset":"2天後重置"}`
- **THEN** the firmware applies CPU/RAM/time/events as before and additionally records `cc_session=382000`, `cc_week_pct=16`, and the reset hint

#### Scenario: Usage fields partially absent
- **WHEN** a status message contains `cc_session` but omits `cc_week_pct` and `cc_week_reset`
- **THEN** the firmware records the session counter and treats the weekly percentage and reset hint as having no value

#### Scenario: Older firmware ignores the fields
- **WHEN** firmware without Claude-usage support receives a message containing `cc_session`/`cc_week_pct`/`cc_week_reset`
- **THEN** it ignores the unrecognized fields and applies the known fields unchanged
