"""Command line for the offline half of the sound system.

    uv run ngmap analyze  start.wav
    uv run ngmap phases   flight.wav --extract out/
    uv run ngmap anchors  start.wav --trace start_telemetry.csv --out anchors.json
    uv run ngmap validate anchors.json start.wav --calibrate 67@41.5
    uv run ngmap loop     idle.wav --region 12:40 --out idle_loop.wav

Nothing here synthesises audio: every command takes recordings of the real
aircraft and produces either a description of them or a trimmed copy.
"""

from __future__ import annotations

import argparse
import sys

import numpy as np

from . import audio as audio_mod
from .anchors import AnchorTable, Calibration, calibrate_from_point, calibrate_from_trace
from .build import anchors_from_audio, anchors_from_trace, estimate_offset, validate
from .loops import find_loop, render_loop
from .phases import extract, segment
from .telemetry import detect_events, load_trace


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _track(path: str, args) -> tuple:
    rec = audio_mod.load_audio(path)
    spec = audio_mod.spectrogram(rec, n_fft=args.fft, hop=args.hop)
    track = audio_mod.track_fundamental(spec, fmin=args.fmin, fmax=args.fmax,
                                        n_harmonics=args.harmonics)
    track = audio_mod.refine_track(track, spec, n_harmonics=args.harmonics)
    return rec, spec, track


def _parse_calibration(text: str, track) -> Calibration:
    """--calibrate NG@SECONDS: at that instant the engine was at that NG."""
    try:
        ng_text, time_text = text.split("@")
        ng = float(ng_text)
        when = float(time_text)
    except ValueError as exc:
        raise SystemExit(f"--calibrate wants NG@SECONDS, e.g. 67@41.5 (got {text!r})") from exc

    f0 = float(np.interp(when, track.times, track.f0))
    return calibrate_from_point(f0, ng, how=f"{f0:.1f} Hz at {when:g} s = NG {ng:g} %")


def _parse_region(text: str, duration: float) -> tuple:
    if not text:
        return 0.0, duration
    start, _, end = text.partition(":")
    return (float(start) if start else 0.0), (float(end) if end else duration)


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_analyze(args) -> int:
    rec, _, track = _track(args.audio, args)
    times, rms = audio_mod.rms_envelope(rec)

    calib = _parse_calibration(args.calibrate, track) if args.calibrate else None

    print(f"{args.audio}: {rec.duration:.2f} s, {rec.rate} Hz, {rec.channels} ch")
    print(f"  tracked {len(track.f0)} frames, "
          f"f0 {np.min(track.f0):.1f} - {np.max(track.f0):.1f} Hz, "
          f"median confidence {np.median(track.confidence):.2f}")
    if calib:
        ng = calib.ng_from_hz(track.f0)
        print(f"  calibration: {calib.how}  ({calib.hz_per_percent:.3f} Hz per % NG)")
        print(f"  implied NG {np.min(ng):.1f} - {np.max(ng):.1f} %")
    print(f"  level {np.min(rms):.1f} to {np.max(rms):.1f} dBFS")

    if args.csv:
        ng = calib.ng_from_hz(track.f0) if calib else np.zeros_like(track.f0)
        with open(args.csv, "w", encoding="utf-8") as handle:
            handle.write("t,f0_hz,confidence,ng_percent,rms_db\n")
            level = np.interp(track.times, times, rms)
            for i, t in enumerate(track.times):
                handle.write(f"{t:.4f},{track.f0[i]:.3f},{track.confidence[i]:.4f},"
                             f"{ng[i]:.3f},{level[i]:.2f}\n")
        print(f"  wrote {args.csv}")
    return 0


