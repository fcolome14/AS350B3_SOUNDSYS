"""Spectral analysis of real AS350B3 recordings.

Everything here works on material recorded in the aircraft, not on anything
synthesised. The one assumption is the physical one the whole module rests on:
the tonal content of a turbine is locked to shaft speed, so the frequency of the
NG whine in the recording IS a reading of NG at that instant. Extract that track
and you can place a recording against simulated NG without any telemetry at all.

Only numpy and scipy are required. librosa is nice for exploration but nothing
here needs it.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.io import wavfile
from scipy.signal import get_window


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

@dataclass
class Recording:
    samples: np.ndarray  # mono float32 in [-1, 1]
    rate: int
    path: str = ""
    channels: int = 1

    @property
    def duration(self) -> float:
        return len(self.samples) / float(self.rate)


def load_wav(path: str, mono: bool = True) -> Recording:
    rate, data = wavfile.read(path)

    # scipy hands back whatever the file holds; normalise to float [-1, 1].
    if data.dtype.kind == "i":
        data = data.astype(np.float64) / float(np.iinfo(data.dtype).max + 1)
    elif data.dtype.kind == "u":
        info = np.iinfo(data.dtype)
        data = (data.astype(np.float64) - info.max / 2.0) / (info.max / 2.0)
    else:
        data = data.astype(np.float64)

    channels = 1 if data.ndim == 1 else data.shape[1]
    if mono and data.ndim > 1:
        data = data.mean(axis=1)

    return Recording(samples=data.astype(np.float32), rate=int(rate), path=path,
                     channels=channels)


def save_wav(path: str, samples: np.ndarray, rate: int) -> None:
    """Writes float32 WAV - the format the C++ loader is happiest with."""
    data = np.asarray(samples, dtype=np.float32)
    peak = float(np.max(np.abs(data))) if data.size else 0.0
    if peak > 1.0:
        data = data / peak
    wavfile.write(path, int(rate), data)


# ---------------------------------------------------------------------------
# Short-time spectrum
# ---------------------------------------------------------------------------

@dataclass
class Spectrogram:
    times: np.ndarray      # frame centres, seconds
    freqs: np.ndarray      # bin centres, Hz
    magnitude: np.ndarray  # [freq, frame]
    rate: int
    hop: int


def spectrogram(rec: Recording, n_fft: int = 8192, hop: int = 1024,
                window: str = "hann") -> Spectrogram:
    """A long window on purpose.

    8192 samples at 48 kHz is ~170 ms and ~6 Hz of resolution. A turbine start
    sweeps slowly enough for that to be safe, and the resolution is what lets
    the NG whine be separated from the rotor harmonics further down.
    """
    x = rec.samples
    if len(x) < n_fft:
        x = np.pad(x, (0, n_fft - len(x)))

    win = get_window(window, n_fft, fftbins=True)
    starts = np.arange(0, len(x) - n_fft + 1, hop)
    frames = np.stack([x[s:s + n_fft] * win for s in starts], axis=1)
    mag = np.abs(np.fft.rfft(frames, axis=0))

    freqs = np.fft.rfftfreq(n_fft, 1.0 / rec.rate)
    times = (starts + n_fft / 2.0) / rec.rate
    return Spectrogram(times=times, freqs=freqs, magnitude=mag, rate=rec.rate, hop=hop)


# ---------------------------------------------------------------------------
# Harmonic tracking - the NG readout
# ---------------------------------------------------------------------------

@dataclass
class PitchTrack:
    times: np.ndarray
    f0: np.ndarray          # Hz
    confidence: np.ndarray  # 0..1, normalised harmonic energy
    n_harmonics: int


def track_fundamental(
    spec: Spectrogram,
    *,
    fmin: float = 40.0,
    fmax: float = 900.0,
    n_harmonics: int = 8,
    candidates: int = 700,
    jump_penalty: float = 12.0,
    band: int = 40,
) -> PitchTrack:
    """Harmonic-sum tracking with a banded Viterbi over the candidate grid.

    Why not plain autocorrelation: in cabin recordings the fundamental of the
    gas generator is often masked by rotor thump and airframe rumble, while its
    harmonics are perfectly clear. Summing energy over the harmonic series finds
    the shaft speed even when the fundamental itself is missing, and the Viterbi
    stage stops the track from jumping an octave whenever a harmonic gets louder
    than the fundamental - the classic failure on this material.
    """
    grid = np.geomspace(fmin, fmax, candidates)
    mag = spec.magnitude
    freqs = spec.freqs
    df = freqs[1] - freqs[0]

    # Score every candidate in every frame: the summed magnitude of its first
    # n_harmonics partials.
    scores = np.zeros((candidates, mag.shape[1]))
    for h in range(1, n_harmonics + 1):
        bins = np.clip(np.round(grid * h / df).astype(int), 0, len(freqs) - 1)
        scores += mag[bins, :] / h  # later partials weigh less

    # Per-frame normalisation, so a loud frame cannot dominate the path.
    peak = scores.max(axis=0, keepdims=True)
    confidence_raw = np.where(peak > 0, scores / np.maximum(peak, 1e-12), 0.0)

    # Banded Viterbi: a shaft cannot change speed by much in one hop, so only
    # transitions within +/- `band` candidates are considered.
    log_scores = np.log(confidence_raw + 1e-6)
    n_frames = log_scores.shape[1]
    best = np.zeros_like(log_scores)
    back = np.zeros(log_scores.shape, dtype=np.int32)
    best[:, 0] = log_scores[:, 0]

    offsets = np.arange(-band, band + 1)
    step_cost = jump_penalty * (np.abs(offsets) / max(band, 1)) ** 2

    for f in range(1, n_frames):
        prev = best[:, f - 1]
        # shifted[k, i] = prev[i - offsets[k]] - cost(offset)
        shifted = np.full((len(offsets), candidates), -np.inf)
        for k, off in enumerate(offsets):
            src = np.arange(candidates) - off
            valid = (src >= 0) & (src < candidates)
            shifted[k, valid] = prev[src[valid]] - step_cost[k]
        best_k = np.argmax(shifted, axis=0)
        best[:, f] = shifted[best_k, np.arange(candidates)] + log_scores[:, f]
        back[:, f] = np.arange(candidates) - offsets[best_k]

    path = np.zeros(n_frames, dtype=np.int32)
    path[-1] = int(np.argmax(best[:, -1]))
    for f in range(n_frames - 1, 0, -1):
        path[f - 1] = back[path[f], f]

    f0 = grid[path]
    conf = confidence_raw[path, np.arange(n_frames)]
    return PitchTrack(times=spec.times, f0=f0, confidence=conf, n_harmonics=n_harmonics)


def refine_track(track: PitchTrack, spec: Spectrogram, n_harmonics: int = 8) -> PitchTrack:
    """Parabolic refinement of each frame against the nearest harmonic peaks.

    The candidate grid is coarse (a few cents); this pulls each estimate onto the
    actual spectral peak so the NG readout is smooth enough to differentiate.
    """
    freqs = spec.freqs
    df = freqs[1] - freqs[0]
    mag = spec.magnitude
    refined = np.array(track.f0, dtype=float)

    for i, f0 in enumerate(track.f0):
        estimates = []
        weights = []
        for h in range(1, n_harmonics + 1):
            centre = int(round(f0 * h / df))
            if centre <= 0 or centre >= len(freqs) - 1:
                continue
            lo, mid, hi = mag[centre - 1, i], mag[centre, i], mag[centre + 1, i]
            if mid <= lo or mid <= hi:
                continue  # not a local peak, skip this partial
            denom = lo - 2.0 * mid + hi
            delta = 0.5 * (lo - hi) / denom if denom != 0 else 0.0
            estimates.append((centre + delta) * df / h)
            weights.append(mid)
        if estimates:
            refined[i] = float(np.average(estimates, weights=weights))

    return PitchTrack(times=track.times, f0=refined, confidence=track.confidence,
                      n_harmonics=track.n_harmonics)


def rms_envelope(rec: Recording, hop: int = 1024, window: int = 4096) -> tuple:
    """Frame-wise RMS in dB - used for phase segmentation and trimming."""
    x = rec.samples
    starts = np.arange(0, max(1, len(x) - window + 1), hop)
    energy = np.array([float(np.sqrt(np.mean(x[s:s + window] ** 2))) for s in starts])
    times = (starts + window / 2.0) / rec.rate
    return times, 20.0 * np.log10(np.maximum(energy, 1e-9))
