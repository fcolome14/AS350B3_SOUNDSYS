"""Take a cockpit noise out of a recording without touching anything else.

Knocks on the camera, switches being actuated, the pilot shifting in the seat:
short events on top of an engine that is not doing anything sudden underneath.

The removal is a subtraction, never an invention: inside the event's window each
STFT bin is pulled down towards a reference interpolated between the frames just
before and just after it - the recording's own idea of what the engine was doing
across that gap - and the gain is clipped at 1, so a bin that was already at or
below the reference is left exactly as it was. Outside the windows the output is
sample-for-sample the input.
"""

from __future__ import annotations

import numpy as np
from scipy.ndimage import maximum_filter1d, median_filter, uniform_filter1d
from scipy.signal import find_peaks, istft, stft


def find_events(samples: np.ndarray, rate: float, *, region: tuple | None = None,
                sigma: float = 3.5) -> list:
    """(start, end) of every broadband transient, from positive spectral flux."""
    _, times, spec = stft(samples, rate, nperseg=2048, noverlap=2048 - 128)
    level = 10.0 * np.log10(np.abs(spec) ** 2 + 1e-15)
    flux = np.sum(np.maximum(0.0, np.diff(level, axis=1)), axis=0)
    ftimes = times[1:]

    peaks, _ = find_peaks(flux, height=float(np.median(flux) + sigma * np.std(flux)), distance=10)
    events = []
    for i in peaks:
        if region is not None and not (region[0] <= ftimes[i] <= region[1]):
            continue
        # The event lasts as long as the flux stays above half its own height.
        half = float(np.median(flux) + 0.5 * (flux[i] - np.median(flux)))
        lo = hi = i
        while lo > 0 and flux[lo] > half:
            lo -= 1
        while hi < len(flux) - 1 and flux[hi] > half:
            hi += 1
        events.append((float(ftimes[max(0, lo - 4)]), float(ftimes[min(len(ftimes) - 1, hi + 6)])))

    merged = []
    for a, b in sorted(events):
        if merged and a - merged[-1][1] < 0.05:
            merged[-1] = (merged[-1][0], b)
        else:
            merged.append((a, b))
    return merged


def remove_events(samples: np.ndarray, rate: float, spans, *, n_fft: int = 1024) -> np.ndarray:
    """Attenuate each span down to what the recording was doing either side of it."""
    if not spans:
        return samples
    hop = n_fft // 8
    _, times, spec = stft(samples, rate, nperseg=n_fft, noverlap=n_fft - hop)
    mag = np.abs(spec)
    gain = np.ones(mag.shape)

    for start, end in spans:
        j0, j1 = int(np.searchsorted(times, start)), int(np.searchsorted(times, end))
        if j1 - j0 < 3:
            j1 = j0 + 3
        before = np.median(mag[:, max(0, j0 - 8):max(1, j0)], axis=1)
        after = np.median(mag[:, j1:j1 + 8], axis=1)
        ramp = np.linspace(0.0, 1.0, j1 - j0)[None, :]
        reference = before[:, None] * (1.0 - ramp) + after[:, None] * ramp
        wanted = np.minimum(1.0, reference / np.maximum(mag[:, j0:j1], 1e-12))
        # A few frames of taper at each end, so the window itself is not a click.
        n = j1 - j0
        taper = np.minimum(1.0, np.minimum(np.arange(n), n - 1 - np.arange(n)) / 3.0)
        gain[:, j0:j1] = np.minimum(gain[:, j0:j1], 1.0 - (1.0 - wanted) * taper[None, :])

    _, cleaned = istft(spec * gain, rate, nperseg=n_fft, noverlap=n_fft - hop)
    cleaned = np.pad(cleaned, (0, max(0, len(samples) - len(cleaned))))[:len(samples)]
    return cleaned.astype(samples.dtype, copy=False)


def fade_lines(samples: np.ndarray, rate: float, *, above_hz: float, start: float, full: float,
               n_fft: int = 4096) -> np.ndarray:
    """Let the tonal lines above `above_hz` die away between `start` and `full`.

    For an engine that the recording leaves idling where the simulator needs it
    stopped: the turbine whine is a handful of narrow lines standing well clear
    of everything around them, so each bin on a line is pulled down to the floor
    its neighbours in frequency set - a subtraction again, gain never above 1.
    The pull grows from nothing at `start` to all of it at `full` and stays, so
    the whine fades as it falls instead of being cut. Bins that are not on a
    line, and everything below `above_hz`, are left exactly as they were.
    """
    hop = n_fft // 8
    freqs, times, spec = stft(samples, rate, nperseg=n_fft, noverlap=n_fft - hop)
    mag = np.abs(spec)
    band = freqs >= above_hz

    # A line persists from frame to frame, noise does not: averaging a few
    # frames lets a line be told from the floor a few dB above it.
    steady = uniform_filter1d(mag[band], size=9, axis=1)
    floor = median_filter(steady, size=(int(400.0 / (freqs[1] - freqs[0])) | 1, 1))
    on_line = maximum_filter1d(steady > 1.6 * floor, size=5, axis=0)
    wanted = np.where(on_line, np.minimum(1.0, floor / np.maximum(steady, 1e-12)), 1.0)

    depth = np.clip((times - start) / max(full - start, 1e-9), 0.0, 1.0)
    gain = np.ones(mag.shape)
    gain[band] = wanted ** depth[None, :]

    _, cleaned = istft(spec * gain, rate, nperseg=n_fft, noverlap=n_fft - hop)
    cleaned = np.pad(cleaned, (0, max(0, len(samples) - len(cleaned))))[:len(samples)]
    return cleaned.astype(samples.dtype, copy=False)
