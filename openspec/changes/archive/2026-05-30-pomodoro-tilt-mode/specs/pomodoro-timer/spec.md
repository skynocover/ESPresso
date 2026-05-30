## ADDED Requirements

### Requirement: Enter Pomodoro mode by landscape tilt

The firmware SHALL enter a fullscreen Pomodoro timer mode when the device is tilted into the calibrated landscape orientation (gravity vector dominated by −Y axis), and a 25-minute countdown SHALL start automatically on entry. The landscape classification SHALL apply hysteresis equivalent to the upright/face-down classifier so boundary jitter does not toggle mode.

#### Scenario: Tilt to landscape starts countdown
- **WHEN** the device is rotated into the calibrated landscape pose (left side down, ay ≤ −0.80g) and held for the configured debounce
- **THEN** the firmware switches to the Pomodoro screen and the 25:00 countdown begins immediately

#### Scenario: Boundary jitter does not toggle mode
- **WHEN** the gravity vector hovers near the landscape boundary
- **THEN** hysteresis prevents rapid entry/exit of Pomodoro mode

### Requirement: Cancel grace window after auto-start

The firmware SHALL provide a configurable grace window (default 3 seconds) immediately after entering Pomodoro mode during which a return to upright cancels the session without it counting as a started Pomodoro.

#### Scenario: Quick flip-back cancels
- **WHEN** the user returns the device to upright within the grace window after entry
- **THEN** the firmware exits Pomodoro mode and does not treat the session as started

#### Scenario: Past the grace window the session sticks
- **WHEN** the user returns to upright after the grace window has elapsed
- **THEN** the firmware exits Pomodoro mode and the session is considered to have run (countdown discarded)

### Requirement: Tap toggles running and paused

While in Pomodoro mode, a single touch tap reported by the touch-input capability SHALL toggle the countdown between running and paused. The remaining time SHALL freeze while paused and resume from the same value when tapped again.

#### Scenario: Tap during running pauses
- **WHEN** the countdown is running and a tap event arrives
- **THEN** the firmware freezes the remaining time and shows a paused indicator

#### Scenario: Tap during paused resumes
- **WHEN** the countdown is paused and a tap event arrives
- **THEN** the firmware resumes the countdown from the frozen remaining time

### Requirement: Return to upright exits Pomodoro mode

Returning the device to the upright pose (ax ≥ +0.80g) SHALL exit Pomodoro mode and restore the dashboard UI. Exiting after the grace window SHALL discard the in-progress countdown (no persistence).

#### Scenario: Upright exit during running
- **WHEN** the device is returned to upright while the countdown is running and the grace window has elapsed
- **THEN** the firmware exits Pomodoro mode and the dashboard UI is restored

#### Scenario: Upright exit during paused
- **WHEN** the device is returned to upright while the countdown is paused
- **THEN** the firmware exits Pomodoro mode and discards the paused session

### Requirement: Device-local timing independent of the host link

The countdown SHALL be driven by the device-local `millis()` clock, aligned for second ticks against the PCF85063 RTC if needed. The firmware SHALL NOT depend on the Mac agent or the host-link TCP connection being up; disconnection or absence of the agent SHALL NOT affect countdown progression or accuracy.

#### Scenario: Countdown runs with no agent
- **WHEN** the agent is not running or the host-link TCP is disconnected
- **THEN** the countdown still progresses correctly until completion

#### Scenario: Countdown unaffected by host-link traffic
- **WHEN** host-link status messages arrive during a Pomodoro session
- **THEN** the countdown timing is not influenced by their arrival or content

### Requirement: Completion indication uses screen and backlight only

When the countdown reaches zero the firmware SHALL show a completion screen (large "DONE" text or equivalent) and SHALL pulse the LCD backlight (GPIO40 PWM) to draw attention. The firmware SHALL NOT use the ES8311 audio codec or the onboard speaker in this change.

#### Scenario: Completion shows DONE with backlight pulse
- **WHEN** the countdown reaches zero
- **THEN** the firmware switches to the completion screen and pulses the backlight

#### Scenario: No audio is emitted
- **WHEN** the completion screen is shown
- **THEN** the ES8311 codec and I2S peripherals are not engaged

### Requirement: Completion screen exits on user action or timeout

The completion screen SHALL persist until one of: the device is returned to upright (exit to dashboard), tap (exit to dashboard), face-down hold (deep sleep via motion-sleep), or a configurable timeout (default 60 s) after which it auto-returns to the dashboard.

#### Scenario: Tap dismisses completion
- **WHEN** the completion screen is showing and a tap event arrives
- **THEN** the firmware returns to the dashboard UI

#### Scenario: Upright dismisses completion
- **WHEN** the device is returned to upright while the completion screen is showing
- **THEN** the firmware returns to the dashboard UI

#### Scenario: Timeout dismisses completion
- **WHEN** the completion screen has been shown for longer than the configured timeout
- **THEN** the firmware returns to the dashboard UI

### Requirement: Pomodoro state is volatile

Pomodoro mode state (running/paused/done, remaining time) SHALL NOT be persisted to NVS, RTC memory, or any non-volatile store. Loss of power, deep sleep, or face-down preemption SHALL clear the state; resumed sessions are not supported in this change.

#### Scenario: Power cycle clears state
- **WHEN** the device loses power during a Pomodoro session
- **THEN** the next boot starts in dashboard mode with no in-progress countdown

#### Scenario: Deep sleep clears state
- **WHEN** the device enters deep sleep from any Pomodoro state
- **THEN** the countdown progress is discarded and the next wake starts in dashboard mode
