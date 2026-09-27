// simlink contract + SimLinkAdapter: the path from the VEMD's model to the
// engine inputs, without a network and then over a real loopback socket.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "TestSupport.hpp"
#include "simlink/EnginePacket.hpp"
#include "simlink/Udp.hpp"
#include "soundsys/link/SimLinkAdapter.hpp"

using simlink::EnginePacket;
using simlink::EngineState;
using soundsys::EventType;
using soundsys::ParamId;
using soundsys::link::LinkConfig;
using soundsys::link::SimLinkAdapter;

namespace {

struct RecordingSink final : soundsys::link::ILinkSink {
    std::vector<std::pair<EventType, float>> events;
    std::vector<float>                       ng;
    soundsys::EnginePhase                    phase = soundsys::EnginePhase::Off;
    void event(EventType t, float v) override { events.emplace_back(t, v); }
    void parameter(ParamId id, float v) override {
        if (id == ParamId::NgPercent) ng.push_back(v);
        if (id == ParamId::EnginePhase) {
            phase = static_cast<soundsys::EnginePhase>(static_cast<int>(v));
        }
    }
    std::vector<EventType> types() const {
        std::vector<EventType> out;
        for (const auto& e : events) out.push_back(e.first);
        return out;
    }
};

EnginePacket packet(std::uint32_t seq, double t, EngineState st, float ng,
                    std::uint32_t starts = 1, std::uint8_t flags = 0) {
    EnginePacket p;
    p.seq = seq;
    p.simTime = t;
    p.state = st;
    p.ngPercent = ng;
    p.startCount = starts;
    p.flags = flags;
    return p;
}

void testWireFormat() {
    EnginePacket p = packet(123456u, 987.654321, EngineState::Starting, 42.5f, 7,
                            simlink::flags::kStarterEngaged | simlink::flags::kTwistGripFlight);
    p.nrPercent = 84.6f;
    p.t4Celsius = 666.0f;
    p.torquePercent = 15.1f;
    p.collectivePercent = 35.0f;

    std::uint8_t buf[64];
    const std::size_t n = simlink::encode(p, buf, sizeof(buf));
    CHECK(n == simlink::kWireSize);
    // Fixed bytes on the wire, whatever the host: the magic reads "A35S".
    CHECK(buf[0] == 'A' && buf[1] == '3' && buf[2] == '5' && buf[3] == 'S');

    EnginePacket q;
    CHECK(simlink::decode(buf, n, q));
    CHECK(q.seq == p.seq);
    CHECK(q.simTime == p.simTime);
    CHECK(q.startCount == p.startCount);
    CHECK(q.state == p.state);
    CHECK(q.flags == p.flags);
    CHECK(q.ngPercent == p.ngPercent);
    CHECK(q.nrPercent == p.nrPercent);
    CHECK(q.t4Celsius == p.t4Celsius);
    CHECK(q.torquePercent == p.torquePercent);
    CHECK(q.collectivePercent == p.collectivePercent);
    CHECK(q.has(simlink::flags::kTwistGripFlight));
    CHECK(!q.has(simlink::flags::kGeneratorOnline));

    CHECK(simlink::encode(p, buf, 10) == 0);  // no room
    CHECK(!simlink::decode(buf, n - 1, q));   // truncated

    std::uint8_t bad[64];
    std::memcpy(bad, buf, n);
    bad[0] ^= 0xFF;
    CHECK(!simlink::decode(bad, n, q));       // not ours

    std::memcpy(bad, buf, n);
    bad[24] = 9;
    CHECK(!simlink::decode(bad, n, q));       // unknown engine state

    // A newer VEMD appending fields: an old listener still reads v1 in place.
    std::uint8_t newer[80] = {};
    std::memcpy(newer, buf, n);
    newer[4] = 2;   // version 2
    newer[6] = 80;  // longer datagram
    CHECK(simlink::decode(newer, sizeof(newer), q));
    CHECK(q.seq == p.seq && q.ngPercent == p.ngPercent);
}

// The VEMD's NG leaves as a 20 Hz staircase; it must reach the engine as a curve.
void testInterpolationRemovesTheStaircase() {
    RecordingSink  sink;
    SimLinkAdapter link(sink, LinkConfig{0.060, 1.0});

    const double senderToLocal = 3.0;  // clocks with different origins
    const double rate = 2.0;           // NG %/s
    std::uint32_t seq = 0;
    std::uint32_t lcg = 1u;
    double        nextSend = 0.0;

    std::vector<std::pair<double, float>> out;  // (local time, NG handed over)
    for (int step = 0; step < 2000; ++step) {   // 5 ms host loop, 10 s
        const double local = step * 0.005;
        // Deliver every packet whose (jittered) arrival time has come.
        while (true) {
            lcg = lcg * 1664525u + 1013904223u;
            const double jitter = 0.002 + 0.004 * (lcg >> 8) / 16777216.0;  // 2-6 ms
            const double arrival = nextSend + senderToLocal + jitter;
            if (arrival > local) break;
            link.onPacket(packet(seq++, nextSend, EngineState::Starting,
                                 static_cast<float>(20.0 + rate * nextSend)),
                          arrival);
            nextSend += 0.05;
        }
        const std::size_t before = sink.ng.size();
        link.update(local);
        if (sink.ng.size() > before) out.emplace_back(local, sink.ng.back());
    }

    double worstStep = 0.0, worstLag = 0.0;
    for (std::size_t i = 1; i < out.size(); ++i) {
        if (out[i].first < 4.0) continue;  // let the clock estimate settle
        worstStep = std::max(worstStep, std::fabs(double(out[i].second) - out[i - 1].second));
        // Expected: the sender's NG, played ~playoutDelay plus the fastest
        // network delay behind real time.
        const double expected = 20.0 + rate * (out[i].first - senderToLocal - 0.002 - 0.060);
        worstLag = std::max(worstLag, std::fabs(out[i].second - expected));
    }
    std::printf("interpolation: worst step %.4f %% per 5 ms (staircase would be %.2f), "
                "worst error vs delayed truth %.3f %%\n", worstStep, rate * 0.05, worstLag);
    CHECK(worstStep < rate * 0.005 * 2.0);  // within 2x an ideal 5 ms increment
    CHECK(worstLag < 0.02);
}

void testEventsFromState() {
    using simlink::flags::kGeneratorOnline;

    // Off -> start -> generator -> idle -> off, with every other packet lost.
    RecordingSink  sink;
    SimLinkAdapter link(sink);
    std::uint32_t  seq = 0;
    double         t = 0.0;
    const auto send = [&](EngineState st, float ng, std::uint32_t starts, std::uint8_t f = 0) {
        if (seq % 2 == 0) link.onPacket(packet(seq, t, st, ng, starts, f), t + 10.0);
        ++seq;
        t += 0.05;
    };
    for (int i = 0; i < 6; ++i) send(EngineState::Off, 0.0f, 0);
    for (int i = 0; i < 10; ++i) send(EngineState::Starting, 5.0f + i, 1);
    for (int i = 0; i < 6; ++i) send(EngineState::Starting, 52.0f, 1, kGeneratorOnline);
    for (int i = 0; i < 6; ++i) send(EngineState::GroundIdle, 68.0f, 1, kGeneratorOnline);
    for (int i = 0; i < 6; ++i) send(EngineState::Off, 60.0f, 1);

    const std::vector<EventType> expected = {EventType::EngineStartCommand,
                                             EventType::GeneratorOnline, EventType::IdleReached,
                                             EventType::EngineShutdown};
    CHECK(sink.types() == expected);
    CHECK(sink.events.front().second == 5.0f);  // start carries the NG it was heard at

    // Restart with no Off in between (R then S inside one sensor period).
    RecordingSink  r;
    SimLinkAdapter restart(r);
    restart.onPacket(packet(0, 0.00, EngineState::GroundIdle, 68.0f, 1), 0.0);
    restart.onPacket(packet(1, 0.05, EngineState::Starting, 0.5f, 2), 0.05);
    const std::vector<EventType> restarted = {EventType::StartSequenceComplete,
                                              EventType::EngineShutdown,
                                              EventType::EngineStartCommand};
    CHECK(r.types() == restarted);

    // Duplicates and packets overtaken on the way are ignored.
    RecordingSink  s;
    SimLinkAdapter stale(s);
    stale.onPacket(packet(10, 1.00, EngineState::Starting, 30.0f), 0.0);
    stale.onPacket(packet(9, 0.95, EngineState::Starting, 29.0f), 0.01);
    stale.onPacket(packet(10, 1.00, EngineState::Starting, 30.0f), 0.02);
    CHECK(stale.stats().stale == 2);
    CHECK(s.events.size() == 1);
}

void testWatchdogAndSenderRestart() {
    RecordingSink  sink;
    SimLinkAdapter link(sink, LinkConfig{0.060, 1.0});
    std::uint32_t  seq = 0;
    for (double t = 0.0; t < 2.0; t += 0.05) {
        link.onPacket(packet(seq++, t, EngineState::GroundIdle, 68.0f), t);
        link.update(t);
    }
    CHECK(link.stats().linkUp);
    link.update(2.5);  // 0.55 s of silence: still up
    CHECK(link.stats().linkUp);
    link.update(3.2);  // > 1 s: VEMD gone
    CHECK(!link.stats().linkUp);
    link.update(4.0);
    const std::vector<EventType> expected = {EventType::StartSequenceComplete,
                                             EventType::EngineShutdown};
    CHECK(sink.types() == expected);  // exactly one shutdown

    // A new VEMD process: clock and sequence start again from zero.
    RecordingSink  r;
    SimLinkAdapter again(r);
    again.onPacket(packet(500, 30.0, EngineState::GroundIdle, 68.0f), 0.0);
    again.onPacket(packet(0, 0.0, EngineState::Off, 0.0f, 0), 0.1);
    again.onPacket(packet(1, 0.05, EngineState::Starting, 0.3f, 1), 0.15);
    const std::vector<EventType> restarted = {EventType::StartSequenceComplete,
                                              EventType::EngineShutdown,
                                              EventType::EngineStartCommand};
    CHECK(r.types() == restarted);
    CHECK(again.stats().stale == 0);
}

void testLoopbackSocket() {
    simlink::UdpReceiver rx;
    std::string          error;
    CHECK(rx.open(0, "127.0.0.1", &error));
    if (!rx.isOpen()) {
        std::fprintf(stderr, "loopback: %s\n", error.c_str());
        return;
    }
    simlink::UdpSender tx;
    CHECK(tx.open("127.0.0.1", rx.port(), &error));

    std::uint8_t out[64];
    const std::size_t n = simlink::encode(packet(77, 1.5, EngineState::Starting, 33.0f), out, sizeof(out));
    CHECK(tx.send(out, n));

    std::uint8_t in[128];
    const int    got = rx.receive(in, sizeof(in), 1000);
    CHECK(got == static_cast<int>(n));
    EnginePacket p;
    CHECK(got > 0 && simlink::decode(in, static_cast<std::size_t>(got), p));
    CHECK(p.seq == 77 && p.ngPercent == 33.0f);
    CHECK(rx.receive(in, sizeof(in), 20) == 0);  // nothing else: timeout

    std::string   host = "unchanged";
    std::uint16_t port = 1;
    CHECK(simlink::parseEndpoint("raspberrypi.local:5000", host, port));
    CHECK(host == "raspberrypi.local" && port == 5000);
    CHECK(simlink::parseEndpoint("127.0.0.1", host, port));
    CHECK(host == "127.0.0.1" && port == 5000);
    CHECK(!simlink::parseEndpoint("host:99999", host, port));
    CHECK(!simlink::parseEndpoint(":49350", host, port));
    CHECK(host == "127.0.0.1" && port == 5000);  // failures change nothing
}

// The phase the aircraft is in, derived from the state the VEMD reports.
void testPhaseDerivation() {
    using soundsys::EnginePhase;
    using simlink::flags::kTwistGripFlight;

    RecordingSink  sink;
    SimLinkAdapter link(sink, LinkConfig{0.060, 1.0, 30.0, 22.0});
    std::uint32_t  seq = 0;
    double         t = 0.0;
    const auto step = [&](EngineState st, float ng, std::uint8_t f, float collective) {
        EnginePacket p = packet(seq++, t, st, ng, 1, f);
        p.collectivePercent = collective;
        link.onPacket(p, t);
        link.update(t);
        t += 0.05;
        return sink.phase;
    };

    CHECK(step(EngineState::Off, 0.0f, 0, 0.0f) == EnginePhase::Off);
    CHECK(step(EngineState::Starting, 20.0f, 0, 0.0f) == EnginePhase::Start);
    CHECK(step(EngineState::GroundIdle, 68.0f, 0, 0.0f) == EnginePhase::Idle);
    CHECK(step(EngineState::GroundIdle, 79.0f, kTwistGripFlight, 5.0f) == EnginePhase::Flight);
    // Collective up: a takeoff, as far as the sound is concerned...
    CHECK(step(EngineState::GroundIdle, 90.0f, kTwistGripFlight, 35.0f) == EnginePhase::Takeoff);
    // ...and it does not fall back at the first wobble of the lever.
    CHECK(step(EngineState::GroundIdle, 88.0f, kTwistGripFlight, 25.0f) == EnginePhase::Takeoff);
    CHECK(step(EngineState::GroundIdle, 80.0f, kTwistGripFlight, 18.0f) == EnginePhase::Flight);
    // Engine off after running: the shutdown gets heard out, and stays.
    CHECK(step(EngineState::Off, 40.0f, 0, 0.0f) == EnginePhase::Shutdown);
    CHECK(step(EngineState::Off, 0.0f, 0, 0.0f) == EnginePhase::Shutdown);
    // The VEMD going quiet is not a shutdown: just stop.
    link.update(t + 2.0);
    CHECK(sink.phase == EnginePhase::Off);
}

}  // namespace

int main() {
    testWireFormat();
    testPhaseDerivation();
    testInterpolationRemovesTheStaircase();
    testEventsFromState();
    testWatchdogAndSenderRestart();
    testLoopbackSocket();
    return test::summary("simlink");
}
