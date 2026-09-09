"""Find a seamless loop inside a steady phase of a real recording.

The idle and cruise voices play a loop; a loop with a bad splice ticks once per
revolution of the loop, which on tonal material is the most audible defect in
the whole sound system. The search here is the usual one for periodic signals:
fix the loop start, then choose the end that makes the waveform just before it
look most like the waveform just before the start - i.e. the end lands on the
same phase of the same rotation.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .audio import Recording, save_wav


@dataclass
class LoopPoints:
    start: float          # seconds
    end: float            # seconds
    correlation: float    # -1..1 at the splice
    crossfade_ms: float

    @property
    def duration(self) -> float:
        return self.end - self.start


def find_loop(rec: Recording, *, region_start: float, region_end: float,
              target_seconds: float = 4.0, search_seconds: float = 0.25,
              match_ms: float = 40.0, crossfade_ms: float = 15.0) -> LoopPoints:
    """Search for the best loop of roughly `target_seconds` inside a region."""
    rate = rec.rate
    a = int(region_start * rate)
    b = min(len(rec.samples), int(region_end * rate))
    if b - a < int((target_seconds + 2 * search_seconds) * rate):
        raise ValueError("region is too short for the requested loop length")

    match = int(match_ms * 0.001 * rate)
    # Leave room for the match window before the loop start.
    start = a + match
    reference = rec.samples[start - match:start]
    ref_norm = float(np.linalg.norm(reference)) + 1e-9

    nominal_end = start + int(target_seconds * rate)
    search = int(search_seconds * rate)
    lo = max(start + match + 1, nominal_end - search)
    hi = min(b, nominal_end + search)

    best_score = -2.0
    best_end = nominal_end

    # Step by one sample: at 48 kHz a whole-sample search is already finer than
    # the period of anything audible up here, and the region is small.
    for end in range(lo, hi):
        candidate = rec.samples[end - match:end]
        denom = (float(np.linalg.norm(candidate)) + 1e-9) * ref_norm
        score = float(np.dot(candidate, reference)) / denom
        if score > best_score:
            best_score = score
            best_end = end

    return LoopPoints(start=start / rate, end=best_end / rate,
                      correlation=best_score, crossfade_ms=crossfade_ms)


def render_loop(rec: Recording, points: LoopPoints, out_path: str) -> None:
    """Write the loop as its own file, with the splice crossfaded.

    The tail that would have run past the loop end is folded back over the head
    with an equal-power crossfade, so playing the file end-to-start is seamless
    and the C++ LoopVoice needs no loop metadata at all - it just wraps.
    """
    rate = rec.rate
    start = int(points.start * rate)
    end = int(points.end * rate)
    fade = int(points.crossfade_ms * 0.001 * rate)

    if end + fade > len(rec.samples):
        fade = max(0, len(rec.samples) - end)

    body = np.array(rec.samples[start:end], dtype=np.float32)
    if fade > 0 and body.size > fade:
        tail = rec.samples[end:end + fade].astype(np.float32)
        # Equal power, so a correlated splice does not bulge in level.
        t = np.linspace(0.0, 1.0, fade, dtype=np.float32)
        body[:fade] = body[:fade] * np.sqrt(t) + tail * np.sqrt(1.0 - t)

    save_wav(out_path, body, rate)
