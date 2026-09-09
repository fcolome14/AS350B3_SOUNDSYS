// soundsys/AnchorTable.hpp - NG% <-> recording-time correspondence.
//
// The physical idea: in a free turbine the acoustic signature is a function of
// shaft speed, not of wall-clock time. So a real start recording can be replayed
// against ANY simulated spool-up rate as long as we always sit at the point of
// the recording whose NG matches the simulated NG.
//
// This table is that correspondence, built offline (see tools/ngmap) by marking
// 4-6 acoustic landmarks in the recording and reading the NG the real telemetry
// had at those instants:
//
//     starter engage -> light-off -> ignition off -> generator online -> idle
//
// Between anchors we interpolate linearly, which is accurate enough because the
// anchors are placed exactly where the slope of NG(t) changes.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace soundsys {

struct Anchor {
    double      ng = 0.0;   // gas generator speed, percent
    double      t = 0.0;    // time into the recording, seconds
    std::string label;      // "light_off", "gen_online", ...
};

class AnchorTable {
public:
    // Loads the JSON emitted by `python -m ngmap build`. Returns false and fills
    // `error` on malformed input, non-monotonic anchors or fewer than 2 anchors.
    bool loadJson(const std::string& path, std::string* error = nullptr);
    bool parseJson(const std::string& text, std::string* error = nullptr);

    // Direct construction (tests, hard-coded fallbacks). Same validation.
    bool setAnchors(std::vector<Anchor> anchors, std::string* error = nullptr);

    bool valid() const noexcept { return anchors_.size() >= 2; }

    // Forward map: simulated NG -> position in the recording, seconds.
    // Clamped to [firstTime, lastTime] outside the anchored range.
    double timeForNg(double ng) const noexcept;

    // Inverse map: position in the recording -> the NG it was recorded at.
    double ngForTime(double t) const noexcept;

    // d(recording time)/d(NG) at `ng`, seconds per percent. This is the term
    // that converts a simulated NG rate into a playback speed, i.e. the whole
    // point of the table.
    double timePerNg(double ng) const noexcept;

    double firstNg() const noexcept { return valid() ? anchors_.front().ng : 0.0; }
    double lastNg() const noexcept { return valid() ? anchors_.back().ng : 0.0; }
    double firstTime() const noexcept { return valid() ? anchors_.front().t : 0.0; }
    double lastTime() const noexcept { return valid() ? anchors_.back().t : 0.0; }

    // Time of a named landmark, or -1 if the table does not carry it.
    double timeOfLabel(const std::string& label) const noexcept;

    const std::vector<Anchor>& anchors() const noexcept { return anchors_; }

    // Metadata carried alongside the anchors.
    const std::string& assetPath() const noexcept { return assetPath_; }
    const std::string& engine() const noexcept { return engine_; }
    const std::string& source() const noexcept { return source_; }

private:
    std::vector<Anchor> anchors_;
    std::string assetPath_;
    std::string engine_;
    std::string source_;
};

} // namespace soundsys
