// soundsys/backend/MiniaudioDevice.hpp - the only place that talks to the OS.
//
// Kept behind this thin wrapper so the engine, the voices and the tests never
// include miniaudio, and the whole module can run headless (offline rendering,
// CI) with no audio device present.
#pragma once

#include <cstdint>
#include <string>

namespace soundsys {

class AudioEngine;

class MiniaudioDevice {
public:
    MiniaudioDevice();
    ~MiniaudioDevice();
    MiniaudioDevice(const MiniaudioDevice&) = delete;
    MiniaudioDevice& operator=(const MiniaudioDevice&) = delete;

    // Opens the default playback device and starts pulling from `engine`, which
    // must outlive the device. The engine is prepared with the rate the device
    // actually negotiated, which is rarely the one you asked for.
    bool start(AudioEngine& engine, double sampleRate, std::uint32_t channels,
               std::string* error = nullptr);

    void stop();

    bool running() const noexcept { return running_; }
    double sampleRate() const noexcept { return sampleRate_; }
    std::uint32_t channels() const noexcept { return channels_; }
    const std::string& deviceName() const noexcept { return deviceName_; }

    static bool available() noexcept;

    // Opaque backend state; public only so the device callback can reach it.
    struct Impl;

private:
    Impl*         impl_ = nullptr;
    double        sampleRate_ = 0.0;
    std::uint32_t channels_ = 0;
    bool          running_ = false;
    std::string   deviceName_;
};

}  // namespace soundsys
