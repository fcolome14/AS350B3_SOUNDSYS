# AS350B3_SOUNDSYS

Sound system module for the AS350 B3 simulator: a small, event-driven audio
engine in C++17 that plays **real aircraft recordings** in step with the
simulated engine, plus an offline Python tool that turns raw recordings into the
assets it consumes.

No game engine, no audio middleware. Three third-party pieces, all fetched and
pinned by CMake: `libsamplerate` (resampling), `miniaudio` (device backend),
`dr_wav` (WAV I/O). The module builds and passes its tests without any of them.

## What it does

The hard part is the start sequence. One real Arriel 2B1 start has to serve
every start the simulator produces - faster, slower, aborted, hung - staying
locked to the NG on the gauges. The trick is that a turbine's tone is a function
of shaft speed, so the recording is a function of NG rather than of time: an
**anchor table** (`NG% <-> time into the recording`) says where in the recording
any given NG lives, and the playback head is driven there by rate, never by
jumping.

That also means the whine in the recording *is* a reading of NG, which is how
the Python tool builds and checks those tables - and how it can build one from
library material that came with no telemetry at all.

Two ways to move the head are provided, and they do not sound the same;
[docs/sync.md](docs/sync.md) has the measurements. The default,
`PlaybackMode::PitchLocked`, keeps the pitch the recording had at each NG, which
is what the engine does. `PlaybackMode::Varispeed` is the cheaper turntable
approach: identical at the recorded rate, and off by exactly the playback rate
as soon as the simulated start differs.

## Architecture

```
AudioEngine          mixes voices, owns the clock, knows nothing about turbines
 |- lock-free SPSC queue   discrete events   (EngineStartCommand, GeneratorOnline...)
 |- atomic parameter store continuous inputs (NG%, NR%, T4, torque...)
 |- Limiter                master bus safety
 |
 +- ISoundVoice       one class per sound; add a voice, change nothing else
     |- TurbineStartVoice  the real start recording, driven by the anchor table
     +- LoopVoice          a seamless loop pitched by a parameter (idle, rotor)
```

The simulator only ever does two things: `pushEvent(...)` and
`setParameter(...)`. Neither blocks, neither allocates, and the audio thread
never takes a lock.

```cpp
soundsys::AudioEngine engine;

soundsys::TurbineStartConfig cfg;
cfg.wavPath     = "assets/wav/cabin_start.wav";
cfg.anchorsPath = "assets/anchors/arriel2b1_start.json";
auto voice = std::make_unique<soundsys::TurbineStartVoice>(cfg);
voice->load(&error);
engine.addVoice(std::move(voice));

soundsys::MiniaudioDevice device;
device.start(engine, 48000.0, 2, &error);   // prepares the engine at the device rate

engine.pushEvent(soundsys::EventType::EngineStartCommand);
// then, every physics tick:
engine.setParameter(soundsys::ParamId::NgPercent, ng);
```

## Running against the VEMD

In the simulator this module is its own process, `soundsys_host`, fed by the
VEMD (AS350B3_VEMD, which owns the engine model) over UDP through the small
[`simlink`](simlink/README.md) contract. Neither project builds against the
other's code. How it stays in sync, how to run it and what was measured end to
end: [docs/integration.md](docs/integration.md).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cd build && ctest -C Release --output-on-failure
```

Options: `SOUNDSYS_USE_LIBSAMPLERATE`, `SOUNDSYS_USE_MINIAUDIO`,
`SOUNDSYS_BUILD_APPS`, `SOUNDSYS_BUILD_TESTS` (all `ON`). With both libraries off
the module still builds, runs and passes its tests on built-in code paths.

Two demo programs stand in for the simulator:

```bash
build/bin/Release/soundsys_render --wav rec.wav --anchors anchors.json \
    --speed 1.4 --out out.wav --csv sync.csv    # offline, no audio device
build/bin/Release/soundsys_play   --wav rec.wav --anchors anchors.json --speed 0.7
```

## The offline tool

`tools/ngmap` analyses real recordings: pitch/NG tracking, phase segmentation,
anchor-table construction and validation, loop-point extraction. numpy and scipy
only, managed with [uv](https://docs.astral.sh/uv/) — the repository root is a uv
workspace whose single member is that tool, so one sync sets everything up:

```bash
uv sync
uv run ngmap anchors recordings/cabin_start.wav --trace telemetry/start.csv --out assets/anchors/arriel2b1_start.json
```

`uv sync --extra plots` adds matplotlib, which is only ever used for looking at
tracks by eye. The C++ dependencies stay with CMake — uv does not manage those.

Full workflow in [docs/assets.md](docs/assets.md).

## Tests

`tests/test_anchor_table.cpp` covers the map, its inverse, its slope and the
invariants that keep a bad table off the audio thread.

`tests/test_sync.cpp` is the interesting one: it builds a signal whose pitch is a
known multiple of NG - the property a real turbine recording has - drives a start
through the engine at 0.65x, 1.0x and 1.6x the recorded spool rate, and measures
the pitch of what comes out. It asserts that the pitch-locked head reproduces
`k x NG` at every rate, that the varispeed head is off by exactly the playback
rate, and that a hung start holds its tone.

## Layout

```
include/soundsys/   public headers
src/                implementation
apps/               offline renderer and live demo
tests/              self-contained tests, no framework
tools/ngmap/        offline analysis of real recordings (Python)
assets/             wav/ recordings and loops, anchors/ the JSON tables
docs/               sync.md (the design), assets.md (the pipeline)
```
