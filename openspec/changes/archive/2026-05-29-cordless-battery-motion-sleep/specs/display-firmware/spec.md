## MODIFIED Requirements

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
