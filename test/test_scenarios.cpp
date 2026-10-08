/**
 * @file test_scenarios.cpp
 * @brief Whole-day scenarios with every stage of the chain switched on.
 *
 * The profiles are synthetic: shaped like the real thing, not recorded.
 * Recorded data can be replayed with test/replay.cpp (see test/README.md).
 */

#include "test_harness.h"

/**
 * Freezer at -18 °C that defrosts every 6 hours: the coil heater lifts the
 * air to -8 °C in 10 minutes, holds it there for 10, and the compressor pulls
 * it back down in 15.
 */
static float freezer(uint32_t s) {
    const uint32_t period = 6 * 3600;
    uint32_t t = s % period;
    const uint32_t start = 3 * 3600;
    if (t < start)            return -18.0f;
    t -= start;
    if (t < 600)              return -18.0f + 10.0f * t / 600.0f;    // heat-up
    if (t < 1200)             return -8.0f;                           // hold
    if (t < 2100)             return -8.0f - 10.0f * (t - 1200) / 900.0f; // pull-down
    return -18.0f;
}

/// Freezer configuration: soft -22..-15, hard high -5, 45-minute delay.
static Cfg freezerCfg() {
    Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 300);
    c.hiHard            = -5.0f;
    c.alarmDelaySeconds = 45 * 60;
    c.faultDelaySeconds = 60;
    return c;
}

int main() {
    printf("test_scenarios\n");

    SECTION("freezer: four defrosts a day, no alarm");
    {
        RangeMonitor m(freezerCfg());
        Feeder f(m, 0, 10000);
        int pendings = 0;
        State prev = State::UNKNOWN;
        for (uint32_t s = 0; s < 24 * 3600; s += 10) {
            State st = f.feed(freezer(s));
            if (st == State::PENDING_HIGH && prev != State::PENDING_HIGH) pendings++;
            prev = st;
        }
        CHECK(f.alarms == 0);
        CHECK(pendings == 4);          // every defrost seen, none alarmed
        CHECK_STATE(m.state(), State::OK);
    }

    SECTION("freezer: the same defrost with the default config alarms");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 300);  // hard = soft, no delay
        RangeMonitor m(c);
        Feeder f(m, 0, 10000);
        for (uint32_t s = 0; s < 24 * 3600; s += 10) f.feed(freezer(s));
        CHECK(f.alarms == 4);
    }

    SECTION("freezer: compressor failure, slow rise, alarm after the delay");
    {
        RangeMonitor m(freezerCfg());
        Feeder f(m, 0, 10000);
        f.hold(-18.0f, 3600);
        // +3 °C per hour from -18 °C; the EMA crosses -15 °C after about an
        // hour, and the alarm follows 45 minutes later, still below -5 °C.
        uint32_t failAt = f.now, alarmAt = 0;
        for (uint32_t s = 0; s < 4 * 3600 && alarmAt == 0; s += 10) {
            State st = f.feed(-18.0f + 3.0f * s / 3600.0f);
            if (st == State::ALARM_HIGH) alarmAt = f.now;
        }
        CHECK(alarmAt != 0);
        uint32_t minutes = (alarmAt - failAt) / 60000UL;
        CHECK(minutes >= 105 && minutes <= 115);
        CHECK(f.alarms == 1);
    }

    SECTION("freezer: door left open, warm air, hard limit is immediate");
    {
        RangeMonitor m(freezerCfg());
        Feeder f(m, 0, 10000);
        f.hold(-18.0f, 3600);
        uint32_t openAt = f.now, critAt = 0;
        for (uint32_t s = 0; s < 3600 && critAt == 0; s += 10) {
            if (f.feed(15.0f) == State::CRITICAL_HIGH) critAt = f.now;
        }
        CHECK(critAt != 0);
        // EMA from -18 towards +15 crosses -5 after tau * ln(33/20) ≈ 150 s,
        // plus one sample held back by the median filter and one sample of
        // quantisation.
        CHECK((critAt - openAt) / 1000UL <= 180);
    }

    SECTION("freezer: sensor unplugged, FAULT after the fault delay");
    {
        RangeMonitor m(freezerCfg());
        Feeder f(m, 0, 10000);
        f.hold(-18.0f, 600);
        CHECK_STATE(f.hold(NAN, 50), State::OK);
        CHECK_STATE(f.hold(NAN, 20), State::FAULT);
        CHECK_STATE(f.hold(-18.0f, 10), State::OK);
    }

    SECTION("server room: AC failure");
    {
        Cfg c;
        c.loSoft            = 15.0f;
        c.hiSoft            = 27.0f;
        c.hiHard            = 32.0f;
        c.hysteresis        = 1.0f;
        c.tauSeconds        = 120;
        c.alarmDelaySeconds = 600;
        CHECK(c.validate() == Err::NONE);
        RangeMonitor m(c);
        Feeder f(m, 0, 30000);
        f.hold(22.0f, 3600);
        // +6 °C per hour: ALARM ~10 min after 27 °C, CRITICAL at 32 °C
        bool sawAlarm = false, sawCrit = false;
        for (uint32_t s = 0; s < 3 * 3600; s += 30) {
            State st = f.feed(22.0f + 6.0f * s / 3600.0f);
            sawAlarm |= (st == State::ALARM_HIGH);
            sawCrit  |= (st == State::CRITICAL_HIGH);
        }
        CHECK(sawAlarm && sawCrit);
        CHECK(f.alarms == 1);          // ALARM → CRITICAL is an escalation, not a new alarm
    }

    return finish("test_scenarios");
}
