// apps/render_start.cpp - render a start sequence offline, no audio device.
//
// This is the tool you use while tuning the anchor table: it takes the same
// inputs the simulator will feed at run time (a start command plus an NG trace)
// and writes both the resulting audio and a CSV of what the sync loop did, so a
// bad anchor shows up as a plot, not as "it sounds a bit off".
//
//   soundsys_render --wav rec.wav --anchors anchors.json --speed 1.5 --out out.wav

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "NgSource.hpp"
#include "soundsys/AudioBuffer.hpp"
#include "soundsys/AudioEngine.hpp"
#include "soundsys/voices/LoopVoice.hpp"
#include "soundsys/VarispeedReader.hpp"
#include "soundsys/voices/TurbineStartVoice.hpp"

namespace {

struct Options {
    std::string wav;
    std::string anchors;
    std::string trace;
    std::string idleLoop;
    std::string out = "start_render.wav";
    std::string csv;
    double      speed = 1.0;
    std::string mode = "pitch-locked";
    double      sampleRate = 48000.0;
    double      tailSeconds = 2.0;
    std::uint32_t blockFrames = 256;
};

void usage() {
    std::printf(
        "usage: soundsys_render --wav <recording.wav> --anchors <anchors.json>\n"
        "                       [--trace <ng.csv> | --speed <factor>]\n"
        "                       [--mode pitch-locked|varispeed]\n"
        "                       [--idle-loop <loop.wav>] [--out <out.wav>]\n"
        "                       [--csv <diagnostics.csv>] [--rate <hz>]\n"
        "                       [--tail <seconds>] [--block <frames>]\n\n"
        "  --speed  replays the anchor table's own NG profile that many times\n"
        "           faster (1.0 = exactly as recorded). Ignored with --trace.\n"
        "  --mode   pitch-locked keeps the tone the recording had at each NG;\n"
        "           varispeed drags the pitch along with the playback rate.\n");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", what);
                return {};
            }
            return argv[++i];
        };
        if (arg == "--wav") o.wav = next("--wav");
        else if (arg == "--anchors") o.anchors = next("--anchors");
        else if (arg == "--trace") o.trace = next("--trace");
        else if (arg == "--idle-loop") o.idleLoop = next("--idle-loop");
        else if (arg == "--out") o.out = next("--out");
        else if (arg == "--csv") o.csv = next("--csv");
        else if (arg == "--speed") o.speed = std::stod(next("--speed"));
        else if (arg == "--mode") o.mode = next("--mode");
        else if (arg == "--rate") o.sampleRate = std::stod(next("--rate"));
        else if (arg == "--tail") o.tailSeconds = std::stod(next("--tail"));
        else if (arg == "--block") o.blockFrames = static_cast<std::uint32_t>(std::stoul(next("--block")));
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

    // The NG the "simulator" will produce. Either a real trace or the reference
    // profile scaled - both arrive at the engine through the same parameter.
    app::NgSource ng;
    if (!opt.trace.empty()) {
        if (!ng.loadCsv(opt.trace, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    } else {
        ng.fromAnchors(startVoice->anchors(), opt.speed);
    }
    if (ng.empty()) {
        std::fprintf(stderr, "no NG data to drive the start with\n");
        return 1;
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

    soundsys::AudioFormat fmt;
    fmt.sampleRate = opt.sampleRate;
    fmt.maxBlockFrames = std::min(opt.blockFrames, soundsys::kMaxBlockFrames);
    engine.prepare(fmt);

    const double totalSeconds = ng.duration() + opt.tailSeconds;
    const std::uint64_t totalFrames =
        static_cast<std::uint64_t>(totalSeconds * fmt.sampleRate);
    const std::uint32_t channels = 2;

    std::vector<float> output(static_cast<std::size_t>(totalFrames) * channels, 0.0f);

    FILE* csv = nullptr;
    if (!opt.csv.empty()) {
        csv = std::fopen(opt.csv.c_str(), "w");
        if (csv) {
            std::fprintf(csv, "t,ng,target_pos,play_pos,sync_error,speed\n");
        }
    }

    engine.pushEvent(soundsys::EventType::EngineStartCommand, static_cast<float>(ng.at(0.0)));

    std::uint64_t frame = 0;
    while (frame < totalFrames) {
        const std::uint32_t block = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(fmt.maxBlockFrames, totalFrames - frame));
        const double t = static_cast<double>(frame) / fmt.sampleRate;

        // One parameter update per block: exactly what the simulator does when
        // its physics tick and the audio callback do not line up.
        engine.setParameter(soundsys::ParamId::NgPercent, static_cast<float>(ng.at(t)));

        engine.render(output.data() + static_cast<std::size_t>(frame) * channels, block, channels);

        if (csv) {
            std::fprintf(csv, "%.4f,%.3f,%.5f,%.5f,%.5f,%.5f\n", t, ng.at(t),
                         start->targetSeconds(), start->positionSeconds(),
                         start->syncErrorSeconds(), start->playbackSpeed());
        }
        frame += block;
    }
    if (csv) std::fclose(csv);

    if (!soundsys::writeWavFloat32(opt.out, output.data(), totalFrames, channels,
                                   fmt.sampleRate, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    std::printf("rendered %.2f s to %s\n", totalSeconds, opt.out.c_str());
    std::printf("  playback mode  : %s\n", opt.mode.c_str());
    std::printf("  resampler      : %s\n",
                soundsys::VarispeedReader::usingLibsamplerate() ? "libsamplerate"
                                                                : "built-in interpolation");
    std::printf("  final sync err : %+.1f ms\n", start->syncErrorSeconds() * 1000.0);
    std::printf("  dropped events : %llu\n",
                static_cast<unsigned long long>(engine.droppedEvents()));
    if (csv) std::printf("  diagnostics    : %s\n", opt.csv.c_str());
    return 0;
}
