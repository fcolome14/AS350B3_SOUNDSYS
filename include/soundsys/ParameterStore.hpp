// soundsys/ParameterStore.hpp - latest-value-wins continuous parameters.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "soundsys/Events.hpp"

namespace soundsys {

// Plain snapshot handed to voices once per block, so every voice in a block
// sees the same coherent set of values and never touches an atomic in its
// inner loop.
struct ParameterSnapshot {
    float ng = 0.0f;
    float nr = 0.0f;
    float n2 = 0.0f;
    float t4 = 0.0f;
    float torque = 0.0f;
    float collective = 0.0f;
    float airspeed = 0.0f;
    float masterGain = 1.0f;
    float phase = 0.0f;  // EnginePhase

    // Seconds of wall time covered by the block this snapshot belongs to.
    // Voices integrate rates with it instead of keeping their own clock.
    double blockSeconds = 0.0;
};

class ParameterStore {
public:
    ParameterStore() {
        for (auto& v : values_) v.store(0.0f, std::memory_order_relaxed);
        set(ParamId::MasterGain, 1.0f);
    }

    // Callable from any thread; never blocks.
    void set(ParamId id, float value) noexcept {
        values_[static_cast<std::size_t>(id)].store(value, std::memory_order_relaxed);
    }

    float get(ParamId id) const noexcept {
        return values_[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
    }

    ParameterSnapshot snapshot(double blockSeconds) const noexcept {
        ParameterSnapshot s;
        s.ng = get(ParamId::NgPercent);
        s.nr = get(ParamId::NrPercent);
        s.n2 = get(ParamId::N2Percent);
        s.t4 = get(ParamId::T4Celsius);
        s.torque = get(ParamId::TorquePercent);
        s.collective = get(ParamId::CollectivePercent);
        s.airspeed = get(ParamId::AirspeedKt);
        s.masterGain = get(ParamId::MasterGain);
        s.phase = get(ParamId::EnginePhase);
        s.blockSeconds = blockSeconds;
        return s;
    }

private:
    std::array<std::atomic<float>, static_cast<std::size_t>(ParamId::Count)> values_{};
};

} // namespace soundsys
