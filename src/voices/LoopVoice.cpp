#include "soundsys/voices/LoopVoice.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace soundsys {

LoopVoice::LoopVoice(LoopVoiceConfig cfg) : cfg_(std::move(cfg)) {}

bool LoopVoice::load(std::string* error) {
    if (!buffer_.loadWav(cfg_.wavPath, error)) return false;
    loaded_ = true;
    return true;
}

void LoopVoice::prepare(const AudioFormat& fmt) {
    if (!loaded_) return;
    reader_.prepare(&buffer_, fmt.sampleRate);
    reader_.setLooping(true);
    fadeInStep_ = (cfg_.fadeInSeconds > 0.0)
                      ? static_cast<float>(1.0 / (cfg_.fadeInSeconds * fmt.sampleRate))
                      : 1.0f;
    fadeOutStep_ = (cfg_.fadeOutSeconds > 0.0)
                       ? static_cast<float>(1.0 / (cfg_.fadeOutSeconds * fmt.sampleRate))
                       : 1.0f;
}

void LoopVoice::onEvent(const SoundEvent& ev) {
    if (!loaded_) return;

    if (ev.type == cfg_.startOn) {
        if (state_ == State::Idle) {
            reader_.reset(0.0);
            gain_ = 0.0f;
        }
        state_ = State::FadingIn;
    } else if (ev.type == cfg_.stopOn) {
        if (state_ != State::Idle) state_ = State::FadingOut;
    }
}

float LoopVoice::parameterValue(const ParameterSnapshot& p) const {
    switch (cfg_.pitchParam) {
        case ParamId::NgPercent:  return p.ng;
        case ParamId::NrPercent:  return p.nr;
        case ParamId::N2Percent:  return p.n2;
        case ParamId::T4Celsius:  return p.t4;
        case ParamId::TorquePercent: return p.torque;
        case ParamId::CollectivePercent: return p.collective;
        case ParamId::AirspeedKt: return p.airspeed;
        default: return static_cast<float>(cfg_.referenceValue);
    }
}

void LoopVoice::onParameters(const ParameterSnapshot& p) {
    if (state_ == State::Idle || !loaded_) return;

    // Same varispeed idea as the start voice, but around a fixed reference: the
    // loop was recorded at `referenceValue`, so play it at the ratio between the
    // live value and that reference.
    const double value = static_cast<double>(parameterValue(p));
    double speed = 1.0;
    if (cfg_.referenceValue > 0.0 && value > 0.0) {
        speed = std::pow(value / cfg_.referenceValue, cfg_.pitchExponent);
    }
    reader_.setSpeed(std::clamp(speed, cfg_.minSpeed, cfg_.maxSpeed));
}

void LoopVoice::process(float* const* out, std::uint32_t frames) {
    if (state_ == State::Idle || !loaded_ || !reader_.ready()) return;

    float  scratchL[kMaxBlockFrames];
    float  scratchR[kMaxBlockFrames];
    float* scratch[kBusChannels] = {scratchL, scratchR};

    reader_.read(scratch, kBusChannels, frames);

    for (std::uint32_t i = 0; i < frames; ++i) {
        if (state_ == State::FadingIn) {
            gain_ += fadeInStep_;
            if (gain_ >= 1.0f) {
                gain_ = 1.0f;
                state_ = State::Running;
            }
        } else if (state_ == State::FadingOut) {
            gain_ -= fadeOutStep_;
            if (gain_ <= 0.0f) {
                gain_ = 0.0f;
                state_ = State::Idle;
            }
        }
        const float g = cfg_.gain * gain_;
        out[0][i] += scratch[0][i] * g;
        out[1][i] += scratch[1][i] * g;
        if (state_ == State::Idle) break;
    }
}

}  // namespace soundsys
