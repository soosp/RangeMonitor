#pragma once

/**
 * @file test_harness.h
 * @brief Minimal assertion helpers and a sample feeder for RangeMonitor.
 *
 * RangeMonitor takes the time as an argument, so the tests drive the clock
 * directly — no mock Arduino.h, no real time. Without ARDUINO defined the
 * header compiles as plain C++ with a no-op lock.
 */

#include "../src/RangeMonitor.h"
#include <cstdio>
#include <cmath>

using State = RangeMonitor::State;
using Cfg   = RangeMonitor::Config;
using Err   = RangeMonitor::ConfigError;

/** @brief Counts failures across a test binary; returned as the exit code. */
inline int& testFailures() { static int failures = 0; return failures; }

/** @brief Records a failing check and prints a one-line result. */
inline void checkImpl(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        testFailures()++;
        printf("  FAIL %s:%d  %s\n", file, line, expr);
    }
}

/** @brief Asserts a condition without aborting, so one run reports every failure. */
#define CHECK(expr) checkImpl((expr), #expr, __FILE__, __LINE__)

/** @brief Asserts that two states are equal and prints both names if not. */
#define CHECK_STATE(actual, expected) checkState((actual), (expected), #actual, __FILE__, __LINE__)

inline void checkState(State a, State e, const char* expr, const char* file, int line) {
    if (a != e) {
        char an[RangeMonitor::STATE_NAME_SIZE], en[RangeMonitor::STATE_NAME_SIZE];
        RangeMonitor::stateName(a, an, sizeof(an));
        RangeMonitor::stateName(e, en, sizeof(en));
        testFailures()++;
        printf("  FAIL %s:%d  %s is %s, expected %s\n", file, line, expr, an, en);
    }
}

/** @brief Asserts |a - b| <= tol. */
#define CHECK_NEAR(a, b, tol) checkImpl(std::fabs((double)(a) - (double)(b)) <= (tol), \
                                        #a " ~= " #b, __FILE__, __LINE__)

/** @brief Prints a section header inside a test binary. */
#define SECTION(name) printf("- %s\n", name)

/** @brief Prints the summary line and returns the exit code. */
inline int finish(const char* name) {
    if (testFailures() == 0) printf("%s: OK\n", name);
    else                     printf("%s: %d FAILED\n", name, testFailures());
    return testFailures() == 0 ? 0 : 1;
}

/**
 * @brief Feeds a monitor at a fixed interval and remembers the clock.
 *
 * Counts transitions into an alarm state, which is what most scenarios
 * assert on: "no alarm during the defrost", "exactly one alarm after".
 */
struct Feeder {
    RangeMonitor& m;
    uint32_t      now;        ///< Current time [ms]
    uint32_t      stepMs;     ///< Sampling interval [ms]
    int           alarms = 0; ///< Transitions into an alarm state

    Feeder(RangeMonitor& mon, uint32_t start = 0, uint32_t step = 10000)
        : m(mon), now(start), stepMs(step) {}

    /// One sample at the current time, then advance the clock.
    State feed(float v) {
        RangeMonitor::Transition t = m.update(v, now);
        if (t.changed() && RangeMonitor::isAlarm(t.to) && !RangeMonitor::isAlarm(t.from))
            alarms++;
        now += stepMs;
        return t.to;
    }

    /// The same value for @p seconds of samples. Returns the final state.
    State hold(float v, uint32_t seconds) {
        State s = m.state();
        uint32_t end = now + seconds * 1000UL;
        while ((int32_t)(end - now) > 0) s = feed(v);
        return s;
    }
};
