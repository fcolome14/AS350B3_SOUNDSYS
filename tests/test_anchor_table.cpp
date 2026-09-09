// Anchor table: the map, its inverse, its slope, and the invariants that stop a
// bad table from reaching the audio thread.

#include <cmath>
#include <string>
#include <vector>

#include "TestSupport.hpp"
#include "soundsys/AnchorTable.hpp"

using soundsys::Anchor;
using soundsys::AnchorTable;

namespace {

const char* kTableJson = R"json({
  "schema": "soundsys.anchors/1",
  "engine": "Arriel 2B1",
  "asset": "assets/wav/start.wav",
  "anchors": [
    {"ng":  0.0, "t":  0.0, "label": "starter_engage"},
    {"ng": 15.0, "t":  6.0, "label": "light_off"},
    {"ng": 45.0, "t": 18.0, "label": "ignition_off"},
    {"ng": 67.0, "t": 30.0, "label": "idle"}
  ]
})json";

void testForwardAndInverse() {
    AnchorTable table;
    std::string error;
    CHECK(table.parseJson(kTableJson, &error));
    CHECK(error.empty());
    CHECK(table.valid());
    CHECK(table.engine() == "Arriel 2B1");
    CHECK(table.anchors().size() == 4);

    // On the anchors themselves the map is exact.
    CHECK_NEAR(table.timeForNg(15.0), 6.0, 1e-9);
    CHECK_NEAR(table.timeForNg(45.0), 18.0, 1e-9);
    CHECK_NEAR(table.ngForTime(18.0), 45.0, 1e-9);

    // Halfway along a segment, linear interpolation.
    CHECK_NEAR(table.timeForNg(30.0), 12.0, 1e-9);
    CHECK_NEAR(table.ngForTime(12.0), 30.0, 1e-9);

    // Round trip.
    for (double ng = 1.0; ng < 67.0; ng += 3.5) {
        CHECK_NEAR(table.ngForTime(table.timeForNg(ng)), ng, 1e-6);
    }

    // Clamped outside the anchored range, never extrapolated: an NG above idle
    // must not walk the playback head past the end of the recording.
    CHECK_NEAR(table.timeForNg(-10.0), 0.0, 1e-9);
    CHECK_NEAR(table.timeForNg(120.0), 30.0, 1e-9);

    CHECK_NEAR(table.timeOfLabel("light_off"), 6.0, 1e-9);
    CHECK_NEAR(table.timeOfLabel("no_such_label"), -1.0, 1e-9);
}

void testSlope() {
    AnchorTable table;
    CHECK(table.parseJson(kTableJson, nullptr));

    // Seconds of recording per percent of NG, per segment.
    CHECK_NEAR(table.timePerNg(7.5), 6.0 / 15.0, 1e-9);
    CHECK_NEAR(table.timePerNg(30.0), 12.0 / 30.0, 1e-9);
    CHECK_NEAR(table.timePerNg(60.0), 12.0 / 22.0, 1e-9);

    // The slope is what turns an NG rate into a playback speed: a start that
    // covers this segment twice as fast as the recording must play at 2x.
    const double ngRate = 2.0 * (30.0 / 12.0);  // %/s, twice the recorded rate
    CHECK_NEAR(table.timePerNg(30.0) * ngRate, 2.0, 1e-9);
}

void testRejectsBadTables() {
    std::string error;

    AnchorTable tooShort;
    CHECK(!tooShort.parseJson(R"json({"anchors":[{"ng":0,"t":0}]})json", &error));
    CHECK(!error.empty());

    // NG goes up, time goes back down: not invertible, and silently accepting it
    // would make the playback head jump backwards mid-start.
    AnchorTable nonMonotonic;
    error.clear();
    CHECK(!nonMonotonic.parseJson(
        R"json({"anchors":[{"ng":0,"t":0},{"ng":20,"t":10},{"ng":40,"t":4}]})json", &error));
    CHECK(error.find("time") != std::string::npos);

    // Duplicate NG: a vertical segment, i.e. an infinite playback speed.
    AnchorTable duplicate;
    error.clear();
    CHECK(!duplicate.parseJson(
        R"json({"anchors":[{"ng":0,"t":0},{"ng":20,"t":5},{"ng":20,"t":9}]})json", &error));
    CHECK(error.find("NG") != std::string::npos);

    AnchorTable malformed;
    CHECK(!malformed.parseJson("{ not json", nullptr));

    AnchorTable missingField;
    CHECK(!missingField.parseJson(R"json({"anchors":[{"ng":0},{"ng":20,"t":5}]})json", nullptr));
}

void testUnsortedInputIsSorted() {
    AnchorTable table;
    std::vector<Anchor> anchors = {{45.0, 18.0, "ignition_off"},
                                   {0.0, 0.0, "starter_engage"},
                                   {15.0, 6.0, "light_off"}};
    CHECK(table.setAnchors(anchors, nullptr));
    CHECK_NEAR(table.firstNg(), 0.0, 1e-9);
    CHECK_NEAR(table.lastNg(), 45.0, 1e-9);
    CHECK_NEAR(table.timeForNg(15.0), 6.0, 1e-9);
}

}  // namespace

int main() {
    testForwardAndInverse();
    testSlope();
    testRejectsBadTables();
    testUnsortedInputIsSorted();
    return test::summary("anchor_table");
}
