// soundsys/voices/PhasePlayerVoice.hpp - one recording per phase, played as recorded.
//
// The simplest thing that can sound right: for each phase of flight, the real
// recording of that phase. A one-shot for the phases that go somewhere (the
// start, the twist grip to FLIGHT, takeoff, shutdown) and a seamless loop for
// the phases that hold still (ground idle, flight idle, cruise), which keeps
// playing for as long as the aircraft stays there.
//
// Nothing is stretched, resampled or pitched: every sample comes out as it went
// in, so there is no artefact to hear. The price is that inside a phase the
// sound no longer follows NG - a start that takes 5 s longer than the recording
// simply reaches idle 5 s later on the gauges than in the sound, which is far
// less noticeable than any processing artefact.
//
// A phase's parts play in order: one-shot, then its loop, crossfaded. Changing
// phase crossfades to the new phase's first part from wherever the old one was.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "soundsys/AudioBuffer.hpp"
#include "soundsys/EnginePhase.hpp"
#include "soundsys/ISoundVoice.hpp"

namespace soundsys {

struct PhaseClips {
    std::string oneShot;  // played once on entering the phase (may be empty)
    std::string loop;     // played for as long as the phase lasts (may be empty)
    float       gain = 1.0f;
};

struct PhasePlayerConfig {
    std::array<PhaseClips, static_cast<std::size_t>(kEnginePhaseCount)> phases{};

    double crossfadeSeconds = 1.0;   // between phases
    double loopJoinSeconds = 0.6;    // one-shot into its own loop
    float  gain = 1.0f;
};

class PhasePlayerVoice final : public ISoundVoice {
public:
    explicit PhasePlayerVoice(PhasePlayerConfig cfg);
    ~PhasePlayerVoice() override;

    // Loads every clip named in the config (each file once). Control thread.
    bool load(std::string* error = nullptr);

    const char* name() const override { return "PhasePlayerVoice"; }
    void prepare(const AudioFormat& fmt) override;
    void onEvent(const SoundEvent& ev) override;
    void onParameters(const ParameterSnapshot& p) override;
    void process(float* const* out, std::uint32_t frames) override;
    bool isActive() const override;

    // --- diagnostics ------------------------------------------------------
    EnginePhase phase() const noexcept { return phase_; }
    // What is sounding right now, e.g. "start" or "idle_loop".
    const char* playing() const noexcept;

private:
    struct Clip {
        std::shared_ptr<AudioBuffer> buffer;
        std::string                  path;
        std::string                  label;  // file name only, for the status line
    };
    // One playing copy of a clip. Four slots is more than a crossfade can ever
    // need (outgoing phase, its pending loop, incoming phase, its loop).
    struct Player {
        const AudioBuffer* buffer = nullptr;
        const char*        name = "";
        double             position = 0.0;  // in source frames
        double             step = 1.0;      // source frames per output frame
        bool               looping = false;
        bool               loopPending = false;  // a loop follows this one-shot
        EnginePhase        phase = EnginePhase::Off;
        float              level = 0.0f;
        float              target = 0.0f;
        float              fadeStep = 1.0f;
        bool               active = false;
    };

    void startClip(const Clip& clip, bool looping, bool loopPending, EnginePhase phase,
                   double fadeSeconds);
    void switchTo(EnginePhase phase);
    const Clip* clipFor(const std::string& path) const;
    std::size_t freeSlot();

    PhasePlayerConfig cfg_;
    std::vector<Clip> clips_;  // one per distinct file
    std::array<Player, 4> players_{};

    EnginePhase phase_ = EnginePhase::Off;
    bool        loaded_ = false;
    double      sampleRate_ = 48000.0;
};

}  // namespace soundsys
