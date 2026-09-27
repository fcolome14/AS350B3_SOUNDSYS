#include "soundsys/TimeStretchReader.hpp"

#include <algorithm>
#include <cmath>

namespace soundsys {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Grain alignment is matched on x[n] - 0.95 x[n-1]: a first-order high-pass
// that hands the decision to the 1-8 kHz whine instead of the rumble below it.
constexpr double kPreEmphasis = 0.95;

// Position may advance at any rate, including zero (a hung start holds its
// tone) - only the sign is fixed, since the grains are laid down forwards.
constexpr double kMinSpeed = 0.0;
constexpr double kMaxSpeed = 32.0;

}  // namespace

void TimeStretchReader::prepare(const AudioBuffer* source, double outputSampleRate) {
    source_ = source;
    outputRate_ = outputSampleRate > 0.0 ? outputSampleRate : 48000.0;
    ratePerOutFrame_ =
        (source_ && source_->sampleRate() > 0.0) ? source_->sampleRate() / outputRate_ : 1.0;
    channels_ = source_ ? source_->channels() : 0;

    const auto toFrames = [&](double ms) {
        return static_cast<std::uint32_t>(std::max(1.0, ms * 0.001 * outputRate_));
    };
    hopFrames_ = std::max<std::uint32_t>(64, toFrames(cfg_.grainMs) / 2);
    grainFrames_ = hopFrames_ * 2;
    searchFrames_ = toFrames(cfg_.searchMs);
    correlationFrames_ = std::min(toFrames(cfg_.correlationMs), hopFrames_);
    jitterFrames_ = std::max(0.0, cfg_.jitterMs) * 0.001 * outputRate_ * ratePerOutFrame_;

    // Hann, so that two half-overlapped grains sum to unity gain.
    window_.resize(grainFrames_);
    for (std::uint32_t i = 0; i < grainFrames_; ++i) {
        window_[i] = static_cast<float>(
            0.5 - 0.5 * std::cos(2.0 * kPi * i / static_cast<double>(grainFrames_ - 1)));
    }

    if (channels_ > 0) {
        ready_.assign(static_cast<std::size_t>(channels_) * hopFrames_, 0.0f);
        tail_.assign(static_cast<std::size_t>(channels_) * hopFrames_, 0.0f);
        head_.assign(static_cast<std::size_t>(channels_) * hopFrames_, 0.0f);
    }
    reference_.assign(correlationFrames_, 0.0f);

    reset(0.0);
}

void TimeStretchReader::reset(double positionSeconds) {
    atEnd_ = false;
    primed_ = false;
    rng_ = 0x9E3779B9u;  // same scatter sequence on every start: renders are repeatable
    emitted_ = hopFrames_;  // forces a grain to be built on the next read
    const double rate = (source_ && source_->sampleRate() > 0.0) ? source_->sampleRate() : 1.0;
    position_ = std::max(0.0, positionSeconds) * rate;
    std::fill(tail_.begin(), tail_.end(), 0.0f);
    std::fill(ready_.begin(), ready_.end(), 0.0f);
    std::fill(reference_.begin(), reference_.end(), 0.0f);
}

void TimeStretchReader::setSpeed(double speed) {
    if (!(speed >= 0.0)) speed = 0.0;  // also catches NaN
    speed_ = std::clamp(speed, kMinSpeed, kMaxSpeed);
}

double TimeStretchReader::positionSeconds() const {
    if (!source_ || source_->sampleRate() <= 0.0) return 0.0;
    double pos = std::max(0.0, position_);
    if (looping_ && source_->frames() > 0) {
        pos = std::fmod(pos, static_cast<double>(source_->frames()));
    }
    return pos / source_->sampleRate();
}

