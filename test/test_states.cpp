/**
 * @file test_states.cpp
 * @brief State machine: soft/hard limits, delay, hysteresis, one-sided ranges.
 *
 * Smoothing and spike filtering are off here (tau = 0, spikeFilter = false),
 * so the value the state machine sees is exactly the sample fed in.
 */

#include "test_harness.h"

/// Freezer-like range without smoothing: soft -22..-15, hysteresis 1.
static Cfg plain() {
    Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 0);
    c.spikeFilter = false;
    return c;
}

int main() {
    printf("test_states\n");

    SECTION("first valid sample leaves UNKNOWN");
    {
        RangeMonitor m(plain());
        CHECK_STATE(m.state(), State::UNKNOWN);
        RangeMonitor::Transition t = m.update(-18.0f, 0);
        CHECK_STATE(t.from, State::UNKNOWN);
        CHECK_STATE(t.to, State::OK);
        CHECK(t.changed());
        t = m.update(-18.0f, 10000);
        CHECK(!t.changed());
    }

    SECTION("default: hard follows soft, alarm is immediate");
    {
        RangeMonitor m(plain());
        Feeder f(m);
        f.feed(-18.0f);
        CHECK_STATE(f.feed(-14.9f), State::CRITICAL_HIGH);
        // Inside the limit but within the hysteresis band: held
        CHECK_STATE(f.feed(-15.5f), State::CRITICAL_HIGH);
        CHECK_STATE(f.feed(-15.99f), State::CRITICAL_HIGH);
        // Back by the full hysteresis: cleared (no ALARM step, soft == hard)
        CHECK_STATE(f.feed(-16.0f), State::OK);
        // Exactly on the limit is not outside it
        CHECK_STATE(f.feed(-15.0f), State::OK);
        CHECK_STATE(f.feed(-22.0f), State::OK);
        CHECK_STATE(f.feed(-22.1f), State::CRITICAL_LOW);
        CHECK_STATE(f.feed(-21.5f), State::CRITICAL_LOW);
        CHECK_STATE(f.feed(-21.0f), State::OK);
    }

    SECTION("soft limit with delay: PENDING, then ALARM");
    {
        Cfg c = plain();
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 600;          // 10 minutes
        RangeMonitor m(c);
        Feeder f(m);                        // 10 s steps
        f.feed(-18.0f);
        CHECK_STATE(f.feed(-12.0f), State::PENDING_HIGH);
        uint32_t pendingAt = f.now - f.stepMs;
        // Still pending just before the delay is served
        while (f.now - pendingAt < 600000UL) CHECK_STATE(f.feed(-12.0f), State::PENDING_HIGH);
        // Delay served
        CHECK_STATE(f.feed(-12.0f), State::ALARM_HIGH);
        CHECK(f.alarms == 1);
        // Alarm cleared only by the hysteresis
        CHECK_STATE(f.feed(-15.5f), State::ALARM_HIGH);
        CHECK_STATE(f.feed(-16.0f), State::OK);
    }

    SECTION("short excursion inside the delay raises nothing");
    {
        Cfg c = plain();
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 2700;         // 45 minutes
        RangeMonitor m(c);
        Feeder f(m);
        f.hold(-18.0f, 600);
        CHECK_STATE(f.hold(-8.0f, 1800), State::PENDING_HIGH);  // 30-minute defrost
        CHECK_STATE(f.hold(-18.0f, 600), State::OK);
        CHECK(f.alarms == 0);
    }

    SECTION("pending is cleared only by the hysteresis");
    {
        Cfg c = plain();
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 600;
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f);
        // Hovering around the limit: the delay must not restart each time
        for (int i = 0; i < 40; i++) f.feed((i % 2) ? -14.8f : -15.3f);  // 400 s
        CHECK_STATE(m.state(), State::PENDING_HIGH);
        f.hold(-15.3f, 250);
        CHECK_STATE(m.state(), State::ALARM_HIGH);
        CHECK(f.alarms == 1);
    }

    SECTION("hard limit during the delay: immediate CRITICAL");
    {
        Cfg c = plain();
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 2700;
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f);
        CHECK_STATE(f.feed(-10.0f), State::PENDING_HIGH);
        CHECK_STATE(f.feed(-4.0f), State::CRITICAL_HIGH);
        // Back below hard - hyst, still outside soft: ALARM, no new delay
        CHECK_STATE(f.feed(-5.5f), State::CRITICAL_HIGH);
        CHECK_STATE(f.feed(-6.0f), State::ALARM_HIGH);
        CHECK_STATE(f.feed(-14.5f), State::ALARM_HIGH);
        CHECK_STATE(f.feed(-16.0f), State::OK);
        CHECK(f.alarms == 1);
    }

    SECTION("CRITICAL straight back into range: OK");
    {
        Cfg c = plain();
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 2700;
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f);
        CHECK_STATE(f.feed(0.0f), State::CRITICAL_HIGH);
        CHECK_STATE(f.feed(-18.0f), State::OK);
    }

    SECTION("lower side mirrors the upper side");
    {
        Cfg c = plain();
        c.loHard = -30.0f;
        c.alarmDelaySeconds = 60;
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f);
        CHECK_STATE(f.feed(-25.0f), State::PENDING_LOW);
        f.hold(-25.0f, 60);
        CHECK_STATE(m.state(), State::ALARM_LOW);
        CHECK_STATE(f.feed(-31.0f), State::CRITICAL_LOW);
        CHECK_STATE(f.feed(-29.5f), State::CRITICAL_LOW);
        CHECK_STATE(f.feed(-28.0f), State::ALARM_LOW);
        CHECK_STATE(f.feed(-21.5f), State::ALARM_LOW);
        CHECK_STATE(f.feed(-21.0f), State::OK);
    }

    SECTION("jump from one side to the other");
    {
        RangeMonitor m(plain());
        Feeder f(m);
        f.feed(-18.0f);
        CHECK_STATE(f.feed(-10.0f), State::CRITICAL_HIGH);
        CHECK_STATE(f.feed(-30.0f), State::CRITICAL_LOW);
        CHECK_STATE(f.feed(-10.0f), State::CRITICAL_HIGH);
    }

    SECTION("one-sided range: server room, upper limits only");
    {
        Cfg c;
        c.hiSoft = 27.0f;
        c.hiHard = 32.0f;
        c.hysteresis = 1.0f;
        c.alarmDelaySeconds = 300;
        c.spikeFilter = false;
        CHECK(c.validate() == Err::NONE);
        RangeMonitor m(c);
        Feeder f(m);
        CHECK_STATE(f.feed(22.0f), State::OK);
        CHECK_STATE(f.feed(-40.0f), State::OK);   // no lower limit at all
        CHECK_STATE(f.feed(28.0f), State::PENDING_HIGH);
        f.hold(28.0f, 300);
        CHECK_STATE(m.state(), State::ALARM_HIGH);
        CHECK_STATE(f.feed(33.0f), State::CRITICAL_HIGH);
        CHECK_STATE(f.feed(25.0f), State::OK);
    }

    SECTION("hard-only side");
    {
        Cfg c;
        c.hiHard = 40.0f;
        c.hysteresis = 2.0f;
        c.spikeFilter = false;
        CHECK(c.validate() == Err::NONE);
        RangeMonitor m(c);
        Feeder f(m);
        CHECK_STATE(f.feed(35.0f), State::OK);
        CHECK_STATE(f.feed(41.0f), State::CRITICAL_HIGH);
        CHECK_STATE(f.feed(38.5f), State::CRITICAL_HIGH);
        CHECK_STATE(f.feed(38.0f), State::OK);
    }

    SECTION("no limits: always OK");
    {
        Cfg c;
        c.spikeFilter = false;
        RangeMonitor m(c);
        Feeder f(m);
        CHECK_STATE(f.feed(1e6f), State::OK);
        CHECK_STATE(f.feed(-1e6f), State::OK);
    }

    SECTION("stateSinceMs follows the last change");
    {
        RangeMonitor m(plain());
        m.update(-18.0f, 1000);
        m.update(-18.0f, 2000);
        CHECK(m.snapshot(2000).stateSinceMs == 1000);
        m.update(-10.0f, 3000);
        CHECK(m.snapshot(3000).stateSinceMs == 3000);
    }

    return finish("test_states");
}
