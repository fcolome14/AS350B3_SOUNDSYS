# Running the sound against the VEMD

The VEMD (AS350B3_VEMD) owns the engine model and all its logic; this module
owns the sound. They are separate projects, separate builds and separate
processes, joined by one small contract: [`simlink`](../simlink/README.md).

```
AS350B3_VEMD process                         AS350B3_SOUNDSYS process (soundsys_host)
--------------------                         ----------------------------------------
VemdModel  (20 Hz sensor tick)               UdpReceiver
   |                                             |
SimLinkPublisher  ---- UDP :49350 ---->      SimLinkAdapter
   one EnginePacket per sensor sample            - re-time + interpolate NG, NR, T4...
   (full state, sample timestamp)                - START / idle / generator / shutdown
                                                   derived from state changes
                                                 - watchdog: VEMD silent -> shutdown
                                                 |
                                             AudioEngine -> voices -> miniaudio
```

## Why two processes

- **Independence.** The VEMD does not link, include or know about the audio
  engine; it publishes and forgets. The sound can be rebuilt, restarted or
  replaced without touching the VEMD, and a crash in one leaves the other up.
- **Placement.** The VEMD is meant to run on a Raspberry Pi panel; the sound
  can run on the same Pi or on the PC with the speakers - only the target
  address changes (`SIMLINK_TARGET`).
- **Language-agnostic.** Anything that writes the 48-byte packet can drive
  the sound (a flight model, a replay tool, a Python test harness).

If you ever want both in one process, `SimLinkAdapter` does not care where
packets come from: call `onPacket()` directly instead of reading a socket.

## What "in sync" takes

1. **Send the engine, not the display.** The packet carries the continuous NG
   (`ngFiltered`), not the 0.1 % digit that refreshes every 0.2 s. A 5 Hz
   staircase would make the start voice stop and lurch.
2. **Timestamp the sample, then play it back late.** Each packet carries the
   VEMD clock at its sensor sample. The adapter plays samples 60 ms behind that
   clock (more than one 50 ms sensor period, plus jitter) and interpolates
   between them, so NG reaches the engine as a curve. Net lag, VEMD model to
   loudspeaker: ~60 ms playout + ~10 ms device buffer - well under the 200 ms
   refresh of the NG digit.
3. **Events are state, not messages.** A lost datagram costs 50 ms of
   resolution, never a START:

   | Change between two packets | Event |
   |---|---|
   | `Off` -> running, or `startCount` changed | `EngineStartCommand` (with the current NG) |
   | first packet heard is already `GroundIdle` | `StartSequenceComplete` (idle voices directly) |
   | `Starting` -> `GroundIdle` | `IdleReached` |
   | generator flag set | `GeneratorOnline` |
   | running -> `Off`, or no packets for 1 s | `EngineShutdown` |

4. **Same NG scale on both sides.** An anchor table maps NG to a position in a
   recording, so its NG axis must be the VEMD's. The VEMD's ground idle is
   **68.0 %** (`startup_profile::kNgGroundIdle`); tables built with another
   idle value have to be rescaled (the bridged 2B1 start was built at an
   assumed 67 % and is used through its `*.vemd.anchors.json` copy).

## Running it

Build both (each with its own CMake; the VEMD finds `../AS350B3_SOUNDSYS/simlink`
by itself). Then, sound first:

```bash
build/bin/Release/soundsys_host.exe --phase start=work/phases/start.wav,work/phases/idle_loop.wav --phase idle=,work/phases/idle_loop.wav --phase flight=work/phases/flight_engage.wav,work/phases/flight_loop.wav --phase takeoff=work/phases/takeoff.wav,work/phases/cruise_loop.wav --phase shutdown=work/phases/shutdown.wav
```

(Or from VS Code: `.vscode/launch.json` has it ready.) Each `--phase` gives the
recording of that phase: `<phase>=<one-shot>[,<loop>]`. The one-shot plays on
entering the phase and the loop holds it for as long as the aircraft stays
there, so idle lasts as long as the pilot wants and the twist grip to FLIGHT
plays its own recording. Nothing is stretched or pitched, so there is no
processing artefact to hear; the cost is that inside a phase the sound no
longer follows NG.

The phase comes from the VEMD's own state: `Starting` -> start, `GroundIdle` ->
idle, twist grip FLIGHT -> flight, collective past `takeoffCollective` ->
takeoff, engine off after running -> shutdown. Losing the link is not a
shutdown: the sound simply stops.

The older NG-driven path is still there, for when the sound has to track the
gauge rather than the phase (`--wav`/`--anchors`/`--loop`, see
[sync.md](sync.md)); the two are mutually exclusive.

and the VEMD, as usual (press `S` for START, `R` to stop, `G` twist grip):

```bash
../AS350B3_VEMD/build/Release/vemd_starter.exe
```

Without the panel window - same model, same publisher, START after 1 s:

```bash
../AS350B3_VEMD/build/Release/vemd_headless.exe --seconds 60
```

Useful host options: `--record session.wav` (keep what was played),
`--gain 0.5`, `--delay-ms 60`, `--bind 127.0.0.1`. By default the host listens on every
interface so a VEMD on another machine can reach it; the first time, Windows
may ask whether to allow `soundsys_host` through the firewall.

VEMD on the Pi, sound on the PC at 192.168.1.20:

```bash
SIMLINK_TARGET=192.168.1.20:49350 ./vemd_starter
```

## Measured end to end

`vemd_headless` (seed 12345, OAT 9.5 C: START at 3 s, twist grip to FLIGHT at
60 s, 88 s in all) driving `soundsys_host` with the bridged 2B1 start and the
idle/flight loop bank, over loopback. The host heard START at 2.4 s, idle at
43.1 s (NG 67.9 %), and shut the engine down when the VEMD exited. The recorded
main NG line against the frequency the VEMD's NG predicts (6497 Hz x NG / 68):

| Phase | Samples | Median | p10 - p90 |
|---|---|---|---|
| Start (NG 12 % to idle) | 32 | 0.998 | 0.965 - 1.007 |
| Idle held | 18 | 1.000 | 0.999 - 1.002 |
| Twist grip to FLIGHT (incl. the overshoot) | 12 | 1.000 | 0.998 - 1.001 |
| Flight held | 16 | 1.000 | 0.999 - 1.001 |

No gap anywhere while the engine ran: level never below -36 dBFS from START
to link loss (median -23.8 dBFS at gain 0.3), flat through the start-to-loops
handover and the twist grip. The sound follows the gauges, not a clock.

## Extending the contract

New inputs (bleed valve, fuel flow, a LightOff flag for its own voice...) are
appended to `EnginePacket`, `kVersion` goes up, and both sides keep working in
either order of upgrade: an old host reads the fields it knows from a newer
VEMD. New voices then read them as `ParamId`s like any other parameter.
