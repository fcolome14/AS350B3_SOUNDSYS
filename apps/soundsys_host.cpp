// apps/soundsys_host.cpp - the audio process of the simulator.
//
// Listens for simlink engine packets (from the VEMD, or anything else that
// speaks the contract in simlink/EnginePacket.hpp), turns them into engine
// inputs through SimLinkAdapter, and plays the result on the default device.
//
// One process per module: the VEMD can be rebuilt, crash, restart or run on
// another machine, and the audio process neither knows nor cares - it hears
// packets or it does not.
//
//   soundsys_host --wav start.wav --anchors start.json [--idle-loop idle.wav]
//                 [--port 49350] [--record session.wav]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "simlink/Udp.hpp"
#include "soundsys/AudioBuffer.hpp"
#include "soundsys/AudioEngine.hpp"
#include "soundsys/backend/MiniaudioDevice.hpp"
#include "soundsys/link/SimLinkAdapter.hpp"
#include "soundsys/EnginePhase.hpp"
#include "soundsys/voices/NgLoopBankVoice.hpp"
#include "soundsys/voices/PhasePlayerVoice.hpp"
#include "soundsys/voices/TurbineStartVoice.hpp"

namespace {

std::atomic<bool> gStop{false};
void onSignal(int) { gStop.store(true); }

struct Options {
    std::string   wav;
    std::string   anchors;
    std::vector<soundsys::LoopBankEntry> loops;  // steady-state loops, by recorded NG
    soundsys::PhasePlayerConfig phases;          // one recording per phase of flight
    bool                        usePhases = false;
    std::string   bind = "0.0.0.0";
    std::string   mode = "pitch-locked";
    std::string   record;
    std::uint16_t port = simlink::kDefaultPort;
    double        delayMs = 60.0;
    double        timeoutS = 1.0;
    double        recordSeconds = 300.0;
    double        runSeconds = 0.0;  // 0 = until Ctrl+C
    float         gain = 1.0f;
    bool          quiet = false;
};

void usage() {
    std::printf(
        "usage, one recording per phase (recommended):\n"
        "  soundsys_host --phase start=start.wav,idle_loop.wav --phase idle=,idle_loop.wav\n"
        "                --phase flight=flight_engage.wav,flight_loop.wav\n"
        "                --phase takeoff=takeoff.wav,cruise_loop.wav\n"
        "                --phase shutdown=shutdown.wav [--port ...] [--record ...]\n\n"
        "  --phase <phase>=<one_shot.wav>[,<loop.wav>]   either part may be left out\n"
        "  phases: off start idle flight takeoff cruise landing shutdown rotor_brake\n"
        "  The one-shot plays on entering the phase, then the loop holds it for as\n"
        "  long as the aircraft stays there. Nothing is stretched or pitched.\n\n"
        "usage, NG-driven start (the older path):\n"
        "  soundsys_host --wav <start.wav> --anchors <start.json>\n"
        "                     [--loop <ng>:<loop.wav> ...] [--mode pitch-locked|varispeed]\n"
        "                     [--port <udp port>] [--bind <ipv4>] [--delay-ms <ms>]\n"
        "                     [--timeout <s>] [--gain <0..1>] [--record <out.wav>]\n"
        "                     [--record-seconds <s>] [--seconds <s>] [--quiet]\n\n"
        "Listens for simlink engine packets (default port %u) and plays the engine.\n"
        "Each --loop is a seamless loop recorded at that NG (e.g. 68:idle.wav,\n"
        "80.7:flight.wav): they take over when the start ends and follow NG from then on.\n"
        "Ctrl+C to stop.\n",
        static_cast<unsigned>(simlink::kDefaultPort));
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (arg == "--wav") o.wav = next();
        else if (arg == "--anchors") o.anchors = next();
        else if (arg == "--phase") {
            // <phase>=<one_shot>[,<loop>]; either side of the comma may be empty.
            const std::string v = next();
            const std::size_t eq = v.find('=');
            soundsys::EnginePhase phase{};
            if (eq == std::string::npos || !soundsys::phaseFromName(v.substr(0, eq).c_str(), phase)) {
                std::fprintf(stderr, "--phase wants <phase>=<one_shot.wav>[,<loop.wav>], got \"%s\"\n",
                             v.c_str());
                return false;
            }
            const std::string files = v.substr(eq + 1);
            const std::size_t comma = files.find(',');
            auto& clips = o.phases.phases[static_cast<std::size_t>(phase)];
            clips.oneShot = comma == std::string::npos ? files : files.substr(0, comma);
            clips.loop = comma == std::string::npos ? std::string() : files.substr(comma + 1);
            o.usePhases = true;
        }
        else if (arg == "--loop") {
            // <ng>:<file>. Split at the FIRST colon, so "68:C:/x.wav" still works.
            const std::string v = next();
            const std::size_t colon = v.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= v.size()) {
                std::fprintf(stderr, "--loop wants <ng>:<loop.wav>, got \"%s\"\n", v.c_str());
                return false;
            }
            o.loops.push_back({v.substr(colon + 1), std::stod(v.substr(0, colon))});
        }
        else if (arg == "--mode") o.mode = next();
        else if (arg == "--port") o.port = static_cast<std::uint16_t>(std::stoul(next()));
        else if (arg == "--bind") o.bind = next();
        else if (arg == "--delay-ms") o.delayMs = std::stod(next());
        else if (arg == "--timeout") o.timeoutS = std::stod(next());
        else if (arg == "--gain") o.gain = std::stof(next());
        else if (arg == "--record") o.record = next();
        else if (arg == "--record-seconds") o.recordSeconds = std::stod(next());
        else if (arg == "--seconds") o.runSeconds = std::stod(next());
        else if (arg == "--quiet") o.quiet = true;
        else if (arg == "-h" || arg == "--help") { usage(); return false; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return false;
        }
    }
    if (!o.usePhases && (o.wav.empty() || o.anchors.empty())) {
        usage();
        return false;
    }
    return true;
}

