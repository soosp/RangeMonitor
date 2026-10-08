/**
 * @file RangeMonitor.h
 * @brief Range monitor for a scalar physical quantity: spike filter, EMA,
 *        soft limits with alarm delay, hard limits, fault detection.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Péter Soós — https://github.com/soosp
 *
 * -----------------------------------------------------------------------------
 * PROCESSING CHAIN
 * -----------------------------------------------------------------------------
 *
 *     raw ─► validity ─► [median of 3] ─► EMA ─┬─► hard limits: immediate
 *                                               └─► soft limits: after a delay
 *
 * Both limit pairs are checked against the same smoothed value, so the
 * default configuration (hard limits equal to the soft ones) behaves exactly
 * like a plain EMA + hysteresis monitor: the alarm is raised as soon as the
 * smoothed value leaves the range.
 *
 * Soft limits tolerate excursions that are expected and short-lived — a
 * defrost cycle in a freezer, an opened door — by waiting alarmDelaySeconds
 * before the alarm is raised. Hard limits catch excursions that must never be
 * tolerated, however short. Moving a hard limit outside its soft limit is what
 * turns the delay on.
 *
 * Any limit may be NAN:
 *   - soft NAN: that side has no soft limit (e.g. a server room that only
 *     needs an upper limit);
 *   - hard NAN: the hard limit follows the soft limit on that side. This is
 *     the default, and it keeps following the soft limit when that changes.
 *
 * -----------------------------------------------------------------------------
 * STATES
 * -----------------------------------------------------------------------------
 *
 *   UNKNOWN        no valid sample yet
 *   OK             within range
 *   PENDING_x      outside the soft limit, alarm delay running
 *   ALARM_x        outside the soft limit for at least alarmDelaySeconds
 *   CRITICAL_x     outside the hard limit
 *   FAULT          only invalid samples for at least faultDelaySeconds
 *
 * Every exit back towards OK requires the value to return inside the limit by
 * at least the hysteresis band. That includes PENDING: without it, a value
 * hovering around a limit would restart the delay with every crossing and
 * never raise an alarm.
 *
 * Leaving CRITICAL while still outside the soft limit goes straight to ALARM,
 * not to PENDING: the delay has already been served by the excursion itself.
 *
 * FAULT is an overlay. When valid samples return, evaluation resumes from the
 * state that was active before the fault (a running alarm delay keeps its
 * start time), so a short sensor dropout during an alarm does not produce an
 * ALARM → FAULT → PENDING → ALARM sequence. If the outage lasted longer than
 * the EMA warm-up time (EMA_WARMUP_TAU_MULTIPLIER × tau), the filter is reset
 * and evaluation starts from scratch instead, because the old state is stale.
 *
 * -----------------------------------------------------------------------------
 * TIME
 * -----------------------------------------------------------------------------
 *
 * Every time-dependent rule — EMA weight, alarm delay, fault delay, warm-up —
 * uses the actual time between samples, so irregular sampling is handled
 * correctly. update(raw, nowMs) takes the time explicitly, which is what makes
 * the whole state machine testable on a host PC and lets recorded data be
 * replayed. update(raw) uses millis() and exists only in Arduino builds.
 * Timestamps are uint32_t milliseconds and wrap-around safe.
 *
 * -----------------------------------------------------------------------------
 * THREAD SAFETY AND MEMORY
 * -----------------------------------------------------------------------------
 *
 * ESP32: every public method takes a FreeRTOS mutex created from a static
 * buffer (xSemaphoreCreateMutexStatic), so no heap is used. ESP8266, AVR and
 * host builds have no preemptive threads; there the lock is a no-op.
 *
 * The class allocates nothing dynamically and holds no strings. It is
 * non-copyable and non-movable (it owns a mutex on ESP32).
 *
 * The header is C++11-compatible, because the Arduino AVR core compiles with
 * -std=gnu++11.
 */

#pragma once

#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef ARDUINO
    #include <Arduino.h>
#endif

