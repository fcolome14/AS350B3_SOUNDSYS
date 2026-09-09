// soundsys/voices/LoopVoice.hpp - a looping asset whose pitch follows a parameter.
//
// Used for the ground-idle / flight run loop that takes over when the start
// sequence finishes, and it is the same shape you want for the rotor loop:
// one seamless loop recorded at a known reference NG (or NR), pitched by the
// ratio between the live parameter and that reference.
#pragma once

#include <string>

#include "soundsys/AudioBuffer.hpp"
#include "soundsys/ISoundVoice.hpp"
#include "soundsys/VarispeedReader.hpp"

namespace soundsys {

struct LoopVoiceConfig {
    std::string wavPath;

    ParamId pitchParam = ParamId::NgPercent;
    double  referenceValue = 67.0;  // parameter value the loop was recorded at
    double  pitchExponent = 1.0;    // 1.0 = pitch strictly proportional to the parameter
    double  minSpeed = 0.25;
    double  maxSpeed = 3.0;

    float  gain = 1.0f;
    double fadeInSeconds = 0.4;
    double fadeOutSeconds = 0.6;

    // Events that start / stop the loop. The default pair makes this the voice
    // the turbine start hands over to.
    EventType startOn = EventType::StartSequenceComplete;
    EventType stopOn = EventType::EngineShutdown;
};

class LoopVoice final : public ISoundVoice {
public:
    explicit LoopVoice(LoopVoiceConfig cfg);

    bool load(std::string* error = nullptr);

    const char* name() const override { return "LoopVoice"; }
    void prepare(const AudioFormat& fmt) override;
    void onEvent(const SoundEvent& ev) override;
    void onParameters(const ParameterSnapshot& p) override;
    void process(float* const* out, std::uint32_t frames) override;
    bool isActive() const override { return state_ != State::Idle; }

    double playbackSpeed() const noexcept { return reader_.speed(); }

private:
    enum class State { Idle, FadingIn, Running, FadingOut };

    float parameterValue(const ParameterSnapshot& p) const;

    LoopVoiceConfig cfg_;
    AudioBuffer     buffer_;
    VarispeedReader reader_;

    State  state_ = State::Idle;
    bool   loaded_ = false;
    float  gain_ = 0.0f;
    float  fadeInStep_ = 0.0f;
    float  fadeOutStep_ = 0.0f;
};

} // namespace soundsys
