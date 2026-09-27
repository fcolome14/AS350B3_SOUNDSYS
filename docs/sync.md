# Keeping a real recording in sync with a simulated start

## The problem

We have one real Arriel 2B1 start: a cockpit recording plus a ~20 Hz telemetry
trace (NG%, T4, torque). The simulator produces starts that are *not* that one -
faster on a warm engine, slower on a cold day, occasionally hung. The recording
has to follow whatever NG the flight model produces, without drifting out of
step with the gauges and without sounding wrong.

## The physical fact everything rests on

A turbine's tonal content is locked to shaft speed. Blade-passing frequency is
`blades x RPM`, so at NG = 50 % the engine sounds the same whether it took 10 s
or 30 s to get there. Two consequences:

1. **The recording is a function of NG, not of time.** If we always sit at the
   point of the recording whose NG matches the simulated NG, the sound is right.
   That is what the anchor table is for.
2. **The whine in the recording is a reading of NG.** Track its frequency and
   you have a second NG trace, straight from the audio - which is how
   `tools/ngmap` builds and checks anchor tables, and how it can build one from
   library material that came with no telemetry at all.

## The anchor table

Marking 4-6 acoustic landmarks - starter engage, light-off, igniters off,
generator online, idle - and reading the NG the telemetry had at each gives a
piecewise map `NG% <-> time into the recording`. Between anchors we interpolate
linearly, which is accurate because the anchors are placed exactly where the
slope of NG(t) changes.

The table must be strictly increasing on both axes; `AnchorTable::setAnchors`
rejects anything else, because a non-invertible table means the playback head
would have to jump backwards mid-start.

## Driving the head

Each block, the voice asks the table where the simulated NG puts it, and drives
the *rate*, never the position:

    speed = d(target)/dt            feed-forward: the rate NG demands
          + kP * (target - actual)  feedback: absorbs drift, kP small

Jumping the head straight to the target would click and glissando. The
feed-forward term alone gets it right; the proportional term only cleans up
rounding and event-timing slop, so its gain can stay too low to hear.
Measured sync error on the test signal: **16-35 ms** across spool rates from
0.65x to 1.6x.

## Varispeed vs pitch-locked: the part that was not obvious

The original plan was varispeed - read the tape faster or slower, like a
turntable, letting pitch and duration couple naturally "as they do in the real
turbine". Half of that is right: pitch and duration *are* coupled in a
turntable, and it is cheap and artefact-free. But it is not what the engine
does, and `tests/test_sync.cpp` measures the difference:

| spool rate | what the engine would sound like | varispeed produces |
|-----------:|---------------------------------:|-------------------:|
| 1.0x       | `k x NG`                         | `k x NG`           |
| 1.6x       | `k x NG`                         | `1.6 x k x NG`     |
| 0.65x      | `k x NG`                         | `0.65 x k x NG`    |

Varispeed multiplies the pitch by the playback rate. At 1.0x that is invisible,
which is why the idea looks sound; at 1.6x the start comes out a major sixth
sharp and hands over to the idle loop at the wrong pitch entirely.

There is a second failure with no rate involved at all: **a plateau**. If NG
stops moving - a hung start, or simply arriving at idle - the target position
stops, so the head stops, so the tape stops and the tone dies. The real engine
sits there humming.

So the module ships two heads behind one interface
(`soundsys/PlaybackHead.hpp`):

- **`PlaybackMode::PitchLocked` (default)** - the position follows NG at any
  rate including zero, while the audio is rebuilt from overlapping grains that
  always play at their recorded rate (WSOLA: each grain is nudged within a small
  search window to the offset that best continues the previous one, which is
  what keeps tonal material from phasing). Pitch = the pitch the recording had
  at that NG, full stop. A hung start holds its tone.
- **`PlaybackMode::Varispeed`** - the original design. Cheaper, no grain
  artefacts at all, and exactly right whenever the simulated start closely
  matches the recorded one. Worth using if your start model is tightly fitted to
  the recorded start, or as an A/B reference.

