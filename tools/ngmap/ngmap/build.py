"""Turn a real recording (plus telemetry, if you have it) into an anchor table."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .anchors import Anchor, AnchorTable, Calibration
from .audio import PitchTrack
from .telemetry import Event, Trace


@dataclass
class Alignment:
    offset: float       # trace time = audio time + offset
    correlation: float  # peak normalised correlation, 0..1
    searched: float     # +/- seconds searched


def estimate_offset(track: PitchTrack, calib: Calibration, trace: Trace,
                    max_offset: float = 30.0, step: float = 0.02) -> Alignment:
    """Line the recording up with the telemetry by shape, not by timestamp.

    Cockpit recorders and data logs are almost never started at the same moment.
    Since the pitch track is a proxy for NG, the two curves are the same curve;
    the lag that maximises their correlation is the offset between the clocks.
    """
    ng_audio = calib.ng_from_hz(track.f0)
    grid = np.arange(-max_offset, max_offset + step, step)

    a = np.asarray(ng_audio, dtype=float)
    a = (a - a.mean()) / (a.std() + 1e-9)

    best = (-1e30, 0.0)
    for offset in grid:
        sampled = np.interp(track.times + offset, trace.t, trace.ng,
                            left=np.nan, right=np.nan)
        mask = ~np.isnan(sampled)
        if mask.sum() < len(a) * 0.4:
            continue
        b = sampled[mask]
        b = (b - b.mean()) / (b.std() + 1e-9)
        score = float(np.mean(a[mask] * b))
        if score > best[0]:
            best = (score, float(offset))

    return Alignment(offset=best[1], correlation=best[0], searched=max_offset)


def anchors_from_trace(trace: Trace, events: list[Event], *, offset: float = 0.0,
                       asset: str = "", engine: str = "Arriel 2B1",
                       audio_duration: float | None = None) -> AnchorTable:
    """Anchors at the landmarks found in telemetry, expressed in recording time.

    `offset` is the alignment from estimate_offset: audio_time = trace_time - offset.
    """
    table = AnchorTable(engine=engine, asset=asset,
                        source=f"telemetry {trace.source}, offset {offset:+.3f} s")

    for event in events:
        t_audio = event.t - offset
        if t_audio < 0:
            continue
        if audio_duration is not None and t_audio > audio_duration:
            continue
        table.anchors.append(Anchor(ng=round(float(event.ng), 3),
                                    t=round(float(t_audio), 4),
                                    label=event.label, how=event.how))

    _enforce_monotonic(table)
    return table


def anchors_from_audio(track: PitchTrack, calib: Calibration, *,
                       ng_values: list[float] | None = None,
                       asset: str = "", engine: str = "Arriel 2B1",
                       min_confidence: float = 0.15) -> AnchorTable:
    """Anchors from the recording alone - no telemetry needed.

    The pitch track already is an NG track once calibrated, so the table is just
    "when did the recording pass each NG". Use this when the audio you have is
    not the audio your telemetry came from, which is the usual case with library
    material.
    """
    ng_track = calib.ng_from_hz(track.f0)
    good = track.confidence >= min_confidence
    if good.sum() < 10:
        raise ValueError("pitch track too weak to build anchors from")

    times = np.asarray(track.times)[good]
    ng = np.asarray(ng_track)[good]

    # Keep only the monotone rising part - a start recording that runs on into
    # idle wobble would otherwise make the map non-invertible.
    peak = int(np.argmax(ng))
    times, ng = times[: peak + 1], ng[: peak + 1]
    ng = np.maximum.accumulate(ng)

    if ng_values is None:
        lo, hi = float(ng[0]), float(ng[-1])
        ng_values = list(np.linspace(lo + 0.02 * (hi - lo), hi, 6))

    table = AnchorTable(engine=engine, asset=asset,
                        source=f"audio pitch track, {calib.how}")
    for value in ng_values:
        if value < ng[0] or value > ng[-1]:
            continue
        t = float(np.interp(value, ng, times))
        table.anchors.append(Anchor(ng=round(float(value), 3), t=round(t, 4),
                                    label=f"ng_{value:.0f}",
                                    how="pitch track crossing"))

    _enforce_monotonic(table)
    return table


def _enforce_monotonic(table: AnchorTable, min_gap_ng: float = 0.05,
                       min_gap_t: float = 0.02) -> None:
    """Drops anchors that would make the table non-invertible.

    Two landmarks landing on the same NG (or the same instant) is a real thing -
    the igniters and the generator can trip within a second of each other on a
    brisk start - and the C++ loader rejects the file outright if it happens, so
    the redundant one is dropped here with its reason kept in `notes`.
    """
    kept: list[Anchor] = []
    dropped: list[str] = []
    for anchor in sorted(table.anchors, key=lambda a: a.ng):
        if kept and (anchor.ng - kept[-1].ng < min_gap_ng or
                     anchor.t - kept[-1].t < min_gap_t):
            dropped.append(f"{anchor.label or anchor.ng} (too close to "
                           f"{kept[-1].label or kept[-1].ng})")
            continue
        kept.append(anchor)
    table.anchors = kept
    if dropped:
        note = "dropped: " + ", ".join(dropped)
        table.notes = (table.notes + "; " + note).strip("; ")


def validate(table: AnchorTable, track: PitchTrack, calib: Calibration,
             min_confidence: float = 0.15) -> dict:
    """How well does the table describe the recording it was built from?

    For every frame of the pitch track: the NG the table claims for that instant,
    against the NG the audio itself is singing. A few percent is normal; a large
    error in one segment means an anchor in that segment is in the wrong place.
    """
    good = track.confidence >= min_confidence
    times = np.asarray(track.times)[good]
    measured = calib.ng_from_hz(np.asarray(track.f0)[good])

    inside = (times >= min(a.t for a in table.anchors)) & \
             (times <= max(a.t for a in table.anchors))
    times, measured = times[inside], measured[inside]
    if times.size == 0:
        return {"frames": 0}

    predicted = table.ng_for_time(times)
    error = measured - predicted
    scale = max(float(np.mean(np.abs(measured))), 1e-9)

    return {
        "frames": int(times.size),
        "rms_ng_error": float(np.sqrt(np.mean(error ** 2))),
        "max_ng_error": float(np.max(np.abs(error))),
        "rms_percent_of_ng": float(100.0 * np.sqrt(np.mean(error ** 2)) / scale),
        "worst_at_seconds": float(times[int(np.argmax(np.abs(error)))]),
    }
