#include "soundsys/link/SimLinkAdapter.hpp"

#include "soundsys/AudioEngine.hpp"

namespace soundsys::link {

using simlink::EngineState;
namespace wireflags = simlink::flags;

void EngineSink::event(EventType type, float value) { engine_.pushEvent(type, value); }

void EngineSink::parameter(ParamId id, float value) { engine_.setParameter(id, value); }

SimLinkAdapter::SimLinkAdapter(ILinkSink& sink, LinkConfig cfg) : sink_(sink), cfg_(cfg) {}

void SimLinkAdapter::onDatagram(const void* data, std::size_t size, double localNow) {
    simlink::EnginePacket p;
    if (!simlink::decode(static_cast<const std::uint8_t*>(data), size, p)) {
        ++stats_.malformed;
        return;
    }
    onPacket(p, localNow);
}

void SimLinkAdapter::resetTimeline() {
    haveLast_ = false;
    head_ = 0;
    count_ = 0;
    offsetValid_ = false;
}

void SimLinkAdapter::onPacket(const simlink::EnginePacket& p, double localNow) {
    ++stats_.received;

    if (haveLast_) {
        // A sender clock that jumps back is a new VEMD process, not a late packet.
        if (p.simTime + 1.0 < last_.simTime) {
            if (last_.state != EngineState::Off) sink_.event(EventType::EngineShutdown, 0.0f);
            resetTimeline();
        } else if (static_cast<std::int32_t>(p.seq - last_.seq) <= 0 || p.simTime <= last_.simTime) {
            ++stats_.stale;
            return;
        }
    }

    // Local-minus-sender clock offset. The packets that arrive fastest carry
    // the truest offset, so a new minimum is taken at once, while larger values
    // only pull it up slowly (clock drift). A packet held up by jitter then
    // barely moves the playback instant.
    const double offset = localNow - p.simTime;
    if (!offsetValid_ || offset < clockOffset_) {
        clockOffset_ = offset;
    } else {
        clockOffset_ += (offset - clockOffset_) * 0.02;
    }
    offsetValid_ = true;

    history_[head_] = Sample{p.simTime,     p.ngPercent,     p.nrPercent,
                             p.t4Celsius,   p.torquePercent, p.collectivePercent};
    head_ = (head_ + 1) % kHistory;
    if (count_ < kHistory) ++count_;

    deriveEvents(p);

    last_ = p;
    haveLast_ = true;
    lastPacketLocal_ = localNow;
    ++stats_.accepted;
    stats_.lastSeq = p.seq;
    stats_.linkUp = true;
    stats_.lastPacketAge = 0.0;
    stats_.state = p.state;
    stats_.clockOffset = clockOffset_;
}

void SimLinkAdapter::deriveEvents(const simlink::EnginePacket& p) {
    const bool running = p.state != EngineState::Off;
    const bool wasRunning = haveLast_ && last_.state != EngineState::Off;

    if (!running) {
        if (wasRunning) sink_.event(EventType::EngineShutdown, p.ngPercent);
        return;
    }

    // A new start: first thing heard, engine was off, or the counter moved.
    const bool fresh = !haveLast_ || last_.state == EngineState::Off ||
                       p.startCount != last_.startCount;
    if (fresh) {
        // Restarted without an Off in between (stop + start inside one sensor
        // period): end the previous run first so its voices let go.
        if (wasRunning) sink_.event(EventType::EngineShutdown, p.ngPercent);
        // Joining a start already under way is fine - the start voice enters
        // its recording at the current NG. Joining at idle skips straight to
        // the idle voices.
        sink_.event(p.state == EngineState::Starting ? EventType::EngineStartCommand
                                                     : EventType::StartSequenceComplete,
                    p.ngPercent);
    } else if (last_.state == EngineState::Starting && p.state == EngineState::GroundIdle) {
        sink_.event(EventType::IdleReached, p.ngPercent);
    }

    const bool genNow = p.has(wireflags::kGeneratorOnline);
    const bool genBefore = !fresh && last_.has(wireflags::kGeneratorOnline);
    if (genNow && !genBefore) sink_.event(EventType::GeneratorOnline, p.ngPercent);
}

void SimLinkAdapter::update(double localNow) {
    if (stats_.linkUp) {
        stats_.lastPacketAge = localNow - lastPacketLocal_;
        if (stats_.lastPacketAge > cfg_.linkTimeout) {
            // The VEMD went quiet: better silence than an engine idling forever.
            stats_.linkUp = false;
            if (haveLast_ && last_.state != EngineState::Off) {
                sink_.event(EventType::EngineShutdown, 0.0f);
            }
            stats_.state = EngineState::Off;
            resetTimeline();
            return;
        }
    }
    if (count_ == 0) return;

    const Sample s = sampleAt(localNow - clockOffset_ - cfg_.playoutDelay);
    sink_.parameter(ParamId::NgPercent, s.ng);
    sink_.parameter(ParamId::NrPercent, s.nr);
    sink_.parameter(ParamId::T4Celsius, s.t4);
    sink_.parameter(ParamId::TorquePercent, s.torque);
    sink_.parameter(ParamId::CollectivePercent, s.collective);
    stats_.ng = s.ng;
}

SimLinkAdapter::Sample SimLinkAdapter::sampleAt(double t) const {
    const std::size_t oldest = (head_ + kHistory - count_) % kHistory;
    const Sample&     first = history_[oldest];
    if (t <= first.t) return first;

    for (std::size_t k = 1; k < count_; ++k) {
        const Sample& a = history_[(oldest + k - 1) % kHistory];
        const Sample& b = history_[(oldest + k) % kHistory];
        if (t <= b.t) {
            const double span = b.t - a.t;
            const float  u = span > 0.0 ? static_cast<float>((t - a.t) / span) : 1.0f;
            const auto   mix = [u](float x, float y) { return x + (y - x) * u; };
            return Sample{t, mix(a.ng, b.ng), mix(a.nr, b.nr), mix(a.t4, b.t4),
                          mix(a.torque, b.torque), mix(a.collective, b.collective)};
        }
    }
    // Past the newest sample: hold it. Extrapolating would overshoot every time
    // the engine stops accelerating.
    return history_[(head_ + kHistory - 1) % kHistory];
}

}  // namespace soundsys::link