#if defined(ARDUINO_ARCH_ESP32)
    #include <freertos/FreeRTOS.h>
    #include <freertos/semphr.h>
#endif

#if defined(__AVR__)
    #include <avr/pgmspace.h>
#endif

#include <ExponentialAverage.h> // https://github.com/soosp/RunningStatistics

/// Mutex acquisition timeout in milliseconds (ESP32 only).
/// Override before including this header or in build_flags.
#ifndef RANGE_MONITOR_MUTEX_TIMEOUT
    #define RANGE_MONITOR_MUTEX_TIMEOUT 1000
#endif

/**
 * @brief Range monitor for one scalar physical quantity.
 *
 * Suitable for temperature, humidity, pressure, or any value that should stay
 * within a range. See the file header for the processing chain and the state
 * machine.
 *
 * @par Typical use
 * @code
 * // Freezer: soft range -22..-15 °C, 1 °C hysteresis, 5-minute EMA.
 * // Defrost may lift it for up to 40 minutes; above -5 °C is never tolerated.
 * RangeMonitor::Config cfg = RangeMonitor::Config::range(-22.0f, -15.0f, 1.0f, 300);
 * cfg.hiHard            = -5.0f;
 * cfg.alarmDelaySeconds = 45 * 60;
 *
 * RangeMonitor monitor(cfg);
 *
 * RangeMonitor::Transition t = monitor.update(temperature);
 * if (t.changed() && RangeMonitor::isAlarm(t.to)) { ... }
 * @endcode
 */
class RangeMonitor {
public:
    // =========================================================================
    // TYPES
    // =========================================================================

    /**
     * @brief Monitor state. See the file header for the transitions.
     *
     * The numeric values are stable and may be published or stored.
     */
    enum class State : uint8_t {
        UNKNOWN       = 0, ///< No valid sample received yet
        OK            = 1, ///< Value within range
        PENDING_HIGH  = 2, ///< Above the upper soft limit, alarm delay running
        PENDING_LOW   = 3, ///< Below the lower soft limit, alarm delay running
        ALARM_HIGH    = 4, ///< Above the upper soft limit for at least the delay
        ALARM_LOW     = 5, ///< Below the lower soft limit for at least the delay
        CRITICAL_HIGH = 6, ///< Above the upper hard limit
        CRITICAL_LOW  = 7, ///< Below the lower hard limit
        FAULT         = 8  ///< Only invalid samples for at least the fault delay
    };

    /// Number of State values; sizes lookup tables indexed by State.
    static constexpr uint8_t STATE_COUNT = 9;

    /// Longest name returned by stateName(), excluding the null terminator.
    static constexpr size_t STATE_NAME_LEN  = 13;  // "critical_high"

    /// Buffer size for stateName(), including the null terminator.
    static constexpr size_t STATE_NAME_SIZE = STATE_NAME_LEN + 1;

    /**
     * @brief Result of Config::validate() and reconfigure().
     */
    enum class ConfigError : uint8_t {
        NONE = 0,          ///< Configuration is valid
        NOT_FINITE,        ///< A limit or the hysteresis is ±infinity
        SOFT_ORDER,        ///< loSoft >= hiSoft
        HARD_ORDER,        ///< Effective loHard >= effective hiHard
        HI_HARD_INSIDE,    ///< hiHard < hiSoft (hard limit inside the soft range)
        LO_HARD_INSIDE,    ///< loHard > loSoft (hard limit inside the soft range)
        HYSTERESIS,        ///< Hysteresis negative or NAN, or 2 × hysteresis
                           ///< does not fit between the soft limits
        TIME_RANGE,        ///< A time parameter overflows the millisecond clock
        LOCK_TIMEOUT       ///< reconfigure() only: mutex timeout, nothing changed
    };

