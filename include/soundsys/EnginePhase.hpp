// soundsys/EnginePhase.hpp - what the aircraft is doing, as far as sound cares.
//
// The phases a pilot would name, in the order a flight goes through them. The
// simulator decides which one is current (SimLinkAdapter derives it from the
// VEMD's state); the sound side only ever reacts to it changing.
#pragma once

#include <cstdint>

namespace soundsys {

enum class EnginePhase : std::int32_t {
    Off = 0,     // nothing running
    Start,       // starter engaged through to a stable ground idle
    Idle,        // ground idle, twist grip in IDLE
    Flight,      // twist grip in FLIGHT, on the ground or hovering
    Takeoff,     // power up and lift off
    Cruise,      // steady forward flight
    Landing,     // approach and touchdown
    Shutdown,    // fuel off, running down
    RotorBrake,  // brake applied, the last of the noise
    Count
};

inline constexpr std::int32_t kEnginePhaseCount = static_cast<std::int32_t>(EnginePhase::Count);

inline const char* phaseName(EnginePhase p) {
    switch (p) {
        case EnginePhase::Start:      return "start";
        case EnginePhase::Idle:       return "idle";
        case EnginePhase::Flight:     return "flight";
        case EnginePhase::Takeoff:    return "takeoff";
        case EnginePhase::Cruise:     return "cruise";
        case EnginePhase::Landing:    return "landing";
        case EnginePhase::Shutdown:   return "shutdown";
        case EnginePhase::RotorBrake: return "rotor_brake";
        default:                      return "off";
    }
}

// Name to phase; returns false for anything unknown.
inline bool phaseFromName(const char* name, EnginePhase& out) {
    for (std::int32_t i = 0; i < kEnginePhaseCount; ++i) {
        const auto p = static_cast<EnginePhase>(i);
        const char* n = phaseName(p);
        const char* a = name;
        while (*a && *n && *a == *n) { ++a; ++n; }
        if (*a == '\0' && *n == '\0') {
            out = p;
            return true;
        }
    }
    return false;
}

}  // namespace soundsys
