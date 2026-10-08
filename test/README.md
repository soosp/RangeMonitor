# Host tests

`RangeMonitor` takes the time as an argument and, without `ARDUINO` defined,
compiles as plain C++ with a no-op lock. The whole state machine therefore
runs on a PC: no board, no mock `Arduino.h`, no real time.

## Running

Press `Ctrl+Shift+B` in VS Code — the default build task builds and runs every
test and reports a summary.

From a shell:

```sh
bash test/run_tests.sh   # Linux / macOS
test\run_tests.bat       # Windows
```

Both scripts exit non-zero if any test fails. They need nothing but a C++17
compiler (`g++` by default; override with `CXX`).

On Windows the VSCode task prepends `C:\msys64\ucrt64\bin` to `PATH` so the
MSYS2 toolchain is found without changing the system PATH. If MSYS2 lives
elsewhere, edit that entry in `.vscode/tasks.json`.

## What is covered

|File|Area|
|---|---|
|`test_states.cpp`|Soft/hard limits, delay, hysteresis holds, one-sided and hard-only ranges|
|`test_filter.cpp`|Median filter, EMA step response, irregular sampling, warm-up|
|`test_fault.cpp`|Fault delay, FAULT as an overlay, stale-filter reset|
|`test_config.cpp`|Validation rules, reconfigure/reset, clock wrap-around, helpers|
|`test_scenarios.cpp`|Whole days: freezer defrosts, compressor failure, open door, server room|

`test_harness.h` holds `CHECK()` / `CHECK_STATE()` (record a failure and keep
going) and `Feeder`, which feeds a monitor at a fixed interval, keeps the
clock, and counts alarms raised.

## Replaying recorded data

`replay.cpp` is not a test but a tuning tool: it feeds a CSV recording through
a configuration and prints every transition.

```sh
g++ -std=c++17 -Ithird_party replay.cpp -o build/replay
build/replay --lo -22 --hi -15 --hi-hard -5 --hyst 1 --tau 300 --delay 2700 data.csv
```

Input lines are `<seconds>,<value>`; seconds may be fractional and irregular,
an empty value, `nan` or `null` is an invalid sample, and lines that do not
start with a number are skipped. `--trace` prints every sample; `--help`
lists the options.

## third_party/

`ExponentialAverage.h` is a copy of the header from
[RunningStatistics](https://github.com/soosp/RunningStatistics) 1.1.1 (MIT),
so the tests build without a library manager. Device builds use the
installed RunningStatistics library, never this copy.
