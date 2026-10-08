/**
 * @file test_fault.cpp
 * @brief Invalid samples: fault delay, FAULT as an overlay, stale filter reset.
 */

#include "test_harness.h"

static Cfg plain() {
    Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 0);
    c.spikeFilter = false;
    return c;
}

int main() {
    printf("test_fault\n");

    SECTION("fault delay 0: the first invalid sample is a FAULT");
    {
        RangeMonitor m(plain());
        Feeder f(m);
        f.feed(-18.0f);
        CHECK_STATE(f.feed(NAN), State::FAULT);
        CHECK(f.alarms == 1);
        CHECK(std::isnan(m.snapshot(f.now).raw));
        CHECK_STATE(f.feed(-18.0f), State::OK);
    }

    SECTION("+/-infinity counts as invalid");
    {
        RangeMonitor m(plain());
        Feeder f(m);
        f.feed(-18.0f);
        CHECK_STATE(f.feed(INFINITY), State::FAULT);
        CHECK_STATE(f.feed(-18.0f), State::OK);
        CHECK_STATE(f.feed(-INFINITY), State::FAULT);
    }

    SECTION("fault delay: short dropouts are tolerated");
    {
        Cfg c = plain();
        c.faultDelaySeconds = 30;
        RangeMonitor m(c);
        Feeder f(m);                               // 10 s steps
        f.feed(-18.0f);
        CHECK_STATE(f.feed(NAN), State::OK);       // t = 0 of the outage
        CHECK_STATE(f.feed(NAN), State::OK);       // 10 s
        CHECK_STATE(f.feed(NAN), State::OK);       // 20 s
        CHECK_STATE(f.feed(-18.0f), State::OK);    // recovered: timer restarts
        f.feed(NAN); f.feed(NAN); f.feed(NAN);
        CHECK_STATE(f.feed(NAN), State::FAULT);    // 30 s
        CHECK(f.alarms == 1);
    }

    SECTION("invalid before any valid sample");
    {
        RangeMonitor m(plain());
        Feeder f(m);
        CHECK_STATE(f.feed(NAN), State::FAULT);
        CHECK_STATE(f.feed(-18.0f), State::OK);
    }

    SECTION("FAULT during ALARM resumes ALARM, no new delay");
    {
        Cfg c = plain();
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 600;
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f);
        f.hold(-12.0f, 610);
        CHECK_STATE(m.state(), State::ALARM_HIGH);
        CHECK_STATE(f.feed(NAN), State::FAULT);
        CHECK_STATE(f.feed(-12.0f), State::ALARM_HIGH);
        CHECK(f.alarms == 1);                      // ALARM → FAULT stays an alarm
    }

    SECTION("FAULT during PENDING keeps the delay's start time");
    {
        Cfg c = plain();
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 600;
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f);
        f.hold(-12.0f, 400);                       // pending for 400 s
        CHECK_STATE(m.state(), State::PENDING_HIGH);
        f.hold(NAN, 100);
        CHECK_STATE(m.state(), State::FAULT);
        // 500 s since the excursion began: still pending
        CHECK_STATE(f.feed(-12.0f), State::PENDING_HIGH);
        f.hold(-12.0f, 100);
        CHECK_STATE(m.state(), State::ALARM_HIGH);
    }

    SECTION("recovery into range after a fault: OK");
    {
        Cfg c = plain();
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 600;
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f);
        f.hold(-12.0f, 610);
        f.feed(NAN);
        CHECK_STATE(f.feed(-18.0f), State::OK);
    }

    SECTION("an outage longer than 3 x tau resets the filter");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 60);
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 600;
        c.spikeFilter = false;
        RangeMonitor m(c);
        Feeder f(m);
        f.hold(-12.0f, 1200);
        CHECK_STATE(m.state(), State::ALARM_HIGH);
        f.hold(NAN, 200);                          // > 180 s
        CHECK_STATE(m.state(), State::FAULT);
        CHECK(std::isnan(m.value()));              // filter discarded
        // The old alarm is stale: evaluation starts over, with a new delay
        CHECK_STATE(f.feed(-12.0f), State::PENDING_HIGH);
        CHECK_NEAR(m.value(), -12.0f, 1e-6);       // EMA restarted at the sample
    }

    SECTION("an outage shorter than 3 x tau keeps the filter");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 60);
        c.spikeFilter = false;
        RangeMonitor m(c);
        Feeder f(m);
        f.hold(-18.0f, 600);
        f.hold(NAN, 150);
        CHECK_NEAR(m.value(), -18.0f, 1e-3);
    }

    SECTION("tau 0: no filter to go stale, state resumes after any outage");
    {
        Cfg c = plain();
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 600;
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f);
        f.hold(-12.0f, 610);
        f.hold(NAN, 7200);
        CHECK_STATE(f.feed(-12.0f), State::ALARM_HIGH);
    }

    return finish("test_fault");
}
