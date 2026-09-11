#include "soundsys/voices/NgLoopBankVoice.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace soundsys {
namespace {

constexpr double kHalfPi = 1.57079632679489661923;

float parameterValue(const ParameterSnapshot& p, ParamId id) {
    switch (id) {
        case ParamId::NgPercent:         return p.ng;
        case ParamId::NrPercent:         return p.nr;
        case ParamId::N2Percent:         return p.n2;
        case ParamId::T4Celsius:         return p.t4;
        case ParamId::TorquePercent:     return p.torque;
        case ParamId::CollectivePercent: return p.collective;
        case ParamId::AirspeedKt:        return p.airspeed;
        default:                         return 0.0f;
    }
}

}  // namespace

NgLoopBankVoice::NgLoopBankVoice(NgLoopBankConfig cfg) : cfg_(std::move(cfg)) {}

NgLoopBankVoice::~NgLoopBankVoice() = default;

bool NgLoopBankVoice::load(std::string* error) {
    if (cfg_.loops.empty()) {
        if (error) *error = "loop bank has no loops";
        return false;
    }
    loops_.clear();
    for (const LoopBankEntry& e : cfg_.loops) {
        Loop loop;
        loop.reference = e.referenceValue;
        loop.buffer = std::make_unique<AudioBuffer>();
        if (!loop.buffer->loadWav(e.wavPath, error)) return false;
        if (e.referenceValue <= 0.0) {
            if (error) *error = "loop " + e.wavPath + " needs a positive reference value";
            return false;
        }
        loop.reader = std::make_unique<VarispeedReader>();
        loops_.push_back(std::move(loop));
    }
    std::sort(loops_.begin(), loops_.end(),
              [](const Loop& a, const Loop& b) { return a.reference < b.reference; });
    loaded_ = true;
    return true;
}

void NgLoopBankVoice::prepare(const AudioFormat& fmt) {
    if (!loaded_) return;
    sampleRate_ = fmt.sampleRate;
    for (Loop& loop : loops_) {
        loop.reader->prepare(loop.buffer.get(), fmt.sampleRate);
        loop.reader->setLooping(true);
    }
    fadeInStep_ = cfg_.fadeInSeconds > 0.0
                      ? static_cast<float>(1.0 / (cfg_.fadeInSeconds * fmt.sampleRate))
                      : 1.0f;
    fadeOutStep_ = cfg_.fadeOutSeconds > 0.0
                       ? static_cast<float>(1.0 / (cfg_.fadeOutSeconds * fmt.sampleRate))
                       : 1.0f;
}

void NgLoopBankVoice::onEvent(const SoundEvent& ev) {
    if (!loaded_) return;
    if (ev.type == cfg_.startOn) {
        if (state_ == State::Idle) {
            for (Loop& loop : loops_) {
                loop.reader->reset(0.0);
                loop.current = loop.target = 0.0f;
            }
            fade_ = 0.0f;
            haveValue_ = false;
        }
        state_ = State::FadingIn;
    } else if (ev.type == cfg_.stopOn) {
        if (state_ != State::Idle) state_ = State::FadingOut;
    }
}

double NgLoopBankVoice::speed(std::size_t i) const noexcept {
    return i < loops_.size() ? loops_[i].reader->speed() : 0.0;
}

// Equal-power crossfade between the two loops that bracket `value`; the loops
// are independent recordings, so their powers add.
void NgLoopBankVoice::updateWeights(double value) {
    for (Loop& loop : loops_) loop.target = 0.0f;

    if (value <= loops_.front().reference) {
        loops_.front().target = 1.0f;
    } else if (value >= loops_.back().reference) {
        loops_.back().target = 1.0f;
    } else {
        std::size_t i = 0;
        while (i + 2 < loops_.size() && value >= loops_[i + 1].reference) ++i;
        const double span = loops_[i + 1].reference - loops_[i].reference;
        const double u = span > 0.0 ? (value - loops_[i].reference) / span : 1.0;
        loops_[i].target = static_cast<float>(std::cos(u * kHalfPi));
        loops_[i + 1].target = static_cast<float>(std::sin(u * kHalfPi));
    }
}

void NgLoopBankVoice::onParameters(const ParameterSnapshot& p) {
    if (state_ == State::Idle || !loaded_) return;

    // One-pole smoothing: the parameter arrives in steps (the VEMD's 20 Hz
    // sensor, the host's update loop), and a step in a varispeed ratio is a
    // step in pitch - heard as a buzz if it repeats at 20-60 Hz.
    const double raw = static_cast<double>(parameterValue(p, cfg_.param));
    if (!haveValue_) {
        value_ = raw;
        haveValue_ = true;
    } else if (p.blockSeconds > 0.0) {
        const double a = std::exp(-2.0 * 3.14159265358979323846 * cfg_.smoothingHz * p.blockSeconds);
        value_ = raw + a * (value_ - raw);
    }

    updateWeights(value_);
    for (Loop& loop : loops_) {
        double s = value_ > 0.0 ? std::pow(value_ / loop.reference, cfg_.pitchExponent) : 1.0;
        loop.reader->setSpeed(std::clamp(s, cfg_.minSpeed, cfg_.maxSpeed));
    }
}

void NgLoopBankVoice::process(float* const* out, std::uint32_t frames) {
    if (state_ == State::Idle || !loaded_ || frames == 0) return;

    // The fade envelope for this block, shared by every loop.
    float envelope[kMaxBlockFrames];
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (state_ == State::FadingIn) {
            fade_ += fadeInStep_;
            if (fade_ >= 1.0f) {
                fade_ = 1.0f;
                state_ = State::Running;
            }
        } else if (state_ == State::FadingOut) {
            fade_ -= fadeOutStep_;
            if (fade_ <= 0.0f) fade_ = 0.0f;
        }
        envelope[i] = fade_ * cfg_.gain;
    }

    float  scratchL[kMaxBlockFrames];
    float  scratchR[kMaxBlockFrames];
    float* scratch[kBusChannels] = {scratchL, scratchR};
    const float invFrames = 1.0f / static_cast<float>(frames);

    for (Loop& loop : loops_) {
        // Silent for the whole block: skip the read. The loop resumes from
        // where it stopped next time it is needed; nobody can hear its phase.
        if (loop.current <= 0.0f && loop.target <= 0.0f) continue;
        loop.reader->read(scratch, kBusChannels, frames);
        // Weight ramps linearly across the block - no zipper when NG moves.
        const float from = loop.current;
        const float delta = (loop.target - from) * invFrames;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float g = (from + delta * static_cast<float>(i + 1)) * envelope[i];
            out[0][i] += scratchL[i] * g;
            out[1][i] += scratchR[i] * g;
        }
        loop.current = loop.target;
    }

    if (state_ == State::FadingOut && fade_ <= 0.0f) {
        state_ = State::Idle;
        for (Loop& loop : loops_) loop.current = loop.target = 0.0f;
    }
}

}  // namespace soundsys
