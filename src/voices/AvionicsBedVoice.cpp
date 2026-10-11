#include "soundsys/voices/AvionicsBedVoice.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace soundsys {
namespace {

constexpr double kHalfPi = 1.57079632679489661923;

}  // namespace

AvionicsBedVoice::AvionicsBedVoice(AvionicsBedConfig cfg) : cfg_(std::move(cfg)) {}

AvionicsBedVoice::~AvionicsBedVoice() = default;

bool AvionicsBedVoice::load(std::string* error) {
    if (cfg_.oneShot.empty() && cfg_.loop.empty()) {
        if (error) *error = "no avionics clip given";
        return false;
    }
    if (!cfg_.oneShot.empty()) {
        oneShot_ = std::make_shared<AudioBuffer>();
        if (!oneShot_->loadWav(cfg_.oneShot, error)) return false;
    }
    if (!cfg_.loop.empty()) {
        loop_ = std::make_shared<AudioBuffer>();
        if (!loop_->loadWav(cfg_.loop, error)) return false;
    }
    loaded_ = true;
    return true;
}

void AvionicsBedVoice::prepare(const AudioFormat& fmt) { sampleRate_ = fmt.sampleRate; }

void AvionicsBedVoice::startClip(const AudioBuffer& buffer, bool looping, bool loopPending,
                                 double fadeSeconds) {
    const float fadeStep =
        fadeSeconds > 0.0 ? static_cast<float>(1.0 / (fadeSeconds * sampleRate_)) : 1.0f;

    // Whatever is already sounding leaves at the same rate the new clip arrives,
    // so the two envelopes stay complementary and the join keeps its power.
    for (Player& p : players_) {
        if (p.active) {
            p.target = 0.0f;
            p.fadeStep = fadeStep;
            p.loopPending = false;
        }
    }

    Player* slot = nullptr;
    for (Player& p : players_) {
        if (!p.active) {
            slot = &p;
            break;
        }
    }
    if (slot == nullptr) {  // everything busy: take the quietest
        slot = &players_[0];
        for (Player& p : players_) {
            if (p.level < slot->level) slot = &p;
        }
    }

    slot->buffer = &buffer;
    slot->position = 0.0;
    slot->step = buffer.sampleRate() > 0.0 ? buffer.sampleRate() / sampleRate_ : 1.0;
    slot->looping = looping;
    slot->loopPending = loopPending;
    slot->level = 0.0f;
    slot->target = 1.0f;
    slot->fadeStep = fadeStep;
    slot->active = true;
}

void AvionicsBedVoice::fadeOutAll(double fadeSeconds) {
    const float fadeStep =
        fadeSeconds > 0.0 ? static_cast<float>(1.0 / (fadeSeconds * sampleRate_)) : 1.0f;
    for (Player& p : players_) {
        if (p.active) {
            p.target = 0.0f;
            p.fadeStep = fadeStep;
            p.loopPending = false;
        }
    }
}

void AvionicsBedVoice::onEvent(const SoundEvent& ev) {
    if (!loaded_) return;

    if (ev.type == cfg_.startOn) {
        // A link that blinks must not retrigger the master switch on top of a
        // fan that is already running.
        if (running_) return;
        running_ = true;
        if (oneShot_ != nullptr) {
            startClip(*oneShot_, false, loop_ != nullptr, cfg_.fadeInSeconds);
        } else if (loop_ != nullptr) {
            startClip(*loop_, true, false, cfg_.fadeInSeconds);
        }
    } else if (ev.type == cfg_.stopOn) {
        if (!running_) return;
        running_ = false;
        fadeOutAll(cfg_.fadeOutSeconds);
    }
}

void AvionicsBedVoice::onParameters(const ParameterSnapshot&) {
    // Nothing to follow: the fan runs at its own speed whatever the engine does.
}

void AvionicsBedVoice::process(float* const* out, std::uint32_t frames) {
    if (!loaded_ || frames == 0) return;

    // The one-shot is about to end with the loop waiting behind it: bring the
    // loop in now, so the two overlap instead of leaving a hole.
    for (Player& p : players_) {
        if (!p.active || !p.loopPending || p.buffer == nullptr || loop_ == nullptr) continue;
        const double left =
            (static_cast<double>(p.buffer->frames()) - p.position) * p.step / sampleRate_;
        if (left <= cfg_.loopJoinSeconds) {
            p.loopPending = false;
            startClip(*loop_, true, false, cfg_.loopJoinSeconds);
            break;  // startClip touched every player; nothing else to schedule now
        }
    }

    for (Player& p : players_) {
        if (!p.active || p.buffer == nullptr) continue;
        const std::int64_t  total = static_cast<std::int64_t>(p.buffer->frames());
        const std::uint32_t channels = p.buffer->channels();
        if (total <= 0 || channels == 0) {
            p.active = false;
            continue;
        }

        for (std::uint32_t i = 0; i < frames; ++i) {
            if (p.level < p.target) {
                p.level = std::min(p.target, p.level + p.fadeStep);
            } else if (p.level > p.target) {
                p.level = std::max(p.target, p.level - p.fadeStep);
            }

            std::int64_t index = static_cast<std::int64_t>(p.position);
            if (index >= total) {
                if (!p.looping) {
                    p.active = false;
                    break;
                }
                p.position -= static_cast<double>(total);
                index = static_cast<std::int64_t>(p.position);
            }
            const double frac = p.position - static_cast<double>(index);
            // Equal power, same as the phase player: two unrelated clips leaving
            // and arriving at the same rate keep the level steady.
            const float env = static_cast<float>(std::sin(p.level * kHalfPi)) * cfg_.gain;

            for (std::uint32_t c = 0; c < kBusChannels; ++c) {
                const float*       data = p.buffer->channel(std::min(c, channels - 1));
                const std::int64_t next = (index + 1 < total) ? index + 1 : (p.looping ? 0 : index);
                const float s = static_cast<float>(data[index] + (data[next] - data[index]) * frac);
                out[c][i] += s * env;
            }
            p.position += p.step;
        }

        if (p.level <= 0.0f && p.target <= 0.0f) p.active = false;
    }
}

bool AvionicsBedVoice::isActive() const {
    for (const Player& p : players_) {
        if (p.active) return true;
    }
    return false;
}

}  // namespace soundsys