    /**
     * @brief Configuration of one monitor.
     *
     * All limits and the hysteresis use the unit of the monitored quantity.
     * Default-constructed, every limit is NAN, so the monitor checks nothing
     * until limits are set.
     *
     * @note C++11 has no designated initialisers; start from range(),
     *       symmetric() or a default-constructed Config and set members.
     */
    struct Config {
        float    loSoft;            ///< Lower soft limit; NAN: none
        float    hiSoft;            ///< Upper soft limit; NAN: none
        float    loHard;            ///< Lower hard limit; NAN: same as loSoft
        float    hiHard;            ///< Upper hard limit; NAN: same as hiSoft
        float    hysteresis;        ///< Band the value must return by to clear
        uint32_t tauSeconds;        ///< EMA time constant; 0: no smoothing
        uint32_t alarmDelaySeconds; ///< Soft-limit delay; 0: immediate
        uint32_t faultDelaySeconds; ///< Invalid-data delay; 0: immediate
        bool     spikeFilter;       ///< Median of the last 3 valid samples

        /// No limits, no smoothing, no delays, spike filter on.
        Config()
            : loSoft(NAN), hiSoft(NAN), loHard(NAN), hiHard(NAN)
            , hysteresis(0.0f), tauSeconds(0), alarmDelaySeconds(0)
            , faultDelaySeconds(0), spikeFilter(true) {}

        /**
         * @brief Two-sided range with hard limits following the soft ones.
         *
         * @param lo          Lower soft limit.
         * @param hi          Upper soft limit.
         * @param hysteresis  Hysteresis band.
         * @param tauSeconds  EMA time constant in seconds.
         */
        static Config range(float lo, float hi, float hysteresis,
                            uint32_t tauSeconds) {
            Config c;
            c.loSoft     = lo;
            c.hiSoft     = hi;
            c.hysteresis = hysteresis;
            c.tauSeconds = tauSeconds;
            return c;
        }

        /**
         * @brief Two-sided range given as a centre value and a deviation.
         *
         * Equivalent to range(target - delta, target + delta, ...).
         */
        static Config symmetric(float target, float delta, float hysteresis,
                                uint32_t tauSeconds) {
            return range(target - delta, target + delta, hysteresis, tauSeconds);
        }

        /// Upper hard limit in effect: hiHard, or hiSoft if hiHard is NAN.
        float effectiveHiHard() const { return isnan(hiHard) ? hiSoft : hiHard; }

        /// Lower hard limit in effect: loHard, or loSoft if loHard is NAN.
        float effectiveLoHard() const { return isnan(loHard) ? loSoft : loHard; }

        /**
         * @brief Check the configuration for consistency.
         *
         * Checked in this order; the first violation is returned:
         *   1. no limit and no hysteresis is ±infinity;
         *   2. loSoft < hiSoft, if both are set;
         *   3. hiHard >= hiSoft and loHard <= loSoft, where both are set;
         *   4. effective loHard < effective hiHard, if both are set;
         *   5. 0 <= hysteresis, and 2 × hysteresis < hiSoft - loSoft if both
         *      soft limits are set — otherwise the band in which both alarms
         *      are clear would be empty;
         *   6. the delays and the warm-up time fit the 32-bit millisecond
         *      clock (about 49 days).
         *
         * @return ConfigError::NONE if valid.
         */
        ConfigError validate() const {
            if (isinf(loSoft) || isinf(hiSoft) || isinf(loHard) ||
                isinf(hiHard) || isinf(hysteresis)) {
                return ConfigError::NOT_FINITE;
            }
            if (!isnan(loSoft) && !isnan(hiSoft) && !(loSoft < hiSoft)) {
                return ConfigError::SOFT_ORDER;
            }
            if (!isnan(hiHard) && !isnan(hiSoft) && hiHard < hiSoft) {
                return ConfigError::HI_HARD_INSIDE;
            }
            if (!isnan(loHard) && !isnan(loSoft) && loHard > loSoft) {
                return ConfigError::LO_HARD_INSIDE;
            }
            const float hiH = effectiveHiHard();
            const float loH = effectiveLoHard();
            if (!isnan(loH) && !isnan(hiH) && !(loH < hiH)) {
                return ConfigError::HARD_ORDER;
            }
            if (isnan(hysteresis) || hysteresis < 0.0f) {
                return ConfigError::HYSTERESIS;
            }
            if (!isnan(loSoft) && !isnan(hiSoft) &&
                !(2.0f * hysteresis < hiSoft - loSoft)) {
                return ConfigError::HYSTERESIS;
            }
            if (alarmDelaySeconds > MAX_TIME_SECONDS ||
                faultDelaySeconds > MAX_TIME_SECONDS ||
                tauSeconds > MAX_TIME_SECONDS / EMA_WARMUP_TAU_MULTIPLIER) {
                return ConfigError::TIME_RANGE;
            }
            return ConfigError::NONE;
        }
    };

