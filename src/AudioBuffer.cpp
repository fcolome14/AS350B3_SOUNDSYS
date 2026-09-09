#include "soundsys/AudioBuffer.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

namespace soundsys {

bool AudioBuffer::loadWav(const std::string& path, std::string* error) {
    drwav wav;
    if (!drwav_init_file(&wav, path.c_str(), nullptr)) {
        if (error) *error = "could not open WAV file: " + path;
        return false;
    }

    const std::uint32_t channels = wav.channels;
    const std::uint64_t frames = wav.totalPCMFrameCount;
    if (channels == 0 || frames == 0) {
        drwav_uninit(&wav);
        if (error) *error = "empty WAV file: " + path;
        return false;
    }

    std::vector<float> interleaved(static_cast<std::size_t>(frames) * channels);
    const drwav_uint64 read = drwav_read_pcm_frames_f32(&wav, frames, interleaved.data());
    const double rate = static_cast<double>(wav.sampleRate);
    drwav_uninit(&wav);

    if (read == 0) {
        if (error) *error = "could not decode WAV file: " + path;
        return false;
    }

    // De-interleave once, at load time, so the playback head never has to.
    std::vector<std::vector<float>> planar(channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        planar[c].resize(static_cast<std::size_t>(read));
        for (std::uint64_t i = 0; i < read; ++i) {
            planar[c][static_cast<std::size_t>(i)] =
                interleaved[static_cast<std::size_t>(i) * channels + c];
        }
    }

    assign(std::move(planar), rate);
    return true;
}

void AudioBuffer::assign(std::vector<std::vector<float>> channels, double sampleRate) {
    data_ = std::move(channels);
    frames_ = data_.empty() ? 0 : data_.front().size();
    sampleRate_ = sampleRate;
}

bool writeWavFloat32(const std::string& path,
                     const float*       interleaved,
                     std::uint64_t      frames,
                     std::uint32_t      channels,
                     double             sampleRate,
                     std::string*       error) {
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = channels;
    fmt.sampleRate = static_cast<drwav_uint32>(sampleRate);
    fmt.bitsPerSample = 32;

    drwav wav;
    if (!drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr)) {
        if (error) *error = "could not open for writing: " + path;
        return false;
    }
    const drwav_uint64 written = drwav_write_pcm_frames(&wav, frames, interleaved);
    drwav_uninit(&wav);

    if (written != frames) {
        if (error) *error = "short write to: " + path;
        return false;
    }
    return true;
}

} // namespace soundsys
