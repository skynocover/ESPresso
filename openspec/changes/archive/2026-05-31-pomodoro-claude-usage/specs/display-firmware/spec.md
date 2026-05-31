## ADDED Requirements

### Requirement: Render Claude usage panels in the Pomodoro view

While the Pomodoro view is active the firmware SHALL render both usage figures: the this-session token figure (`cc_session − baseline`, abbreviated for the small screen, e.g. `34.4K`) co-located with the countdown inside the timer dial, and the weekly quota (a progress bar driven by `cc_week_pct` plus the `cc_week_reset` hint text) below the dial. Both SHALL be visible across the running, paused, and break phases. The countdown SHALL remain the primary visual element.

#### Scenario: Both figures shown while running
- **WHEN** the Pomodoro countdown is running with `cc_week_pct=16` and a session figure of 34,400 tokens
- **THEN** the firmware shows the session figure (≈ `34.4K`) with the countdown and a weekly bar at 16% with its reset hint below

#### Scenario: Rows persist while paused or on break
- **WHEN** the session is paused or in its break phase
- **THEN** both usage rows remain visible and continue to reflect the latest values

### Requirement: Completion screen highlights the session total

When the Pomodoro work countdown reaches zero, the completion (DONE) screen SHALL prominently display the tokens-used-this-session total as a result figure, alongside the weekly quota. This is in addition to the existing DONE indication (large text + backlight pulse).

#### Scenario: DONE shows the session result
- **WHEN** the work countdown completes after a session that used ~51,700 tokens
- **THEN** the DONE screen prominently shows the session total (e.g. `51.7K`) together with the weekly quota

### Requirement: No-value and frozen states for usage panels

When a usage field has no value (e.g. `cc_week_pct` unavailable because the claude-hud cache is missing), the firmware SHALL render a neutral placeholder (e.g. `—` / empty bar) rather than a fabricated number. When the agent disconnects, the firmware SHALL retain and keep displaying the last received usage values (frozen); it SHALL NOT clear them and is not required to show a staleness indicator in this change.

#### Scenario: Missing weekly percentage
- **WHEN** the weekly fields are absent from incoming messages
- **THEN** the weekly row shows a placeholder (`—` / empty bar) and the session row continues from `cc_session`

#### Scenario: Frozen on disconnect
- **WHEN** the agent stops sending messages mid-session
- **THEN** the usage panels keep showing the last received values without clearing them, while the countdown continues
