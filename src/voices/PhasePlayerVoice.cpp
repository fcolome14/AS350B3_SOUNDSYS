#include "soundsys/voices/PhasePlayerVoice.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace soundsys {
namespace {

constexpr double kHalfPi = 1.57079632679489661923;

std::string fileLabel(const std::string& path) {
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

}  // namespace

PhasePlayerVoice::PhasePlayerVoice(PhasePlayerConfig cfg) : cfg_(std::move(cfg)) {}

PhasePlayerVoice::~PhasePlayerVoice() = default;

const PhasePlayerVoice::Clip* PhasePlayerVoice::clipFor(const std::string& path) const {
    for (const Clip& c : clips_) {
        if (c.path == path) return &c;
    }
    return nullptr;
}

bool PhasePlayerVoice::load(std::string* error) {
    clips_.clear();
    bool any = false;
    for (const PhaseClips& phase : cfg_.phases) {
        for (const std::string* path : {&phase.oneShot, &phase.loop}) {
            if (path->empty() || clipFor(*path) != nullptr) continue;
            Clip clip;
            clip.buffer = std::make_shared<AudioBuffer>();
            if (!clip.buffer->loadWav(*path, error)) return false;
            clip.path = *path;
            clip.label = fileLabel(*path);
            clips_.push_back(std::move(clip));
            any = true;
        }
    }
    if (!any) {
        if (error) *error = "no phase clips given";
        return false;
    }
    loaded_ = true;
    return true;
}

void PhasePlayerVoice::prepare(const AudioFormat& fmt) { sampleRate_ = fmt.sampleRate; }

void PhasePlayerVoice::onEvent(const SoundEvent&) {
    // Phases arrive as a parameter, not as events: a listener that joins late,
    // or misses a datagram, still ends up in the right phase.
}

std::size_t PhasePlayerVoice::freeSlot() {
    for (std::size_t i = 0; i < players_.size(); ++i) {
        if (!players_[i].active) return i;
    }
    // Everything busy (three crossfades on top of each other): take the quietest.
    std::size_t quietest = 0;
    for (std::size_t i = 1; i < players_.size(); ++i) {
        if (players_[i].level < players_[quietest].level) quietest = i;
    }
    return quietest;
}

void PhasePlayerVoice::startClip(const Clip& clip, bool looping, bool loopPending,
                                 EnginePhase phase, double fadeSeconds) {
    const float fadeStep = fadeSeconds > 0.0
                               ? static_cast<float>(1.0 / (fadeSeconds * sampleRate_))
                               : 1.0f;
    // Everything already sounding starts leaving at the same rate, so the two
    // envelopes stay complementary and the crossfade keeps its power.
    for (Player& p : players_) {
        if (p.active) {
            p.target = 0.0f;
            p.fadeStep = fadeStep;
            p.loopPending = false;
        }
    }

    Player& player = players_[freeSlot()];
    player.buffer = clip.buffer.get();
    player.name = clip.label.c_str();
    player.position = 0.0;
    player.step = clip.buffer->sampleRate() > 0.0 ? clip.buffer->sampleRate() / sampleRate_ : 1.0;
    player.looping = looping;
    player.loopPending = loopPending;
    player.phase = phase;
    player.level = 0.0f;
    player.target = 1.0f;
    player.fadeStep = fadeStep;
    player.active = true;
}

void PhasePlayerVoice::switchTo(EnginePhase phase) {
    phase_ = phase;
    const auto index = static_cast<std::size_t>(phase);
    const PhaseClips& clips = cfg_.phases[index];

    if (!clips.oneShot.empty()) {
        if (const Clip* c = clipFor(clips.oneShot)) {
            startClip(*c, false, !clips.loop.empty(), phase, cfg_.crossfadeSeconds);
            return;
        }
    }
    if (!clips.loop.empty()) {
        if (const Clip* c = clipFor(clips.loop)) {
            startClip(*c, true, false, phase, cfg_.crossfadeSeconds);
            return;
        }
    }
    // Nothing recorded for this phase (Off, or one not filled in): fade out.
    const float fadeStep = cfg_.crossfadeSeconds > 0.0
                               ? static_cast<float>(1.0 / (cfg_.crossfadeSeconds * sampleRate_))
                               : 1.0f;
    for (Player& p : players_) {
        if (p.active) {
            p.target = 0.0f;
            p.fadeStep = fadeStep;
            p.loopPending = false;
        }
    }
}

void PhasePlayerVoice::onParameters(const ParameterSnapshot& p) {
    if (!loaded_) return;
    const auto raw = static_cast<std::int32_t>(std::lround(p.phase));
    const EnginePhase wanted =
        (raw >= 0 && raw < kEnginePhaseCount) ? static_cast<EnginePhase>(raw) : EnginePhase::Off;
    if (wanted != phase_) switchTo(wanted);
}

void PhasePlayerVoice::process(float* const* out, std::uint32_t frames) {
    if (!loaded_ || frames == 0) return;

    // A one-shot that is about to end and has a loop behind it: bring the loop
    // in now, so the two overlap instead of leaving a hole.
    for (std::size_t i = 0; i < players_.size(); ++i) {
        Player& p = players_[i];
        if (!p.active || !p.loopPending || p.buffer == nullptr) continue;
        const double left = (static_cast<double>(p.buffer->frames()) - p.position) * p.step /
                            sampleRate_;
        if (left <= cfg_.loopJoinSeconds) {
            const PhaseClips& clips = cfg_.phases[static_cast<std::size_t>(p.phase)];
            const Clip* loop = clipFor(clips.loop);
            p.loopPending = false;
            if (loop != nullptr) startClip(*loop, true, false, p.phase, cfg_.loopJoinSeconds);
            break;  // startClip touched every player; nothing else to schedule now
        }
    }

    for (Player& p : players_) {
        if (!p.active || p.buffer == nullptr) continue;
        const float          phaseGain = cfg_.phases[static_cast<std::size_t>(p.phase)].gain;
        const float          gain = cfg_.gain * phaseGain;
        const std::int64_t   total = static_cast<std::int64_t>(p.buffer->frames());
        const std::uint32_t  channels = p.buffer->channels();

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
            // Equal power: two clips leaving and arriving at the same rate keep
            // the level steady even though they are unrelated recordings.
            const float env = static_cast<float>(std::sin(p.level * kHalfPi)) * gain;

            for (std::uint32_t c = 0; c < kBusChannels; ++c) {
                const float* data = p.buffer->channel(std::min(c, channels - 1));
                const std::int64_t next = (index + 1 < total) ? index + 1
                                          : (p.looping ? 0 : index);
                const float s = static_cast<float>(data[index] +
                                                   (data[next] - data[index]) * frac);
                out[c][i] += s * env;
            }
            p.position += p.step;
        }

        if (p.level <= 0.0f && p.target <= 0.0f) p.active = false;
    }
}

bool PhasePlayerVoice::isActive() const {
    for (const Player& p : players_) {
        if (p.active) return true;
    }
    return false;
}

const char* PhasePlayerVoice::playing() const noexcept {
    const Player* loudest = nullptr;
    for (const Player& p : players_) {
        if (p.active && (loudest == nullptr || p.level > loudest->level)) loudest = &p;
    }
    return loudest != nullptr ? loudest->name : "silent";
}

}  // namespace soundsys
