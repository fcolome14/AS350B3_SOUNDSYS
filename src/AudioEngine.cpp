#include "soundsys/AudioEngine.hpp"

#include <algorithm>
#include <cstring>

namespace soundsys {

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() = default;

ISoundVoice* AudioEngine::addVoice(std::unique_ptr<ISoundVoice> voice) {
    if (!voice) return nullptr;
    voice->setEventSink(this);
    ISoundVoice* raw = voice.get();
    voices_.push_back(std::move(voice));
    if (prepared_) raw->prepare(format_);
    return raw;
}

void AudioEngine::prepare(const AudioFormat& fmt) {
    format_ = fmt;
    format_.channels = kBusChannels;
    format_.maxBlockFrames = std::min(fmt.maxBlockFrames, kMaxBlockFrames);

    busStorage_.assign(static_cast<std::size_t>(kBusChannels) * format_.maxBlockFrames, 0.0f);
    for (std::uint32_t c = 0; c < kBusChannels; ++c) {
        bus_[c] = busStorage_.data() + static_cast<std::size_t>(c) * format_.maxBlockFrames;
    }

    limiter_.prepare(format_.sampleRate);
    for (auto& voice : voices_) voice->prepare(format_);
    prepared_ = true;
}

bool AudioEngine::pushEvent(EventType type, float value) {
    SoundEvent ev;
    ev.type = type;
    ev.value = value;
    ev.frameStamp = frameCounter_;
    if (!queue_.push(ev)) {
        ++dropped_;  // never block the simulator on a full queue
        return false;
    }
    return true;
}

void AudioEngine::emit(const SoundEvent& ev) {
    // Audio thread only. Collected now, delivered at the top of the next block,
    // so a voice cannot recurse into the fan-out it is being called from.
    if (pendingCount_ < kInternalEventCapacity) {
        internalPending_[pendingCount_++] = ev;
    } else {
        ++dropped_;
    }
}

void AudioEngine::render(float* out, std::uint32_t frames, std::uint32_t channels) {
    if (!prepared_ || out == nullptr || channels == 0) return;

    // Chop the device callback into blocks the voices were prepared for.
    std::uint32_t done = 0;
    while (done < frames) {
        const std::uint32_t block = std::min(frames - done, format_.maxBlockFrames);
        renderBlock(block);

        float* dst = out + static_cast<std::size_t>(done) * channels;
        for (std::uint32_t i = 0; i < block; ++i) {
            for (std::uint32_t c = 0; c < channels; ++c) {
                // Mono devices get the left bus; anything above stereo repeats it.
                dst[i * channels + c] = bus_[std::min<std::uint32_t>(c, kBusChannels - 1)][i];
            }
        }
        done += block;
    }
}

void AudioEngine::renderBlock(std::uint32_t frames) {
    for (std::uint32_t c = 0; c < kBusChannels; ++c) {
        std::fill(bus_[c], bus_[c] + frames, 0.0f);
    }

    // 1. Discrete input: the simulator's queue first, then whatever the voices
    //    published for each other during the previous block.
    SoundEvent ev;
    while (queue_.pop(ev)) {
        for (auto& voice : voices_) voice->onEvent(ev);
    }
    internalEvents_.swap(internalPending_);
    internalCount_ = pendingCount_;
    pendingCount_ = 0;
    for (std::size_t i = 0; i < internalCount_; ++i) {
        for (auto& voice : voices_) voice->onEvent(internalEvents_[i]);
    }
    internalCount_ = 0;

    // 2. Continuous input: one coherent snapshot for the whole block.
    const double blockSeconds =
        format_.sampleRate > 0.0 ? static_cast<double>(frames) / format_.sampleRate : 0.0;
    const ParameterSnapshot snapshot = params_.snapshot(blockSeconds);

    // 3. Mix. Voices add into the bus; inactive ones cost a virtual call.
    for (auto& voice : voices_) {
        voice->onParameters(snapshot);
        if (voice->isActive()) voice->process(bus_, frames);
    }

    // 4. Master gain, then the limiter has the last word.
    const float master = snapshot.masterGain;
    if (master != 1.0f) {
        for (std::uint32_t c = 0; c < kBusChannels; ++c) {
            for (std::uint32_t i = 0; i < frames; ++i) bus_[c][i] *= master;
        }
    }
    limiter_.process(bus_, kBusChannels, frames);

    frameCounter_ += frames;
}

}  // namespace soundsys
