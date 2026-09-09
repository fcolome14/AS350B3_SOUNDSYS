// soundsys/TimeStretchReader.hpp - playback head that moves without changing pitch.
//
// Why this exists
// ---------------
// A turbine's tone is set by shaft speed: at NG = 50 % the engine sounds the
// same whether it took 10 s or 30 s to get there. Varispeed cannot reproduce
// that - reading the tape faster raises the pitch by the same factor, so a
// start simulated 1.6x faster than the recording comes out a major sixth sharp,
// and a start that pauses (a hung start, or simply idle) has its tone collapse
// as the head slows to a stop.
//
// So the position is driven by NG exactly as before, but the audio is rebuilt
// from overlapping grains that are always played at their recorded rate. The
// grain placement is WSOLA: each new grain is nudged within a small search
// window to the offset that best continues the previous one, which is what
// keeps a strongly tonal signal like turbine whine from phasing.
//
// Cost: one correlation search per grain (~50 per second), everything else is
// two multiply-adds per sample. No allocation after prepare().
#pragma once

#include <cstdint>
#include <vector>

#include "soundsys/PlaybackHead.hpp"

namespace soundsys {

struct TimeStretchConfig {
    double grainMs = 40.0;      // grain length; hop is half of this
    double searchMs = 5.0;      // WSOLA search radius
    double correlationMs = 10.0;  // how much of the previous grain to match
    int    correlationStride = 2;  // decimation of the correlation, for speed
};

class TimeStretchReader final : public IPlaybackHead {
public:
    TimeStretchReader() = default;
    explicit TimeStretchReader(TimeStretchConfig cfg) : cfg_(cfg) {}

    void prepare(const AudioBuffer* source, double outputSampleRate) override;
    void reset(double positionSeconds) override;
    void setLooping(bool looping) override { looping_ = looping; }
    void setSpeed(double speed) override;
    double speed() const override { return speed_; }
    double positionSeconds() const override;
    bool atEnd() const override { return atEnd_; }
    bool ready() const override { return source_ != nullptr && !source_->empty(); }
    double durationSeconds() const override {
        return source_ ? source_->durationSeconds() : 0.0;
    }
    std::uint32_t read(float* const* out, std::uint32_t channels, std::uint32_t frames) override;

private:
    void  buildNextGrain();
    double findBestOffset(double nominalStart) const;
    float sampleAt(std::uint32_t channel, double sourceFrame) const;

    TimeStretchConfig cfg_;

    const AudioBuffer* source_ = nullptr;
    double outputRate_ = 48000.0;
    double ratePerOutFrame_ = 1.0;  // source frames per output frame at pitch 1.0
    double position_ = 0.0;         // playback head, in source frames
    double speed_ = 1.0;
    bool   looping_ = false;
    bool   atEnd_ = false;

    std::uint32_t grainFrames_ = 0;  // output frames per grain
    std::uint32_t hopFrames_ = 0;    // output frames between grains (grain / 2)
    std::uint32_t searchFrames_ = 0;
    std::uint32_t correlationFrames_ = 0;
    std::uint32_t channels_ = 0;

    std::vector<float> window_;   // Hann, grainFrames_ long
    std::vector<float> ready_;    // channels_ * hopFrames_, the block being emitted
    std::vector<float> tail_;     // channels_ * hopFrames_, second half of last grain
    std::vector<float> reference_;  // correlationFrames_, what the next grain should continue
    std::uint32_t      emitted_ = 0;  // frames already taken out of ready_
    bool               primed_ = false;
};

}  // namespace soundsys
