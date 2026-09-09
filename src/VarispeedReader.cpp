#include "soundsys/VarispeedReader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "soundsys/Config.hpp"

#if defined(SOUNDSYS_WITH_LIBSAMPLERATE)
#include <samplerate.h>
#endif

namespace soundsys {
namespace {

// libsamplerate refuses ratios outside [1/256, 256]; keep well inside that and
// far away from zero, which would stall the playback head entirely.
constexpr double kMinSpeed = 1.0 / 64.0;
constexpr double kMaxSpeed = 32.0;

// Frames of source material staged for the resampler per refill.
constexpr std::uint32_t kInputChunkFrames = 1024;

}  // namespace

// The whole backend, including whether there is one at all, is confined to this
// file. Callers see a pointer of unchanging size.
struct VarispeedReader::Backend {
#if defined(SOUNDSYS_WITH_LIBSAMPLERATE)
    SRC_STATE*         src = nullptr;
    std::vector<float> in;         // interleaved source frames staged for the resampler
    std::vector<float> out;        // interleaved resampler output
    std::uint32_t      avail = 0;  // staged frames not yet consumed
    std::uint32_t      offset = 0; // where those frames start in `in`
    double             stagePos = 0.0;  // next source frame to stage

    ~Backend() {
        if (src) src_delete(src);
    }
#endif
};

VarispeedReader::VarispeedReader() : backend_(new Backend()) {}

VarispeedReader::~VarispeedReader() = default;

bool VarispeedReader::usingLibsamplerate() noexcept {
#if defined(SOUNDSYS_WITH_LIBSAMPLERATE)
    return true;
#else
    return false;
#endif
}

void VarispeedReader::prepare(const AudioBuffer* source, double outputSampleRate) {
    source_ = source;
    outputRate_ = outputSampleRate > 0.0 ? outputSampleRate : 48000.0;

    // Source frames consumed per output frame at speed 1.0. This folds the
    // asset's own sample rate into the speed, so callers think in real time and
    // never in samples.
    ratePerOutFrame_ =
        (source_ && source_->sampleRate() > 0.0) ? source_->sampleRate() / outputRate_ : 1.0;

#if defined(SOUNDSYS_WITH_LIBSAMPLERATE)
    if (backend_->src) {
        src_delete(backend_->src);
        backend_->src = nullptr;
    }
    const int channels = source_ ? static_cast<int>(source_->channels()) : 0;
    if (channels > 0) {
        int err = 0;
        // SINC_FASTEST is the cheapest of the band-limited converters and is
        // still transparent for this material; the linear converter would alias
        // audibly on the high-order whine at speeds above ~2x.
        backend_->src = src_new(SRC_SINC_FASTEST, channels, &err);
        backend_->in.assign(static_cast<std::size_t>(kInputChunkFrames) * channels, 0.0f);
        backend_->out.assign(static_cast<std::size_t>(kMaxBlockFrames) * channels, 0.0f);
    }
#endif

    reset(0.0);
}

void VarispeedReader::reset(double positionSeconds) {
    atEnd_ = false;
    const double rate = (source_ && source_->sampleRate() > 0.0) ? source_->sampleRate() : 1.0;
    cursor_ = std::max(0.0, positionSeconds) * rate;
    if (source_ && cursor_ > static_cast<double>(source_->frames())) {
        cursor_ = static_cast<double>(source_->frames());
    }
#if defined(SOUNDSYS_WITH_LIBSAMPLERATE)
    backend_->avail = 0;
    backend_->offset = 0;
    backend_->stagePos = cursor_;
    if (backend_->src) src_reset(backend_->src);
#endif
}

void VarispeedReader::setSpeed(double speed) {
    if (!(speed > 0.0)) speed = kMinSpeed;  // also catches NaN
    speed_ = std::clamp(speed, kMinSpeed, kMaxSpeed);
}

double VarispeedReader::positionSeconds() const {
    if (!source_ || source_->sampleRate() <= 0.0) return 0.0;
    double consumed = std::max(0.0, cursor_);
    if (looping_ && source_->frames() > 0) {
        consumed = std::fmod(consumed, static_cast<double>(source_->frames()));
    }
    return consumed / source_->sampleRate();
}

std::uint32_t VarispeedReader::read(float* const* out, std::uint32_t channels,
                                    std::uint32_t frames) {
    if (!ready() || channels == 0 || frames == 0) return 0;
#if defined(SOUNDSYS_WITH_LIBSAMPLERATE)
    if (backend_->src) return readResampled(out, channels, frames);
#endif
    return readInterpolated(out, channels, frames);
}

// ---------------------------------------------------------------------------
// Fallback path: Catmull-Rom interpolation straight off the source buffer.
// Not band-limited, so it will alias on aggressive speed-ups, but it keeps the
// module buildable and testable with zero third-party code.
// ---------------------------------------------------------------------------
std::uint32_t VarispeedReader::readInterpolated(float* const* out, std::uint32_t channels,
                                                std::uint32_t frames) {
    const std::int64_t  total = static_cast<std::int64_t>(source_->frames());
    const double        step = speed_ * ratePerOutFrame_;
    const std::uint32_t srcChannels = source_->channels();

    std::uint32_t produced = 0;
    for (; produced < frames; ++produced) {
        if (cursor_ >= static_cast<double>(total)) {
            if (!looping_) {
                atEnd_ = true;
                break;
            }
            cursor_ = std::fmod(cursor_, static_cast<double>(total));
        }

        const std::int64_t i1 = static_cast<std::int64_t>(cursor_);
        const double       frac = cursor_ - static_cast<double>(i1);

        for (std::uint32_t c = 0; c < channels; ++c) {
            const float* data = source_->channel(std::min(c, srcChannels - 1));
            const auto   at = [&](std::int64_t idx) -> double {
                if (looping_) {
                    idx = ((idx % total) + total) % total;
                } else {
                    idx = std::clamp<std::int64_t>(idx, 0, total - 1);
                }
                return static_cast<double>(data[idx]);
            };
            const double y0 = at(i1 - 1), y1 = at(i1), y2 = at(i1 + 1), y3 = at(i1 + 2);
            const double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
            const double b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
            const double d = -0.5 * y0 + 0.5 * y2;
            out[c][produced] = static_cast<float>(((a * frac + b) * frac + d) * frac + y1);
        }
        cursor_ += step;
    }

    for (std::uint32_t c = 0; c < channels; ++c) {
        std::fill(out[c] + produced, out[c] + frames, 0.0f);
    }
    return produced;
}

#if defined(SOUNDSYS_WITH_LIBSAMPLERATE)

void VarispeedReader::refillInput() {
    const std::uint32_t srcChannels = source_->channels();
    const std::int64_t  total = static_cast<std::int64_t>(source_->frames());
    Backend&            b = *backend_;

    // Compact whatever is still unconsumed to the front of the staging buffer.
    if (b.avail > 0 && b.offset > 0) {
        std::memmove(b.in.data(), b.in.data() + static_cast<std::size_t>(b.offset) * srcChannels,
                     static_cast<std::size_t>(b.avail) * srcChannels * sizeof(float));
    }
    b.offset = 0;

    // Staging continues after the last frame staged, not after the last frame
    // consumed - the resampler is still holding the difference.
    std::int64_t pos = static_cast<std::int64_t>(b.stagePos);
    while (b.avail < kInputChunkFrames) {
        if (pos >= total) {
            if (!looping_) break;
            pos = 0;
        }
        const std::uint32_t want = static_cast<std::uint32_t>(
            std::min<std::int64_t>(kInputChunkFrames - b.avail, total - pos));
        for (std::uint32_t c = 0; c < srcChannels; ++c) {
            const float* data = source_->channel(c);
            for (std::uint32_t i = 0; i < want; ++i) {
                b.in[(static_cast<std::size_t>(b.avail) + i) * srcChannels + c] = data[pos + i];
            }
        }
        b.avail += want;
        pos += want;
    }
    b.stagePos = static_cast<double>(pos);
}

std::uint32_t VarispeedReader::readResampled(float* const* out, std::uint32_t channels,
                                             std::uint32_t frames) {
    const std::uint32_t srcChannels = source_->channels();
    Backend&            b = *backend_;

    // libsamplerate's ratio is output rate over input rate; our speed is the
    // reciprocal of that, with the asset/device rate mismatch folded in.
    const double ratio = std::clamp(1.0 / (speed_ * ratePerOutFrame_), 1.0 / 256.0, 256.0);

    std::uint32_t produced = 0;
    while (produced < frames) {
        if (b.avail == 0) {
            refillInput();
            if (b.avail == 0) {
                atEnd_ = true;
                break;
            }
        }

        SRC_DATA data{};
        data.data_in = b.in.data() + static_cast<std::size_t>(b.offset) * srcChannels;
        data.input_frames = static_cast<long>(b.avail);
        data.data_out = b.out.data() + static_cast<std::size_t>(produced) * srcChannels;
        data.output_frames = static_cast<long>(frames - produced);
        data.src_ratio = ratio;
        data.end_of_input = 0;

        if (src_process(b.src, &data) != 0) {
            atEnd_ = true;
            break;
        }

        const std::uint32_t used = static_cast<std::uint32_t>(data.input_frames_used);
        const std::uint32_t gen = static_cast<std::uint32_t>(data.output_frames_gen);

        // The playback head advances by what the resampler actually swallowed,
        // never by what we assumed - that is what makes positionSeconds() usable
        // as feedback for the sync loop.
        cursor_ += static_cast<double>(used);
        b.offset += used;
        b.avail -= used;
        produced += gen;

        if (used == 0 && gen == 0) {
            // The converter wants more history than the staging buffer holds.
            const std::uint32_t before = b.avail;
            refillInput();
            if (b.avail == before) {
                atEnd_ = true;
                break;
            }
        }
    }

    // De-interleave into the caller's planar buffers, duplicating a mono asset
    // across both sides of the bus.
    for (std::uint32_t c = 0; c < channels; ++c) {
        const std::uint32_t sc = std::min(c, srcChannels - 1);
        float*              dst = out[c];
        for (std::uint32_t i = 0; i < produced; ++i) {
            dst[i] = b.out[static_cast<std::size_t>(i) * srcChannels + sc];
        }
        std::fill(dst + produced, dst + frames, 0.0f);
    }
    return produced;
}

#else  // no libsamplerate

void VarispeedReader::refillInput() {}

std::uint32_t VarispeedReader::readResampled(float* const* out, std::uint32_t channels,
                                             std::uint32_t frames) {
    return readInterpolated(out, channels, frames);
}

#endif

}  // namespace soundsys
