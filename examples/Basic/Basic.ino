/**
 * RangeMonitor — Basic
 *
 * Monitors one temperature against a freezer range and prints every state
 * change. The sensor is simulated so the sketch runs on any board; replace
 * readTemperature() with a real reading (and return NAN when it fails).
 *
 * Configuration:
 *   soft range   -22 .. -15 °C    alarm only after 45 minutes outside
 *   hard limit   -5 °C            alarm at once
 *   hysteresis   1 °C
 *   EMA          tau = 5 minutes
 *   fault        after 60 s of failed readings
 */

#include <RangeMonitor.h>

static const uint32_t SAMPLE_INTERVAL_MS = 10000;

RangeMonitor monitor;

// Simulated sensor: -18 °C with a little noise, and a failed reading now and then.
float readTemperature() {
    static uint16_t n = 0;
    n++;
    if (n % 97 == 0) return NAN;
    return -18.0f + (float)random(-20, 21) / 100.0f;
}

void printTransition(const RangeMonitor::Transition& t) {
    char from[RangeMonitor::STATE_NAME_SIZE], to[RangeMonitor::STATE_NAME_SIZE];
    RangeMonitor::stateName(t.from, from, sizeof(from));
    RangeMonitor::stateName(t.to, to, sizeof(to));
    RangeMonitor::Status s = monitor.getStatus();
    Serial.print(from);
    Serial.print(F(" -> "));
    Serial.print(to);
    Serial.print(F("  (value "));
    Serial.print(s.value, 2);
    Serial.println(F(")"));
    if (RangeMonitor::isAlarm(t.to) && !RangeMonitor::isAlarm(t.from)) {
        Serial.println(F("  ALARM raised"));
    } else if (!RangeMonitor::isAlarm(t.to) && RangeMonitor::isAlarm(t.from)) {
        Serial.println(F("  alarm cleared"));
    }
}

void setup() {
    Serial.begin(115200);

    RangeMonitor::Config cfg = RangeMonitor::Config::range(-22.0f, -15.0f, 1.0f, 300);
    cfg.hiHard            = -5.0f;
    cfg.alarmDelaySeconds = 45UL * 60UL;
    cfg.faultDelaySeconds = 60;

    RangeMonitor::ConfigError e = monitor.reconfigure(cfg);
    if (e != RangeMonitor::ConfigError::NONE) {
        Serial.print(F("Invalid configuration, error "));
        Serial.println((int)e);
    }
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last < SAMPLE_INTERVAL_MS) return;
    last = millis();

    RangeMonitor::Transition t = monitor.update(readTemperature());
    if (t.changed()) printTransition(t);
}