Both are one line apart in `TurbineStartConfig`, and both are covered by the
test, which asserts the pitch relationship for each rather than leaving the
trade-off as a footnote.

## Plateaus on real recordings

Two things the synthetic test signal hid and the real 2B1 recording exposed:

- **A stopped head re-reads the same grain.** With a pure tone that is
  harmless; with real broadband noise, the same 40 ms of tape every 20 ms is
  heard as a buzz at the hop rate. `TimeStretchConfig::jitterMs` scatters each
  grain's read point by up to 30 ms, scaled by how far below real time the head
  is moving (nothing at speed >= 1); WSOLA still aligns the tonal part. The
  position itself never moves because of it. Guarded by the "noise at zero
  speed" check in `tests/test_sync.cpp` (self-similarity at the hop lag must
  stay under 0.3; it measures ~0.003).
- **Idle is not a plateau to hold, it is recording to play.** Once NG reaches
  the last anchor, the table has nothing more to say and the rest of the file is
  the engine sitting at idle. `TurbineStartVoice` then free-runs at 1x through
  that recorded idle - correct tone, nothing stretched - and hands over to the
  idle loop near the end of the file as before.

## Grain seams on real recordings

Driven live by the VEMD, the first pitch-locked version buzzed whenever the
head moved at other than 1x - i.e. every time it followed NG. Measured on the
envelope of the 1-8 kHz band, against the same recording played at 1x:
**+20.7 dB of modulation at 50 Hz**, the grain rate. Two causes, both fixed:

1. **The alignment search was too coarse.** It tried 33 offsets 15 samples
   apart; the whine the ear follows has a period of 6-40 samples, so its
   partials landed half out of phase at every seam. The search is now
   sample-accurate (coarse pass every 4 samples, then every sample around the
   winner) and matches on the pre-emphasised signal, so the whine decides the
   alignment rather than the rumble.
2. **A Hann crossfade loses power on noise.** It sums aligned signals to unity
   but two unrelated noise segments dip 3 dB in the middle of every overlap -
   and the broadband part of a real recording read from two places is exactly
   that. The fade now measures the correlation `r` between the outgoing and
   incoming halves and scales itself to keep power constant
   (`k = 1/sqrt((1-s)^2 + s^2 + 2 r s (1-s))`): a plain Hann for `r = 1`, an
   equal-power fade for `r = 0`. This was the bigger of the two: with the
   precise search alone the modulation only fell to +16.5 dB, and changing the
   grain size just moved it (+19-21 dB at 20, 25 and 33 Hz).

With both, 60 ms grains leave **+6.4 dB** at the grain rate - under the
metric's own floor (varispeed, which has no grains at all, scores +8.9 dB at
an arbitrary frequency). The bench is a render of the bridged 2B1 start driven
by the NG trace of the VEMD model.

## After the start: steady phases

Once NG reaches the last anchor, the start voice announces
`StartSequenceComplete` and crossfades out over 2 s on its own recorded idle,
while `NgLoopBankVoice` fades in. The bank holds seamless loops recorded at
known NGs (ground idle 68.0 %, flight 80.7 % for the 2B1) and, at any NG,
plays the two that bracket it - each varispeed-pitched by NG / its own NG,
equal-power crossfaded by distance. That is where varispeed is the right tool:
around a recorded NG the ratio stays near 1, so the pitch error that ruled it
out for the start does not arise, and there are no grains. It covers holding
idle indefinitely, the twist grip to FLIGHT and back, and collective in flight,
all from the VEMD's NG. NG is smoothed at 6 Hz inside the bank, because a step
in a varispeed ratio is a step in pitch.

## Why not a music-grade time stretcher

Because we do not need one. A phase vocoder is built to stretch arbitrary
polyphonic material by large factors; here the material is one strongly periodic
machine tone, stretch factors are modest, and WSOLA on 60 ms grains costs one
short correlation search per grain (about 33 per second) and a few
multiply-adds per sample. No FFT, no middleware. And it only runs during the
start: every steady phase is plain looped playback.
