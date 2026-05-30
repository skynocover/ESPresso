# touch-input Specification

## Purpose

The firmware reads the CST816D touch controller at runtime to deliver coordinate-agnostic tap events to UI consumers (e.g. the Pomodoro timer). Reads are interrupt-driven off the TP_INT line, and the runtime tap path coexists with the controller's existing role as the deep-sleep ext0 wake source.

## Requirements

### Requirement: Initialize CST816D for runtime reads

The firmware SHALL initialize the CST816D touch controller during boot so it can be read at runtime, in addition to its existing role as a deep-sleep ext0 wake source. Initialization SHALL release the controller from reset (TP_RST=GPIO39) and verify presence on the shared I2C bus (SDA=GPIO15, SCL=GPIO14) before declaring the touch input available.

#### Scenario: Touch controller present on I2C
- **WHEN** the firmware boots and the CST816D responds on I2C
- **THEN** the firmware reports the touch input as available and proceeds to use it for runtime events

#### Scenario: Touch controller missing
- **WHEN** the firmware boots and the CST816D does not respond on I2C
- **THEN** the firmware logs a warning and continues with touch events disabled; other UI paths remain functional

### Requirement: Deliver tap events from INT-driven reads

The firmware SHALL detect touch events using the CST816D INT line (TP_INT=GPIO13, active-low) as the trigger: when the line transitions low the firmware SHALL read the controller once over I2C and emit a single "tap" event for the leading touch-down edge. Continuous polling without an INT trigger is not required and SHALL be avoided.

#### Scenario: Touch produces one tap event
- **WHEN** the user touches the screen once and releases
- **THEN** the firmware emits exactly one tap event corresponding to the touch-down edge

#### Scenario: No event without INT
- **WHEN** the INT line stays high
- **THEN** the firmware does not poll the controller over I2C and emits no events

### Requirement: Tap events are coordinate-agnostic

Tap events SHALL be delivered as edge notifications only, without requiring the consumer to interpret X/Y coordinates. Coordinate rotation between portrait and landscape orientations is therefore not required for the tap event in this change. (Coordinate mapping for future gestures or hit-tested buttons is out of scope.)

#### Scenario: Same tap event regardless of orientation
- **WHEN** the same physical touch occurs while the device is upright versus while it is in landscape Pomodoro mode
- **THEN** the firmware emits the same tap event in both cases

### Requirement: Coexistence with deep-sleep wake

The runtime tap path SHALL NOT interfere with the existing role of TP_INT as the ext0 deep-sleep wake source. Tap events SHALL only be emitted while the device is awake; deep-sleep wake behaviour for touch SHALL remain unchanged.

#### Scenario: Sleep wake still works
- **WHEN** the device is in deep sleep and the user touches the screen
- **THEN** the device wakes via ext0 as before, regardless of the runtime tap implementation

#### Scenario: No spurious tap on wake
- **WHEN** the device performs a full wake triggered by touch
- **THEN** the firmware does not deliver the wake-causing touch as a runtime tap event to UI consumers
