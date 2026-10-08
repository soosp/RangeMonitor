/**
 * @file ExponentialAverage.h
 * @brief Single exponential moving average (EMA) for scalar time-series data.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Péter Soós — https://github.com/soosp
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *
 * -----------------------------------------------------------------------------
 * DESIGN PRINCIPLES
 * -----------------------------------------------------------------------------
 *
 * SINGLE-CLASS, GENERAL PURPOSE:
 *   ExponentialAverage computes a single EMA over a stream of scalar samples.
 *   For multiple smoothing time constants (e.g. fast + slow trend tracking),
 *   instantiate multiple objects with different alpha values.
 *
 * HARDWARE-INDEPENDENT:
 *   No dependency on Arduino, ESP-IDF, or any hardware API. Suitable for
 *   use on AVR, ESP8266, ESP32, or any C++11 platform.
 *
 * NO DYNAMIC MEMORY:
 *   All state fits in two floats (~8 bytes). No heap allocation.
 *
 * INITIALISATION:
 *   The first call to addSample() initialises the EMA directly to that value,
 *   avoiding the slow convergence that would result from starting at zero.
 *   isValid() returns false until the first sample is added.
 *
 * THREAD SAFETY:
 *   ExponentialAverage is not thread-safe. If addSample() and value() are
 *   called from different tasks or from an ISR and the main loop, the caller
 *   is responsible for mutual exclusion.
 *
 * EMA FORMULA:
 *   EMA_n = EMA_{n-1} + alpha × (sample - EMA_{n-1})
 *         = (1 - alpha) × EMA_{n-1} + alpha × sample
 *
 *   alpha controls the smoothing:
 *     - High alpha (e.g. 0.5) → fast response, less smoothing
 *     - Low  alpha (e.g. 0.01) → slow response, heavy smoothing
 *
 *   The effective "lag" in number of samples is approximately 1/alpha:
 *     alpha = 0.10 → ~10 samples lag
 *     alpha = 0.01 → ~100 samples lag
 *
 * ALPHA SELECTION GUIDE:
 *   There is no universally correct alpha — the right value depends on the
 *   update rate and the desired response time:
 *
 *     alpha = 1 - exp(-dt / tau)
 *
 *   where dt is the update interval [s] and tau is the desired time constant [s].
 *
 *   Examples (1 s update interval):
 *     tau =  10 s → alpha ≈ 0.095  (≈ 0.10)
 *     tau = 100 s → alpha ≈ 0.010  (≈ 0.01)
 *     tau =  60 s → alpha ≈ 0.016
 *
 * -----------------------------------------------------------------------------
 * TYPICAL USE
 * -----------------------------------------------------------------------------
 * @code
 *   #include <ExponentialAverage.h>
 *
 *   // Fast tracker (~10 samples lag) and slow background estimate (~100 lag)
 *   ExponentialAverage emaFast(0.10f);
 *   ExponentialAverage emaSlow(0.01f);
 *
 *   // In loop() — call once per second:
 *   emaFast.addSample(cpm);
 *   emaSlow.addSample(cpm);
 *
 *   if (emaFast.isValid()) {
 *       Serial.printf("Fast: %.1f  Slow: %.1f\n",
 *                     emaFast.value(), emaSlow.value());
 *
 *       // Detect sustained change vs. short-lived Poisson fluctuation:
 *       float trend = emaFast.value() - emaSlow.value();
 *   }
 * @endcode
 */

#pragma once
#include <math.h>

class ExponentialAverage {
public:
    // =========================================================================
    // CONSTRUCTOR
    // =========================================================================

    /**
     * @brief Construct an EMA with the given smoothing factor.
     *
     * @param alpha  Smoothing factor in (0, 1]. High = fast response,
     *               low = heavy smoothing. Clamped to (0, 1] at runtime.
     */
    explicit ExponentialAverage(float alpha = 0.1f)
        : _alpha(alpha > 0.0f && alpha <= 1.0f ? alpha : 0.1f)
        , _value(NAN)
    {}
    // =========================================================================
    // CORE API
    // =========================================================================

    /**
     * @brief Add a new sample with the stored alpha and update the EMA.
     * 
     * @param sample  New measurement value.
     */
    void addSample(float sample) {
        addSample(sample, _alpha);
    }

    /**
     * @brief Add a new sample using a one-shot alpha (time-corrected update).
     *
     * Applies a single EMA step with the given alpha instead of the stored
     * _alpha. Use when the sampling interval varies — compute the actual
     * elapsed time and derive alpha from it, without permanently changing
     * the stored smoothing factor.
     *
     * The first valid (non-NaN) call initialises the EMA directly to the
     * sample value, avoiding slow convergence from zero.
     *
     * NaN samples and invalid alpha values are silently ignored.
     *
     * @param sample  New measurement value.
     * @param alpha   One-shot smoothing factor in (0, 1].
     */
    void addSample(float sample, float alpha) {
        if (isnan(sample) || alpha <= 0.0f || alpha > 1.0f || isnan(alpha)) return;
        if (isnan(_value)) _value = sample;
        else               _value += alpha * (sample - _value);
    }

    /**
     * @brief Return the current EMA value.
     *
     * Returns NaN if no valid sample has been added yet. Check isValid()
     * before using this value if startup behaviour matters.
     */
    float value()   const { return _value; }

    /**
     * @brief Return true if at least one valid sample has been added.
     */
    bool  isValid() const { return !isnan(_value); }

    /**
     * @brief Return the smoothing factor alpha.
     */
    float alpha() const { return _alpha; }

    /**
     * @brief Compute the EMA smoothing factor alpha for a given sample interval
     *        and time constant.
     *
     *   alpha = 1 - exp(-dt / tau)
     *
     * @param dtSeconds    Sampling interval in seconds.
     * @param tauSeconds   Desired time constant in seconds.
     * @return             Smoothing factor alpha in (0, 1].
     */
    static float computeAlpha(float dtSeconds, float tauSeconds) {
        return 1.0f - expf(-dtSeconds / tauSeconds);
    }

    /**
     * @brief Change the smoothing factor and reset the EMA.
     *
     * Changing alpha while the EMA is running would produce inconsistent
     * history, so the EMA is reset to the uninitialised state. The next
     * addSample() call will re-initialise it directly.
     *
     * @param alpha  New smoothing factor in (0, 1]. Invalid values are ignored.
     */
    void setAlpha(float alpha) {
        if (alpha <= 0.0f || alpha > 1.0f || isnan(alpha)) return;
        _alpha = alpha;
        _value = NAN;
    }

    /**
     * @brief Reset the EMA to the uninitialised state.
     *
     * The next addSample() call will re-initialise the EMA directly to
     * the new sample value.
     */
    void reset() { _value = NAN; }

private:
    float _alpha;  ///< Smoothing factor in (0, 1]
    float _value;  ///< Current EMA value, or NaN if uninitialised
};
