## ADDED Requirements

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
The firmware SHALL control the LCD backlight (GPIO40 PWM) for always-on power saving, supporting reduced brightness and full blanking on a configurable schedule or condition. (No AMOLED burn-in concern on this LCD panel.)

#### Scenario: Scheduled dimming
- **WHEN** the configured dimming condition is met (e.g. night hours or prolonged inactivity)
- **THEN** the firmware reduces backlight brightness via PWM

#### Scenario: Blanking
- **WHEN** the configured blanking condition is met
- **THEN** the firmware turns the backlight off, and restores it when the condition clears
