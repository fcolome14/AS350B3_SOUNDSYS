// soundsys/Events.hpp - the discrete/continuous input contract of the module.
//
// Everything the simulator tells the audio module arrives through one of two
// channels:
//   * Events     - discrete, timestamped, delivered exactly once (SPSC queue).
//   * Parameters - continuous, latest-value-wins (atomic store per id).
//
// Neither channel ever blocks the simulator thread.
#pragma once

#include <cstdint>

namespace soundsys {

enum class EventType : std::uint16_t {
    None = 0,

    // --- Engine start sequence -------------------------------------------
    EngineStartCommand,   // starter engaged (fuel flow lever / start button)
    EngineStartAbort,     // aborted start, spool-down
    LightOff,             // combustion detected (T4 rise)
    IgnitionOff,          // igniters cut
    GeneratorOnline,      // GEN contactor closed
    IdleReached,          // NG stabilised at ground idle
    EngineShutdown,

    // --- Emitted by voices, consumed by other voices ----------------------
    StartSequenceComplete, // TurbineStartVoice finished -> idle loop takes over

    // --- Misc airframe ----------------------------------------------------
    RotorBrakeOn,
    RotorBrakeOff,
    WarningHornOn,
    WarningHornOff,

    Count
};

// A discrete event. `value` carries an optional payload (e.g. severity, index);
// `frameStamp` is the engine frame counter at push time, useful for logging and
// for voices that want to know how stale an event is.
struct SoundEvent {
    EventType     type = EventType::None;
    float         value = 0.0f;
    std::uint64_t frameStamp = 0;
};

// Continuous parameters. Add ids here; ParameterStore sizes itself from Count.
enum class ParamId : std::uint16_t {
    NgPercent = 0,   // gas generator speed, %
    NrPercent,       // rotor speed, %
    N2Percent,       // power turbine speed, %
    T4Celsius,       // turbine outlet temperature
    TorquePercent,
    CollectivePercent,
    AirspeedKt,
    MasterGain,      // linear, 0..1+
    // What the aircraft is doing (soundsys/EnginePhase.hpp), carried as a float
    // so it travels the same latest-value-wins path as everything else: a voice
    // that misses the moment it changed still ends up in the right phase.
    EnginePhase,
    Count
};

} // namespace soundsys
