// apps/NgSource.hpp - where the demo apps get their simulated NG% from.
//
// In the simulator this is the turbine model. Here it is either a recorded
// trace (CSV: time_s, ng_percent) or the reference profile of the anchor table
// itself, time-scaled - which is the cheapest way to hear the whole point of
// the module: the same recording driven through a start that is 1.5x faster or
// half as fast, still in sync, pitched accordingly.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "soundsys/AnchorTable.hpp"

namespace app {

class NgSource {
public:
    // NG samples from a CSV with two numeric columns: time (s), NG (%).
    // A header line is tolerated, as are extra columns (only the first two are
    // read), which makes raw telemetry exports work unmodified.
    bool loadCsv(const std::string& path, std::string* error) {
        std::ifstream in(path);
        if (!in) {
            if (error) *error = "could not open trace: " + path;
            return false;
        }
        samples_.clear();
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            std::replace(line.begin(), line.end(), ';', ',');
            std::stringstream ss(line);
            std::string a, b;
            if (!std::getline(ss, a, ',') || !std::getline(ss, b, ',')) continue;
            char* endA = nullptr;
            char* endB = nullptr;
            const double t = std::strtod(a.c_str(), &endA);
            const double ng = std::strtod(b.c_str(), &endB);
            if (endA == a.c_str() || endB == b.c_str()) continue;  // header row
            samples_.push_back({t, ng});
        }
        if (samples_.size() < 2) {
            if (error) *error = "trace needs at least 2 rows: " + path;
            return false;
        }
        std::sort(samples_.begin(), samples_.end(),
                  [](const Sample& x, const Sample& y) { return x.t < y.t; });
        const double t0 = samples_.front().t;
        for (Sample& s : samples_) s.t -= t0;
        return true;
    }

    // The anchor table's own NG(t), replayed `speed` times faster.
    void fromAnchors(const soundsys::AnchorTable& table, double speed) {
        samples_.clear();
        if (!table.valid() || speed <= 0.0) return;
        const double span = table.lastTime() - table.firstTime();
        const int    steps = 400;
        for (int i = 0; i <= steps; ++i) {
            const double u = static_cast<double>(i) / steps;
            const double recordingTime = table.firstTime() + u * span;
            samples_.push_back({(recordingTime - table.firstTime()) / speed,
                                table.ngForTime(recordingTime)});
        }
    }

    bool empty() const { return samples_.empty(); }
    double duration() const { return samples_.empty() ? 0.0 : samples_.back().t; }

    // Linear interpolation, held at both ends.
    double at(double t) const {
        if (samples_.empty()) return 0.0;
        if (t <= samples_.front().t) return samples_.front().ng;
        if (t >= samples_.back().t) return samples_.back().ng;
        // Walk forward from the last hit: callers sample monotonically in time.
        while (cursor_ + 1 < samples_.size() && samples_[cursor_ + 1].t < t) ++cursor_;
        while (cursor_ > 0 && samples_[cursor_].t > t) --cursor_;
        const Sample& a = samples_[cursor_];
        const Sample& b = samples_[cursor_ + 1];
        const double span = b.t - a.t;
        if (span <= 0.0) return b.ng;
        return a.ng + (b.ng - a.ng) * (t - a.t) / span;
    }

private:
    struct Sample {
        double t = 0.0;
        double ng = 0.0;
    };

    std::vector<Sample>  samples_;
    mutable std::size_t  cursor_ = 0;
};

}  // namespace app
