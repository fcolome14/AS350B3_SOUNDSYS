// soundsys/AudioEngine.hpp - the mixer. Knows nothing about helicopters.
//
// Threading contract
// ------------------
//   Control thread (simulator)      Audio thread (device callback)
//   ---------------------------     ------------------------------
//   addVoice()      (before start)  render()
//   pushEvent()     ->  SPSC queue  ->  drained, fanned out to voices
//   setParameter()  ->  atomics     ->  snapshotted once per block
//
// No mutex is ever taken on the audio side, nothing is allocated on the audio
// side, and a full event queue drops rather than blocks the simulator.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "soundsys/Config.hpp"
#include "soundsys/Events.hpp"
#include "soundsys/ISoundVoice.hpp"
#include "soundsys/Limiter.hpp"
#include "soundsys/ParameterStore.hpp"
#include "soundsys/SpscQueue.hpp"

namespace soundsys {

class AudioEngine final : public IEventSink {
public:
    AudioEngine();
    ~AudioEngine() override;

    // --- control thread ---------------------------------------------------

    // Register voices before prepare(). The engine owns them for its lifetime.
    ISoundVoice* addVoice(std::unique_ptr<ISoundVoice> voice);

    // Fixes the format and prepares every registered voice.
    void prepare(const AudioFormat& fmt);

    const AudioFormat& format() const noexcept { return format_; }

    // Thread-safe, non-blocking. Returns false if the queue was full.
    bool pushEvent(EventType type, float value = 0.0f);

    // Thread-safe, non-blocking, latest value wins.
    void setParameter(ParamId id, float value) noexcept { params_.set(id, value); }
    float parameter(ParamId id) const noexcept { return params_.get(id); }

    // --- audio thread -----------------------------------------------------

    // Fills `out` with `frames` frames of interleaved audio for `channels`
    // channels. Safe to call with channels 1 or 2; the internal bus is stereo.
    void render(float* out, std::uint32_t frames, std::uint32_t channels);

    // IEventSink - voices publishing to other voices, audio thread only.
    void emit(const SoundEvent& ev) override;

    // --- diagnostics ------------------------------------------------------
    std::uint64_t framesRendered() const noexcept { return frameCounter_; }
    std::uint64_t droppedEvents() const noexcept { return dropped_; }
    std::size_t   voiceCount() const noexcept { return voices_.size(); }
    float         limiterGain() const noexcept { return limiter_.gain(); }

private:
    void renderBlock(std::uint32_t frames);

    std::vector<std::unique_ptr<ISoundVoice>> voices_;
    ParameterStore                            params_;
    SpscQueue<SoundEvent, kEventQueueCapacity> queue_;

    // Events emitted by voices during a block, delivered at the top of the next.
    // Fixed storage: the audio thread must not allocate, so a burst beyond this
    // is dropped and counted rather than growing a vector under the callback.
    static constexpr std::size_t kInternalEventCapacity = 32;
    std::array<SoundEvent, kInternalEventCapacity> internalEvents_{};
    std::array<SoundEvent, kInternalEventCapacity> internalPending_{};
    std::size_t internalCount_ = 0;
    std::size_t pendingCount_ = 0;

    AudioFormat  format_;
    Limiter      limiter_;

    std::vector<float> busStorage_;      // kBusChannels * maxBlockFrames
    float*             bus_[kBusChannels]{};

    std::uint64_t frameCounter_ = 0;
    std::uint64_t dropped_ = 0;
    bool          prepared_ = false;
};

} // namespace soundsys