// Linear interpolation is enough here: the read rate inside a grain is fixed
// (it only ever converts the asset rate to the device rate), so there is no
// wideband sweep for the interpolation error to smear.
float TimeStretchReader::sampleAt(std::uint32_t channel, double sourceFrame) const {
    const std::int64_t total = static_cast<std::int64_t>(source_->frames());
    if (total == 0) return 0.0f;

    std::int64_t i0 = static_cast<std::int64_t>(std::floor(sourceFrame));
    const double frac = sourceFrame - static_cast<double>(i0);
    std::int64_t i1 = i0 + 1;

    if (looping_) {
        i0 = ((i0 % total) + total) % total;
        i1 = ((i1 % total) + total) % total;
    } else {
        i0 = std::clamp<std::int64_t>(i0, 0, total - 1);
        i1 = std::clamp<std::int64_t>(i1, 0, total - 1);
    }

    const float* data = source_->channel(channel);
    return static_cast<float>(data[i0] + (data[i1] - data[i0]) * frac);
}

// WSOLA: of all the places within +/- searchFrames_ of where the head says the
// next grain starts, take the one whose opening looks most like the material
// the previous grain was about to continue into.
//
// The match has to be sample-accurate. The whine the ear follows sits at
// 1-8 kHz, a period of 6-40 samples; land a grain a few samples off and its
// partials half-cancel against the previous grain's tail at every seam - an
// amplitude dip every hop, heard as a 50 Hz buzz. (The first version searched
// a 15-sample grid and measured +20 dB of 50 Hz modulation on the real start.)
double TimeStretchReader::findBestOffset(double nominalStart) const {
    if (!primed_ || searchFrames_ == 0 || correlationFrames_ == 0) return nominalStart;

    const double step = ratePerOutFrame_;
    const int    stride = std::max(1, cfg_.correlationStride);
    const int    radius = static_cast<int>(searchFrames_);

    // Normalised correlation against the pre-emphasised reference, so neither
    // a loud stretch of tape nor the rumble can win on energy alone.
    const auto score = [&](int k) {
        const double base = nominalStart + k * step;
        double       dot = 0.0;
        double       energy = 0.0;
        for (std::uint32_t i = 0; i < correlationFrames_; i += stride) {
            const double pos = base + i * step;
            const double s = sampleAt(0, pos) - kPreEmphasis * sampleAt(0, pos - step);
            dot += s * reference_[i];
            energy += s * s;
        }
        return dot / std::sqrt(energy + 1e-12);
    };

    // Coarse pass every few samples, then every sample around the winner.
    constexpr int kCoarse = 4;
    int           best = 0;
    double        bestScore = -1e30;
    for (int k = -radius; k <= radius; k += kCoarse) {
        const double s = score(k);
        if (s > bestScore) {
            bestScore = s;
            best = k;
        }
    }
    const int centre = best;
    for (int k = centre - kCoarse + 1; k < centre + kCoarse; ++k) {
        if (k == centre || k < -radius || k > radius) continue;
        const double s = score(k);
        if (s > bestScore) {
            bestScore = s;
            best = k;
        }
    }
    return nominalStart + best * step;
}

double TimeStretchReader::scatter() {
    // xorshift32: a few instructions, no state beyond one word, fine for
    // decorrelating grain positions.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<double>(rng_) / 2147483647.5 - 1.0;
}

