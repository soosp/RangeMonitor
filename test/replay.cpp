/**
 * @file replay.cpp
 * @brief Replays recorded data through RangeMonitor and prints every transition.
 *
 * Not a test: a tool for tuning a configuration against real measurements
 * (e.g. a freezer's defrost cycles) before putting it on a device.
 *
 * Input: CSV on stdin or in a file, one sample per line:
 *
 *     <seconds>,<value>
 *
 * Seconds may be fractional and need not be evenly spaced. An empty value,
 * "nan" or "null" is an invalid sample. Lines that do not start with a number
 * (headers, comments) are skipped.
 *
 * Build and run (see test/README.md):
 *
 *     g++ -std=c++17 -Ithird_party replay.cpp -o build/replay
 *     build/replay --lo -22 --hi -15 --hi-hard -5 --hyst 1 --tau 300 \
 *                  --delay 2700 data.csv
 *
 * Options (all optional; limits default to NAN = not set):
 *     --lo, --hi, --lo-hard, --hi-hard, --hyst <float>
 *     --tau, --delay, --fault-delay <seconds>
 *     --no-spike         disable the median filter
 *     --trace            print every sample, not only transitions
 */

#include "../src/RangeMonitor.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

static void usage() {
    fprintf(stderr,
        "usage: replay [--lo F] [--hi F] [--lo-hard F] [--hi-hard F] [--hyst F]\n"
        "              [--tau S] [--delay S] [--fault-delay S] [--no-spike]\n"
        "              [--trace] [file.csv]\n");
}

static void fmtTime(double s, char* buf, size_t len) {
    unsigned long t = (unsigned long)s;
    snprintf(buf, len, "%3lud %02lu:%02lu:%02lu",
             t / 86400, (t / 3600) % 24, (t / 60) % 60, t % 60);
}

int main(int argc, char** argv) {
    RangeMonitor::Config c;
    bool        trace = false;
    const char* path  = nullptr;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool hasArg = (i + 1 < argc);
        if      (!strcmp(a, "--lo")          && hasArg) c.loSoft            = strtof(argv[++i], nullptr);
        else if (!strcmp(a, "--hi")          && hasArg) c.hiSoft            = strtof(argv[++i], nullptr);
        else if (!strcmp(a, "--lo-hard")     && hasArg) c.loHard            = strtof(argv[++i], nullptr);
        else if (!strcmp(a, "--hi-hard")     && hasArg) c.hiHard            = strtof(argv[++i], nullptr);
        else if (!strcmp(a, "--hyst")        && hasArg) c.hysteresis        = strtof(argv[++i], nullptr);
        else if (!strcmp(a, "--tau")         && hasArg) c.tauSeconds        = strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(a, "--delay")       && hasArg) c.alarmDelaySeconds = strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(a, "--fault-delay") && hasArg) c.faultDelaySeconds = strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(a, "--no-spike"))              c.spikeFilter       = false;
        else if (!strcmp(a, "--trace"))                 trace               = true;
        else if (a[0] != '-')                           path                = a;
        else { usage(); return 2; }
    }

    if (c.validate() != RangeMonitor::ConfigError::NONE) {
        fprintf(stderr, "invalid configuration (ConfigError %u)\n",
                (unsigned)c.validate());
        return 2;
    }

    FILE* in = path ? fopen(path, "r") : stdin;
    if (!in) { perror(path); return 2; }

    RangeMonitor m(c);
    char   line[256], from[RangeMonitor::STATE_NAME_SIZE], to[RangeMonitor::STATE_NAME_SIZE];
    char   when[32];
    double t0 = NAN;
    unsigned long samples = 0, alarms = 0;

    while (fgets(line, sizeof(line), in)) {
        char* end;
        double sec = strtod(line, &end);
        if (end == line) continue;                       // header or comment
        if (std::isnan(t0)) t0 = sec;

        float v = NAN;
        char* p = strchr(end, ',');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            char* vend;
            float parsed = strtof(p, &vend);
            if (vend != p) v = parsed;                   // "nan", "null", "" → NAN
        }

        uint32_t ms = (uint32_t)((sec - t0) * 1000.0 + 0.5);
        RangeMonitor::Transition tr = m.update(v, ms);
        samples++;

        if (tr.changed() || trace) {
            RangeMonitor::Snapshot s = m.snapshot(ms);
            fmtTime(sec - t0, when, sizeof(when));
            RangeMonitor::stateName(tr.from, from, sizeof(from));
            RangeMonitor::stateName(tr.to, to, sizeof(to));
            if (tr.changed())
                printf("%s  raw %8.2f  ema %8.2f  %s -> %s\n", when, s.raw, s.value, from, to);
            else
                printf("%s  raw %8.2f  ema %8.2f  %s\n", when, s.raw, s.value, to);
        }
        if (tr.changed() && RangeMonitor::isAlarm(tr.to) && !RangeMonitor::isAlarm(tr.from))
            alarms++;
    }
    if (in != stdin) fclose(in);

    printf("%lu samples, %lu alarm(s)\n", samples, alarms);
    return 0;
}