    /**
     * @brief Consistent copy of the monitor's runtime state.
     *
     * All fields are captured under one mutex acquisition.
     */
    struct Snapshot {
        State    state;         ///< Current state
        float    value;         ///< Smoothed value (EMA); NAN before the first valid sample
        float    raw;           ///< Last raw sample; NAN if it was invalid
        bool     valueSettled;  ///< True once the EMA has run for the warm-up time
        uint32_t sampleCount;   ///< Valid samples since construction or reset()
        uint32_t stateSinceMs;  ///< Timestamp of the last state change
    };

    /**
     * @brief State change produced by one update() call.
     *
     * from == to when the state did not change, or when update() could not
     * take the mutex.
     */
    struct Transition {
        State from;  ///< State before the update
        State to;    ///< State after the update

        /// True if the update changed the state.
        bool changed() const { return from != to; }
    };

    // =========================================================================
    // CONSTANTS
    // =========================================================================

    /// The EMA counts as settled after this many tau periods of valid data.
    /// An outage longer than this resets the filter.
    static constexpr uint32_t EMA_WARMUP_TAU_MULTIPLIER = 3;

    /// Largest time parameter in seconds that fits the 32-bit millisecond clock.
    static constexpr uint32_t MAX_TIME_SECONDS = 4294967UL;  // UINT32_MAX / 1000

    /// Window of the spike filter (fixed: median of 3).
    static constexpr uint8_t SPIKE_WINDOW = 3;

    // =========================================================================
    // LIFECYCLE
    // =========================================================================

    /**
     * @brief Construct a monitor.
     *
     * An invalid configuration is replaced by a default-constructed Config
     * (no limits), so the monitor never runs on inconsistent limits; check
     * configError() or validate the Config first.
     *
     * @param cfg  Initial configuration.
     */
    explicit RangeMonitor(const Config& cfg = Config())
        : _cfgError(cfg.validate())
    {
        _cfg = (_cfgError == ConfigError::NONE) ? cfg : Config();
        _clearRuntime();
#if defined(ARDUINO_ARCH_ESP32)
        _mutex = xSemaphoreCreateMutexStatic(&_mutexBuf);
#endif
    }

    /// Destructor. The mutex lives in a static buffer; nothing is freed.
    ~RangeMonitor() {
#if defined(ARDUINO_ARCH_ESP32)
        vSemaphoreDelete(_mutex);
#endif
    }

    RangeMonitor(const RangeMonitor&)            = delete;
    RangeMonitor& operator=(const RangeMonitor&) = delete;
    RangeMonitor(RangeMonitor&&)                 = delete;
    RangeMonitor& operator=(RangeMonitor&&)      = delete;

    // =========================================================================
    // DATA INPUT
    // =========================================================================

    /**
     * @brief Feed one sample taken at @p nowMs.
     *
     * Pass NAN (or ±infinity) for a failed reading. Timestamps must not go
     * backwards; they may wrap around.
     *
     * @param raw    Measured value, or NAN on sensor error.
     * @param nowMs  Time of the sample in milliseconds.
     * @return       The state transition (from == to if unchanged or on
     *               mutex timeout).
     */
    Transition update(float raw, uint32_t nowMs) {
        Transition t = { State::UNKNOWN, State::UNKNOWN };
        if (!_lock()) return t;
        t.from = _state;

        if (isnan(raw) || isinf(raw)) {
            _handleInvalid(nowMs);
        } else {
            _handleValid(raw, nowMs);
        }

        t.to = _state;
        _unlock();
        return t;
    }

#ifdef ARDUINO
    /**
     * @brief Feed one sample taken now (millis()).
     * @see update(float, uint32_t)
     */
    Transition update(float raw) {
        return update(raw, (uint32_t)millis());
    }
#endif

