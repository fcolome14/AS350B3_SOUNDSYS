# simlink

The wire contract between the simulator's modules. Today one producer (the
VEMD, which owns the engine model) and one consumer (the sound system); the
format has no knowledge of either.

A small CMake project on its own - one header for the packet, one for UDP, one
`.cpp` - with no dependency on the audio engine, so a module can pull in just
this directory:

```cmake
# sibling checkout
add_subdirectory(../AS350B3_SOUNDSYS/simlink ${CMAKE_BINARY_DIR}/simlink)
# or from git
FetchContent_Declare(simlink GIT_REPOSITORY https://github.com/fcolome14/AS350B3_SOUNDSYS.git
                     GIT_TAG <tag> SOURCE_SUBDIR simlink)
FetchContent_MakeAvailable(simlink)

target_link_libraries(my_module PRIVATE simlink::simlink)
```

## Transport

UDP, default port **49350**, one datagram per **engine sensor sample** (20 Hz
in the VEMD). Localhost by default; the listener binds `0.0.0.0`, so the VEMD
can run on another machine (e.g. the Raspberry Pi panel) and point at the PC
running the sound.

## `EnginePacket` v1

| Field | Type | Meaning |
|---|---|---|
| `seq` | u32 | +1 per datagram; the listener drops duplicates and late arrivals |
| `simTime` | f64 | sender clock **at the sensor sample**, s - not the send time |
| `startCount` | u32 | +1 on every START |
| `state` | u8 | `Off`, `Starting`, `GroundIdle` |
| `flags` | u8 | starter engaged, generator online, twist grip FLIGHT, ENG PARAM OVER LIMIT |
| `ngPercent` | f32 | NG, **continuous** (not the 0.1 % display digit) |
| `nrPercent` | f32 | NR, % of nominal |
| `t4Celsius` | f32 | TOT |
| `torquePercent` | f32 | TRQ |
| `collectivePercent` | f32 | lever position, 0 = down |

Byte layout and encoding rules are in the header comment of
[`include/simlink/EnginePacket.hpp`](include/simlink/EnginePacket.hpp).

## Rules both sides rely on

1. **Every datagram is the whole state.** No deltas, no one-shot messages. A
   lost datagram costs one sample of resolution; discrete events (START, idle,
   generator online, shutdown) are reconstructed by the listener from the
   difference between two states, with `startCount` distinguishing a restart
   that never passed through `Off`.
2. **Timestamps are sample times.** The listener plays samples back a fixed
   delay behind the sender clock and interpolates, so it needs to know when a
   value was true, not when it left.
3. **Additive versioning.** New fields are appended and `version` bumped; a
   listener reads the v1 fields of any newer datagram. Changing the meaning or
   position of an existing field needs a new `kMagic`.
4. **Fire and forget.** The sender never waits for a listener and never learns
   whether one exists.
