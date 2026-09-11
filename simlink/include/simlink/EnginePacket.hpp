// simlink/EnginePacket.hpp - the wire contract between the VEMD and its listeners.
//
// One UDP datagram per engine sensor sample (20 Hz in the VEMD). Every datagram
// carries the COMPLETE engine state, never a delta, so a lost datagram costs
// 50 ms of resolution and nothing else: discrete things - a START, the
// generator coming online - are recovered by the receiver as the difference
// between two states.
//
// Header-only and dependency-free, so both sides compile the same definition.
// Explicit little-endian byte order at fixed offsets: no struct memcpy, so
// padding and compiler differences (x86 desktop, ARM Raspberry Pi) cannot
// change the layout.
//
// Wire layout, version 1 (48 bytes):
//
//   off  size  field
//     0     4  magic        "A35S"
//     4     2  version      1
//     6     2  size         bytes in this datagram (>= 48; newer versions append)
//     8     4  seq          +1 per datagram, wraps
//    12     8  simTime      sender clock at the sensor sample, seconds (f64)
//    20     4  startCount   +1 on every START
//    24     1  state        EngineState
//    25     1  flags        flags::*
//    26     2  reserved     0
//    28     4  ngPercent    f32
//    32     4  nrPercent    f32
//    36     4  t4Celsius    f32
//    40     4  torquePercent      f32
//    44     4  collectivePercent  f32
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace simlink {

inline constexpr std::uint32_t kMagic = 0x53353341u;  // 'A' '3' '5' 'S' in wire order
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t   kWireSize = 48;
inline constexpr std::uint16_t kDefaultPort = 49350;

enum class EngineState : std::uint8_t {
    Off = 0,
    Starting = 1,
    GroundIdle = 2,  // start sequence finished; IDLE/FLIGHT is the twist-grip flag
};

namespace flags {
inline constexpr std::uint8_t kStarterEngaged = 1u << 0;
inline constexpr std::uint8_t kGeneratorOnline = 1u << 1;
inline constexpr std::uint8_t kTwistGripFlight = 1u << 2;
inline constexpr std::uint8_t kEngParamOverLimit = 1u << 3;
}  // namespace flags

struct EnginePacket {
    std::uint32_t seq = 0;
    double        simTime = 0.0;         // when the SENSOR sampled, not when it was sent
    std::uint32_t startCount = 0;        // a new start is visible even if no Off was seen
    EngineState   state = EngineState::Off;
    std::uint8_t  flags = 0;
    float         ngPercent = 0.0f;      // continuous NG, NOT the 0.1 % display digit
    float         nrPercent = 0.0f;      // rotor speed, % of nominal
    float         t4Celsius = 0.0f;
    float         torquePercent = 0.0f;
    float         collectivePercent = 0.0f;  // 0 = fully down, 100 = fully up

    bool has(std::uint8_t f) const { return (flags & f) != 0; }
};

namespace detail {

inline void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t get32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return v;
}
inline std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}
inline void putF32(std::uint8_t* p, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    put32(p, u);
}
inline float getF32(const std::uint8_t* p) {
    const std::uint32_t u = get32(p);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

}  // namespace detail

// Serialises into `out`. Returns the number of bytes written, or 0 if `capacity`
// is too small.
inline std::size_t encode(const EnginePacket& p, std::uint8_t* out, std::size_t capacity) {
    if (capacity < kWireSize) return 0;
    using namespace detail;
    std::memset(out, 0, kWireSize);
    put32(out + 0, kMagic);
    put16(out + 4, kVersion);
    put16(out + 6, static_cast<std::uint16_t>(kWireSize));
    put32(out + 8, p.seq);
    std::uint64_t t;
    std::memcpy(&t, &p.simTime, sizeof(t));
    put64(out + 12, t);
    put32(out + 20, p.startCount);
    out[24] = static_cast<std::uint8_t>(p.state);
    out[25] = p.flags;
    putF32(out + 28, p.ngPercent);
    putF32(out + 32, p.nrPercent);
    putF32(out + 36, p.t4Celsius);
    putF32(out + 40, p.torquePercent);
    putF32(out + 44, p.collectivePercent);
    return kWireSize;
}

// Parses a datagram. Rejects anything that is not a simlink engine packet of a
// version we can read. A NEWER version is accepted as long as it keeps the v1
// fields in place (new fields only ever get appended), so an old listener keeps
// working against a newer VEMD.
inline bool decode(const std::uint8_t* in, std::size_t length, EnginePacket& out) {
    using namespace detail;
    if (length < 8 || get32(in) != kMagic) return false;
    const std::uint16_t version = get16(in + 4);
    const std::uint16_t size = get16(in + 6);
    if (version < 1 || size < kWireSize || size > length) return false;
    if (in[24] > static_cast<std::uint8_t>(EngineState::GroundIdle)) return false;

    out.seq = get32(in + 8);
    const std::uint64_t t = get64(in + 12);
    std::memcpy(&out.simTime, &t, sizeof(t));
    out.startCount = get32(in + 20);
    out.state = static_cast<EngineState>(in[24]);
    out.flags = in[25];
    out.ngPercent = getF32(in + 28);
    out.nrPercent = getF32(in + 32);
    out.t4Celsius = getF32(in + 36);
    out.torquePercent = getF32(in + 40);
    out.collectivePercent = getF32(in + 44);
    return true;
}

}  // namespace simlink
