// soundsys/Limiter.hpp - final safety net on the master bus.
//
// Voices are mixed by simple addition, so simultaneous loud events can exceed
// full scale. A fast-attack / slow-release gain envelope keeps the bus inside
// [-1, 1] without the buzz of hard clipping.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace soundsys {

class Limiter {
public:
    void prepare(double sampleRate, double attackMs = 1.0, double releaseMs = 120.0) {
        attackCoef_  = coefFor(attackMs, sampleRate);
        releaseCoef_ = coefFor(releaseMs, sampleRate);
        gain_ = 1.0f;
    }

    void setCeiling(float ceiling) noexcept { ceiling_ = ceiling; }

    void process(float* const* bus, std::uint32_t channels, std::uint32_t frames) noexcept {
        for (std::uint32_t i = 0; i < frames; ++i) {
            float peak = 0.0f;
            for (std::uint32_t c = 0; c < channels; ++c) {
                peak = std::max(peak, std::fabs(bus[c][i]));
            }
            const float target = (peak > ceiling_) ? (ceiling_ / peak) : 1.0f;
            // Attack fast when we need to pull down, release slowly on the way back.
            const float coef = (target < gain_) ? attackCoef_ : releaseCoef_;
            gain_ = target + coef * (gain_ - target);
            for (std::uint32_t c = 0; c < channels; ++c) {
                bus[c][i] = std::clamp(bus[c][i] * gain_, -1.0f, 1.0f);
            }
        }
    }

    float gain() const noexcept { return gain_; }

private:
    static float coefFor(double ms, double sampleRate) {
        if (ms <= 0.0 || sampleRate <= 0.0) return 0.0f;
        return static_cast<float>(std::exp(-1.0 / (0.001 * ms * sampleRate)));
    }

    float ceiling_ = 0.98f;
    float gain_ = 1.0f;
    float attackCoef_ = 0.0f;
    float releaseCoef_ = 0.0f;
};

} // namespace soundsys