    // =========================================================================
    // STATE QUERIES
    // =========================================================================

    /**
     * @brief Return a consistent snapshot of the runtime state.
     *
     * @param nowMs  Current time, used for valueSettled.
     * @return       Snapshot; on mutex timeout state is UNKNOWN and the
     *               values are NAN.
     */
    Snapshot snapshot(uint32_t nowMs) const {
        Snapshot s = { State::UNKNOWN, NAN, NAN, false, 0, 0 };
        if (!_lock()) return s;
        s.state        = _state;
        s.value        = _ema.value();
        s.raw          = _lastRaw;
        s.valueSettled = _isSettled(nowMs);
        s.sampleCount  = _sampleCount;
        s.stateSinceMs = _stateSinceMs;
        _unlock();
        return s;
    }

#ifdef ARDUINO
    /// Snapshot at millis(). @see snapshot(uint32_t)
    Snapshot snapshot() const { return snapshot((uint32_t)millis()); }
#endif

    /**
     * @brief Return the current state.
     * @return Current state, or UNKNOWN on mutex timeout.
     */
    State state() const {
        if (!_lock()) return State::UNKNOWN;
        State s = _state;
        _unlock();
        return s;
    }

    /**
     * @brief Return the smoothed value.
     * @return EMA value, or NAN before the first valid sample or on mutex timeout.
     */
    float value() const {
        if (!_lock()) return NAN;
        float v = _ema.value();
        _unlock();
        return v;
    }

    /// True if the current state is an alarm. @see isAlarm(State)
    bool isAlarming() const { return isAlarm(state()); }

    // =========================================================================
    // CONFIGURATION
    // =========================================================================

    /**
     * @brief Copy the active configuration into @p out.
     * @return false on mutex timeout (@p out unchanged).
     */
    bool getConfig(Config& out) const {
        if (!_lock()) return false;
        out = _cfg;
        _unlock();
        return true;
    }

    /**
     * @brief Result of validating the configuration passed to the constructor.
     *
     * Anything other than NONE means the constructor fell back to a Config
     * with no limits.
     */
    ConfigError configError() const { return _cfgError; }

    /**
     * @brief Replace the configuration at runtime.
     *
     * The filter (median window and EMA) and the state are kept: the EMA
     * describes the measured quantity, not the limits, so there is no reason
     * to discard it, and keeping the state avoids a spurious edge at the
     * moment of reconfiguration. The new limits take effect at the next
     * update(), which leaves the current state through the normal transitions.
     *
     * @param cfg  New configuration.
     * @return     ConfigError::NONE on success; the validation error or
     *             LOCK_TIMEOUT otherwise, in which case nothing changed.
     */
    ConfigError reconfigure(const Config& cfg) {
        ConfigError e = cfg.validate();
        if (e != ConfigError::NONE) return e;
        if (!_lock()) return ConfigError::LOCK_TIMEOUT;
        _cfg      = cfg;
        _cfgError = ConfigError::NONE;
        _unlock();
        return ConfigError::NONE;
    }

    /**
     * @brief Discard all runtime state: filter, counters, state (→ UNKNOWN).
     *
     * The configuration is kept.
     *
     * @return false on mutex timeout.
     */
    bool reset() {
        if (!_lock()) return false;
        _clearRuntime();
        _unlock();
        return true;
    }

    // =========================================================================
    // STATE HELPERS
    // =========================================================================

