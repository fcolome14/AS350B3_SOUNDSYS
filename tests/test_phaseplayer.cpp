// PhasePlayerVoice: one recording per phase, played as recorded.
//
// Each phase is stood up by a tone of its own, so what is sounding can be read
// straight off the output: 400 Hz start, 500 Hz idle loop, 600 Hz flight
// engage, 700 Hz flight loop, 800 Hz shutdown.

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
#include "soundsys/voices/PhasePlayerVoice.hpp"

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
    soundsys::AudioEngine       engine;
    soundsys::PhasePlayerVoice* voice = nullptr;
    std::vector<float>          out;
    double                      t = 0.0;

    void run(double seconds, soundsys::EnginePhase phase) {
        engine.setParameter(soundsys::ParamId::EnginePhase, static_cast<float>(phase));
        const std::uint32_t block = 256;
        std::vector<float>  buf(block * 2);
        const double        end = t + seconds;
        while (t < end) {
            engine.render(buf.data(), block, 2);
            for (std::uint32_t i = 0; i < block; ++i) out.push_back(buf[i * 2]);
            t += block / kRate;
        }
    }

    double freq(double t0, double window = 0.4) const {
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
    using soundsys::EnginePhase;

    const std::string startClip = "test_phase_start.wav";
    const std::string idleLoop = "test_phase_idle.wav";
    const std::string engage = "test_phase_engage.wav";
    const std::string flightLoop = "test_phase_flight.wav";
    const std::string shutdown = "test_phase_shutdown.wav";
    CHECK(writeTone(startClip, 400.0, 3.0));
    CHECK(writeTone(idleLoop, 500.0, 1.0));
    CHECK(writeTone(engage, 600.0, 2.0));
    CHECK(writeTone(flightLoop, 700.0, 1.0));
    CHECK(writeTone(shutdown, 800.0, 2.0));

    soundsys::PhasePlayerConfig cfg;
    const auto at = [&](EnginePhase p) -> soundsys::PhaseClips& {
        return cfg.phases[static_cast<std::size_t>(p)];
    };
    at(EnginePhase::Start) = {startClip, idleLoop, 1.0f};
    at(EnginePhase::Idle) = {"", idleLoop, 1.0f};
    at(EnginePhase::Flight) = {engage, flightLoop, 1.0f};
    at(EnginePhase::Shutdown) = {shutdown, "", 1.0f};
    cfg.crossfadeSeconds = 0.5;
    cfg.loopJoinSeconds = 0.3;

    auto        voice = std::make_unique<soundsys::PhasePlayerVoice>(cfg);
    std::string error;
    CHECK(voice->load(&error));

    Rig rig;
    rig.voice = static_cast<soundsys::PhasePlayerVoice*>(rig.engine.addVoice(std::move(voice)));
    soundsys::AudioFormat fmt;
    fmt.sampleRate = kRate;
    fmt.maxBlockFrames = 256;
    rig.engine.prepare(fmt);

    // Nothing before a phase is set.
    rig.run(1.0, EnginePhase::Off);
    CHECK(rig.rms(0.5) < 1e-6);

    // Start: its own recording, then its loop, which holds indefinitely.
    rig.run(30.0, EnginePhase::Start);
    CHECK_NEAR(rig.freq(2.0), 400.0, 8.0);    // inside the start clip
    CHECK_NEAR(rig.freq(6.0), 500.0, 8.0);    // rolled into the idle loop
    CHECK_NEAR(rig.freq(30.0), 500.0, 8.0);   // 25 s later, still there
    CHECK(rig.rms(30.0) > 0.3);

    // Twist grip: the engage recording, then the flight loop.
    const double tFlight = rig.t;
    rig.run(20.0, EnginePhase::Flight);
    CHECK_NEAR(rig.freq(tFlight + 1.2), 600.0, 10.0);
    CHECK_NEAR(rig.freq(tFlight + 5.0), 700.0, 10.0);
    CHECK_NEAR(rig.freq(tFlight + 19.0), 700.0, 10.0);

    // No hole anywhere, including both crossfades.
    for (double t = 1.5; t < rig.t - 0.3; t += 0.2) CHECK(rig.rms(t) > 0.2);

    // Shutdown: heard out, then silence that stays.
    const double tStop = rig.t;
    rig.run(6.0, EnginePhase::Shutdown);
    CHECK_NEAR(rig.freq(tStop + 1.2), 800.0, 10.0);
    CHECK(rig.rms(tStop + 4.0) < 1e-4);
    CHECK(!rig.voice->isActive());

    // A phase with nothing recorded for it is silence, not a crash.
    rig.run(2.0, EnginePhase::Cruise);
    CHECK(rig.rms(rig.t - 0.5) < 1e-4);

    std::printf("phase player: start %.0f Hz, idle loop %.0f Hz at 30 s, engage %.0f Hz, "
                "flight loop %.0f Hz, shutdown %.0f Hz\n",
                rig.freq(2.0), rig.freq(30.0), rig.freq(tFlight + 1.2), rig.freq(tFlight + 19.0),
                rig.freq(tStop + 1.2));

    for (const std::string& f : {startClip, idleLoop, engage, flightLoop, shutdown}) {
        std::remove(f.c_str());
    }
    return test::summary("phase_player");
}
