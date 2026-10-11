// soundsys/voices/AvionicsBedVoice.hpp - the cockpit bed that runs under every phase.
//
// The avionics fan is not a phase of flight: it belongs to the VEMD being
// powered, not to what the engine is doing. It starts at MASTER ON with the
// engine still stopped, holds under start, idle, flight and shutdown alike, and
// stops only when the display goes dark (or the link is lost).
//
// Two clips, both played as recorded: a one-shot (the master switch and the fan
// running up) handing over to a seamless loop (the fan, steady). Nothing is
// pitched or stretched - the fan does not care about NG.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "soundsys/AudioBuffer.hpp"
#include "soundsys/ISoundVoice.hpp"

namespace soundsys {

struct AvionicsBedConfig {
    std::string oneShot;  // MASTER ON and the fan running up (may be empty)
    std::string loop;     // the fan, steady (may be empty)

    float  gain = 1.0f;
    double fadeInSeconds = 0.05;   // the master switch should still sound like a switch
    double loopJoinSeconds = 0.4;  // one-shot into its own loop
    double fadeOutSeconds = 0.5;   // when the display goes dark

    // The display coming up and going down. The defaults are what soundsys_host
    // emits when the VEMD link appears and when it is lost.
    EventType startOn = EventType::VemdOn;
    EventType stopOn = EventType::VemdOff;
};

class AvionicsBedVoice final : public ISoundVoice {
public:
    explicit AvionicsBedVoice(AvionicsBedConfig cfg);
    ~AvionicsBedVoice() override;

    // Loads both clips. Control thread, before the device starts.
    bool load(std::string* error = nullptr);

    const char* name() const override { return "AvionicsBedVoice"; }
    void prepare(const AudioFormat& fmt) override;
    void onEvent(const SoundEvent& ev) override;
    void onParameters(const ParameterSnapshot& p) override;
    void process(float* const* out, std::uint32_t frames) override;
    bool isActive() const override;

    // --- diagnostics ------------------------------------------------------
    bool running() const noexcept { return running_; }

private:
    struct Player {
        const AudioBuffer* buffer = nullptr;
        double             position = 0.0;
        double             step = 1.0;
        bool               looping = false;
        bool               loopPending = false;
        float              level = 0.0f;
        float              target = 0.0f;
        float              fadeStep = 1.0f;
        bool               active = false;
    };

    void startClip(const AudioBuffer& buffer, bool looping, bool loopPending, double fadeSeconds);
    void fadeOutAll(double fadeSeconds);

    AvionicsBedConfig            cfg_;
    std::shared_ptr<AudioBuffer> oneShot_;
    std::shared_ptr<AudioBuffer> loop_;
    std::array<Player, 3>        players_{};
    double                       sampleRate_ = 48000.0;
    bool                         loaded_ = false;
    bool                         running_ = false;
};

}  // namespace soundsys
