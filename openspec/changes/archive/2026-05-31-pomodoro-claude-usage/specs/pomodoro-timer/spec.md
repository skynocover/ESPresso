## ADDED Requirements

### Requirement: Snapshot a usage baseline on Pomodoro entry

On entering Pomodoro mode the firmware SHALL snapshot the current `cc_session` cumulative counter as a baseline. The tokens-used-this-session figure SHALL be computed as `cc_session − baseline`. Each fresh entry into Pomodoro mode SHALL re-snapshot the baseline (resetting the session figure to zero). The baseline SHALL be volatile (RAM only) like all other Pomodoro state.

#### Scenario: Baseline captured at entry
- **WHEN** the device tilts into Pomodoro mode while `cc_session` is 380000
- **THEN** the firmware records baseline 380000 and the session figure begins at 0

#### Scenario: Re-entry resets the session figure
- **WHEN** the user exits and later re-enters Pomodoro mode
- **THEN** a new baseline is snapshotted and the session figure restarts from 0

### Requirement: Session figure spans the whole Pomodoro session

The tokens-used-this-session figure SHALL accumulate across the entire time the device is in Pomodoro mode — work running, paused, completion, and the break phase — until the session is exited (return to upright). Break-phase Claude Code usage SHALL be included.

#### Scenario: Usage during break is included
- **WHEN** Claude Code is used during the break phase of an active Pomodoro session
- **THEN** that usage is reflected in the session figure (no per-phase reset)

### Requirement: Self-heal when the cumulative counter regresses

If the firmware receives a `cc_session` value lower than the current baseline (e.g. the agent restarted or transcripts were rotated), it SHALL re-snapshot the baseline to the new value so the displayed session figure never goes negative or spikes. At most the session under-counts once across such an event.

#### Scenario: Agent restart does not produce a negative figure
- **WHEN** `cc_session` drops below the stored baseline
- **THEN** the firmware resets the baseline to the received value and the session figure stays non-negative

### Requirement: Usage figures do not affect countdown timing or agent independence

The presence, absence, or content of Claude Code usage fields SHALL NOT influence countdown progression. The countdown SHALL continue to run from the device-local `millis()` clock with no agent connection, per the existing device-local timing requirement; only the usage panels degrade when the agent is absent.

#### Scenario: Countdown unaffected by usage data
- **WHEN** usage fields arrive, change, or stop arriving during a session
- **THEN** the countdown timing is unaffected and continues to completion

#### Scenario: Timer runs while usage is unavailable
- **WHEN** no agent is connected so no usage fields arrive
- **THEN** the countdown still runs to completion and the usage panels show no value
