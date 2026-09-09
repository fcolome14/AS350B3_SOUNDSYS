#include "soundsys/voices/TurbineStartVoice.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace soundsys {

TurbineStartVoice::TurbineStartVoice(TurbineStartConfig cfg) : cfg_(std::move(cfg)) {
    gain_ = cfg_.gain;
    head_ = makePlaybackHead(cfg_.mode);
}

bool TurbineStartVoice::load(std::string* error) {
    if (!buffer_.loadWav(cfg_.wavPath, error)) return false;
    if (!anchors_.loadJson(cfg_.anchorsPath, error)) return false;

    // The table describes positions inside this recording; if it was built from
    // a different (or re-trimmed) file, every sync decision below is wrong.
    if (anchors_.lastTime() > buffer_.durationSeconds() + 0.01) {
        if (error) {
            *error = "anchor table ends at " + std::to_string(anchors_.lastTime()) +
                     " s but the recording is only " +
                     std::to_string(buffer_.durationSeconds()) + " s long";
        }
        return false;
    }

    loaded_ = true;
    return true;
}

void TurbineStartVoice::prepare(const AudioFormat& fmt) {
    if (!loaded_) return;
    head_->prepare(&buffer_, fmt.sampleRate);
    head_->setLooping(false);
    fadeStep_ = (cfg_.fadeOutSeconds > 0.0)
                    ? static_cast<float>(1.0 / (cfg_.fadeOutSeconds * fmt.sampleRate))
                    : 1.0f;
}

void TurbineStartVoice::beginStart(double ng) {
    // Enter the recording where the simulated NG already is, so a start armed
    // mid-spool (a re-light, a saved state) does not replay the crank.
    const double start = std::max(anchors_.timeForNg(ng), anchors_.firstTime());
    head_->reset(start);
    targetPos_ = start;
    lastTargetPos_ = start;
    smoothedFeedForward_ = 1.0;
    maxNgSeen_ = ng;
    haveLastTarget_ = false;
    handoffEmitted_ = false;
    fadeGain_ = 1.0f;
    state_ = State::Running;
}

void TurbineStartVoice::onEvent(const SoundEvent& ev) {
    if (!loaded_) return;

    switch (ev.type) {
        case EventType::EngineStartCommand:
            // ev.value carries the NG the simulator is at, 0 for a cold start.
            beginStart(static_cast<double>(ev.value));
            break;

        case EventType::EngineStartAbort:
        case EventType::EngineShutdown:
            // A spool-down is not this voice's recording played backwards, so it
            // bows out and leaves the shutdown to a voice of its own.
            if (state_ == State::Running) state_ = State::FadingOut;
            break;

        default:
            break;
    }
}

void TurbineStartVoice::onParameters(const ParameterSnapshot& p) {
    if (state_ == State::Idle || !loaded_) return;

    const double dt = p.blockSeconds;
    targetPos_ = anchors_.timeForNg(static_cast<double>(p.ng));

    // A start that gives up (starter dropped, hung start rescued by the pilot)
    // shows up as NG falling away from its peak, with no event to announce it.
    maxNgSeen_ = std::max(maxNgSeen_, static_cast<double>(p.ng));
    if (state_ == State::Running && p.ng < maxNgSeen_ - cfg_.abortNgDrop) {
        state_ = State::FadingOut;
    }

    if (!haveLastTarget_) {
        lastTargetPos_ = targetPos_;
        haveLastTarget_ = true;
    }

    if (dt > 0.0) {
        // Feed-forward: how fast the recording has to run for its NG to keep up
        // with the simulated NG. Equal to timePerNg(NG) * dNG/dt, computed as a
        // difference so anchor segment changes are handled for free.
        const double rawFeedForward = (targetPos_ - lastTargetPos_) / dt;

        // One-pole low pass: NG telemetry is noisy at block rate and unsmoothed
        // speed jitter is audible as warble.
        const double coef = std::exp(-2.0 * 3.14159265358979323846 * cfg_.speedSmoothingHz * dt);
        smoothedFeedForward_ = rawFeedForward + coef * (smoothedFeedForward_ - rawFeedForward);
    }
    lastTargetPos_ = targetPos_;

    // Feedback: nudge the head towards where NG says it should be. Small gain,
    // so it corrects drift over seconds instead of bending the pitch audibly.
    const double error = targetPos_ - head_->positionSeconds();
    const double speed = smoothedFeedForward_ + cfg_.positionGain * error;
    head_->setSpeed(std::clamp(speed, cfg_.minSpeed, cfg_.maxSpeed));

    // Hand over to the idle loop once the recording is spent. The event goes
    // through the engine, so this voice never learns who takes the baton.
    const bool finished = head_->atEnd() ||
                          head_->positionSeconds() >=
                              buffer_.durationSeconds() - cfg_.handoffMarginSeconds;
    if (finished && !handoffEmitted_) {
        handoffEmitted_ = true;
        if (sink_) {
            SoundEvent done;
            done.type = EventType::StartSequenceComplete;
            done.value = p.ng;
            sink_->emit(done);
        }
        if (state_ == State::Running) state_ = State::FadingOut;
    }
}

void TurbineStartVoice::process(float* const* out, std::uint32_t frames) {
    if (state_ == State::Idle || !loaded_ || !head_->ready()) return;

    float  scratchL[kMaxBlockFrames];
    float  scratchR[kMaxBlockFrames];
    float* scratch[kBusChannels] = {scratchL, scratchR};

    const std::uint32_t got = head_->read(scratch, kBusChannels, frames);
    if (got == 0 && state_ != State::FadingOut) state_ = State::FadingOut;

    for (std::uint32_t i = 0; i < frames; ++i) {
        if (state_ == State::FadingOut) {
            fadeGain_ -= fadeStep_;
            if (fadeGain_ <= 0.0f) {
                fadeGain_ = 0.0f;
                state_ = State::Idle;
            }
        }
        const float g = gain_ * fadeGain_;
        out[0][i] += scratch[0][i] * g;
        out[1][i] += scratch[1][i] * g;
        if (state_ == State::Idle) break;
    }
}

}  // namespace soundsys
