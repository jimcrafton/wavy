#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "jitnodes/jitnodes.h"
#include "wavy/clipPlayer.h"
#include "wavy/pluginHost.h"

// The track layer of ClipPlayer with a fake instrument (always runs), PluginHost/VstProcessor against wavyChaos, and Surge XT through the
// whole path (skipped when it is not installed).
namespace {

using namespace wavy;
using namespace jitnodes;

constexpr size_t block = 128;
constexpr double rate = 48000.0;

// An instrument double: outputs 0.5 on both channels while a note is held plus `ring` samples after the last note off. reset() releases.
class FakeInstrument : public ExternalProcessor {
public:
    struct Seen {
        uint64_t frame;
        uint8_t status;
        uint8_t data1;
    };
    size_t ring = 0;
    std::vector<Seen> seen;
    int resets = 0;
    uint64_t blocks = 0;

    void process(const float* const*, float* const* out, size_t frames, const EventBuffer& events) noexcept override {
        size_t next = 0;
        for (size_t i = 0; i < frames; ++i) {
            while (next < events.size() && events[next].offset == i) {
                const AudioEvent& e = events[next++];
                seen.push_back({blocks * block + i, e.status, e.data1});
                if (e.isNoteOn()) {
                    ++held_;
                    ringLeft_ = 0;
                } else if (e.isNoteOff() && held_ > 0 && --held_ == 0) {
                    ringLeft_ = ring;
                }
            }
            const bool sounding = held_ > 0 || ringLeft_ > 0;
            if (held_ == 0 && ringLeft_ > 0) --ringLeft_;
            out[0][i] = out[1][i] = sounding ? 0.5f : 0.0f;
        }
        ++blocks;
    }
    void reset() noexcept override {
        ++resets;
        if (held_ > 0) {
            held_ = 0;
            ringLeft_ = ring;
        }
    }

private:
    int held_ = 0;
    size_t ringLeft_ = 0;
};

// Doubles its input (an effect).
class FakeEffect : public ExternalProcessor {
public:
    void process(const float* const* in, float* const* out, size_t frames, const EventBuffer&) noexcept override {
        for (size_t c = 0; c < 2; ++c) {
            for (size_t i = 0; i < frames; ++i) {
                out[c][i] = in[c][i] * 2.0f;
            }
        }
    }
};

std::unique_ptr<AudioProgram> constantClip(float value) {
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    const NodeId k = net.addNode("wrangle");
    net.node(k).setString("code", "x = " + std::to_string(value) + ";");
    net.connect(line, k);
    return AudioProgram::compile(net, k, {block, {"x"}});
}

struct Rendered {
    std::vector<float> l, r;
};

Rendered renderFrames(ClipPlayer& player, size_t frames, size_t chunk = 480) {
    Rendered out;
    out.l.resize(frames);
    out.r.resize(frames);
    for (size_t at = 0; at < frames; at += chunk) {
        const size_t n = std::min(chunk, frames - at);
        float* ptr[] = {out.l.data() + at, out.r.data() + at};
        player.render(ptr, n);
    }
    return out;
}

float peakOf(const std::vector<float>& v, size_t from, size_t to) {
    float m = 0.0f;
    for (size_t i = from; i < to && i < v.size(); ++i) {
        m = std::max(m, std::fabs(v[i]));
    }
    return m;
}

struct TrackFixture {
    ClipPlayer player{block, 2};
    std::shared_ptr<FakeInstrument> synth = std::make_shared<FakeInstrument>();
    TrackFixture(const char* name = "fakeSynth") {
        ProcessorBank::instance().set(name, synth);
        player.setTrackProgram(0, makePluginTrackProgram(name, block), {synth});
        synth->blocks = 0;  // setTrackProgram warms the program with one block
    }
};

}  // namespace

