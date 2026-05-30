## ADDED Requirements

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
