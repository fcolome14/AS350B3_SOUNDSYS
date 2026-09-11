#include "soundsys/PlaybackHead.hpp"

#include "soundsys/TimeStretchReader.hpp"
#include "soundsys/VarispeedReader.hpp"

namespace soundsys {

std::unique_ptr<IPlaybackHead> makePlaybackHead(PlaybackMode mode) {
    return makePlaybackHead(mode, TimeStretchConfig{});
}

std::unique_ptr<IPlaybackHead> makePlaybackHead(PlaybackMode mode, const TimeStretchConfig& stretch) {
    switch (mode) {
        case PlaybackMode::Varispeed:
            return std::make_unique<VarispeedReader>();
        case PlaybackMode::PitchLocked:
        default:
            return std::make_unique<TimeStretchReader>(stretch);
    }
}

}  // namespace soundsys
