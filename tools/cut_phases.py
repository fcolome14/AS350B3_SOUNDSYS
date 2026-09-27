"""Cut one runtime asset per phase of flight out of cabin video audio.

    uv run --with av python tools/cut_phases.py

Input: the .m4a files listed in SOURCES, with the spans of each phase read off
the video. Output: work/phases/*.wav plus phases.json, ready for

    soundsys_host --phase start=work/phases/start.wav,work/phases/idle_loop.wav ...

One-shots are taken as recorded, with 30 ms fades at the ends. Loops have their
start and length chosen by ranking the seam: the wrap must not stand out from
any other moment of the same loop. Every asset keeps the relative level of its
own recording and is left 3 dB below full scale, so two of them crossfading
never reach the limiter.

PyAV is only needed for the AAC decode (libsndfile cannot read .m4a); once the
WAVs under work/phases/raw exist, --skip-decode runs without it.
"""
from __future__ import annotations

import json
import os
import sys

import numpy as np

from ngmap import audio
from ngmap.loops import find_loop, render_loop

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "work", "phases")
RAW = os.path.join(OUT, "raw")
HEADROOM = 10 ** (-3.0 / 20.0)

# name -> (file, the span we take assets from)
SOURCES = {
    # youtube.com/watch?v=YsGanu5yn5M, start at 03:39
    "idle_flight": ("C:/Users/ferra/Downloads/AS350B3e Start, IDLE, FLIGHT MODE.m4a", (219, 328)),
    # youtube.com/watch?v=bWyMVuVv3E4, start at 00:44
    "cruise_land": ("C:/Users/ferra/Downloads/AS350B3e Start, Cruise, Land.m4a", (44, 370)),
}

# The chain the sim flies today comes from one take ("idle_flight"); the other
# carries the phases it lacks and doubles as a second take.
ONE_SHOTS = [
    # name,                     source,        from,    to
    ("start.wav",               "idle_flight", 219.0,  264.0),  # switch on -> idle stabilised
    ("start_take2.wav",         "cruise_land",  44.0,   92.0),
    ("flight_engage.wav",       "idle_flight", 290.0,  303.0),  # twist grip, with its overshoot
    ("flight_engage_take2.wav", "cruise_land", 140.0,  151.5),
    ("takeoff.wav",             "idle_flight", 309.0,  328.0),
    ("takeoff_take2.wav",       "cruise_land", 154.0,  162.5),
    ("landing.wav",             "cruise_land", 238.0,  267.0),
    ("shutdown.wav",            "cruise_land", 303.0,  345.0),
    ("rotor_brake.wav",         "cruise_land", 345.0,  370.0),
]
LOOPS = [
    # name,                   source,        from,    to,     candidate lengths
    ("idle_loop.wav",         "idle_flight", 266.0,  289.5, (12.0, 16.0, 20.0)),
    ("idle_loop_take2.wav",   "cruise_land",  95.0,  139.0, (18.0, 22.0, 26.0)),
    ("flight_loop.wav",       "idle_flight", 303.2,  309.0, (3.0, 4.0, 4.8)),
    ("cruise_loop.wav",       "cruise_land", 208.0,  237.0, (12.0, 16.0, 20.0)),
]


def decode(path: str, out_wav: str) -> None:
    import av  # only needed here

    container = av.open(path)
    resampler = av.audio.resampler.AudioResampler(format="fltp", layout="mono", rate=48000)
    chunks = []
    for frame in container.decode(container.streams.audio[0]):
        for out in resampler.resample(frame):
            chunks.append(out.to_ndarray()[0].copy())
    container.close()
    audio.save_wav(out_wav, np.concatenate(chunks), 48000)


