// NgLoopBankVoice: steady engine at any NG, for as long as it stays there.
//
// Two synthetic loops stand in for the recorded ones: a 1000 Hz tone "recorded"
// at NG 68 and a 1200 Hz tone at NG 80. The bank must sound the right tone at
// each NG, move smoothly between them, keep going indefinitely, and stop when
// told to.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "TestSupport.hpp"
#include "soundsys/AudioBuffer.hpp"
#include "soundsys/AudioEngine.hpp"
#include "soundsys/voices/NgLoopBankVoice.hpp"

namespace {

constexpr double kRate = 48000.0;

bool writeTone(const std::string& path, double hz, double seconds) {
    // A whole number of cycles, so the file loops without a seam.
    const std::size_t   n = static_cast<std::size_t>(seconds * kRate);
    std::vector<float>  s(n);
    for (std::size_t i = 0; i < n; ++i) {
        s[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * hz * i / kRate));
    }
    return soundsys::writeWavFloat32(path, s.data(), n, 1, kRate, nullptr);
}

struct Rig {
    soundsys::AudioEngine     engine;
    soundsys::NgLoopBankVoice* bank = nullptr;
    std::vector<float>        out;  // left channel, whole run
    double                    t = 0.0;

    // Renders `seconds`, setting NG from ng(t) once per 20 ms like the VEMD does.
    void run(double seconds, const std::function<double(double)>& ng) {
        const std::uint32_t block = 256;
        std::vector<float>  buf(block * 2);
        double              nextNg = t;
        const double        end = t + seconds;
        while (t < end) {
            if (t >= nextNg) {
                engine.setParameter(soundsys::ParamId::NgPercent, static_cast<float>(ng(t)));
                nextNg += 0.05;  // 20 Hz staircase, on purpose
            }
            engine.render(buf.data(), block, 2);
            for (std::uint32_t i = 0; i < block; ++i) out.push_back(buf[i * 2]);
            t += block / kRate;
        }
    }

    double frequencyAt(double t0, double window = 0.5) const {
        const std::size_t a = static_cast<std::size_t>(t0 * kRate);
        const std::size_t b = static_cast<std::size_t>((t0 + window) * kRate);
        int crossings = 0;
        for (std::size_t i = a + 1; i < b && i < out.size(); ++i) {
            if ((out[i - 1] < 0.0f) != (out[i] < 0.0f)) ++crossings;
        }
        return crossings / (2.0 * window);
    }

    double rmsAt(double t0, double window = 0.5) const {
        const std::size_t a = static_cast<std::size_t>(t0 * kRate);
        const std::size_t b = static_cast<std::size_t>((t0 + window) * kRate);
        double e = 0.0;
        for (std::size_t i = a; i < b && i < out.size(); ++i) e += double(out[i]) * out[i];
        return std::sqrt(e / double(b - a));
    }
};

}  // namespace

int main() {
    const std::string idle = "test_bank_idle.wav";
    const std::string flight = "test_bank_flight.wav";
    CHECK(writeTone(idle, 1000.0, 2.0));
    CHECK(writeTone(flight, 1200.0, 2.0));

    soundsys::NgLoopBankConfig cfg;
    cfg.loops = {{flight, 80.0}, {idle, 68.0}};  // order does not matter
    auto voice = std::make_unique<soundsys::NgLoopBankVoice>(cfg);
    std::string error;
    CHECK(voice->load(&error));

    Rig rig;
    rig.bank = static_cast<soundsys::NgLoopBankVoice*>(rig.engine.addVoice(std::move(voice)));
    soundsys::AudioFormat fmt;
    fmt.sampleRate = kRate;
    fmt.maxBlockFrames = 256;
    rig.engine.prepare(fmt);

    // Silent until the start sequence hands over.
    rig.run(1.0, [](double) { return 68.0; });
    CHECK(rig.rmsAt(0.4) < 1e-6);

    rig.engine.pushEvent(soundsys::EventType::StartSequenceComplete, 68.0f);

    // Ground idle held for a minute: thirty times the loop length, still there.
    rig.run(60.0, [](double) { return 68.0; });
    CHECK_NEAR(rig.frequencyAt(10.0), 1000.0, 10.0);
    CHECK_NEAR(rig.frequencyAt(60.0), 1000.0, 10.0);
    CHECK(rig.rmsAt(60.0) > 0.3);

    // Twist grip: NG ramps 68 -> 80 in 5 s (in 20 Hz steps), then holds.
    const double t0 = rig.t;
    rig.run(10.0, [t0](double t) { return 68.0 + std::min(1.0, (t - t0) / 5.0) * 12.0; });
    CHECK_NEAR(rig.frequencyAt(t0 + 8.0), 1200.0, 12.0);
    // Never a gap during the crossfade.
    for (double t = t0; t < t0 + 6.0; t += 0.25) CHECK(rig.rmsAt(t, 0.25) > 0.25);

    // Above the top loop: that loop, pitched with NG.
    rig.run(4.0, [](double) { return 90.0; });
    CHECK_NEAR(rig.frequencyAt(rig.t - 1.0), 1200.0 * 90.0 / 80.0, 14.0);

    // Shutdown: fades out and stays out.
    rig.engine.pushEvent(soundsys::EventType::EngineShutdown, 0.0f);
    rig.run(3.0, [](double) { return 90.0; });
    CHECK(rig.rmsAt(rig.t - 0.6) < 1e-6);
    CHECK(!rig.bank->isActive());

    std::printf("loop bank: idle %.1f Hz at 60 s, flight %.1f Hz, NG 90 -> %.1f Hz\n",
                rig.frequencyAt(60.0), rig.frequencyAt(t0 + 8.0), rig.frequencyAt(t0 + 13.0));

    std::remove(idle.c_str());
    std::remove(flight.c_str());
    return test::summary("loop_bank");
}
