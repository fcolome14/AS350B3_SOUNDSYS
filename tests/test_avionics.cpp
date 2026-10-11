// AvionicsBedVoice: the cockpit bed follows the display, not the engine.
//
// The two clips are tones, so what is sounding can be read off the output:
// 400 Hz for the master switch and the fan running up, 700 Hz for the fan loop.

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "TestSupport.hpp"
#include "soundsys/AudioBuffer.hpp"
#include "soundsys/AudioEngine.hpp"
#include "soundsys/voices/AvionicsBedVoice.hpp"

namespace {

constexpr double kRate = 48000.0;

bool writeTone(const std::string& path, double hz, double seconds) {
    const std::size_t  n = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> s(n);
    for (std::size_t i = 0; i < n; ++i) {
        s[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * hz * i / kRate));
    }
    return soundsys::writeWavFloat32(path, s.data(), n, 1, kRate, nullptr);
}

struct Rig {
    soundsys::AudioEngine        engine;
    soundsys::AvionicsBedVoice*  voice = nullptr;
    std::vector<float>           out;
    double                       t = 0.0;

    void run(double seconds) {
        const std::uint32_t block = 256;
        std::vector<float>  buf(block * 2);
        const double        end = t + seconds;
        while (t < end) {
            engine.render(buf.data(), block, 2);
            for (std::uint32_t i = 0; i < block; ++i) out.push_back(buf[i * 2]);
            t += block / kRate;
        }
    }

    double freq(double t0, double window = 0.3) const {
        const std::size_t a = static_cast<std::size_t>(t0 * kRate);
        const std::size_t b = std::min(out.size(), static_cast<std::size_t>((t0 + window) * kRate));
        int               crossings = 0;
        for (std::size_t i = a + 1; i < b; ++i) {
            if ((out[i - 1] < 0.0f) != (out[i] < 0.0f)) ++crossings;
        }
        return crossings / (2.0 * window);
    }

    double rms(double t0, double window = 0.2) const {
        const std::size_t a = static_cast<std::size_t>(t0 * kRate);
        const std::size_t b = std::min(out.size(), static_cast<std::size_t>((t0 + window) * kRate));
        double            e = 0.0;
        for (std::size_t i = a; i < b; ++i) e += double(out[i]) * out[i];
        return b > a ? std::sqrt(e / double(b - a)) : 0.0;
    }
};

}  // namespace

int main() {
    const std::string oneShot = "test_avionics_master.wav";
    const std::string loop = "test_avionics_fan.wav";
    CHECK(writeTone(oneShot, 400.0, 1.0));
    CHECK(writeTone(loop, 700.0, 0.4));

    soundsys::AvionicsBedConfig cfg;
    cfg.oneShot = oneShot;
    cfg.loop = loop;
    cfg.fadeInSeconds = 0.02;
    cfg.loopJoinSeconds = 0.1;
    cfg.fadeOutSeconds = 0.2;

    Rig  rig;
    auto voice = std::make_unique<soundsys::AvionicsBedVoice>(cfg);
    std::string error;
    CHECK(voice->load(&error));
    rig.voice = static_cast<soundsys::AvionicsBedVoice*>(rig.engine.addVoice(std::move(voice)));
    soundsys::AudioFormat fmt;
    fmt.sampleRate = kRate;
    rig.engine.prepare(fmt);

    // Nothing until the display comes up, whatever the engine is doing.
    rig.run(0.3);
    CHECK(rig.rms(0.05) < 1e-6);
    CHECK(!rig.voice->running());

    // MASTER ON: the one-shot, then its loop, and the loop keeps going well
    // past the end of the one-shot recording.
    rig.engine.pushEvent(soundsys::EventType::VemdOn);
    rig.run(0.6);
    CHECK_NEAR(rig.freq(0.45), 400.0, 25.0);
    CHECK(rig.voice->running());
    rig.run(1.2);
    CHECK_NEAR(rig.freq(1.6), 700.0, 25.0);
    CHECK(rig.rms(1.6) > 0.1);

    // The link blinking must not fire the master switch on top of the fan.
    rig.engine.pushEvent(soundsys::EventType::VemdOn);
    rig.run(0.5);
    CHECK_NEAR(rig.freq(2.2), 700.0, 25.0);

    // Display dark: fade out and stay quiet.
    rig.engine.pushEvent(soundsys::EventType::VemdOff);
    rig.run(0.8);
    CHECK(!rig.voice->running());
    CHECK(rig.rms(3.2, 0.3) < 1e-4);
    CHECK(!rig.voice->isActive());

    std::remove(oneShot.c_str());
    std::remove(loop.c_str());
    return test::summary("avionics_bed");
}
