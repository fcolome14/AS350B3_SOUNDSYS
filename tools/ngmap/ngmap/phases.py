"""Cut a long AS350B3 recording into the phases the sim needs as separate assets.

Given twenty minutes of cabin audio - start, ground idle, flight idle, takeoff,
cruise, shutdown - this finds where each steady section is and where the
transitions are, so each one can become its own voice asset: a start recording
with an anchor table, and seamless loops for everything that holds still.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .anchors import Calibration
from .audio import PitchTrack, Recording, save_wav


@dataclass
class Phase:
    kind: str        # "steady" | "spool_up" | "spool_down" | "quiet"
    start: float
    end: float
    ng_median: float  # calibrated NG% (or Hz if no calibration was given)
    ng_start: float
    ng_end: float

    @property
    def duration(self) -> float:
        return self.end - self.start

    def describe(self) -> str:
        return (f"{self.kind:<11} {self.start:7.2f} - {self.end:7.2f} s "
                f"({self.duration:6.2f} s)  NG {self.ng_start:5.1f} -> {self.ng_end:5.1f} "
                f"(median {self.ng_median:5.1f})")


def segment(track: PitchTrack, rms_db: np.ndarray, rms_times: np.ndarray,
            calib: Calibration | None = None, *,
            steady_rate: float = 0.35,   # %NG per second
            quiet_db: float = -55.0,
            min_duration: float = 2.0) -> list[Phase]:
    """Classify every frame, then merge runs into phases.

    The classifier is deliberately blunt - rate of change of NG plus level -
    because the interesting decisions (which steady section is the best loop,
    where the start actually begins) are made by a human looking at the list.
    """
    ng = calib.ng_from_hz(track.f0) if calib else np.asarray(track.f0, dtype=float)
    times = np.asarray(track.times)
    if times.size < 3:
        return []

    rate = np.gradient(ng, times)
    level = np.interp(times, rms_times, rms_db)

    kinds = np.empty(times.size, dtype=object)
    for i in range(times.size):
        if level[i] < quiet_db:
            kinds[i] = "quiet"
        elif rate[i] > steady_rate:
            kinds[i] = "spool_up"
        elif rate[i] < -steady_rate:
            kinds[i] = "spool_down"
        else:
            kinds[i] = "steady"

    phases: list[Phase] = []
    run_start = 0
    for i in range(1, times.size + 1):
        if i < times.size and kinds[i] == kinds[run_start]:
            continue
        segment_slice = slice(run_start, i)
        start_t, end_t = float(times[run_start]), float(times[i - 1])
        if end_t - start_t >= min_duration:
            phases.append(Phase(
                kind=str(kinds[run_start]),
                start=start_t,
                end=end_t,
                ng_median=float(np.median(ng[segment_slice])),
                ng_start=float(ng[run_start]),
                ng_end=float(ng[i - 1]),
            ))
        run_start = i

    return _merge_adjacent(phases)


def _merge_adjacent(phases: list[Phase], gap: float = 0.5) -> list[Phase]:
    """Glue same-kind phases separated by a sliver of something else."""
    if not phases:
        return phases
    merged = [phases[0]]
    for phase in phases[1:]:
        last = merged[-1]
        if phase.kind == last.kind and phase.start - last.end <= gap:
            merged[-1] = Phase(kind=last.kind, start=last.start, end=phase.end,
                               ng_median=0.5 * (last.ng_median + phase.ng_median),
                               ng_start=last.ng_start, ng_end=phase.ng_end)
        else:
            merged.append(phase)
    return merged


def extract(rec: Recording, start: float, end: float, out_path: str,
            fade_ms: float = 20.0) -> None:
    """Cut one phase out to its own file, with short fades at the seams."""
    a = max(0, int(start * rec.rate))
    b = min(len(rec.samples), int(end * rec.rate))
    clip = np.array(rec.samples[a:b], dtype=np.float32)

    fade = int(fade_ms * 0.001 * rec.rate)
    if fade > 0 and clip.size > 2 * fade:
        ramp = np.linspace(0.0, 1.0, fade, dtype=np.float32)
        clip[:fade] *= ramp
        clip[-fade:] *= ramp[::-1]

    save_wav(out_path, clip, rec.rate)