def seam_rank(x: np.ndarray) -> float:
    """Where the wrap ranks among every other moment of the loop, 0 = invisible."""
    twice = np.concatenate([x, x])
    n, hop = 1024, 480
    win = np.hanning(n)
    frames = np.stack([np.abs(np.fft.rfft(twice[i:i + n] * win))
                       for i in range(0, len(twice) - n, hop)], axis=1)
    frames /= np.linalg.norm(frames, axis=0, keepdims=True) + 1e-12
    change = 1.0 - np.sum(frames[:, 1:] * frames[:, :-1], axis=0)
    k = int(round((len(x) - n / 2) / hop))
    return float(np.mean(change < change[k - 1:k + 2].max()) * 100)


def main() -> int:
    os.makedirs(RAW, exist_ok=True)
    if "--skip-decode" not in sys.argv:
        for key, (path, _) in SOURCES.items():
            print(f"decoding {os.path.basename(path)}")
            decode(path, os.path.join(RAW, key + ".wav"))

    src = {k: audio.load_audio(os.path.join(RAW, k + ".wav")) for k in SOURCES}
    rate = next(iter(src.values())).rate
    gain = {k: HEADROOM / float(np.max(np.abs(src[k].samples[int(a * rate):int(b * rate)])))
            for k, (_, (a, b)) in SOURCES.items()}

    def cut(key, a, b, fade_ms=30.0):
        x = src[key].samples[int(a * rate):int(b * rate)].astype(np.float64) * gain[key]
        n = int(fade_ms * 0.001 * rate)
        ramp = np.linspace(0.0, 1.0, n)
        x[:n] *= ramp
        x[-n:] *= ramp[::-1]
        return x

    manifest = {"sources": {k: v[0] for k, v in SOURCES.items()}, "assets": {}}
    print(f"\n{'asset':26s} {'s':>6s} {'rms dBFS':>9s}  notes")

    for name, key, a, b in ONE_SHOTS:
        x = cut(key, a, b)
        audio.save_wav(os.path.join(OUT, name), x.astype(np.float32), rate)
        print(f"{name:26s} {b-a:6.1f} {20*np.log10(np.sqrt(np.mean(x**2))):9.1f}  "
              f"{key} {a:.1f}-{b:.1f}s")
        manifest["assets"][name] = {"kind": "one_shot", "source": key, "from": a, "to": b}

    tmp = os.path.join(OUT, "_cand.wav")
    for name, key, a, b, lengths in LOOPS:
        whole = audio.Recording(samples=cut(key, a, b, fade_ms=1.0).astype(np.float32), rate=rate)
        span = b - a
        search = 0.35 if span > 10.0 else 0.15
        best = None
        for start in np.arange(0.2, max(0.3, span - min(lengths) - 2 * search - 0.2), 0.7):
            for secs in lengths:
                if span - start < secs + 2 * search + 0.2:
                    continue
                pts = find_loop(whole, region_start=start, region_end=span - 0.1,
                                target_seconds=secs, search_seconds=search, crossfade_ms=250.0)
                render_loop(whole, pts, tmp)
                rank = seam_rank(audio.load_audio(tmp).samples.astype(np.float64))
                if best is None or rank < best[0]:
                    best = (rank, pts)
        if best is None:
            raise SystemExit(f"{name}: {span:.1f}s is too short for lengths {lengths}")
        render_loop(whole, best[1], os.path.join(OUT, name))
        lp = audio.load_audio(os.path.join(OUT, name)).samples.astype(np.float64)
        print(f"{name:26s} {best[1].duration:6.1f} {20*np.log10(np.sqrt(np.mean(lp**2))):9.1f}  "
              f"{key} {a+best[1].start:.1f}-{a+best[1].end:.1f}s, seam percentile {best[0]:.0f}")
        manifest["assets"][name] = {"kind": "loop", "source": key, "from": a + best[1].start,
                                    "to": a + best[1].end, "seam_percentile": round(best[0], 1)}
    if os.path.exists(tmp):
        os.remove(tmp)

    with open(os.path.join(OUT, "phases.json"), "w") as fh:
        json.dump(manifest, fh, indent=2)
    print(f"\nwrote {len(manifest['assets'])} assets to {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
