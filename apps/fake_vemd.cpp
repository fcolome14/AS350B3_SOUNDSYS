// apps/fake_vemd.cpp - a VEMD that is not there, for listening to the sound alone.
//
// Sends simlink packets at 20 Hz through a scripted flight: display on, start,
// ground idle, twist grip to FLIGHT, a pull on the collective, back down, engine
// off. That is every transition soundsys_host reacts to, so the whole chain can
// be heard (and recorded) without the simulator running.
//
//   fake_vemd [--host 127.0.0.1] [--port 49350] [--speed 1.0] [--hold 8]
//             [--quiet]
//
// The NG profile is the one measured off the cockpit recording the start asset
// comes from: light-off at about 5 s, 12 % at 6 s, 21 % at 17 s, the FLI screen
// at 60 % at 37.5 s and idle at 66.3 % at 43 s. --speed compresses the lot.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "simlink/EnginePacket.hpp"
#include "simlink/Udp.hpp"

namespace {

std::atomic<bool> gStop{false};
void onSignal(int) { gStop.store(true); }

// (seconds since the start switch, NG %) off the real recording.
const std::vector<std::pair<double, double>> kStartProfile = {
    {0.0, 0.0},   {2.0, 4.0},   {5.0, 10.5}, {6.0, 12.0}, {11.0, 16.9},
    {17.0, 21.2}, {25.0, 33.0}, {33.0, 47.0}, {37.5, 60.0}, {43.0, 66.3},
};

double ngAt(double t) {
    if (t <= kStartProfile.front().first) return kStartProfile.front().second;
    for (std::size_t i = 1; i < kStartProfile.size(); ++i) {
        if (t <= kStartProfile[i].first) {
            const auto& a = kStartProfile[i - 1];
            const auto& b = kStartProfile[i];
            const double f = (t - a.first) / (b.first - a.first);
            return a.second + (b.second - a.second) * f;
        }
    }
    return kStartProfile.back().second;
}

double approach(double from, double to, double f) { return from + (to - from) * f; }

void usage() {
    std::printf(
        "usage: fake_vemd [--host 127.0.0.1] [--port %u] [--speed 1.0] [--hold 8] [--quiet]\n\n"
        "Plays a scripted flight into soundsys_host at 20 Hz:\n"
        "  display on (engine off) -> start -> ground idle -> FLIGHT -> takeoff\n"
        "  -> back to FLIGHT -> engine off -> silence, then it stops sending so\n"
        "  the link drops and the avionics bed goes with it.\n\n"
        "  --hold   seconds to sit in each phase after the start (default 8)\n"
        "  --speed  run the whole script that many times faster\n",
        static_cast<unsigned>(simlink::kDefaultPort));
}

}  // namespace

