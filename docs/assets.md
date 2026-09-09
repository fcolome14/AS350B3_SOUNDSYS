# From raw AS350B3 recordings to runtime assets

The pipeline takes real recordings - a cabin start, a stretch of ground idle,
some cruise, a shutdown - and produces exactly two kinds of artefact:

* a **start recording** with an **anchor table** (`.wav` + `.json`), and
* **seamless loops** for every phase that holds still.

Nothing is synthesised anywhere in this pipeline.

## 0. Install the tool

```bash
cd tools/ngmap
pip install -e .          # numpy + scipy only
```

## 1. Look at what you have

```bash
python -m ngmap analyze recordings/cabin_start.wav --csv start_track.csv
```

Prints duration, the tracked fundamental range and the confidence. The track is
the gas generator whine; `--fmin/--fmax` bracket it if something else in the
cabin is louder, and `--harmonics` says how many partials to sum (8 is a good
default for a turbine, whose fundamental is often masked by airframe rumble
while its harmonics are clean).

If you know one operating point - ground idle is the easy one - give it as
`--calibrate NG@SECONDS` and every reading is reported in NG% instead of Hz:

```bash
python -m ngmap analyze recordings/cabin_start.wav --calibrate 67@41.5
```

## 2. Cut a long recording into phases

```bash
python -m ngmap phases recordings/flight.wav --calibrate 67@120 --extract work/
```

Classifies every frame as `spool_up` / `steady` / `spool_down` / `quiet`, merges
runs, prints the list with the NG each one sat at, and (with `--extract`) writes
each phase to its own file with short fades at the seams.

## 3. Build the anchor table for the start

With telemetry (the accurate route - use the 20 Hz 2B1 trace):

```bash
python -m ngmap anchors recordings/cabin_start.wav \
    --trace telemetry/start_2b1.csv \
    --out assets/anchors/arriel2b1_start.json
```

It aligns the recording to the trace by correlating the audio's own NG readout
against the logged NG (recorder and data log are never started together), finds
the landmarks in the telemetry - starter engage, light-off from peak dT4/dt,
igniters off and generator online at their NG thresholds, idle at the plateau -
and writes them as `(NG, time-into-the-recording)` pairs. The threshold events
are `--ignition-ng` / `--generator-ng` if your variant differs.

Without telemetry (library material):

```bash
python -m ngmap anchors recordings/library_start.wav --calibrate 67@38 \
    --out assets/anchors/library_start.json
```

Here the anchors come from the audio alone: calibrated, the pitch track *is* the
NG track, so the table is just "when did this recording pass each NG".

Either way the command finishes with a self-check - the NG the table claims for
each instant against the NG the audio is actually singing - and refuses to write
a table that is not strictly increasing on both axes.

## 4. Check a table you already have

```bash
python -m ngmap validate assets/anchors/arriel2b1_start.json \
    recordings/cabin_start.wav --calibrate 67@41.5
```

Under about 2 % rms is good. A large error confined to one segment means the
anchor at that end of the segment is in the wrong place.

## 5. Make the loops

```bash
python -m ngmap loop work/03_steady_67.wav --region 4:28 --seconds 5 \
    --out assets/wav/idle_loop.wav
```

Fixes the loop start, then picks the end whose waveform best matches it, so the
splice lands on the same phase of the same rotation; the file is written with an
equal-power crossfade already applied, so `LoopVoice` just wraps and needs no
loop metadata. A splice correlation below ~0.5 is reported as a warning - try
another region or another length rather than shipping a tick.

## 6. Hear it against a simulated start

```bash
build/bin/Release/soundsys_render --wav assets/wav/cabin_start.wav \
    --anchors assets/anchors/arriel2b1_start.json \
    --speed 1.4 --idle-loop assets/wav/idle_loop.wav \
    --out /tmp/start_x1.4.wav --csv /tmp/sync.csv
```

`--speed` replays the recording's own NG profile that many times faster, which
is the quickest way to hear whether the table holds up; `--trace` plays a real
NG trace instead. `sync.csv` has the target position, the actual position, the
error and the playback speed per block - a bad anchor shows up as a step in the
error column.

## Layout

```
assets/
  wav/            recordings and loops (float32 or PCM WAV, any sample rate)
  anchors/        the JSON tables, one per start recording
```

Keep the anchor table next to the recording it was built from, and rebuild it if
the recording is ever re-trimmed: the loader compares the table's end against
the file's duration and refuses the pair if the table runs past it, but it
cannot catch a trim at the front.
