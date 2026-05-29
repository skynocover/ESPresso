# battery-status Specification

## Purpose
TBD - created by archiving change cordless-battery-motion-sleep. Update Purpose after archive.
## Requirements
### Requirement: Read and display battery state from AXP2101
The firmware SHALL read battery presence, charge percentage, and charging status from the AXP2101 PMU and reflect them in the status bar. When no battery is connected, the battery indicator SHALL be hidden so layout reclaims the space.

#### Scenario: Battery present on USB power
- **WHEN** a battery is connected and the device is on USB power and charging
- **THEN** the status bar shows the charge percentage with a charging glyph

#### Scenario: Battery present on battery power
- **WHEN** a battery is connected and the device is running off the battery (not charging)
- **THEN** the status bar shows the charge percentage with a battery-level glyph that reflects the current level

#### Scenario: No battery connected
- **WHEN** the AXP2101 reports no battery present (or percentage read fails)
- **THEN** the battery indicator is hidden and surrounding elements reclaim the space

### Requirement: Low-battery alert
The firmware SHALL alert the user when the battery charge falls to or below configurable warning thresholds, escalating as the level drops. The alert SHALL be visual; an audible cue via the ES8311 codec MAY be emitted and SHALL be configurable.

#### Scenario: Battery reaches warning threshold
- **WHEN** the battery level falls to or below the warning threshold (e.g. ≤20%) while not charging
- **THEN** the battery indicator changes to a warning color (e.g. flashing red)

#### Scenario: Battery reaches critical threshold
- **WHEN** the battery level falls to or below the critical threshold (e.g. ≤10%) while not charging
- **THEN** the firmware emits an escalated visual warning and, if the audible cue is enabled, plays a short alert tone

#### Scenario: Alert clears on charge
- **WHEN** the device is plugged in and begins charging after a low-battery alert
- **THEN** the warning state clears and the indicator returns to the normal charging display

### Requirement: Battery-rail continuity for the RTC
The firmware SHALL rely on the AXP2101/PCF85063 battery-backed rail so the RTC keeps time across power transitions and deep sleep, and SHALL NOT disable that rail when entering sleep.

#### Scenario: RTC survives sleep and power loss
- **WHEN** the device enters deep sleep or loses main power while a battery is connected
- **THEN** the PCF85063 RTC continues to keep time and the clock is correct immediately on the next wake

