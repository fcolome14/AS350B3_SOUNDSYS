// End-to-end check of the whole idea:
//
// A synthetic "recording" is generated whose pitch is a known multiple of NG -
// exactly the property a real turbine recording has. The engine is then driven
// with a start that spools up faster (and separately, slower) than the recorded
// one, and the pitch of what comes out is measured. If the NG -> position
// mapping and the varispeed playback are right, the output frequency must equal
// kHzPerNg * NG_simulated at every instant, whatever the spool rate.
//
// This is the test that would catch a wrong anchor, an inverted slope, a
// resampler fed the reciprocal ratio, or a sync loop that lags.

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "TestSupport.hpp"
#include "soundsys/AudioBuffer.hpp"
#include "soundsys/AudioEngine.hpp"
#include "soundsys/voices/TurbineStartVoice.hpp"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kHzPerNg = 20.0;  // 67 % NG -> 1340 Hz, a plausible N1 whine

// The reference start: NG% against time in the recording. Deliberately
// piecewise, with a slow crank, a fast mid-section and a lazy approach to idle,
// so a mapping that ignores the segment slopes cannot pass by accident.
struct Point {
    double ng;
    double t;
    const char* label;
};

const Point kProfile[] = {
    { 0.0,  0.0, "starter_engage"},
    { 8.0,  3.0, "light_off"},
    {15.0,  6.0, "ignition_off"},
    {30.0, 12.0, "gen_online"},
    {45.0, 18.0, "accel"},
    {58.0, 24.0, "approach_idle"},
    {67.0, 30.0, "idle"},
};
constexpr int kProfileCount = static_cast<int>(sizeof(kProfile) / sizeof(kProfile[0]));
const double  kProfileDuration = kProfile[kProfileCount - 1].t;

double ngAtRecordingTime(double t) {
    if (t <= kProfile[0].t) return kProfile[0].ng;
    for (int i = 1; i < kProfileCount; ++i) {
        if (t <= kProfile[i].t) {
            const double u = (t - kProfile[i - 1].t) / (kProfile[i].t - kProfile[i - 1].t);
            return kProfile[i - 1].ng + u * (kProfile[i].ng - kProfile[i - 1].ng);
        }
    }
    return kProfile[kProfileCount - 1].ng;
}

// A mono sine whose instantaneous frequency is kHzPerNg * NG(t).
bool writeSyntheticRecording(const std::string& path) {
    const std::uint64_t frames = static_cast<std::uint64_t>(kProfileDuration * kSampleRate);
    std::vector<float>  samples(static_cast<std::size_t>(frames));
    double              phase = 0.0;
    for (std::uint64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double freq = kHzPerNg * ngAtRecordingTime(t);
        phase += 2.0 * 3.14159265358979323846 * freq / kSampleRate;
        // Fade the first 100 ms in so the very first zero crossings are clean.
        const double fade = std::min(1.0, t / 0.1);
        samples[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * fade * std::sin(phase));
    }
    return soundsys::writeWavFloat32(path, samples.data(), frames, 1, kSampleRate, nullptr);
}

