#include "soundsys/PlaybackHead.hpp"

#include "soundsys/TimeStretchReader.hpp"
#include "soundsys/VarispeedReader.hpp"

namespace soundsys {

std::unique_ptr<IPlaybackHead> makePlaybackHead(PlaybackMode mode) {
    switch (mode) {
        case PlaybackMode::Varispeed:
            return std::make_unique<VarispeedReader>();
        case PlaybackMode::PitchLocked:
        default:
            return std::make_unique<TimeStretchReader>();
    }
}

}  // namespace soundsys
