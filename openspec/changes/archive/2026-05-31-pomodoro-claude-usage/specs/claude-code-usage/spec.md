## ADDED Requirements

### Requirement: Token metric definition

The Claude Code token usage metric SHALL be computed from each Claude Code transcript entry of type `assistant` as the sum of its `message.usage.input_tokens`, `message.usage.output_tokens`, and `message.usage.cache_creation_input_tokens`. The metric SHALL exclude `cache_read_input_tokens`. The metric SHALL NOT depend on which model produced the entry (no per-model weighting), because the figure represents tokens, not cost.

#### Scenario: Cache-read tokens are excluded
- **WHEN** an assistant entry reports `input_tokens=8496`, `output_tokens=211`, `cache_creation_input_tokens=6693`, and `cache_read_input_tokens=18999`
- **THEN** the entry contributes `15400` to the token metric (the `18999` cache-read tokens are not counted)

#### Scenario: Non-assistant entries contribute nothing
- **WHEN** a transcript entry is not of type `assistant` or carries no `message.usage`
- **THEN** it contributes zero to the token metric

### Requirement: All-projects aggregation

The token metric SHALL aggregate Claude Code activity across all project transcripts under `~/.claude/projects/`, not a single project. Activity in any repository during a session SHALL be summed into one figure.

#### Scenario: Multiple projects summed
- **WHEN** the user has active Claude Code sessions in more than one project directory
- **THEN** the token metric reflects the combined total across all of them

### Requirement: Monotonic cumulative counter

The system SHALL expose the token metric as a single cumulative counter that only increases while the producing process runs. Consumers SHALL treat only differences of this counter as meaningful; its absolute value carries no defined meaning. The counter MAY reset to a lower value when the producing process restarts or when underlying transcript files are rotated or removed.

#### Scenario: Counter increases as work accrues
- **WHEN** new assistant turns are written to any transcript while the producer runs
- **THEN** the cumulative counter increases by the token metric of those turns

#### Scenario: Difference is the unit of meaning
- **WHEN** a consumer wants the tokens used over an interval
- **THEN** it subtracts the counter value at the start of the interval from the current value

### Requirement: Deduplication of transcript entries

When summing the token metric the system SHALL deduplicate entries by the pair `(message.id, requestId)` so that the same logical assistant response copied into multiple transcripts (e.g. resumed or forked sessions) is counted at most once.

#### Scenario: Duplicated entry counted once
- **WHEN** the same `(message.id, requestId)` appears in two transcript files
- **THEN** its token metric is added to the cumulative counter only once

### Requirement: Weekly quota percentage source

The weekly Claude Code usage SHALL be expressed as a 0–100 integer percentage of the subscription's weekly allowance, sourced from the claude-hud plugin cache at `~/.claude/plugins/claude-hud/.usage-cache.json` (`data.sevenDay`), aligned to the subscription reset window (`data.sevenDayResetAt`). The system SHALL NOT call any Anthropic usage API directly, to avoid depending on Keychain credential access, private endpoints, or OAuth token refresh. When the cache is missing, unreadable, or lacks the expected keys, the weekly percentage SHALL be treated as unavailable rather than fabricated.

#### Scenario: Weekly percentage read from cache
- **WHEN** the claude-hud cache reports `data.sevenDay=16`
- **THEN** the weekly usage is reported as 16% of the weekly allowance

#### Scenario: Cache unavailable
- **WHEN** the claude-hud cache file is absent or fails to parse
- **THEN** the weekly percentage is reported as unavailable and no API call is attempted

### Requirement: Local-only, unauthenticated transport awareness

The usage figures SHALL be derived only from local files on the host (no cloud calls) and SHALL carry no transcript content — only aggregate token counts and a quota percentage. It is acknowledged that these figures travel over the unauthenticated LAN host-link, so anyone on the same network segment could read them; given their low sensitivity this is accepted and not mitigated in this capability.

#### Scenario: No transcript content leaves the host
- **WHEN** usage figures are produced for transmission
- **THEN** they contain only numeric token totals and a quota percentage, never message text
