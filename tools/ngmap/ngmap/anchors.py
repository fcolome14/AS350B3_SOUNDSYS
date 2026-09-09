"""The NG% <-> recording-time table, and the calibration that produces it.

This is the artefact the C++ side actually consumes (soundsys/AnchorTable.hpp).
Everything else in this package exists to produce or check one of these.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field, asdict

import numpy as np

SCHEMA = "soundsys.anchors/1"


@dataclass
class Anchor:
    ng: float
    t: float
    label: str = ""
    how: str = ""  # provenance, ignored by the C++ reader


@dataclass
class AnchorTable:
    anchors: list[Anchor] = field(default_factory=list)
    engine: str = "Arriel 2B1"
    asset: str = ""
    source: str = ""
    notes: str = ""

    # ------------------------------------------------------------------
    def validate(self) -> list[str]:
        """Same invariants the C++ loader enforces, reported all at once."""
        problems = []
        if len(self.anchors) < 2:
            problems.append("need at least 2 anchors")
        ordered = sorted(self.anchors, key=lambda a: a.ng)
        for prev, cur in zip(ordered, ordered[1:]):
            if cur.ng <= prev.ng:
                problems.append(
                    f"NG not strictly increasing: {prev.label or prev.ng} -> "
                    f"{cur.label or cur.ng} ({prev.ng} -> {cur.ng})"
                )
            if cur.t <= prev.t:
                problems.append(
                    f"time not strictly increasing: {prev.label or prev.ng} -> "
                    f"{cur.label or cur.ng} ({prev.t} -> {cur.t})"
                )
        return problems

    def time_for_ng(self, ng):
        ordered = sorted(self.anchors, key=lambda a: a.ng)
        return np.interp(ng, [a.ng for a in ordered], [a.t for a in ordered])

    def ng_for_time(self, t):
        ordered = sorted(self.anchors, key=lambda a: a.t)
        return np.interp(t, [a.t for a in ordered], [a.ng for a in ordered])

    # ------------------------------------------------------------------
    def to_json(self) -> str:
        payload = {
            "schema": SCHEMA,
            "engine": self.engine,
            "asset": self.asset,
            "source": self.source,
            "notes": self.notes,
            "anchors": [
                {k: v for k, v in asdict(a).items() if v != "" or k in ("ng", "t")}
                for a in sorted(self.anchors, key=lambda a: a.ng)
            ],
        }
        return json.dumps(payload, indent=2)

    def write(self, path: str) -> None:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(self.to_json() + "\n")

    @staticmethod
    def read(path: str) -> "AnchorTable":
        with open(path, encoding="utf-8") as handle:
            payload = json.load(handle)
        return AnchorTable(
            anchors=[Anchor(ng=float(a["ng"]), t=float(a["t"]),
                            label=a.get("label", ""), how=a.get("how", ""))
                     for a in payload.get("anchors", [])],
            engine=payload.get("engine", ""),
            asset=payload.get("asset", ""),
            source=payload.get("source", ""),
            notes=payload.get("notes", ""),
        )


# ---------------------------------------------------------------------------
# Turning a pitch track into NG
# ---------------------------------------------------------------------------

@dataclass
class Calibration:
    """Hz per percent NG, i.e. the constant that turns the whine into a reading.

    For a fixed geometry this is a single number: the tracked partial is some
    integer multiple of shaft rotation, so f = k * NG with k constant. Getting k
    right matters less than it looks - it cancels out of the anchor table, which
    only ever stores (NG, time) pairs - but it is what lets the tool convert an
    audio-only recording into NG in the first place, and what makes the
    validation plot meaningful.
    """

    hz_per_percent: float
    reference_ng: float = 0.0
    reference_hz: float = 0.0
    how: str = ""

    def ng_from_hz(self, hz):
        return np.asarray(hz, dtype=float) / self.hz_per_percent

    def hz_from_ng(self, ng):
        return np.asarray(ng, dtype=float) * self.hz_per_percent


def calibrate_from_point(f0_hz: float, ng_percent: float, how: str = "") -> Calibration:
    """One known operating point is enough: idle is the easy one to identify."""
    if ng_percent <= 0:
        raise ValueError("calibration NG must be positive")
    return Calibration(hz_per_percent=f0_hz / ng_percent, reference_ng=ng_percent,
                       reference_hz=f0_hz,
                       how=how or f"{f0_hz:.1f} Hz assumed to be NG {ng_percent:g} %")


def calibrate_from_trace(track_times, track_f0, trace_t, trace_ng,
                         offset: float = 0.0, min_ng: float = 20.0) -> Calibration:
    """Least-squares fit of f0 against a telemetry NG trace.

    Used when you have both the recording and the cockpit trace: it is the
    honest way to get k, and the residual it leaves is a direct measure of how
    well the "pitch follows NG" assumption holds for this aircraft.
    """
    ng_at_audio = np.interp(np.asarray(track_times) + offset, trace_t, trace_ng)
    mask = ng_at_audio > min_ng
    if mask.sum() < 10:
        raise ValueError("not enough overlap above the NG floor to calibrate")

    ng = ng_at_audio[mask]
    f0 = np.asarray(track_f0)[mask]
    # Through the origin: at zero shaft speed there is no tone.
    k = float(np.sum(ng * f0) / np.sum(ng * ng))
    residual = f0 - k * ng
    rms = float(np.sqrt(np.mean(residual ** 2)))
    return Calibration(
        hz_per_percent=k,
        reference_ng=float(np.max(ng)),
        reference_hz=float(k * np.max(ng)),
        how=f"least squares against telemetry, residual {rms:.1f} Hz rms "
            f"({100.0 * rms / max(k * float(np.mean(ng)), 1e-9):.1f} %)",
    )