def cmd_phases(args) -> int:
    rec, _, track = _track(args.audio, args)
    rms_times, rms = audio_mod.rms_envelope(rec)
    calib = _parse_calibration(args.calibrate, track) if args.calibrate else None

    found = segment(track, rms, rms_times, calib, min_duration=args.min_duration)
    unit = "NG %" if calib else "Hz"
    print(f"{args.audio}: {len(found)} phases ({unit})")
    for i, phase in enumerate(found):
        print(f"  [{i:2d}] {phase.describe()}")

    if args.extract:
        import os
        os.makedirs(args.extract, exist_ok=True)
        for i, phase in enumerate(found):
            if phase.kind == "quiet" or phase.duration < args.min_duration:
                continue
            name = f"{i:02d}_{phase.kind}_{phase.ng_median:.0f}.wav"
            path = os.path.join(args.extract, name)
            extract(rec, phase.start, phase.end, path)
            print(f"       -> {path}")
    return 0


def cmd_anchors(args) -> int:
    rec, _, track = _track(args.audio, args)

    if args.trace:
        trace = load_trace(args.trace)
        calib = (_parse_calibration(args.calibrate, track) if args.calibrate
                 else calibrate_from_trace(track.times, track.f0, trace.t, trace.ng))
        alignment = estimate_offset(track, calib, trace, max_offset=args.max_offset)
        print(f"alignment: audio leads telemetry by {alignment.offset:+.3f} s "
              f"(correlation {alignment.correlation:.3f})")
        if alignment.correlation < 0.8:
            print("  WARNING: weak alignment - check that the trace and the "
                  "recording are from the same start", file=sys.stderr)

        events = detect_events(trace, ignition_off_ng=args.ignition_ng,
                               generator_ng=args.generator_ng)
        print(f"landmarks in {args.trace}:")
        for event in events:
            print(f"  {event.label:<16} t={event.t:7.2f} s  NG={event.ng:5.1f} %  ({event.how})")

        table = anchors_from_trace(trace, events, offset=alignment.offset,
                                   asset=args.audio, engine=args.engine,
                                   audio_duration=rec.duration)
    else:
        if not args.calibrate:
            raise SystemExit("without --trace you must give --calibrate NG@SECONDS "
                             "so the pitch track can be read as NG")
        calib = _parse_calibration(args.calibrate, track)
        print(f"calibration: {calib.how}")
        table = anchors_from_audio(track, calib, asset=args.audio, engine=args.engine)

    problems = table.validate()
    print(f"anchors ({len(table.anchors)}):")
    for anchor in sorted(table.anchors, key=lambda a: a.ng):
        print(f"  NG {anchor.ng:6.2f} %  ->  t = {anchor.t:7.3f} s   {anchor.label}")
    for problem in problems:
        print(f"  PROBLEM: {problem}", file=sys.stderr)

    report = validate(table, track, calib)
    if report.get("frames"):
        print(f"self-check against the recording: {report['rms_ng_error']:.2f} %NG rms "
              f"({report['rms_percent_of_ng']:.1f} % of NG), worst "
              f"{report['max_ng_error']:.2f} at {report['worst_at_seconds']:.2f} s")

    if args.out:
        table.write(args.out)
        print(f"wrote {args.out}")
    return 1 if problems else 0


def cmd_validate(args) -> int:
    table = AnchorTable.read(args.anchors)
    rec, _, track = _track(args.audio, args)

    if args.trace:
        trace = load_trace(args.trace)
        calib = calibrate_from_trace(track.times, track.f0, trace.t, trace.ng)
    elif args.calibrate:
        calib = _parse_calibration(args.calibrate, track)
    else:
        raise SystemExit("give --calibrate NG@SECONDS or --trace")

    print(f"{args.anchors}: {len(table.anchors)} anchors, engine {table.engine!r}")
    for problem in table.validate():
        print(f"  PROBLEM: {problem}", file=sys.stderr)

    report = validate(table, track, calib)
    if not report.get("frames"):
        print("no overlap between the anchor range and the tracked audio", file=sys.stderr)
        return 1

    print(f"  frames compared : {report['frames']}")
    print(f"  rms NG error    : {report['rms_ng_error']:.3f} % "
          f"({report['rms_percent_of_ng']:.2f} % of NG)")
    print(f"  worst NG error  : {report['max_ng_error']:.3f} % at "
          f"{report['worst_at_seconds']:.2f} s")
    return 0 if report["rms_percent_of_ng"] < args.tolerance else 1


