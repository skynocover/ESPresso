# motion-sleep Specification

## Purpose
TBD - created by archiving change cordless-battery-motion-sleep. Update Purpose after archive.
## Requirements
### Requirement: Orientation detection from the IMU
The firmware SHALL determine device orientation from the QMI8658 accelerometer's gravity vector, classifying at least "upright/active" versus "face-down/sleep" poses. The axis sign mapping SHALL be calibrated against the physical board, and classification SHALL apply hysteresis so boundary jitter does not toggle state.

#### Scenario: Upright is classified active
- **WHEN** the device is standing upright in its normal viewing pose
- **THEN** the firmware classifies orientation as active

#### Scenario: Face-down is classified sleep
- **WHEN** the device is laid flat with the screen facing down
- **THEN** the firmware classifies orientation as the sleep pose

#### Scenario: Jitter does not toggle state
- **WHEN** the gravity vector hovers near the classification boundary
- **THEN** hysteresis prevents rapid back-and-forth state changes

### Requirement: Enter deep sleep when laid face-down
The firmware SHALL enter `esp_deep_sleep` after the device has been continuously in the face-down pose for a configurable debounce duration. Before sleeping it SHALL turn off the backlight and instruct the AXP2101 to disable the display power rail (ALDO3) to minimize standby current.

#### Scenario: Sustained face-down triggers sleep
- **WHEN** the device remains face-down for at least the configured debounce duration
- **THEN** the firmware turns off the backlight, disables the display rail, and enters deep sleep

#### Scenario: Brief face-down does not sleep
- **WHEN** the device is face-down for less than the debounce duration before being lifted
- **THEN** the firmware does not enter deep sleep

### Requirement: Wake from deep sleep
The firmware SHALL configure two deep-sleep wake sources: a periodic RTC timer (1–2s interval) and a touch interrupt on ext0 (TP_INT, GPIO13). It SHALL NOT depend on a QMI8658 hardware interrupt, because that line is not exposed on a usable RTC GPIO.

#### Scenario: Timer wake, still face-down
- **WHEN** the RTC timer wakes the device and a fast I2C read shows it is still face-down
- **THEN** the firmware re-enters deep sleep without initializing the display or WiFi, keeping the wake window to tens of milliseconds

#### Scenario: Timer wake, now upright
- **WHEN** the RTC timer wakes the device and the gravity read shows it is now upright
- **THEN** the firmware performs a full boot, initializing the display, WiFi, and host link

#### Scenario: Touch wake
- **WHEN** the touch controller asserts TP_INT during deep sleep
- **THEN** the device wakes and performs a full boot regardless of orientation

### Requirement: State restoration after wake
On a full wake the firmware SHALL re-seed the clock from the PCF85063 RTC so the time is correct immediately, render the UI, and reconnect the host link. Data fields with no fresh value (CPU/RAM/events) SHALL render as stale until the agent reconnects.

#### Scenario: Clock correct immediately, data catches up
- **WHEN** the device performs a full wake after deep sleep
- **THEN** the clock shows the correct time at once, while CPU/RAM/events show stale state until the agent reconnects (~3–5s)

### Requirement: Landscape orientation classification

The firmware SHALL extend orientation classification to include a "landscape" pose, derived from the QMI8658 accelerometer gravity vector with the −Y axis dominant (left-side-down, per the existing board calibration table). Classification SHALL apply hysteresis comparable to the upright and face-down classifiers, and SHALL be exposed for use by the Pomodoro mode trigger.

#### Scenario: Left-side-down is classified landscape
- **WHEN** the device is rotated so the left side is down and `ay ≤ −0.80g`
- **THEN** the firmware classifies orientation as landscape

#### Scenario: Landscape hysteresis
- **WHEN** the gravity Y component hovers near the landscape entry threshold
- **THEN** hysteresis prevents rapid toggling of the landscape state

### Requirement: Face-down preempts foreground state

Face-down detection SHALL be evaluated at the highest priority on every IMU tick, before any other orientation classification (upright, landscape, etc.). When the face-down hold duration elapses, the firmware SHALL enter deep sleep regardless of which foreground state is active (dashboard, Pomodoro running, Pomodoro paused, Pomodoro done) and SHALL discard any in-progress Pomodoro state before sleeping.

#### Scenario: Face-down during Pomodoro running
- **WHEN** the device is laid face-down for the configured hold duration while the Pomodoro countdown is running
- **THEN** the firmware enters deep sleep and the countdown progress is discarded

#### Scenario: Face-down during Pomodoro paused
- **WHEN** the device is laid face-down for the configured hold duration while a Pomodoro session is paused
- **THEN** the firmware enters deep sleep and the paused session is discarded

#### Scenario: Face-down during completion screen
- **WHEN** the device is laid face-down for the configured hold duration while the Pomodoro completion screen is showing
- **THEN** the firmware enters deep sleep

#### Scenario: Priority order
- **WHEN** an IMU sample classifies as both landscape-eligible and face-down-eligible during boundary conditions
- **THEN** the firmware evaluates face-down first and lets face-down win

