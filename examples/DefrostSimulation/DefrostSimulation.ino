/**
 * RangeMonitor — DefrostSimulation
 *
 * Runs one simulated day of a freezer through two configurations in a few
 * seconds, using the explicit-time update(value, nowMs). The freezer holds
 * -18 °C and defrosts every 6 hours: the air rises to -8 °C in 10 minutes,
 * stays there for 10, and is pulled back down in 15.
 *
 *   1. Default limits (hard = soft): every defrost raises an alarm.
 *   2. Hard limit at -5 °C and a 45-minute delay on the soft limit: the
 *      defrosts are seen (PENDING) but none of them alarms.
 *
 * The same technique replays recorded data: feed the samples with their
 * own timestamps. On a PC, test/replay.cpp does exactly that from a CSV file.
 */

#include <RangeMonitor.h>

float freezer(uint32_t s) {
    uint32_t t = s % (6UL * 3600UL);
    const uint32_t start = 3UL * 3600UL;
    if (t < start) return -18.0f;
    t -= start;
    if (t < 600)  return -18.0f + 10.0f * t / 600.0f;
    if (t < 1200) return -8.0f;
    if (t < 2100) return -8.0f - 10.0f * (t - 1200) / 900.0f;
    return -18.0f;
}

void printTime(uint32_t s) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
             (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60),
             (unsigned long)(s % 60));
    Serial.print(buf);
}

void simulateDay(RangeMonitor& m) {
    char from[RangeMonitor::STATE_NAME_SIZE], to[RangeMonitor::STATE_NAME_SIZE];
    uint16_t alarms = 0;
    for (uint32_t s = 0; s < 24UL * 3600UL; s += 10) {
        RangeMonitor::Transition t = m.update(freezer(s), s * 1000UL);
        if (!t.changed()) continue;
        printTime(s);
        Serial.print(F("  "));
        Serial.print(RangeMonitor::stateName(t.from, from, sizeof(from)));
        Serial.print(F(" -> "));
        Serial.println(RangeMonitor::stateName(t.to, to, sizeof(to)));
        if (RangeMonitor::isAlarm(t.to) && !RangeMonitor::isAlarm(t.from)) alarms++;
    }
    Serial.print(F("Alarms: "));
    Serial.println(alarms);
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println(F("1. Default: hard limits follow the soft ones"));
    RangeMonitor plain(RangeMonitor::Config::range(-22.0f, -15.0f, 1.0f, 300));
    simulateDay(plain);

    Serial.println(F("2. Hard limit -5 C, 45-minute delay on the soft limit"));
    RangeMonitor::Config cfg = RangeMonitor::Config::range(-22.0f, -15.0f, 1.0f, 300);
    cfg.hiHard            = -5.0f;
    cfg.alarmDelaySeconds = 45UL * 60UL;
    RangeMonitor tolerant(cfg);
    simulateDay(tolerant);
}

void loop() {}
