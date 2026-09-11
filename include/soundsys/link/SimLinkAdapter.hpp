// soundsys/link/SimLinkAdapter.hpp - VEMD engine packets in, engine inputs out.
//
// The AudioEngine knows nothing about networks. This is the one piece that
// does, and it has three jobs:
//
//  1. Re-timing. Packets carry the sender's clock at each 20 Hz sensor sample.
//     Handed over as they arrive, NG would reach the voices as a 20 Hz
//     staircase, and the start voice reads a stair as "stop, lurch, stop" (the
//     zero-speed / 6x flip-flop measured on the bridged start demo). So the
//     samples are played back a fixed delay behind the sender's clock and
//     interpolated: NG arrives as a continuous curve, ~60 ms late - less than a
//     third of one refresh of the 5 Hz digit the pilot reads.
//  2. Events from state. A START is not a message that can go missing: it is
//     the start counter changing between two packets. Same for idle, generator
//     and shutdown. A lost datagram costs 50 ms of resolution, never an event.
//  3. Watchdog. If the VEMD goes quiet the engine is shut down, so the voices
//     fade out instead of idling forever.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "simlink/EnginePacket.hpp"
#include "soundsys/Events.hpp"

namespace soundsys {
class AudioEngine;
}

namespace soundsys::link {

// Where the adapter's output goes. The engine in production, a recorder in tests.
class ILinkSink {
public:
    virtual ~ILinkSink() = default;
    virtual void event(EventType type, float value) = 0;
    virtual void parameter(ParamId id, float value) = 0;
};

class EngineSink final : public ILinkSink {
public:
    explicit EngineSink(AudioEngine& engine) : engine_(engine) {}
    void event(EventType type, float value) override;
    void parameter(ParamId id, float value) override;

private:
    AudioEngine& engine_;
};

struct LinkConfig {
    // How far behind the sender's clock samples are played back. Must exceed
    // one sensor period (50 ms) so there is always a sample on each side of the
    // playback instant; the rest absorbs network jitter.
    double playoutDelay = 0.060;
    // Silence from the VEMD longer than this shuts the engine down.
    double linkTimeout = 1.0;
};

struct LinkStats {
    std::uint64_t received = 0;
    std::uint64_t accepted = 0;
    std::uint64_t stale = 0;      // duplicates or overtaken on the way
    std::uint64_t malformed = 0;  // not a simlink engine packet
    std::uint32_t lastSeq = 0;
    bool          linkUp = false;
    double        lastPacketAge = 0.0;  // s since the newest packet arrived
    double        clockOffset = 0.0;    // local clock minus sender clock, s
    float         ng = 0.0f;            // NG last handed to the engine
    simlink::EngineState state = simlink::EngineState::Off;
};

class SimLinkAdapter {
public:
    explicit SimLinkAdapter(ILinkSink& sink, LinkConfig cfg = {});

    // A raw datagram as it came off the socket. `localNow` is the listener's
    // monotonic clock in seconds; any origin, as long as it is the same one
    // passed to update().
    void onDatagram(const void* data, std::size_t size, double localNow);
    void onPacket(const simlink::EnginePacket& packet, double localNow);

    // Call every few milliseconds, packets or not: pushes the interpolated
    // parameters and runs the watchdog.
    void update(double localNow);

    const LinkStats& stats() const { return stats_; }

private:
    struct Sample {
        double t;
        float  ng, nr, t4, torque, collective;
    };

    void   deriveEvents(const simlink::EnginePacket& p);
    void   resetTimeline();
    Sample sampleAt(double senderTime) const;

    ILinkSink& sink_;
    LinkConfig cfg_;
    LinkStats  stats_;

    static constexpr std::size_t kHistory = 32;  // 1.6 s at 20 Hz
    std::array<Sample, kHistory> history_{};
    std::size_t head_ = 0;   // next slot to write
    std::size_t count_ = 0;

    simlink::EnginePacket last_{};
    bool   haveLast_ = false;
    double lastPacketLocal_ = 0.0;
    double clockOffset_ = 0.0;
    bool   offsetValid_ = false;
};

}  // namespace soundsys::link
