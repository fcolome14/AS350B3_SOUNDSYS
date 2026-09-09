#include "soundsys/backend/MiniaudioDevice.hpp"

#include "soundsys/AudioEngine.hpp"

#if defined(SOUNDSYS_WITH_MINIAUDIO)
// Playback only: no decoders, no encoders, no waveform generators. The module
// loads its own assets and synthesises nothing.
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#endif

namespace soundsys {

#if defined(SOUNDSYS_WITH_MINIAUDIO)

struct MiniaudioDevice::Impl {
    ma_device    device{};
    AudioEngine* engine = nullptr;
    bool         initialised = false;
};

namespace {

void dataCallback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    (void)input;
    auto* impl = static_cast<MiniaudioDevice::Impl*>(device->pUserData);
    if (impl == nullptr || impl->engine == nullptr) return;
    impl->engine->render(static_cast<float*>(output), frameCount, device->playback.channels);
}

}  // namespace

bool MiniaudioDevice::available() noexcept { return true; }

MiniaudioDevice::MiniaudioDevice() : impl_(new Impl()) {}

MiniaudioDevice::~MiniaudioDevice() {
    stop();
    delete impl_;
}

bool MiniaudioDevice::start(AudioEngine& engine, double sampleRate, std::uint32_t channels,
                            std::string* error) {
    stop();

    impl_->engine = &engine;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = channels;
    config.sampleRate = static_cast<ma_uint32>(sampleRate);
    config.dataCallback = dataCallback;
    config.pUserData = impl_;

    if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS) {
        if (error) *error = "could not open the default playback device";
        impl_->engine = nullptr;
        return false;
    }
    impl_->initialised = true;

    // The device decides; prepare the engine for what we actually got.
    sampleRate_ = static_cast<double>(impl_->device.sampleRate);
    channels_ = impl_->device.playback.channels;
    deviceName_ = impl_->device.playback.name;

    AudioFormat fmt;
    fmt.sampleRate = sampleRate_;
    fmt.channels = channels_;
    fmt.maxBlockFrames = kMaxBlockFrames;
    engine.prepare(fmt);

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        if (error) *error = "could not start the playback device";
        stop();
        return false;
    }

    running_ = true;
    return true;
}

void MiniaudioDevice::stop() {
    if (impl_ != nullptr && impl_->initialised) {
        ma_device_uninit(&impl_->device);
        impl_->initialised = false;
        impl_->engine = nullptr;
    }
    running_ = false;
}

#else  // no miniaudio

struct MiniaudioDevice::Impl {};

bool MiniaudioDevice::available() noexcept { return false; }

MiniaudioDevice::MiniaudioDevice() = default;

MiniaudioDevice::~MiniaudioDevice() { delete impl_; }

bool MiniaudioDevice::start(AudioEngine&, double, std::uint32_t, std::string* error) {
    if (error) *error = "built without miniaudio (SOUNDSYS_USE_MINIAUDIO=OFF)";
    return false;
}

void MiniaudioDevice::stop() {}

#endif

}  // namespace soundsys
