#include "soundsys/TimeStretchReader.hpp"

#include <algorithm>
#include <cmath>

namespace soundsys {
namespace {

constexpr double kPi = 3.14159265358979323846;

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

    // Hann, so that two half-overlapped grains sum to unity gain.
    window_.resize(grainFrames_);
    for (std::uint32_t i = 0; i < grainFrames_; ++i) {
        window_[i] = static_cast<float>(
            0.5 - 0.5 * std::cos(2.0 * kPi * i / static_cast<double>(grainFrames_ - 1)));
    }

    if (channels_ > 0) {
        ready_.assign(static_cast<std::size_t>(channels_) * hopFrames_, 0.0f);
        tail_.assign(static_cast<std::size_t>(channels_) * hopFrames_, 0.0f);
    }
    reference_.assign(correlationFrames_, 0.0f);

    reset(0.0);
}

void TimeStretchReader::reset(double positionSeconds) {
    atEnd_ = false;
    primed_ = false;
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
double TimeStretchReader::findBestOffset(double nominalStart) const {
    if (!primed_ || searchFrames_ == 0 || correlationFrames_ == 0) return nominalStart;

    const double step = ratePerOutFrame_;
    const int    stride = std::max(1, cfg_.correlationStride);
    const double radius = static_cast<double>(searchFrames_) * step;

    double bestOffset = 0.0;
    double bestScore = -1e30;

    // 33 candidate offsets across the window is plenty: the correlation surface
    // of a tonal signal is smooth at this scale, and this keeps the search at a
    // few tens of microseconds per grain.
    constexpr int kCandidates = 33;
    for (int c = 0; c < kCandidates; ++c) {
        const double offset =
            -radius + 2.0 * radius * static_cast<double>(c) / (kCandidates - 1);
        double dot = 0.0;
        double energy = 0.0;
        for (std::uint32_t i = 0; i < correlationFrames_; i += stride) {
            const double s = sampleAt(0, nominalStart + offset + i * step);
            dot += s * reference_[i];
            energy += s * s;
        }
        // Normalised correlation, so a loud stretch of tape cannot win on level
        // alone.
        const double score = dot / std::sqrt(energy + 1e-9);
        if (score > bestScore) {
            bestScore = score;
            bestOffset = offset;
        }
    }
    return nominalStart + bestOffset;
}

void TimeStretchReader::buildNextGrain() {
    const double step = ratePerOutFrame_;
    const double start = findBestOffset(position_);

    // First half overlap-adds onto the previous grain's tail; second half is
    // kept for the next one.
    for (std::uint32_t c = 0; c < channels_; ++c) {
        float* readyCh = ready_.data() + static_cast<std::size_t>(c) * hopFrames_;
        float* tailCh = tail_.data() + static_cast<std::size_t>(c) * hopFrames_;
        for (std::uint32_t i = 0; i < hopFrames_; ++i) {
            const float head = sampleAt(c, start + i * step) * window_[i];
            readyCh[i] = tailCh[i] + head;
        }
        for (std::uint32_t i = 0; i < hopFrames_; ++i) {
            const std::uint32_t j = hopFrames_ + i;
            tailCh[i] = sampleAt(c, start + j * step) * window_[j];
        }
    }

    // What the next grain should continue: the source right after this grain's
    // hop, read at the same rate.
    for (std::uint32_t i = 0; i < correlationFrames_; ++i) {
        reference_[i] = sampleAt(0, start + (hopFrames_ + i) * step);
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
