// soundsys/VarispeedReader.hpp - variable-rate playback head over an AudioBuffer.
//
// This is deliberately NOT a time-stretcher. Music time-stretching preserves
// pitch while changing duration; playing a recording faster or slower, like a
// turntable, couples the two - which is the cheap operation, and the right one
// whenever the playback rate stays close to 1 (see docs/sync.md for where that
// assumption stops holding).
//
// Backed by libsamplerate in variable-ratio mode when available (the ratio may
// change every block without clicks), with a Catmull-Rom interpolating fallback
// so the module still builds and runs with no third-party code at all.
//
// The backend lives behind a pointer on purpose: which one is compiled in is a
// build option, and a header whose member layout changed with a build option
// would silently corrupt memory in any translation unit compiled without the
// same options.
#pragma once

#include <cstdint>
#include <memory>

#include "soundsys/AudioBuffer.hpp"
#include "soundsys/PlaybackHead.hpp"

namespace soundsys {

class VarispeedReader final : public IPlaybackHead {
public:
    VarispeedReader();
    ~VarispeedReader() override;
    VarispeedReader(const VarispeedReader&) = delete;
    VarispeedReader& operator=(const VarispeedReader&) = delete;

    // `outputSampleRate` is the engine rate; the source's own rate is taken from
    // the buffer, so `speed` below is expressed in real-world time, not samples.
    void prepare(const AudioBuffer* source, double outputSampleRate) override;

    void reset(double positionSeconds = 0.0) override;
    void setLooping(bool looping) override { looping_ = looping; }

    // Playback speed as a multiple of real time: 1.0 = as recorded, 2.0 = twice
    // as fast and an octave up. Clamped to the resampler's usable range.
    void setSpeed(double speed) override;
    double speed() const override { return speed_; }

    // Current playback head, in seconds into the recording. Advanced by exactly
    // the number of source frames actually consumed, so it stays truthful across
    // ratio changes - that is what lets a voice close a feedback loop around it.
    double positionSeconds() const override;

    bool atEnd() const override { return atEnd_; }
    bool ready() const override { return source_ != nullptr && !source_->empty(); }
    double durationSeconds() const override {
        return source_ ? source_->durationSeconds() : 0.0;
    }

    // Renders `frames` frames into planar out[0..channels-1] (overwrites, does
    // not add). Returns frames actually produced; a short read means the source
    // ran out and looping is off.
    std::uint32_t read(float* const* out, std::uint32_t channels, std::uint32_t frames) override;

    static bool usingLibsamplerate() noexcept;

private:
    struct Backend;

    std::uint32_t readInterpolated(float* const* out, std::uint32_t channels,
                                   std::uint32_t frames);
    std::uint32_t readResampled(float* const* out, std::uint32_t channels, std::uint32_t frames);
    void refillInput();

    std::unique_ptr<Backend> backend_;

    const AudioBuffer* source_ = nullptr;
    double outputRate_ = 48000.0;
    double ratePerOutFrame_ = 1.0;  // source frames per output frame at speed 1.0
    double cursor_ = 0.0;           // frames of source actually consumed
    double speed_ = 1.0;
    bool   looping_ = false;
    bool   atEnd_ = false;
};

}  // namespace soundsys