bool writeAnchors(const std::string& path, const std::string& assetPath) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "{\n  \"schema\": \"soundsys.anchors/1\",\n");
    std::fprintf(f, "  \"engine\": \"synthetic\",\n");
    std::fprintf(f, "  \"asset\": \"%s\",\n  \"anchors\": [\n", assetPath.c_str());
    for (int i = 0; i < kProfileCount; ++i) {
        std::fprintf(f, "    {\"ng\": %.3f, \"t\": %.3f, \"label\": \"%s\"}%s\n", kProfile[i].ng,
                     kProfile[i].t, kProfile[i].label, (i + 1 < kProfileCount) ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
    return true;
}

// Zero-crossing frequency estimate over one window of a single channel.
double measureFrequency(const std::vector<float>& interleaved, std::uint32_t channels,
                        std::uint64_t startFrame, std::uint64_t windowFrames) {
    int crossings = 0;
    for (std::uint64_t i = 1; i < windowFrames; ++i) {
        const float prev = interleaved[(startFrame + i - 1) * channels];
        const float cur = interleaved[(startFrame + i) * channels];
        if ((prev < 0.0f && cur >= 0.0f) || (prev >= 0.0f && cur < 0.0f)) ++crossings;
    }
    const double seconds = static_cast<double>(windowFrames) / kSampleRate;
    return crossings / (2.0 * seconds);
}

struct RenderResult {
    std::vector<float> audio;
    std::uint32_t      channels = 2;
    double             finalSyncError = 0.0;
    double             maxSyncError = 0.0;
};

// Runs a full start through the engine with NG(t) = reference profile replayed
// `spoolFactor` times faster. `holdFrom`, if positive, freezes NG at that wall
// time - a hung start.
RenderResult renderStart(const std::string& wav, const std::string& anchorsPath,
                         double spoolFactor, soundsys::PlaybackMode mode,
                         double holdFrom = -1.0, double extraSeconds = 0.0) {
    RenderResult result;

    soundsys::TurbineStartConfig cfg;
    cfg.wavPath = wav;
    cfg.anchorsPath = anchorsPath;
    cfg.mode = mode;

    auto voice = std::make_unique<soundsys::TurbineStartVoice>(cfg);
    std::string error;
    if (!voice->load(&error)) {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
        return result;
    }

    soundsys::AudioEngine engine;
    auto* start = static_cast<soundsys::TurbineStartVoice*>(engine.addVoice(std::move(voice)));

    soundsys::AudioFormat fmt;
    fmt.sampleRate = kSampleRate;
    fmt.maxBlockFrames = 256;
    engine.prepare(fmt);

    const double simDuration = kProfileDuration / spoolFactor + extraSeconds;
    const std::uint64_t totalFrames = static_cast<std::uint64_t>(simDuration * kSampleRate);
    result.audio.assign(static_cast<std::size_t>(totalFrames) * result.channels, 0.0f);

    engine.pushEvent(soundsys::EventType::EngineStartCommand, 0.0f);

    for (std::uint64_t frame = 0; frame < totalFrames; frame += fmt.maxBlockFrames) {
        const std::uint32_t block = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(fmt.maxBlockFrames, totalFrames - frame));
        const double t = static_cast<double>(frame) / kSampleRate;

        const double simTime = (holdFrom > 0.0) ? std::min(t, holdFrom) : t;
        engine.setParameter(soundsys::ParamId::NgPercent,
                            static_cast<float>(ngAtRecordingTime(simTime * spoolFactor)));
        engine.render(result.audio.data() + static_cast<std::size_t>(frame) * result.channels,
                      block, result.channels);

        if (t > 1.0) {
            result.maxSyncError = std::max(result.maxSyncError, std::fabs(start->syncErrorSeconds()));
        }
    }
    result.finalSyncError = start->syncErrorSeconds();
    return result;
}