void TimeStretchReader::buildNextGrain() {
    const double step = ratePerOutFrame_;

    // The slower the head, the more often the same material comes round again,
    // so the more the read point is scattered around it. The position itself is
    // untouched - only where this one grain is read from moves.
    const double slow = 1.0 - std::min(1.0, speed_);
    const double start = findBestOffset(position_ + jitterFrames_ * slow * scatter());

    for (std::uint32_t c = 0; c < channels_; ++c) {
        float* headCh = head_.data() + static_cast<std::size_t>(c) * hopFrames_;
        for (std::uint32_t i = 0; i < hopFrames_; ++i) headCh[i] = sampleAt(c, start + i * step);
    }

    // How alike are the two signals about to be crossfaded? WSOLA lines up the
    // tonal part, but the broadband part of a real recording (air, combustion,
    // rotor wash) read from two different places is unrelated noise. A plain
    // Hann crossfade keeps amplitude for the first and loses 3 dB of power in
    // the middle of every overlap for the second - a dip once per hop, heard as
    // a buzz at the hop rate whatever the grain size (measured +19-21 dB of
    // modulation at 20-50 Hz on the real start). So the fade is scaled from
    // the measured correlation r to keep POWER constant:
    //     g_new = k s,  g_old = k (1 - s),  k = 1 / sqrt((1-s)^2 + s^2 + 2 r s (1-s))
    // r = 1 gives the plain Hann fade, r = 0 an equal-power one.
    double r = 1.0;
    if (primed_) {
        double dot = 0.0, eOld = 0.0, eNew = 0.0;
        for (std::uint32_t i = 0; i < hopFrames_; ++i) {
            double a = 0.0, b = 0.0;
            for (std::uint32_t c = 0; c < channels_; ++c) {
                a += tail_[static_cast<std::size_t>(c) * hopFrames_ + i];
                b += head_[static_cast<std::size_t>(c) * hopFrames_ + i];
            }
            dot += a * b;
            eOld += a * a;
            eNew += b * b;
        }
        r = std::clamp(dot / std::sqrt(eOld * eNew + 1e-12), 0.0, 1.0);
    }

    for (std::uint32_t c = 0; c < channels_; ++c) {
        float*       readyCh = ready_.data() + static_cast<std::size_t>(c) * hopFrames_;
        float*       tailCh = tail_.data() + static_cast<std::size_t>(c) * hopFrames_;
        const float* headCh = head_.data() + static_cast<std::size_t>(c) * hopFrames_;
        for (std::uint32_t i = 0; i < hopFrames_; ++i) {
            const double s = window_[i];  // rising half of the Hann window
            const double f = 1.0 - s;
            if (primed_) {
                const double k = 1.0 / std::sqrt(f * f + s * s + 2.0 * r * s * f);
                readyCh[i] = static_cast<float>(k * (f * tailCh[i] + s * headCh[i]));
            } else {
                readyCh[i] = static_cast<float>(s * headCh[i]);  // first grain: fade in from silence
            }
        }
        // The second half stays raw; it is faded when the next grain arrives.
        for (std::uint32_t i = 0; i < hopFrames_; ++i) {
            tailCh[i] = sampleAt(c, start + (hopFrames_ + i) * step);
        }
    }

    // What the next grain should continue: the source right after this grain's
    // hop, read at the same rate, pre-emphasised like the candidates.
    for (std::uint32_t i = 0; i < correlationFrames_; ++i) {
        const double pos = start + (hopFrames_ + i) * step;
        reference_[i] =
            static_cast<float>(sampleAt(0, pos) - kPreEmphasis * sampleAt(0, pos - step));
    }

    // The head advances by one hop of real time, scaled by the demanded speed.
    // speed_ = 0 leaves it in place and the grains loop over the same material.
    position_ += static_cast<double>(hopFrames_) * speed_ * step;
    emitted_ = 0;
    primed_ = true;

    if (!looping_ && position_ >= static_cast<double>(source_->frames())) {
        atEnd_ = true;
    }
}

std::uint32_t TimeStretchReader::read(float* const* out, std::uint32_t channels,
                                      std::uint32_t frames) {
    if (!ready() || channels == 0 || frames == 0 || hopFrames_ == 0) return 0;

    std::uint32_t produced = 0;
    while (produced < frames) {
        if (emitted_ >= hopFrames_) {
            if (atEnd_) break;
            buildNextGrain();
        }
        const std::uint32_t take = std::min(frames - produced, hopFrames_ - emitted_);
        for (std::uint32_t c = 0; c < channels; ++c) {
            const std::uint32_t sc = std::min(c, channels_ - 1);
            const float* src = ready_.data() + static_cast<std::size_t>(sc) * hopFrames_ + emitted_;
            std::copy(src, src + take, out[c] + produced);
        }
        emitted_ += take;
        produced += take;
    }

    for (std::uint32_t c = 0; c < channels; ++c) {
        std::fill(out[c] + produced, out[c] + frames, 0.0f);
    }
    return produced;
}

}  // namespace soundsys
