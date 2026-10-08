# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `getStatus()` and the `Status` struct, matching the naming used across the
  library suite (`DigitalOutput`, `DS18B20Sensor`)

### Deprecated

- `snapshot()` and `Snapshot`: renamed to `getStatus()` and `Status`. The old
  names still work but produce a compile-time deprecation warning

### Fixed

- Make `run_tests.sh` executable

## [0.1.0] - 2026-10-08

### Added

- `RangeMonitor`: range monitor for one scalar measurement
  - median-of-3 spike filter ahead of a time-based EMA
  - soft limits with an alarm delay (`PENDING_*` → `ALARM_*`)
  - hard limits that alarm at once (`CRITICAL_*`); an unset hard limit
    follows its soft limit
  - any limit may be unset, for one-sided ranges
  - hysteresis on every exit, including the exit from `PENDING_*`
  - `FAULT` after a configurable run of invalid samples, resumed from the
    interrupted state when readings return; the filter is discarded after an
    outage longer than 3 × tau
  - `update()` returns the state `Transition`; `update(value, nowMs)` takes
    the time explicitly
  - `Config::validate()` with a specific `ConfigError` for each rule
  - `reconfigure()` keeps the filter and the state
  - `isAlarm()`, `isHigh()`, `isLow()`, `stateName()` helpers
  - ESP32: thread-safe, mutex in a static buffer; ESP8266 / AVR: no-op lock
- Host test suite in `test/`, runnable from VS Code with Ctrl+Shift+B
- `test/replay.cpp`: replays a CSV recording through a configuration
- Examples: `Basic`, `DefrostSimulation`

[Unreleased]: https://github.com/soosp/RangeMonitor/compare/0.1.0...HEAD
[0.1.0]: https://github.com/soosp/RangeMonitor/releases/tag/0.1.0