TEST(TrackProgram, EventClipsReachTheInstrumentAtTheirFramesAndMakeItAudible) {
    TrackFixture f("fakeSynthA");
    f.player.addEventClip(0, 1000, 5000, {{10, 0x90, 60, 100}, {3000, 0x80, 60, 0}});
    f.player.play();
    const Rendered out = renderFrames(f.player, 8000);

    ASSERT_EQ(f.synth->seen.size(), 2u);
    EXPECT_EQ(f.synth->seen[0].frame, 1010u);
    EXPECT_EQ(f.synth->seen[1].frame, 4000u);
    EXPECT_EQ(peakOf(out.l, 0, 1010), 0.0f);
    EXPECT_FLOAT_EQ(out.l[1010], 0.5f);
    EXPECT_FLOAT_EQ(out.r[3999], 0.5f);
    EXPECT_EQ(peakOf(out.l, 4000, 8000), 0.0f);
}

TEST(TrackProgram, AClipThatEndsWithANoteHeldReleasesItAtTheClipEnd) {
    TrackFixture f("fakeSynthB");
    f.player.addEventClip(0, 0, 2000, {{100, 0x90, 64, 100}});
    f.player.play();
    const Rendered out = renderFrames(f.player, 4000);

    ASSERT_EQ(f.synth->seen.size(), 2u);
    EXPECT_EQ(f.synth->seen[1].status & 0xF0, 0x80);
    EXPECT_EQ(f.synth->seen[1].data1, 64);
    EXPECT_LT(f.synth->seen[1].frame, 2000u);
    EXPECT_EQ(peakOf(out.l, 2100, 4000), 0.0f);
}

TEST(TrackProgram, StopAndSeekResetTheProcessorsAndTheTailRingsWhileStopped) {
    TrackFixture f("fakeSynthC");
    f.synth->ring = 1000;
    f.player.addEventClip(0, 0, 48000, {{0, 0x90, 60, 100}});
    f.player.play();
    renderFrames(f.player, 2000);
    EXPECT_EQ(f.synth->resets, 0);

    f.player.stop();
    const Rendered stopped = renderFrames(f.player, 4000);
    EXPECT_GE(f.synth->resets, 1);
    EXPECT_GT(peakOf(stopped.l, 0, 500), 0.4f) << "the release tail keeps ringing after Stop";
    EXPECT_EQ(peakOf(stopped.l, 2500, 4000), 0.0f) << "and then falls silent";

    const int before = f.synth->resets;
    f.player.seek(0);
    f.player.play();
    renderFrames(f.player, 480);
    EXPECT_GT(f.synth->resets, before);
}

TEST(TrackProgram, AudioClipsOnATrackAreSummedIntoItsInputAndOthersGoStraightToTheMaster) {
    ClipPlayer player(block, 2);
    ProcessorBank::instance().set("fakeFx", std::make_shared<FakeEffect>());
    player.setTrackProgram(1, makePluginTrackProgram("fakeFx", block));
    player.addClip(0, 4096, constantClip(0.25f), 1.0f, {}, 1);
    player.addClip(0, 4096, constantClip(0.125f), 1.0f, {}, 1);
    player.addClip(0, 4096, constantClip(0.1f));
    player.play();
    const Rendered out = renderFrames(player, 2048);
    for (size_t i = 0; i < 2048; ++i) {
        ASSERT_NEAR(out.l[i], 2.0f * (0.25f + 0.125f) + 0.1f, 1e-5f);
        ASSERT_NEAR(out.r[i], 2.0f * (0.25f + 0.125f) + 0.1f, 1e-5f);
    }
}

TEST(TrackProgram, TracksWithoutAProgramAndPlayersWithoutTracksKeepWorking) {
    ClipPlayer player(block, 2);
    EXPECT_FALSE(player.hasTrackProgram(0));
    player.addClip(0, 4096, constantClip(0.25f), 1.0f, {}, 3);
    player.play();
    EXPECT_FLOAT_EQ(renderFrames(player, 256).l[200], 0.25f);
    player.stop();
    EXPECT_EQ(peakOf(renderFrames(player, 256).l, 0, 256), 0.0f);
    EXPECT_THROW(player.setTrackProgram(ClipPlayer::maxTracks, constantClip(0.0f)), std::invalid_argument);
    EXPECT_THROW(player.setTrackProgram(0, constantClip(0.0f)), std::invalid_argument) << "a track program is stereo";
}