// The assertion that matters: measured pitch == pitchFactor * kHzPerNg * NG.
//
// pitchFactor is 1.0 for a physically correct head - a turbine's tone depends on
// NG and on nothing else. For the varispeed head it is the playback rate, which
// is exactly what that mode costs you.
void checkPitchTracksNg(const RenderResult& r, double spoolFactor, double pitchFactor,
                        double tolerancePercent, const char* label) {
    std::printf("%-26s spool x%.2f  %6.2f s out  max sync error %5.1f ms\n", label, spoolFactor,
                static_cast<double>(r.audio.size() / r.channels) / kSampleRate,
                r.maxSyncError * 1000.0);
    CHECK(!r.audio.empty());
    if (r.audio.empty()) return;

    const std::uint64_t totalFrames = r.audio.size() / r.channels;
    const std::uint64_t window = static_cast<std::uint64_t>(0.25 * kSampleRate);

    int checked = 0;
    // Skip the first second (the speed filter is still converging) and the last
    // half second (the voice is fading out into the handover).
    for (std::uint64_t frame = static_cast<std::uint64_t>(1.0 * kSampleRate);
         frame + window < totalFrames - static_cast<std::uint64_t>(0.5 * kSampleRate);
         frame += window) {
        const double centre = (static_cast<double>(frame) + 0.5 * window) / kSampleRate;
        const double ng = ngAtRecordingTime(centre * spoolFactor);
        if (ng < 10.0) continue;  // too few cycles per window to measure reliably

        const double expected = pitchFactor * kHzPerNg * ng;
        const double measured = measureFrequency(r.audio, r.channels, frame, window);
        CHECK_NEAR(measured, expected, expected * tolerancePercent * 0.01);
        ++checked;
    }
    CHECK(checked > 10);
}

}  // namespace

int main() {
    const std::string wav = "test_sync_asset.wav";
    const std::string anchorsPath = "test_sync_anchors.json";

    CHECK(writeSyntheticRecording(wav));
    CHECK(writeAnchors(anchorsPath, wav));

    using soundsys::PlaybackMode;

    // --- the default head: position follows NG, pitch stays physical ---------
    for (const double spool : {1.0, 1.6, 0.65}) {
        const RenderResult r = renderStart(wav, anchorsPath, spool, PlaybackMode::PitchLocked);
        // Whatever the spool rate, the tone must be the tone of the engine at
        // that NG: pitch factor 1.0.
        checkPitchTracksNg(r, spool, 1.0, 4.0, "pitch-locked");
        CHECK_NEAR(r.maxSyncError, 0.0, 0.15);
    }

    // --- varispeed: identical when the sim matches the recording -------------
    const RenderResult varispeedAsRecorded =
        renderStart(wav, anchorsPath, 1.0, PlaybackMode::Varispeed);
    checkPitchTracksNg(varispeedAsRecorded, 1.0, 1.0, 3.0, "varispeed as recorded");
    CHECK_NEAR(varispeedAsRecorded.maxSyncError, 0.0, 0.05);

    // ...and off by exactly the playback rate as soon as it does not. This is
    // not a bug in the varispeed head, it is what varispeed is: the pitch is
    // dragged along with the tape speed. Asserted here so the trade-off is a
    // measured number rather than a footnote.
    const RenderResult varispeedFast =
        renderStart(wav, anchorsPath, 1.6, PlaybackMode::Varispeed);
    checkPitchTracksNg(varispeedFast, 1.6, 1.6, 4.0, "varispeed spooled fast");

    // --- a hung start: NG freezes at 5 s and never reaches idle --------------
    // The physical answer is a steady tone at that NG. The pitch-locked head
    // holds it; varispeed would slide down to a rumble as the head stops.
    const RenderResult hung =
        renderStart(wav, anchorsPath, 1.0, PlaybackMode::PitchLocked, 5.0, 6.0);
    const std::uint64_t hungFrames = hung.audio.size() / hung.channels;
    const std::uint64_t window = static_cast<std::uint64_t>(0.25 * kSampleRate);
    const double        heldNg = ngAtRecordingTime(5.0);
    // From 7 s on: a second past the freeze, the sync loop has settled and the
    // tone should just sit there.
    for (double t : {7.0, 9.0, 11.0}) {
        const std::uint64_t at = static_cast<std::uint64_t>(t * kSampleRate);
        if (at + window >= hungFrames) continue;
        const double measured = measureFrequency(hung.audio, hung.channels, at, window);
        CHECK_NEAR(measured, kHzPerNg * heldNg, kHzPerNg * heldNg * 0.05);
    }

    std::remove(wav.c_str());
    std::remove(anchorsPath.c_str());
    return test::summary("sync");
}