def cmd_loop(args) -> int:
    rec = audio_mod.load_audio(args.audio)
    start, end = _parse_region(args.region, rec.duration)
    points = find_loop(rec, region_start=start, region_end=end,
                       target_seconds=args.seconds, search_seconds=args.search,
                       crossfade_ms=args.crossfade)
    print(f"{args.audio}: loop {points.start:.3f} - {points.end:.3f} s "
          f"({points.duration:.3f} s), splice correlation {points.correlation:.3f}")
    if points.correlation < 0.5:
        print("  WARNING: poor splice; try another region or another loop length",
              file=sys.stderr)
    if args.out:
        render_loop(rec, points, args.out)
        print(f"  wrote {args.out}")
    return 0


# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="ngmap", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def add_common(p, with_calibrate=True):
        p.add_argument("--fft", type=int, default=8192, help="FFT size (default 8192)")
        p.add_argument("--hop", type=int, default=1024, help="hop in samples (default 1024)")
        p.add_argument("--fmin", type=float, default=40.0, help="lowest f0 to consider, Hz")
        p.add_argument("--fmax", type=float, default=900.0, help="highest f0 to consider, Hz")
        p.add_argument("--harmonics", type=int, default=8, help="partials summed per candidate")
        if with_calibrate:
            p.add_argument("--calibrate", metavar="NG@SECONDS",
                           help="known operating point, e.g. 67@41.5 for idle")

    p = sub.add_parser("analyze", help="pitch/level analysis of a recording")
    p.add_argument("audio")
    p.add_argument("--csv", help="write the per-frame track here")
    add_common(p)
    p.set_defaults(func=cmd_analyze)

    p = sub.add_parser("phases", help="segment a long recording into operating phases")
    p.add_argument("audio")
    p.add_argument("--extract", metavar="DIR", help="also cut each phase to its own wav")
    p.add_argument("--min-duration", type=float, default=2.0)
    add_common(p)
    p.set_defaults(func=cmd_phases)

    p = sub.add_parser("anchors", help="build the NG -> time table for a start recording")
    p.add_argument("audio")
    p.add_argument("--trace", help="cockpit telemetry CSV (time, NG, T4, ...)")
    p.add_argument("--out", help="write the anchor JSON here")
    p.add_argument("--engine", default="Arriel 2B1")
    p.add_argument("--ignition-ng", type=float, default=45.0,
                   help="NG at which the igniters cut (default 45)")
    p.add_argument("--generator-ng", type=float, default=52.0,
                   help="NG at which the generator comes online (default 52)")
    p.add_argument("--max-offset", type=float, default=30.0,
                   help="how far to search when aligning audio to telemetry")
    add_common(p)
    p.set_defaults(func=cmd_anchors)

    p = sub.add_parser("validate", help="check an anchor table against its recording")
    p.add_argument("anchors")
    p.add_argument("audio")
    p.add_argument("--trace")
    p.add_argument("--tolerance", type=float, default=5.0,
                   help="fail above this rms error, in %% of NG (default 5)")
    add_common(p)
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("loop", help="find a seamless loop in a steady phase")
    p.add_argument("audio")
    p.add_argument("--region", default="", metavar="START:END", help="seconds, e.g. 12:40")
    p.add_argument("--seconds", type=float, default=4.0, help="target loop length")
    p.add_argument("--search", type=float, default=0.25, help="search radius, seconds")
    p.add_argument("--crossfade", type=float, default=15.0, help="splice crossfade, ms")
    p.add_argument("--out", help="write the looped asset here")
    p.set_defaults(func=cmd_loop)

    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