int main(int argc, char** argv) {
    std::string   host = "127.0.0.1";
    std::uint16_t port = simlink::kDefaultPort;
    double        speed = 1.0;
    double        hold = 8.0;
    bool          quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (arg == "--host") host = next();
        else if (arg == "--port") port = static_cast<std::uint16_t>(std::stoul(next()));
        else if (arg == "--speed") speed = std::stod(next());
        else if (arg == "--hold") hold = std::stod(next());
        else if (arg == "--quiet") quiet = true;
        else if (arg == "-h" || arg == "--help") { usage(); return 0; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return 1;
        }
    }
    if (speed <= 0.0) speed = 1.0;

    simlink::UdpSender tx;
    std::string        error;
    if (!tx.open(host, port, &error)) {
        std::fprintf(stderr, "cannot send to %s:%u - %s\n", host.c_str(),
                     static_cast<unsigned>(port), error.c_str());
        return 1;
    }

    // The script, in sim seconds. Each one is "how long this leg lasts".
    const double tDisplay = 4.0;          // display on, engine still cold
    const double tStart = 43.0;           // switch to a settled ground idle
    const double legs[] = {hold, hold, hold, hold, 6.0};  // idle, flight, takeoff, flight, shutdown
    const double total = tDisplay + tStart + legs[0] + legs[1] + legs[2] + legs[3] + legs[4];

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::printf("fake_vemd -> %s:%u, %.0f s of script at x%.2f\n", host.c_str(),
                static_cast<unsigned>(port), total, speed);

    const auto     wall0 = std::chrono::steady_clock::now();
    simlink::EnginePacket p;
    std::uint8_t   datagram[simlink::kWireSize];
    const char*    lastLeg = "";

    while (!gStop.load()) {
        const double wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        const double t = wall * speed;  // sim time
        if (t >= total) break;

        const char* leg = "display on";
        p.simTime = t;
        p.state = simlink::EngineState::Off;
        p.flags = 0;
        p.nrPercent = 0.0f;
        p.t4Celsius = 20.0f;
        p.torquePercent = 0.0f;
        p.collectivePercent = 5.0f;

        if (t < tDisplay) {
            p.ngPercent = 0.0f;  // master on, nothing turning
        } else if (t < tDisplay + tStart) {
            const double s = t - tDisplay;
            leg = "start";
            p.state = simlink::EngineState::Starting;
            p.startCount = 1;
            p.ngPercent = static_cast<float>(ngAt(s));
            p.nrPercent = static_cast<float>(std::min(70.0, std::max(0.0, (s - 12.0) * 2.4)));
            p.t4Celsius = static_cast<float>(s < 4.0 ? 20.0 + s * 70.0
                                                     : (s < 17.0 ? approach(300.0, 600.0, (s - 4.0) / 13.0)
                                                                 : approach(600.0, 480.0, std::min(1.0, (s - 17.0) / 26.0))));
            if (p.ngPercent < 50.0f) p.flags |= simlink::flags::kStarterEngaged;
            if (p.ngPercent > 52.0f) p.flags |= simlink::flags::kGeneratorOnline;
        } else {
            double s = t - tDisplay - tStart;
            p.state = simlink::EngineState::GroundIdle;
            p.startCount = 1;
            p.flags = simlink::flags::kGeneratorOnline;
            p.ngPercent = 66.3f;
            p.nrPercent = 70.0f;
            p.t4Celsius = 480.0f;
            leg = "ground idle";
            if (s >= legs[0]) {  // twist grip to FLIGHT
                s -= legs[0];
                leg = "flight";
                p.flags |= simlink::flags::kTwistGripFlight;
                p.ngPercent = 80.0f;
                p.nrPercent = 100.0f;
                p.t4Celsius = 560.0f;
                if (s >= legs[1]) {  // collective up
                    s -= legs[1];
                    leg = "takeoff";
                    p.collectivePercent = 45.0f;
                    p.ngPercent = 93.0f;
                    p.torquePercent = 78.0f;
                    p.t4Celsius = 720.0f;
                    if (s >= legs[2]) {  // back down
                        s -= legs[2];
                        leg = "flight";
                        p.collectivePercent = 8.0f;
                        p.ngPercent = 80.0f;
                        p.torquePercent = 20.0f;
                        p.t4Celsius = 560.0f;
                        if (s >= legs[3]) {  // fuel off
                            leg = "shutdown";
                            p.state = simlink::EngineState::Off;
                            p.flags = 0;
                            const double f = std::min(1.0, (s - legs[3]) / legs[4]);
                            p.ngPercent = static_cast<float>(approach(66.0, 0.0, f));
                            p.nrPercent = static_cast<float>(approach(100.0, 0.0, f));
                            p.t4Celsius = static_cast<float>(approach(560.0, 200.0, f));
                            p.torquePercent = 0.0f;
                        }
                    }
                }
            }
        }

        if (!quiet && std::strcmp(leg, lastLeg) != 0) {
            std::printf("\n[%6.1f s] %-12s NG %5.1f %%  NR %5.1f %%  T4 %4.0f C\n", t, leg,
                        static_cast<double>(p.ngPercent), static_cast<double>(p.nrPercent),
                        static_cast<double>(p.t4Celsius));
            std::fflush(stdout);
            lastLeg = leg;
        }

        ++p.seq;
        const std::size_t n = simlink::encode(p, datagram, sizeof(datagram));
        tx.send(datagram, n);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::printf("\nfake_vemd: done - no more packets, so the link will drop\n");
    return 0;
}
