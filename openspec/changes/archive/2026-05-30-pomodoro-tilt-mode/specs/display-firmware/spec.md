## ADDED Requirements

### Requirement: Fullscreen mode switch for Pomodoro

The firmware SHALL support switching the display between the existing dashboard view (clock, CPU/RAM, events) and a fullscreen Pomodoro view. The switch SHALL be driven by the Pomodoro mode entry/exit conditions defined in the `pomodoro-timer` capability, and the Pomodoro view SHALL take over the full panel (no dashboard cards visible) while active.

#### Scenario: Enter Pomodoro view
- **WHEN** the Pomodoro capability signals entry into Pomodoro mode
- **THEN** the firmware hides the dashboard cards and displays the fullscreen Pomodoro view

#### Scenario: Exit back to dashboard
- **WHEN** the Pomodoro capability signals exit (upright return, tap on completion, timeout, etc.)
- **THEN** the firmware restores the dashboard view with the latest received values

### Requirement: Host-link data continues to be received in Pomodoro view

While the Pomodoro view is active, the firmware SHALL continue to accept and parse host-link status messages (CPU, RAM, events, time) and update its in-memory model, but SHALL NOT render those values to the screen. On exit back to the dashboard the firmware SHALL display the most recently received values; stale-detection logic SHALL behave as if the Pomodoro view never interrupted reception.

#### Scenario: Data model updates without rendering
- **WHEN** a status message arrives while the Pomodoro view is active
- **THEN** the firmware updates its in-memory CPU/RAM/events model without redrawing the dashboard

#### Scenario: Dashboard shows latest values on exit
- **WHEN** the firmware exits Pomodoro mode after host-link data arrived during the session
- **THEN** the dashboard immediately reflects the most recently received values

#### Scenario: RTC seeding still applies in Pomodoro view
- **WHEN** a status message carrying a `time` field arrives while the Pomodoro view is active
- **THEN** the firmware seeds the PCF85063 RTC as usual, even though the dashboard clock is not visible
