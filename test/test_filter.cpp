/**
 * @file test_filter.cpp
 * @brief Spike filter (median of 3) and the time-based EMA.
 */

#include "test_harness.h"

int main() {
    printf("test_filter\n");

    SECTION("single spike is rejected by the median");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 0);   // no smoothing
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f); f.feed(-18.0f); f.feed(-18.0f);
        CHECK_STATE(f.feed(85.0f), State::OK);         // classic DS18B20 power-on value
        CHECK_STATE(f.feed(-18.0f), State::OK);
        CHECK_STATE(f.feed(-127.0f), State::OK);       // and a bogus low one
        CHECK_STATE(f.feed(-18.0f), State::OK);
        CHECK(f.alarms == 0);
    }

    SECTION("without the spike filter the same spike alarms");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 0);
        c.spikeFilter = false;
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f); f.feed(-18.0f); f.feed(-18.0f);
        CHECK_STATE(f.feed(85.0f), State::CRITICAL_HIGH);
    }

    SECTION("a real step passes the median after two samples");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 0);
        RangeMonitor m(c);
        Feeder f(m);
        f.feed(-18.0f); f.feed(-18.0f); f.feed(-18.0f);
        CHECK_STATE(f.feed(-10.0f), State::OK);
        CHECK_STATE(f.feed(-10.0f), State::CRITICAL_HIGH);
    }

    SECTION("median of three, all orderings");
    {
        const float v[6][3] = { {1, 2, 3}, {1, 3, 2}, {2, 1, 3},
                                {2, 3, 1}, {3, 1, 2}, {3, 2, 1} };
        for (int i = 0; i < 6; i++) {
            Cfg c;                                   // no limits, no smoothing
            RangeMonitor m(c);
            m.update(v[i][0], 0); m.update(v[i][1], 1); m.update(v[i][2], 2);
            CHECK_NEAR(m.value(), 2.0f, 1e-6);
        }
    }

    SECTION("EMA starts at the first sample");
    {
        Cfg c;
        c.tauSeconds  = 600;
        c.spikeFilter = false;
        RangeMonitor m(c);
        m.update(-18.0f, 0);
        CHECK_NEAR(m.value(), -18.0f, 1e-6);
    }

    SECTION("EMA step response: 63 % after one tau");
    {
        Cfg c;
        c.tauSeconds  = 300;
        c.spikeFilter = false;
        RangeMonitor m(c);
        Feeder f(m, 0, 1000);                          // 1 s steps
        f.feed(0.0f);
        f.hold(10.0f, 300);
        CHECK_NEAR(m.value(), 10.0f * (1.0f - std::exp(-1.0f)), 0.05);
    }

    SECTION("EMA does not depend on the sampling interval");
    {
        Cfg c;
        c.tauSeconds  = 300;
        c.spikeFilter = false;
        RangeMonitor a(c), b(c);
        Feeder fa(a, 0, 1000), fb(b, 0, 37000);       // 1 s vs. irregular-ish 37 s
        fa.feed(0.0f); fb.feed(0.0f);
        fa.hold(10.0f, 1110);
        fb.hold(10.0f, 1110);
        // Both have seen the step for the same time (to within one 37 s step)
        CHECK_NEAR(a.value(), b.value(), 0.25);
    }

    SECTION("valueSettled after 3 x tau");
    {
        Cfg c;
        c.tauSeconds  = 100;
        c.spikeFilter = false;
        RangeMonitor m(c);
        m.update(1.0f, 5000);
        CHECK(!m.getStatus(5000).valueSettled);
        CHECK(!m.getStatus(5000 + 299999).valueSettled);
        CHECK(m.getStatus(5000 + 300000).valueSettled);
    }

    SECTION("two samples at the same instant do not corrupt the EMA");
    {
        Cfg c;
        c.tauSeconds  = 60;
        c.spikeFilter = false;
        RangeMonitor m(c);
        m.update(1.0f, 1000);
        m.update(5.0f, 1000);
        CHECK_NEAR(m.value(), 1.0f, 1e-6);
        CHECK(!std::isnan(m.value()));
    }

    SECTION("smoothing delays an alarm, as configured");
    {
        Cfg c = Cfg::range(-22.0f, -15.0f, 1.0f, 300);
        c.spikeFilter = false;
        RangeMonitor m(c);
        Feeder f(m);                                   // 10 s steps
        f.hold(-18.0f, 3600);
        // Step to -10: the EMA crosses -15 after tau * ln(8/5) ≈ 141 s
        CHECK_STATE(f.hold(-10.0f, 130), State::OK);
        CHECK_STATE(f.hold(-10.0f, 30), State::CRITICAL_HIGH);
    }

    return finish("test_filter");
}