    /**
     * @brief True for states that represent an alarm.
     *
     * ALARM_x, CRITICAL_x and FAULT. PENDING_x is not an alarm: it is the
     * tolerated excursion the delay exists for.
     */
    static bool isAlarm(State s) {
        return s == State::ALARM_HIGH    || s == State::ALARM_LOW    ||
               s == State::CRITICAL_HIGH || s == State::CRITICAL_LOW ||
               s == State::FAULT;
    }

    /// True for the states above the range: PENDING_HIGH, ALARM_HIGH, CRITICAL_HIGH.
    static bool isHigh(State s) {
        return s == State::PENDING_HIGH || s == State::ALARM_HIGH ||
               s == State::CRITICAL_HIGH;
    }

    /// True for the states below the range: PENDING_LOW, ALARM_LOW, CRITICAL_LOW.
    static bool isLow(State s) {
        return s == State::PENDING_LOW || s == State::ALARM_LOW ||
               s == State::CRITICAL_LOW;
    }

    /**
     * @brief Copy the lower-case name of @p s into a caller-supplied buffer.
     *
     * Names: "unknown", "ok", "pending_high", "pending_low", "alarm_high",
     * "alarm_low", "critical_high", "critical_low", "fault". They are stable
     * and suitable as MQTT payloads or JSON values. On AVR the names live in
     * flash.
     *
     * @param s    State.
     * @param buf  Destination; @ref STATE_NAME_SIZE bytes always suffice.
     * @param len  Size of @p buf in bytes, including the null terminator.
     * @return     @p buf, or nullptr if @p buf is null or @p len is 0.
     */
    static char* stateName(State s, char* buf, size_t len) {
        if (buf == nullptr || len == 0) return nullptr;
        uint8_t i = static_cast<uint8_t>(s);
        if (i >= STATE_COUNT) i = 0;
        // Explicit bounded copy rather than strncpy(): the truncation is
        // intended, and newer GCCs warn about strncpy (-Wstringop-truncation).
#if defined(__AVR__)
        PGM_P  src = (PGM_P)pgm_read_ptr(&_stateNames()[i]);
        size_t n   = strlen_P(src);
        if (n > len - 1) n = len - 1;
        memcpy_P(buf, src, n);
#else
        const char* src = _stateNames()[i];
        size_t      n   = strlen(src);
        if (n > len - 1) n = len - 1;
        memcpy(buf, src, n);
#endif
        buf[n] = '\0';
        return buf;
    }

private:
    // =========================================================================
    // SAMPLE HANDLING (mutex held)
    // =========================================================================

    /// Handle an invalid sample: start or continue the fault timer.
    void _handleInvalid(uint32_t nowMs) {
        _lastRaw = NAN;
        if (!_invalidActive) {
            _invalidActive  = true;
            _invalidSinceMs = nowMs;
        }
        if (_state != State::FAULT &&
            nowMs - _invalidSinceMs >= _cfg.faultDelaySeconds * 1000UL) {
            _resumeState = _state;
            _setState(State::FAULT, nowMs);
        }
        // A long outage makes the filter stale: discard it. Recovery then
        // starts from scratch (see _handleValid: !_hasValid). Without
        // smoothing (tau == 0) there is no filter memory to go stale.
        if (_hasValid && _cfg.tauSeconds > 0 &&
            nowMs - _lastValidMs > _warmupMs()) {
            _resetFilter();
        }
    }

    /// Handle a valid sample: filter, smooth, evaluate.
    void _handleValid(float raw, uint32_t nowMs) {
        _invalidActive = false;
        _lastRaw       = raw;

        // Where evaluation continues from. After a filter reset (or before
        // the first sample) nothing is known; after a short fault, resume
        // from the state the fault interrupted.
        State base = !_hasValid               ? State::UNKNOWN
                   : (_state == State::FAULT) ? _resumeState
                   :                            _state;

        float v = _cfg.spikeFilter ? _median(raw) : raw;

        if (!_hasValid) {
            _hasValid     = true;
            _firstValidMs = nowMs;
            _ema.reset();
            _ema.addSample(v, 1.0f);           // EMA starts at the first sample
        } else {
            float a = _alpha(nowMs - _lastValidMs);
            if (a > 0.0f) _ema.addSample(v, a); // dt == 0: nothing to weigh
        }
        _lastValidMs = nowMs;
        _sampleCount++;

        State next = _evaluate(base, _ema.value(), nowMs);
        if (next != _state) _setState(next, nowMs);
    }

