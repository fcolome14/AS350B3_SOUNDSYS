// soundsys/AudioBuffer.hpp - a decoded, de-interleaved audio asset in RAM.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace soundsys {

// Planar float sample storage. Assets are small enough (a start-up recording is
// seconds, not minutes) that streaming from disk buys nothing.
class AudioBuffer {
public:
    bool loadWav(const std::string& path, std::string* error = nullptr);

    // Build in memory (tests, synthesised placeholders).
    void assign(std::vector<std::vector<float>> channels, double sampleRate);

    std::uint32_t channels() const noexcept {
        return static_cast<std::uint32_t>(data_.size());
    }
    std::uint64_t frames() const noexcept { return frames_; }
    double sampleRate() const noexcept { return sampleRate_; }
    double durationSeconds() const noexcept {
        return sampleRate_ > 0.0 ? static_cast<double>(frames_) / sampleRate_ : 0.0;
    }
    bool empty() const noexcept { return frames_ == 0; }

    // Planar access. channel is clamped to the last available channel, so a mono
    // asset can be read as stereo without a branch at the call site.
    const float* channel(std::uint32_t index) const noexcept {
        if (data_.empty()) return nullptr;
        if (index >= data_.size()) index = static_cast<std::uint32_t>(data_.size() - 1);
        return data_[index].data();
    }

private:
    std::vector<std::vector<float>> data_;
    std::uint64_t frames_ = 0;
    double        sampleRate_ = 0.0;
};

// Writes interleaved float samples as a 32-bit float WAV. Used by the offline
// renderer and by the tests; never called from the audio thread.
bool writeWavFloat32(const std::string& path,
                     const float*       interleaved,
                     std::uint64_t      frames,
                     std::uint32_t      channels,
                     double             sampleRate,
                     std::string*       error = nullptr);

} // namespace soundsys