TEST(PluginHost, MissingBinaryReportsAnErrorAndRegistersNothing) {
    PluginHost host(rate, static_cast<int>(block));
    std::string error;
    EXPECT_EQ(host.add("nope", "C:/does/not/exist.vst3", 0, error), nullptr);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(ProcessorBank::instance().get("nope"), nullptr);
    EXPECT_THROW(makePluginTrackProgram("nope", block), std::invalid_argument);
}

#ifdef WAVY_CHAOS_PATH
TEST(PluginHost, ASlowPluginIsSkippedForAWhileAndTriedAgain) {
    PluginHost host(rate, static_cast<int>(block));
    std::string error;
    auto processor = host.add("chaosSlow", WAVY_CHAOS_PATH, 0, error, {2, 6});
    ASSERT_TRUE(processor) << error;
    EXPECT_EQ(ProcessorBank::instance().get("chaosSlow"), processor);
    processor->plugin().setParameter(1, 0.03);  // 30 ms per block against 2.7 ms
    processor->plugin().setParameter(0, 1.0 / 6.0);

    std::vector<float> inL(block, 0.5f), inR(block, 0.5f), l(block), r(block);
    const float* in[2] = {inL.data(), inR.data()};
    float* out[2] = {l.data(), r.data()};
    EventBuffer events;
    bool sawBypass = false;
    for (int i = 0; i < 14; ++i) {
        const uint64_t skippedBefore = processor->skippedBlocks();
        processor->process(in, out, block, events);
        if (processor->skippedBlocks() > skippedBefore) {
            sawBypass = true;
            EXPECT_FLOAT_EQ(l[7], 0.5f) << "a bypassed effect passes its input";
        }
    }
    EXPECT_TRUE(sawBypass);
    EXPECT_GE(processor->skipEpisodes(), 1u);
    EXPECT_GE(processor->skippedBlocks(), 6u);
    EXPECT_GE(processor->plugin().stats().blocks, 4u) << "after the skip the plugin is tried again";
}

TEST(PluginHost, ACrashedPluginIsBypassedWithoutTakingTheProgramDown) {
    PluginHost host(rate, static_cast<int>(block));
    std::string error;
    auto processor = host.add("chaosCrash", WAVY_CHAOS_PATH, 0, error);
    ASSERT_TRUE(processor) << error;
    processor->plugin().setParameter(0, 2.0 / 6.0);

    std::vector<float> inL(block, 0.5f), inR(block, 0.5f), l(block), r(block);
    const float* in[2] = {inL.data(), inR.data()};
    float* out[2] = {l.data(), r.data()};
    EventBuffer events;
    for (int i = 0; i < 4; ++i) {
        processor->process(in, out, block, events);
    }
    EXPECT_TRUE(processor->plugin().failed());
    EXPECT_FLOAT_EQ(l[3], 0.5f);
    EXPECT_GE(processor->skippedBlocks(), 2u);
}
#endif

TEST(PluginHost, SurgeXtPlaysAMidiClipOnATrackThroughTheWholePath) {
    const std::string path = PluginHost::commonVst3Path("Surge Synth Team/Surge XT.vst3/Contents/x86_64-win/Surge XT.vst3");
    if (path.empty()) GTEST_SKIP() << "Surge XT not installed";
    PluginHost host(rate, static_cast<int>(block));
    std::string error;
    auto surge = host.add("surgeTest", path, 0, error);
    ASSERT_TRUE(surge) << error;
    ASSERT_TRUE(surge->isInstrument());

    ClipPlayer player(block, 2);
    player.setTrackProgram(0, makePluginTrackProgram("surgeTest", block), {surge});
    player.addEventClip(0, 4800, 48000, {{0, 0x90, 60, 100}, {24000, 0x80, 60, 0}});
    player.play();
    const Rendered playing = renderFrames(player, 60000);
    EXPECT_EQ(peakOf(playing.l, 0, 4800), 0.0f);
    EXPECT_GT(peakOf(playing.l, 4800 + 4800, 4800 + 24000), 0.01f);
    EXPECT_FALSE(surge->plugin().failed());
    for (float v : playing.l) {
        ASSERT_TRUE(std::isfinite(v));
    }

    player.stop();
    const Rendered after = renderFrames(player, 96000);
    for (float v : after.l) {
        ASSERT_TRUE(std::isfinite(v));
    }
    EXPECT_EQ(surge->skippedBlocks(), 0u);
}