    /**
     * @brief The state machine: next state from @p base for value @p x.
     *
     * Hard limits first (immediate, from any state); then the hysteresis
     * holds of the current excursion; then a fresh evaluation.
     */
    State _evaluate(State base, float x, uint32_t nowMs) {
        const float hiS = _cfg.hiSoft;
        const float loS = _cfg.loSoft;
        const float hiH = _cfg.effectiveHiHard();
        const float loH = _cfg.effectiveLoHard();
        const float h   = _cfg.hysteresis;

        // 1. Hard limits: immediate, whatever the state.
        if (!isnan(hiH) && x > hiH) return State::CRITICAL_HIGH;
        if (!isnan(loH) && x < loH) return State::CRITICAL_LOW;

        // 2. Hold the current excursion while within the hysteresis band.
        //    Comparisons against NAN are false, so a limit removed by
        //    reconfigure() simply releases the hold.
        switch (base) {
        case State::CRITICAL_HIGH:
            if (x > hiH - h) return State::CRITICAL_HIGH;
            if (x > hiS - h) return State::ALARM_HIGH;    // delay already served
            break;
        case State::CRITICAL_LOW:
            if (x < loH + h) return State::CRITICAL_LOW;
            if (x < loS + h) return State::ALARM_LOW;
            break;
        case State::ALARM_HIGH:
            if (x > hiS - h) return State::ALARM_HIGH;
            break;
        case State::ALARM_LOW:
            if (x < loS + h) return State::ALARM_LOW;
            break;
        case State::PENDING_HIGH:
            if (x > hiS - h) return _delayServed(nowMs) ? State::ALARM_HIGH
                                                        : State::PENDING_HIGH;
            break;
        case State::PENDING_LOW:
            if (x < loS + h) return _delayServed(nowMs) ? State::ALARM_LOW
                                                        : State::PENDING_LOW;
            break;
        default:
            break;
        }

        // 3. Fresh evaluation: a new soft-limit excursion starts its delay.
        if (x > hiS) {
            _pendingSinceMs = nowMs;
            return (_cfg.alarmDelaySeconds == 0) ? State::ALARM_HIGH
                                                 : State::PENDING_HIGH;
        }
        if (x < loS) {
            _pendingSinceMs = nowMs;
            return (_cfg.alarmDelaySeconds == 0) ? State::ALARM_LOW
                                                 : State::PENDING_LOW;
        }
        return State::OK;
    }

    // =========================================================================
    // HELPERS (mutex held)
    // =========================================================================

    void _setState(State s, uint32_t nowMs) {
        _state        = s;
        _stateSinceMs = nowMs;
    }

    bool _delayServed(uint32_t nowMs) const {
        return nowMs - _pendingSinceMs >= _cfg.alarmDelaySeconds * 1000UL;
    }

    uint32_t _warmupMs() const {
        return _cfg.tauSeconds * EMA_WARMUP_TAU_MULTIPLIER * 1000UL;
    }

    bool _isSettled(uint32_t nowMs) const {
        return _hasValid && (nowMs - _firstValidMs >= _warmupMs());
    }

    /// EMA weight for a sample @p dtMs after the previous one.
    float _alpha(uint32_t dtMs) const {
        if (_cfg.tauSeconds == 0) return 1.0f;  // no smoothing
        return ExponentialAverage::computeAlpha(dtMs / 1000.0f,
                                                (float)_cfg.tauSeconds);
    }

