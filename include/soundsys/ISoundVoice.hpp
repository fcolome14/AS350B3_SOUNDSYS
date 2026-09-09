// soundsys/ISoundVoice.hpp - the only thing a new sound has to implement.
//
// Adding a sound to the simulator = write one class implementing this interface
// and register it with AudioEngine::addVoice(). Nothing else in the module
// changes, and nothing in the module knows what a turbine is.
#pragma once

#include <cstdint>

#include "soundsys/Config.hpp"
#include "soundsys/Events.hpp"
#include "soundsys/ParameterStore.hpp"

namespace soundsys {

// Back-channel a voice can use to publish events of its own (e.g. the start
// voice announcing that the idle loop should take over). Delivered to every
// voice on the next block, on the audio thread.
class IEventSink {
public:
    virtual ~IEventSink() = default;
    virtual void emit(const SoundEvent& ev) = 0;
};

class ISoundVoice {
public:
    virtual ~ISoundVoice() = default;

    // Human readable id, for logs and debug overlays.
    virtual const char* name() const = 0;

    // Called once from the control thread before the device starts. Assets are
    // loaded here, not in process().
    virtual void prepare(const AudioFormat& fmt) = 0;

    // Audio thread. Discrete input, delivered before the block it applies to.
    virtual void onEvent(const SoundEvent& ev) = 0;

    // Audio thread. Continuous input, once per block, before process().
    virtual void onParameters(const ParameterSnapshot& p) = 0;

    // Audio thread. ADDS its output into `out` (planar, out[0]=L, out[1]=R).
    // The buffers are not cleared for you - a silent voice writes nothing.
    virtual void process(float* const* out, std::uint32_t frames) = 0;

    // False when the voice has nothing left to render; the engine then skips it.
    virtual bool isActive() const = 0;

    // Optional: called once after construction so the voice can emit events.
    virtual void setEventSink(IEventSink* sink) { (void)sink; }
};

} // namespace soundsys
