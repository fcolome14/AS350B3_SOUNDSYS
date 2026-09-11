// soundsys/PlaybackHead.hpp - how a recording is replayed against simulated NG.
//
// Two ways to put the playback head where NG says it should be, and they do NOT
// sound the same:
//
//   Varispeed    Read the recording faster or slower, like a turntable. Pitch
//                and duration move together, so a start that spools up 1.6x
//                faster also comes out 1.6x higher in pitch. Cheap, artefact
//                free, and correct only while the playback rate stays near 1.
//                It also collapses on a plateau: if NG stops moving, the head
//                stops and the tone dies.
//
//   PitchLocked  Move the head at whatever rate NG demands, but keep the pitch
//                the recording had at that position (overlap-add / WSOLA).
//                This is the physically right one: a turbine's tone is a
//                function of shaft speed, so at NG = 50 % it sounds the same
//                whether it took 10 s or 30 s to get there - and a hung start
//                holds its tone instead of fading to a rumble.
//
// Both satisfy "stay in sync with NG"; only PitchLocked also satisfies "sound
// like the engine at that NG". See docs/sync.md.
#pragma once

#include <cstdint>
#include <memory>

#include "soundsys/AudioBuffer.hpp"

namespace soundsys {

enum class PlaybackMode {
    PitchLocked,  // position follows NG, pitch stays as recorded
    Varispeed,    // position follows NG, pitch scales with playback rate
};

class IPlaybackHead {
public:
    virtual ~IPlaybackHead() = default;

    // Control thread. Binds the head to a buffer that must outlive it.
    virtual void prepare(const AudioBuffer* source, double outputSampleRate) = 0;

    virtual void reset(double positionSeconds) = 0;
    virtual void setLooping(bool looping) = 0;

    // Position advance as a multiple of real time: 1.0 = as recorded.
    virtual void setSpeed(double speed) = 0;
    virtual double speed() const = 0;

    // Where the head is inside the recording, in seconds.
    virtual double positionSeconds() const = 0;

    virtual bool atEnd() const = 0;
    virtual bool ready() const = 0;
    virtual double durationSeconds() const = 0;

    // Audio thread. Overwrites planar out[0..channels-1]; returns frames made.
    virtual std::uint32_t read(float* const* out, std::uint32_t channels,
                               std::uint32_t frames) = 0;
};

struct TimeStretchConfig;  // soundsys/TimeStretchReader.hpp

std::unique_ptr<IPlaybackHead> makePlaybackHead(PlaybackMode mode);
// Same, with the grain settings of the pitch-locked head (ignored by Varispeed).
std::unique_ptr<IPlaybackHead> makePlaybackHead(PlaybackMode mode, const TimeStretchConfig& stretch);

}  // namespace soundsys
