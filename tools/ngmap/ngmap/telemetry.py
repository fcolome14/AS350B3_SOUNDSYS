"""Cockpit telemetry: load a start trace and find the acoustic landmarks in it.

The trace is whatever the sim's recorder (or the real aircraft's) wrote out: a
CSV with a time column and at least NG%, ideally T4 and torque too. Column names
are matched loosely because every export names them differently.
"""

from __future__ import annotations

import csv
from dataclasses import dataclass

import numpy as np

# Aliases seen in real exports, lowest-common-denominator matching.
_COLUMN_ALIASES = {
    "t": ("t", "time", "time_s", "timestamp", "seconds", "elapsed"),
    "ng": ("ng", "ng%", "ng_percent", "ngpct", "n1", "n1%", "gasgenerator"),
    "t4": ("t4", "t4_c", "tot", "itt", "egt", "t4_celsius"),
    "torque": ("tq", "trq", "torque", "torque%", "torque_percent"),
    "nr": ("nr", "nr%", "nr_percent", "rotor"),
}


def _normalise(name: str) -> str:
    return "".join(ch for ch in name.strip().lower() if ch.isalnum() or ch in "%_")


@dataclass
class Trace:
    """A start trace, resampled onto a uniform time base."""

    t: np.ndarray
    ng: np.ndarray
    t4: np.ndarray | None = None
    torque: np.ndarray | None = None
    nr: np.ndarray | None = None
    source: str = ""

    @property
    def rate(self) -> float:
        if len(self.t) < 2:
            return 0.0
        return 1.0 / float(np.median(np.diff(self.t)))

    def ng_at(self, t: float) -> float:
        return float(np.interp(t, self.t, self.ng))

    def time_at_ng(self, ng: float) -> float:
        """First time the trace reaches `ng` (during the spool-up)."""
        peak = int(np.argmax(self.ng))
        rising_t = self.t[: peak + 1]
        rising_ng = self.ng[: peak + 1]
        return float(np.interp(ng, rising_ng, rising_t))


def load_trace(path: str, resample_hz: float = 50.0) -> Trace:
    """Reads a telemetry CSV and puts it on a uniform grid.

    Uniform sampling matters: every derivative below (dNG/dt, dT4/dt) is taken
    with a fixed step, and real 20 Hz logs jitter.
    """
    with open(path, newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.reader(handle))
    if not rows:
        raise ValueError(f"{path}: empty file")

    header = [_normalise(c) for c in rows[0]]
    index = {}
    for key, aliases in _COLUMN_ALIASES.items():
        for i, name in enumerate(header):
            if name in aliases:
                index[key] = i
                break

    if "t" not in index or "ng" not in index:
        raise ValueError(
            f"{path}: need a time column and an NG column; header was {rows[0]}"
        )

    data: dict[str, list[float]] = {key: [] for key in index}
    for row in rows[1:]:
        if not row:
            continue
        try:
            values = {key: float(row[i]) for key, i in index.items()}
        except (ValueError, IndexError):
            continue  # blank line, unit row, trailing junk
        for key, value in values.items():
            data[key].append(value)

    t = np.asarray(data["t"], dtype=float)
    if t.size < 2:
        raise ValueError(f"{path}: fewer than two usable rows")
    t -= t[0]

    step = 1.0 / resample_hz
    grid = np.arange(0.0, t[-1] + 0.5 * step, step)

    def column(key):
        if key not in data:
            return None
        return np.interp(grid, t, np.asarray(data[key], dtype=float))

    return Trace(
        t=grid,
        ng=column("ng"),
        t4=column("t4"),
        torque=column("torque"),
        nr=column("nr"),
        source=path,
    )


def _smooth(x: np.ndarray, window: int) -> np.ndarray:
    if window < 3 or x is None:
        return x
    kernel = np.ones(window) / window
    return np.convolve(x, kernel, mode="same")


@dataclass
class Event:
    label: str
    t: float
    ng: float
    how: str  # what fixed it, so a questionable anchor can be argued with


def detect_events(
    trace: Trace,
    *,
    ignition_off_ng: float = 45.0,
    generator_ng: float = 52.0,
) -> list[Event]:
    """Finds the landmarks an anchor table is built from.

    Four of these come out of the data itself; the two that are set by the
    engine's control unit rather than by physics (igniters off, generator
    contactor) are NG thresholds you can override for your engine variant.
    """
    events: list[Event] = []
    rate = trace.rate or 50.0
    smooth_window = max(3, int(0.4 * rate))

    ng = _smooth(trace.ng, smooth_window)
    dng = np.gradient(ng, trace.t)

    # 1. Starter engaged: NG first lifts off zero for good.
    moving = np.flatnonzero(ng > 0.5)
    if moving.size:
        events.append(Event("starter_engage", float(trace.t[moving[0]]), float(ng[moving[0]]),
                            "first NG above 0.5 %"))

    # 2. Light-off: the fastest T4 rise there is. Without T4, fall back to the
    #    knee in NG, where combustion starts helping the starter.
    if trace.t4 is not None:
        t4 = _smooth(trace.t4, smooth_window)
        dt4 = np.gradient(t4, trace.t)
        i = int(np.argmax(dt4))
        events.append(Event("light_off", float(trace.t[i]), float(ng[i]),
                            f"peak dT4/dt = {dt4[i]:.0f} C/s"))
    else:
        i = int(np.argmax(np.gradient(dng, trace.t)))
        events.append(Event("light_off", float(trace.t[i]), float(ng[i]),
                            "peak NG acceleration (no T4 in trace)"))

    # 3./4. Control-unit events, taken at their NG thresholds.
    for label, threshold in (("ignition_off", ignition_off_ng),
                             ("gen_online", generator_ng)):
        if float(np.max(ng)) >= threshold:
            events.append(Event(label, trace.time_at_ng(threshold), threshold,
                                f"NG crosses {threshold:g} %"))

    # 5. Idle: NG flattens out and stays flat.
    settled = _find_plateau(trace.t, ng, dng)
    if settled is not None:
        events.append(Event("idle", settled, float(np.interp(settled, trace.t, ng)),
                            "NG rate under 0.3 %/s for 2 s"))

    events.sort(key=lambda e: e.t)
    return events


def _find_plateau(t, ng, dng, *, max_rate=0.3, hold=2.0) -> float | None:
    """First instant after the peak acceleration where NG goes quiet and stays."""
    if len(t) < 3:
        return None
    step = float(np.median(np.diff(t)))
    need = max(1, int(hold / step))
    quiet = np.abs(dng) < max_rate
    # Only look after NG has actually got somewhere, so the pre-start silence
    # does not count as a plateau.
    start = int(np.argmax(ng > 0.6 * float(np.max(ng))))
    run = 0
    for i in range(start, len(t)):
        run = run + 1 if quiet[i] else 0
        if run >= need:
            return float(t[i - need + 1])
    return None
