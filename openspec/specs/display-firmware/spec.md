# display-firmware Specification

## Purpose

The ESP32-S3 firmware acts as a "dumb" renderer for the desktop status display: it drives the ST7789P SPI LCD via Arduino_GFX + LVGL, keeps its own time on the onboard PCF85063 RTC, and renders CPU/RAM/upcoming-event data received over the host-link protocol while handling staleness and backlight power saving.
## Requirements
### Requirement: Initialize LCD display and LVGL
The firmware SHALL initialize the ST7789P SPI display (240×284) and the LVGL rendering stack via Arduino_GFX, enable the backlight (GPIO40), and SHALL render a UI frame on startup before any agent data arrives.

#### Scenario: Panel lights up standalone
- **WHEN** the board powers on with no agent connected
- **THEN** the LCD displays the UI (clock plus placeholder/empty data fields)

### Requirement: Keep time on the onboard RTC independently of the agent
The firmware SHALL keep time on the onboard PCF85063 RTC (I2C 0x51), seeded from the `time` field of incoming status messages, and SHALL render a clock that continues updating regardless of whether the agent is sending data. Once seeded, the clock SHALL advance from the free-running RTC and SHALL NOT freeze when serial data stops. No WiFi/NTP is required.

#### Scenario: Clock runs without agent
- **WHEN** the agent is not running or no serial data is arriving
- **THEN** the on-screen clock still advances using the free-running RTC

#### Scenario: RTC seeded from the host link
- **WHEN** a status message carrying a `time` field arrives and the RTC is unset or has drifted
- **THEN** the firmware sets the RTC from that time and renders the clock from the RTC

### Requirement: Render CPU, RAM, and upcoming events
The firmware SHALL display the latest CPU% and RAM% (e.g. as gauges/arcs) and a list of upcoming events received via the host-link protocol, updating the UI when new messages arrive.

#### Scenario: UI reflects latest message
- **WHEN** the firmware applies a newly parsed status message
- **THEN** the CPU and RAM indicators and the events list update to match it

### Requirement: Indicate stale data
The firmware SHALL detect when no valid status message has been received for a configurable timeout and SHALL visually indicate that the data is stale (e.g. graying values), while continuing to run the clock.

#### Scenario: Data goes stale
- **WHEN** no valid status message has been received for longer than the timeout
- **THEN** the firmware marks the CPU/RAM/events as stale while the clock keeps advancing

#### Scenario: Data resumes
- **WHEN** a valid status message arrives after a stale period
- **THEN** the firmware clears the stale indication and shows the fresh values

### Requirement: Backlight dimming and power saving
The firmware SHALL control the LCD backlight (GPIO40 PWM) for power saving, supporting reduced brightness and full blanking on a configurable schedule or condition. (No AMOLED burn-in concern on this LCD panel.) Backlight control SHALL coordinate with deep-sleep entry and exit: the backlight is turned off before entering deep sleep and restored on a full wake.

#### Scenario: Scheduled dimming
- **WHEN** the configured dimming condition is met (e.g. night hours or prolonged inactivity)
- **THEN** the firmware reduces backlight brightness via PWM

#### Scenario: Blanking
- **WHEN** the configured blanking condition is met
- **THEN** the firmware turns the backlight off, and restores it when the condition clears

#### Scenario: Backlight follows deep-sleep transitions
- **WHEN** the device enters deep sleep
- **THEN** the backlight is turned off before sleeping and restored only on a full wake (not on a brief timer-poll wake)

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

