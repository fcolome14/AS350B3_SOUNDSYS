// soundsys/voices/TurbineStartVoice.hpp - real start recording, simulated timing.
//
// How it stays in sync
// --------------------
// Every block we ask the anchor table where the simulated NG puts us in the
// recording (targetPos). We do NOT jump there - a jump is an audible click and
// a pitch discontinuity. Instead we drive the playback speed:
//
//     speed = d(targetPos)/dt            <- feed-forward: the physically right
//                                           speed for the current NG rate
//           + kP * (targetPos - cursor)  <- feedback: slowly absorbs any drift
//
// The feed-forward term alone already produces the correct pitch, because
// d(targetPos)/dt = timePerNg(NG) * dNG/dt: a start that spools up twice as
// fast as the recording plays back at twice the speed, an octave higher, which
// is exactly what the real engine would sound like. The proportional term only
// has to clean up rounding and event-timing slop, so its gain can stay low
// enough to be inaudible.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "soundsys/AnchorTable.hpp"
#include "soundsys/AudioBuffer.hpp"
#include "soundsys/ISoundVoice.hpp"
#include "soundsys/PlaybackHead.hpp"
#include "soundsys/TimeStretchReader.hpp"

namespace soundsys {

struct TurbineStartConfig {
    std::string wavPath;      // recording of the real start
    std::string anchorsPath;  // JSON from tools/ngmap

    float  gain = 1.0f;

    // How the recording is replayed once the head is positioned. PitchLocked is
    // the physically correct one and the default; Varispeed is cheaper and
    // artefact-free but multiplies the pitch by the playback rate, so it is only
    // right when the simulated start closely matches the recorded one. See
    // soundsys/PlaybackHead.hpp.
    PlaybackMode mode = PlaybackMode::PitchLocked;
    TimeStretchConfig stretch;  // grain settings of the pitch-locked head

    // Position feedback gain, 1/seconds. Low on purpose: the feed-forward term
    // does the work, this only removes slow drift.
    double positionGain = 0.35;

    // Position-advance clamp. Zero is allowed and meaningful: NG stops moving
    // (a hung start), the head stops, and the pitch-locked mode holds the tone
    // it had - which is what the real engine does. The varispeed head applies a
    // small floor of its own, since a resampler cannot run at zero.
    double minSpeed = 0.0;
    double maxSpeed = 6.0;

    // Ignore NG noise below this rate when computing the feed-forward term.
    double speedSmoothingHz = 4.0;

    // The voice hands over to the idle loop once the playback head is within
    // this margin of the end of the recording.
    double handoffMarginSeconds = 0.15;
    double fadeOutSeconds = 0.35;

    // Once NG reaches the last anchor the start is over: the voice announces
    // StartSequenceComplete, whoever plays the steady engine (the loop bank)
    // fades in, and this voice fades out over this long - still playing its own
    // recorded idle at 1x, so the two overlap on the same sound.
    double handoffCrossfadeSeconds = 2.0;

    // If NG falls this far below the highest NG seen during the sequence, the
    // start is treated as aborted even without an explicit event.
    double abortNgDrop = 3.0;
};

class TurbineStartVoice final : public ISoundVoice {
public:
    explicit TurbineStartVoice(TurbineStartConfig cfg);

    // Loads wav + anchors. Call from the control thread before prepare();
    // returns false with a message if either asset is missing or malformed.
    bool load(std::string* error = nullptr);

    const char* name() const override { return "TurbineStartVoice"; }
    void prepare(const AudioFormat& fmt) override;
    void onEvent(const SoundEvent& ev) override;
    void onParameters(const ParameterSnapshot& p) override;
    void process(float* const* out, std::uint32_t frames) override;
    bool isActive() const override { return state_ != State::Idle; }
    void setEventSink(IEventSink* sink) override { sink_ = sink; }

    // --- diagnostics (audio thread values, read-only) ---------------------
    double playbackSpeed() const noexcept { return head_ ? head_->speed() : 0.0; }
    double positionSeconds() const noexcept { return head_ ? head_->positionSeconds() : 0.0; }
    double targetSeconds() const noexcept { return targetPos_; }
    double syncErrorSeconds() const noexcept { return targetPos_ - positionSeconds(); }
    const AnchorTable& anchors() const noexcept { return anchors_; }

private:
    enum class State { Idle, Running, FadingOut };

    void beginStart(double ng);
    void announceHandoff(float ng);

    TurbineStartConfig cfg_;
    AudioBuffer        buffer_;
    AnchorTable        anchors_;
    std::unique_ptr<IPlaybackHead> head_;
    IEventSink*        sink_ = nullptr;

    State  state_ = State::Idle;
    bool   loaded_ = false;
    double targetPos_ = 0.0;
    double maxNgSeen_ = 0.0;
    double lastTargetPos_ = 0.0;
    double smoothedFeedForward_ = 0.0;
    double smoothingCoef_ = 0.0;
    float  fadeGain_ = 1.0f;
    float  fadeStep_ = 0.0f;
    float  gain_ = 1.0f;
    bool   haveLastTarget_ = false;
    bool   handoffEmitted_ = false;
    bool   freeRunning_ = false;  // past the last anchor: recorded idle plays at 1x
    float  fadeOutStep_ = 1.0f;   // abort / shutdown
    float  handoffStep_ = 1.0f;   // crossfade into the steady-state voices
};

} // namespace soundsys
