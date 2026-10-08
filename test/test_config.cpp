/**
 * @file test_config.cpp
 * @brief Validation, reconfiguration, helpers and clock wrap-around.
 */

#include "test_harness.h"
#include <cstring>

int main() {
    printf("test_config\n");

    SECTION("validate(): each rule");
    {
        Cfg ok = Cfg::range(-22.0f, -15.0f, 1.0f, 300);
        CHECK(ok.validate() == Err::NONE);

        Cfg c = ok; c.hiSoft = INFINITY;          CHECK(c.validate() == Err::NOT_FINITE);
        c = ok; c.loSoft = -15.0f;                CHECK(c.validate() == Err::SOFT_ORDER);
        c = ok; c.hiHard = -16.0f;                CHECK(c.validate() == Err::HI_HARD_INSIDE);
        c = ok; c.loHard = -21.0f;                CHECK(c.validate() == Err::LO_HARD_INSIDE);
        c = ok; c.hysteresis = -0.1f;             CHECK(c.validate() == Err::HYSTERESIS);
        c = ok; c.hysteresis = NAN;               CHECK(c.validate() == Err::HYSTERESIS);
        c = ok; c.hysteresis = 3.5f;              CHECK(c.validate() == Err::HYSTERESIS);
        c = ok; c.hysteresis = 3.49f;             CHECK(c.validate() == Err::NONE);
        c = ok; c.alarmDelaySeconds = RangeMonitor::MAX_TIME_SECONDS + 1;
                                                  CHECK(c.validate() == Err::TIME_RANGE);
        c = ok; c.tauSeconds = RangeMonitor::MAX_TIME_SECONDS;
                                                  CHECK(c.validate() == Err::TIME_RANGE);

        // Hard limits only, crossing each other
        Cfg h; h.loHard = 10.0f; h.hiHard = 5.0f; CHECK(h.validate() == Err::HARD_ORDER);
        // Hard-only upper side over a soft-only lower side is fine
        Cfg m; m.loSoft = 15.0f; m.hiHard = 32.0f; CHECK(m.validate() == Err::NONE);
    }

    SECTION("symmetric() == range()");
    {
        Cfg a = Cfg::symmetric(-18.0f, 3.0f, 1.0f, 600);
        CHECK_NEAR(a.loSoft, -21.0f, 1e-6);
        CHECK_NEAR(a.hiSoft, -15.0f, 1e-6);
        CHECK(std::isnan(a.hiHard));
        CHECK_NEAR(a.effectiveHiHard(), -15.0f, 1e-6);
    }

    SECTION("invalid constructor config falls back to no limits");
    {
        Cfg bad = Cfg::range(-15.0f, -22.0f, 1.0f, 0);
        RangeMonitor m(bad);
        CHECK(m.configError() == Err::SOFT_ORDER);
        Cfg active;
        CHECK(m.getConfig(active));
        CHECK(std::isnan(active.hiSoft));
        CHECK_STATE(m.update(100.0f, 0).to, State::OK);
    }

    SECTION("reconfigure(): invalid config is rejected, nothing changes");
    {
        RangeMonitor m(Cfg::range(-22.0f, -15.0f, 1.0f, 0));
        CHECK(m.reconfigure(Cfg::range(0.0f, 0.0f, 0.0f, 0)) == Err::SOFT_ORDER);
        Cfg active;
        m.getConfig(active);
        CHECK_NEAR(active.hiSoft, -15.0f, 1e-6);
    }

    SECTION("reconfigure(): filter and state are kept");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 300);
        c.spikeFilter = false;
        RangeMonitor m(c);
        Feeder f(m);
        f.hold(-10.0f, 3600);
        CHECK_STATE(m.state(), State::CRITICAL_HIGH);
        float before = m.value();

        Cfg wider = Cfg::range(-22.0f, -5.0f, 1.0f, 300);
        CHECK(m.reconfigure(wider) == Err::NONE);
        CHECK_STATE(m.state(), State::CRITICAL_HIGH);  // no edge at reconfigure
        CHECK_NEAR(m.value(), before, 1e-6);
        CHECK_STATE(f.feed(-10.0f), State::OK);        // next update applies it
    }

    SECTION("reconfigure(): a NAN hard limit follows the new soft limit");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 0);
        c.spikeFilter = false;
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-14.0f);
        CHECK_STATE(m.state(), State::CRITICAL_HIGH);
        c.hiSoft = -10.0f;
        m.reconfigure(c);
        CHECK_STATE(f.feed(-14.0f), State::OK);
        CHECK_STATE(f.feed(-9.0f), State::CRITICAL_HIGH);
    }

    SECTION("reset(): back to UNKNOWN, config kept");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 0);
        RangeMonitor m(c);
        m.update(-18.0f, 0);
        CHECK(m.reset());
        CHECK_STATE(m.state(), State::UNKNOWN);
        CHECK(std::isnan(m.value()));
        CHECK(m.snapshot(0).sampleCount == 0);
        Cfg active;
        m.getConfig(active);
        CHECK_NEAR(active.hiSoft, -15.0f, 1e-6);
    }

    SECTION("clock wrap-around inside a delay");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 60);
        c.hiHard = -5.0f;
        c.alarmDelaySeconds = 600;
        c.spikeFilter = false;
        RangeMonitor m(c);
        Feeder f(m, 0xFFFFFFFFUL - 300000UL);          // 5 minutes before the wrap
        f.feed(-12.0f);
        CHECK_STATE(m.state(), State::PENDING_HIGH);
        f.hold(-12.0f, 590);
        CHECK_STATE(m.state(), State::PENDING_HIGH);
        f.hold(-12.0f, 20);
        CHECK_STATE(m.state(), State::ALARM_HIGH);
        CHECK(m.snapshot(f.now).valueSettled);
    }

    SECTION("isAlarm / isHigh / isLow");
    {
        CHECK(!RangeMonitor::isAlarm(State::UNKNOWN));
        CHECK(!RangeMonitor::isAlarm(State::OK));
        CHECK(!RangeMonitor::isAlarm(State::PENDING_HIGH));
        CHECK(!RangeMonitor::isAlarm(State::PENDING_LOW));
        CHECK(RangeMonitor::isAlarm(State::ALARM_HIGH));
        CHECK(RangeMonitor::isAlarm(State::ALARM_LOW));
        CHECK(RangeMonitor::isAlarm(State::CRITICAL_HIGH));
        CHECK(RangeMonitor::isAlarm(State::CRITICAL_LOW));
        CHECK(RangeMonitor::isAlarm(State::FAULT));
        CHECK(RangeMonitor::isHigh(State::PENDING_HIGH) && !RangeMonitor::isHigh(State::ALARM_LOW));
        CHECK(RangeMonitor::isLow(State::CRITICAL_LOW) && !RangeMonitor::isLow(State::FAULT));
    }

    SECTION("stateName()");
    {
        char buf[RangeMonitor::STATE_NAME_SIZE];
        CHECK(strcmp(RangeMonitor::stateName(State::OK, buf, sizeof(buf)), "ok") == 0);
        CHECK(strcmp(RangeMonitor::stateName(State::CRITICAL_HIGH, buf, sizeof(buf)), "critical_high") == 0);
        CHECK(strlen(buf) == RangeMonitor::STATE_NAME_LEN);
        CHECK(strcmp(RangeMonitor::stateName(State::FAULT, buf, sizeof(buf)), "fault") == 0);
        char small[4];
        CHECK(strcmp(RangeMonitor::stateName(State::PENDING_LOW, small, sizeof(small)), "pen") == 0);
        CHECK(RangeMonitor::stateName(State::OK, nullptr, 8) == nullptr);
        CHECK(strcmp(RangeMonitor::stateName((State)200, buf, sizeof(buf)), "unknown") == 0);
    }

    return finish("test_config");
}
