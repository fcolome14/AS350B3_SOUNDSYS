// soundsys/voices/NgLoopBankVoice.hpp - the engine at any steady NG, for as long as it stays there.
//
// A bank of seamless loops, each recorded while the engine held one NG (ground
// idle, flight idle...). At any NG the two loops that bracket it play together,
// each read at NG / its recorded NG - so the tone lands where the engine is -
// and crossfaded by how close NG sits to each one:
//
//     NG 68 ......... idle loop alone, at 1x
//     NG 74 ......... idle loop at x1.09 + flight loop at x0.92, half and half
//     NG 80.7 ....... flight loop alone, at 1x
//     NG 90 ......... flight loop at x1.12
//
// That covers holding idle indefinitely, the twist grip to FLIGHT and back,
// and collective changes in flight, all driven by the NG the VEMD reports and
// none of it by a clock. Around a recorded NG the pitch changes are small, so
// plain varispeed is the right tool here: no grains, nothing to stretch.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "soundsys/AudioBuffer.hpp"
#include "soundsys/ISoundVoice.hpp"
#include "soundsys/VarispeedReader.hpp"

namespace soundsys {

struct LoopBankEntry {
    std::string wavPath;
    double      referenceValue = 0.0;  // parameter value the loop was recorded at
};

struct NgLoopBankConfig {
    std::vector<LoopBankEntry> loops;  // any order

    ParamId param = ParamId::NgPercent;
    double  pitchExponent = 1.0;       // 1 = tone proportional to the parameter
    double  minSpeed = 0.5;
    double  maxSpeed = 2.0;
    double  smoothingHz = 6.0;         // takes the steps out of 20-60 Hz updates

    float  gain = 1.0f;
    double fadeInSeconds = 1.5;        // the handover from the start voice
    double fadeOutSeconds = 1.0;

    EventType startOn = EventType::StartSequenceComplete;
    EventType stopOn = EventType::EngineShutdown;
};

class NgLoopBankVoice final : public ISoundVoice {
public:
    explicit NgLoopBankVoice(NgLoopBankConfig cfg);
    ~NgLoopBankVoice() override;

    // Loads every loop. Fails if any is missing or none is given.
    bool load(std::string* error = nullptr);

    const char* name() const override { return "NgLoopBankVoice"; }
    void prepare(const AudioFormat& fmt) override;
    void onEvent(const SoundEvent& ev) override;
    void onParameters(const ParameterSnapshot& p) override;
    void process(float* const* out, std::uint32_t frames) override;
    bool isActive() const override { return state_ != State::Idle; }

    // --- diagnostics ------------------------------------------------------
    std::size_t loopCount() const noexcept { return loops_.size(); }
    float  weight(std::size_t i) const noexcept { return i < loops_.size() ? loops_[i].target : 0.0f; }
    double speed(std::size_t i) const noexcept;
    double smoothedValue() const noexcept { return value_; }

private:
    enum class State { Idle, FadingIn, Running, FadingOut };

    struct Loop {
        double                           reference = 0.0;
        std::unique_ptr<AudioBuffer>     buffer;
        std::unique_ptr<VarispeedReader> reader;
        float                            current = 0.0f;  // weight at the end of the last block
        float                            target = 0.0f;   // weight for this block
    };

    void updateWeights(double value);

    NgLoopBankConfig  cfg_;
    std::vector<Loop> loops_;  // sorted by reference
    State             state_ = State::Idle;
    bool              loaded_ = false;
    bool              haveValue_ = false;
    double            value_ = 0.0;
    double            sampleRate_ = 48000.0;
    float             fade_ = 0.0f;
    float             fadeInStep_ = 1.0f;
    float             fadeOutStep_ = 1.0f;
};

}  // namespace soundsys