    /// Push @p x into the window and return the median of what it holds.
    /// With fewer than 3 samples the newest one is returned unchanged.
    float _median(float x) {
        _window[_windowPos] = x;
        _windowPos = (uint8_t)((_windowPos + 1) % SPIKE_WINDOW);
        if (_windowCount < SPIKE_WINDOW) _windowCount++;
        if (_windowCount < SPIKE_WINDOW) return x;

        float a = _window[0], b = _window[1], c = _window[2];
        if (a > b) { float t = a; a = b; b = t; }
        if (b > c) { b = c; }
        return (a > b) ? a : b;
    }

    void _resetFilter() {
        _ema.reset();
        _windowCount = 0;
        _windowPos   = 0;
        _hasValid    = false;
    }

    void _clearRuntime() {
        _resetFilter();
        _state          = State::UNKNOWN;
        _resumeState    = State::UNKNOWN;
        _stateSinceMs   = 0;
        _pendingSinceMs = 0;
        _firstValidMs   = 0;
        _lastValidMs    = 0;
        _invalidSinceMs = 0;
        _invalidActive  = false;
        _lastRaw        = NAN;
        _sampleCount    = 0;
    }

    // =========================================================================
    // LOCKING
    // =========================================================================

#if defined(ARDUINO_ARCH_ESP32)
    bool _lock() const {
        return xSemaphoreTake(_mutex, pdMS_TO_TICKS(RANGE_MONITOR_MUTEX_TIMEOUT)) == pdTRUE;
    }
    void _unlock() const { xSemaphoreGive(_mutex); }

    mutable StaticSemaphore_t _mutexBuf;
    SemaphoreHandle_t         _mutex;
#else
    // No preemptive threads on ESP8266, AVR or the host test build.
    bool _lock() const { return true; }
    void _unlock() const {}
#endif

    // =========================================================================
    // STATE NAMES
    // =========================================================================

    /// Name table; in flash on AVR. A function-local static keeps the header
    /// self-contained under C++11 (no inline variables).
#if defined(__AVR__)
    static const char* const* _stateNames() {
        static const char n0[] PROGMEM = "unknown";
        static const char n1[] PROGMEM = "ok";
        static const char n2[] PROGMEM = "pending_high";
        static const char n3[] PROGMEM = "pending_low";
        static const char n4[] PROGMEM = "alarm_high";
        static const char n5[] PROGMEM = "alarm_low";
        static const char n6[] PROGMEM = "critical_high";
        static const char n7[] PROGMEM = "critical_low";
        static const char n8[] PROGMEM = "fault";
        static const char* const names[STATE_COUNT] PROGMEM =
            { n0, n1, n2, n3, n4, n5, n6, n7, n8 };
        return names;
    }
#else
    static const char* const* _stateNames() {
        static const char* const names[STATE_COUNT] = {
            "unknown", "ok", "pending_high", "pending_low", "alarm_high",
            "alarm_low", "critical_high", "critical_low", "fault"
        };
        return names;
    }
#endif

    // =========================================================================
    // MEMBER VARIABLES
    // =========================================================================

    Config             _cfg;
    ConfigError        _cfgError;
    ExponentialAverage _ema;

    State    _state;            ///< Current state
    State    _resumeState;      ///< State interrupted by FAULT
    uint32_t _stateSinceMs;     ///< Time of the last state change
    uint32_t _pendingSinceMs;   ///< Start of the current soft-limit excursion
    uint32_t _firstValidMs;     ///< First valid sample since the last filter reset
    uint32_t _lastValidMs;      ///< Last valid sample
    uint32_t _invalidSinceMs;   ///< Start of the current run of invalid samples
    uint32_t _sampleCount;      ///< Valid samples since construction or reset()
    float    _lastRaw;          ///< Last raw sample, NAN if invalid
    bool     _hasValid;         ///< Filter holds data
    bool     _invalidActive;    ///< In a run of invalid samples

    float    _window[SPIKE_WINDOW]; ///< Spike filter ring buffer
    uint8_t  _windowCount;          ///< Samples in the window (0..3)
    uint8_t  _windowPos;            ///< Next write position
};