// Copies what the device plays into a buffer allocated up front, so the audio
// thread only ever does a memcpy. Written out as a WAV at exit.
struct Recorder {
    std::vector<float>       samples;
    std::atomic<std::size_t> used{0};
    std::uint32_t            channels = 0;

    static void tap(const float* data, std::uint32_t frames, std::uint32_t ch, void* user) {
        auto*             r = static_cast<Recorder*>(user);
        const std::size_t n = static_cast<std::size_t>(frames) * ch;
        const std::size_t at = r->used.load(std::memory_order_relaxed);
        if (at + n > r->samples.size()) return;  // full: the recording just stops
        std::memcpy(r->samples.data() + at, data, n * sizeof(float));
        r->channels = ch;
        r->used.store(at + n, std::memory_order_release);
    }
};

const char* stateName(simlink::EngineState s) {
    switch (s) {
        case simlink::EngineState::Starting:   return "STARTING";
        case simlink::EngineState::GroundIdle: return "IDLE    ";
        default:                               return "OFF     ";
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) return 1;

    if (!soundsys::MiniaudioDevice::available()) {
        std::fprintf(stderr, "this build has no audio backend (SOUNDSYS_USE_MINIAUDIO=OFF)\n");
        return 1;
    }

    // ---- voices ----------------------------------------------------------
    soundsys::AudioEngine       engine;
    soundsys::TurbineStartVoice* start = nullptr;
    soundsys::NgLoopBankVoice*   bank = nullptr;
    soundsys::PhasePlayerVoice*  phases = nullptr;
    std::string                  error;

    if (opt.usePhases) {
        // One recording per phase, played as recorded.
        auto voice = std::make_unique<soundsys::PhasePlayerVoice>(opt.phases);
        if (!voice->load(&error)) {
            std::fprintf(stderr, "phase player: %s\n", error.c_str());
            return 1;
        }
        phases = static_cast<soundsys::PhasePlayerVoice*>(engine.addVoice(std::move(voice)));
        if (!opt.wav.empty() || !opt.loops.empty()) {
            std::fprintf(stderr, "--phase given: ignoring --wav/--anchors/--loop\n");
        }
    } else {
        // The NG-driven path: the start recording placed by an anchor table,
        // handing over to loops that follow NG.
        soundsys::TurbineStartConfig startCfg;
        startCfg.wavPath = opt.wav;
        startCfg.anchorsPath = opt.anchors;
        if (opt.mode == "varispeed") {
            startCfg.mode = soundsys::PlaybackMode::Varispeed;
        } else if (opt.mode != "pitch-locked") {
            std::fprintf(stderr, "unknown --mode %s\n", opt.mode.c_str());
            return 1;
        }

        auto startVoice = std::make_unique<soundsys::TurbineStartVoice>(startCfg);
        if (!startVoice->load(&error)) {
            std::fprintf(stderr, "turbine start voice: %s\n", error.c_str());
            return 1;
        }
        start = static_cast<soundsys::TurbineStartVoice*>(engine.addVoice(std::move(startVoice)));

        if (!opt.loops.empty()) {
            soundsys::NgLoopBankConfig bankCfg;
            bankCfg.loops = opt.loops;
            auto voice = std::make_unique<soundsys::NgLoopBankVoice>(bankCfg);
            if (!voice->load(&error)) {
                std::fprintf(stderr, "loop bank: %s\n", error.c_str());
                return 1;
            }
            bank = static_cast<soundsys::NgLoopBankVoice*>(engine.addVoice(std::move(voice)));
        } else {
            std::fprintf(stderr, "no --loop given: the engine will fall silent after the start\n");
        }
    }
    engine.setParameter(soundsys::ParamId::MasterGain, opt.gain);

    // ---- network ---------------------------------------------------------
    simlink::UdpReceiver rx;
    if (!rx.open(opt.port, opt.bind, &error)) {
        std::fprintf(stderr, "cannot listen: %s\n", error.c_str());
        std::fprintf(stderr, "is another soundsys_host already running? only one can own port %u\n",
                     static_cast<unsigned>(opt.port));
        return 1;
    }

    // ---- device ----------------------------------------------------------
    soundsys::MiniaudioDevice device;
    Recorder                  recorder;
    if (!opt.record.empty()) {
        // Sized for the device rate we are about to ask for; a device that
        // negotiates more simply fills it sooner.
        recorder.samples.assign(static_cast<std::size_t>(opt.recordSeconds * 48000.0 * 2.0), 0.0f);
        device.setTap(&Recorder::tap, &recorder);
    }
    if (!device.start(engine, 48000.0, 2, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    std::printf("soundsys_host: %s @ %.0f Hz | listening on udp %s:%u | %s\n",
                device.deviceName().c_str(), device.sampleRate(), opt.bind.c_str(),
                static_cast<unsigned>(rx.port()),
                opt.usePhases ? "one recording per phase" : opt.mode.c_str());
    std::printf("waiting for the VEMD (Ctrl+C to stop)\n");
    std::fflush(stdout);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    soundsys::link::LinkConfig linkCfg;
    linkCfg.playoutDelay = opt.delayMs / 1000.0;
    linkCfg.linkTimeout = opt.timeoutS;
    soundsys::link::EngineSink     sink(engine);
    soundsys::link::SimLinkAdapter link(sink, linkCfg);

    const auto t0 = std::chrono::steady_clock::now();
    const auto now = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    std::uint8_t         datagram[512];
    double               nextStatus = 0.0;
    bool                 wasUp = false;
    simlink::EngineState lastState = simlink::EngineState::Off;
    soundsys::EnginePhase lastPhase = soundsys::EnginePhase::Off;

    while (!gStop.load() && (opt.runSeconds <= 0.0 || now() < opt.runSeconds)) {
        // Wake at least every couple of milliseconds even with no traffic: the
        // interpolated NG has to keep flowing between the 20 Hz packets.
        int n = rx.receive(datagram, sizeof(datagram), 2);
        while (n > 0) {
            link.onDatagram(datagram, static_cast<std::size_t>(n), now());
            n = rx.receive(datagram, sizeof(datagram), 0);  // drain what is queued
        }
        const double t = now();
        link.update(t);

        const auto& st = link.stats();
        if (st.linkUp != wasUp) {
            std::printf("\n[%7.2f s] %s\n", t,
                        st.linkUp ? "link UP - VEMD heard" : "link DOWN - engine shut down");
            wasUp = st.linkUp;
        }
        if (st.state != lastState) {
            std::printf("\n[%7.2f s] %s NG %.1f %%\n", t, stateName(st.state), st.ng);
            lastState = st.state;
        }
        if (st.phase != lastPhase) {
            std::printf("\n[%7.2f s] phase %s\n", t, soundsys::phaseName(st.phase));
            lastPhase = st.phase;
        }
        if (!opt.quiet && t >= nextStatus) {
            nextStatus = t + 0.5;
            // Which voice is sounding: the start (head speed / sync) or the
            // loop bank (weight of each loop).
            char voices[96] = "";
            if (phases != nullptr) {
                std::snprintf(voices, sizeof(voices), "%s: %s", soundsys::phaseName(st.phase),
                              phases->playing());
            } else if (start != nullptr && start->isActive()) {
                std::snprintf(voices, sizeof(voices), "start x%4.2f sync %+4.0f ms",
                              start->playbackSpeed(), start->syncErrorSeconds() * 1000.0);
            } else if (bank != nullptr && bank->isActive()) {
                int n = std::snprintf(voices, sizeof(voices), "loops");
                for (std::size_t i = 0; i < bank->loopCount() && n < 80; ++i) {
                    n += std::snprintf(voices + n, sizeof(voices) - n, " %.2f", bank->weight(i));
                }
            } else {
                std::snprintf(voices, sizeof(voices), "silent");
            }
            std::printf("\r %s NG %5.1f %%  | pkts %llu (stale %llu, bad %llu)  age %4.0f ms  | %-28s",
                        stateName(st.state), st.ng,
                        static_cast<unsigned long long>(st.accepted),
                        static_cast<unsigned long long>(st.stale),
                        static_cast<unsigned long long>(st.malformed),
                        st.lastPacketAge * 1000.0, voices);
            std::fflush(stdout);
        }
    }

    std::printf("\nstopping\n");
    device.stop();

    if (!opt.record.empty()) {
        const std::size_t used = recorder.used.load(std::memory_order_acquire);
        const std::uint32_t ch = recorder.channels ? recorder.channels : 2;
        if (soundsys::writeWavFloat32(opt.record, recorder.samples.data(), used / ch, ch,
                                      device.sampleRate(), &error)) {
            std::printf("recorded %.1f s to %s\n",
                        static_cast<double>(used / ch) / device.sampleRate(), opt.record.c_str());
        } else {
            std::fprintf(stderr, "%s\n", error.c_str());
        }
    }
    return 0;
}
