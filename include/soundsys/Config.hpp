// soundsys/Config.hpp - compile-time configuration and small shared types.
#pragma once

#include <cstddef>
#include <cstdint>

namespace soundsys {

// Maximum block size the engine will ever hand to a voice. Device callbacks are
// chopped into chunks of at most this many frames so voices can use fixed-size
// stack scratch buffers.
inline constexpr std::uint32_t kMaxBlockFrames = 512;

// The engine mixes on a stereo bus. Voices are handed a planar {L, R} pair.
inline constexpr std::uint32_t kBusChannels = 2;

// Capacity of the lock-free event queue (power of two).
inline constexpr std::size_t kEventQueueCapacity = 256;

// Audio format the engine is currently running at. Handed to voices in prepare()
// so they can pre-compute per-sample increments.
struct AudioFormat {
    double   sampleRate = 48000.0;
    uint32_t maxBlockFrames = kMaxBlockFrames;
    uint32_t channels = kBusChannels;
};

} // namespace soundsys
