// apps/play_start.cpp - the same start, live on the default audio device.
//
// Stands in for the simulator: opens a device, pushes the start command, then
// feeds NG% at 50 Hz from a trace or from the scaled reference profile while the
// audio thread renders. Nothing here is privileged - it is exactly the API the
// flight model will use.
//
//   soundsys_play --wav rec.wav --anchors anchors.json --speed 0.7

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "NgSource.hpp"
#include "soundsys/AudioEngine.hpp"
#include "soundsys/backend/MiniaudioDevice.hpp"
#include "soundsys/voices/LoopVoice.hpp"
#include "soundsys/voices/TurbineStartVoice.hpp"

namespace {

struct Options {
    std::string wav;
    std::string anchors;
    std::string trace;
    std::string idleLoop;
    double      speed = 1.0;
    std::string mode = "pitch-locked";
    double      tailSeconds = 4.0;
    float       gain = 1.0f;
};

void usage() {
    std::printf(
        "usage: soundsys_play --wav <recording.wav> --anchors <anchors.json>\n"
        "                     [--trace <ng.csv> | --speed <factor>]\n"
        "                     [--mode pitch-locked|varispeed]\n"
        "                     [--idle-loop <loop.wav>] [--gain <0..1>]\n"
        "                     [--tail <seconds>]\n");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (arg == "--wav") o.wav = next();
        else if (arg == "--anchors") o.anchors = next();
        else if (arg == "--trace") o.trace = next();
        else if (arg == "--idle-loop") o.idleLoop = next();
        else if (arg == "--speed") o.speed = std::stod(next());
        else if (arg == "--mode") o.mode = next();
        else if (arg == "--tail") o.tailSeconds = std::stod(next());
        else if (arg == "--gain") o.gain = std::stof(next());
        else if (arg == "-h" || arg == "--help") { usage(); return false; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return false;
        }
    }
    if (o.wav.empty() || o.anchors.empty()) {
        usage();
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) return 1;

    if (!soundsys::MiniaudioDevice::available()) {
        std::fprintf(stderr, "this build has no audio backend; use soundsys_render instead\n");
        return 1;
    }

    soundsys::TurbineStartConfig startCfg;
    startCfg.wavPath = opt.wav;
    startCfg.anchorsPath = opt.anchors;
    if (opt.mode == "varispeed") {
        startCfg.mode = soundsys::PlaybackMode::Varispeed;
    } else if (opt.mode != "pitch-locked") {
        std::fprintf(stderr, "unknown --mode %s (want pitch-locked or varispeed)\n",
                     opt.mode.c_str());
        return 1;
    }

    auto startVoice = std::make_unique<soundsys::TurbineStartVoice>(startCfg);
    std::string error;
    if (!startVoice->load(&error)) {
        std::fprintf(stderr, "turbine start voice: %s\n", error.c_str());
        return 1;
    }

    app::NgSource ng;
    if (!opt.trace.empty()) {
        if (!ng.loadCsv(opt.trace, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    } else {
        ng.fromAnchors(startVoice->anchors(), opt.speed);
    }

    soundsys::AudioEngine engine;
    auto* start = static_cast<soundsys::TurbineStartVoice*>(engine.addVoice(std::move(startVoice)));

    if (!opt.idleLoop.empty()) {
        soundsys::LoopVoiceConfig loopCfg;
        loopCfg.wavPath = opt.idleLoop;
        loopCfg.referenceValue = start->anchors().lastNg();
        auto loop = std::make_unique<soundsys::LoopVoice>(loopCfg);
        if (!loop->load(&error)) {
            std::fprintf(stderr, "idle loop: %s\n", error.c_str());
            return 1;
        }
        engine.addVoice(std::move(loop));
    }

    engine.setParameter(soundsys::ParamId::MasterGain, opt.gain);

    // start() prepares the engine at the rate the device negotiated.
    soundsys::MiniaudioDevice device;
    if (!device.start(engine, 48000.0, 2, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("device: %s @ %.0f Hz, %u ch\n", device.deviceName().c_str(), device.sampleRate(),
                device.channels());

    engine.pushEvent(soundsys::EventType::EngineStartCommand, static_cast<float>(ng.at(0.0)));

    const auto   t0 = std::chrono::steady_clock::now();
    const double total = ng.duration() + opt.tailSeconds;
    double       elapsed = 0.0;
    int          line = 0;

    while (elapsed < total) {
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        engine.setParameter(soundsys::ParamId::NgPercent, static_cast<float>(ng.at(elapsed)));

        if (++line % 10 == 0) {
            std::printf("\r t=%5.1f s  NG=%5.1f %%  speed=%4.2fx  sync=%+5.0f ms   ", elapsed,
                        ng.at(elapsed), start->playbackSpeed(),
                        start->syncErrorSeconds() * 1000.0);
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 50 Hz sim tick
    }

    std::printf("\ndone\n");
    device.stop();
    return 0;
}
