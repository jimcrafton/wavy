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
    size_t latency = 0;  // only reported: the double's audio is not actually late
    size_t latencySamples() const override { return latency; }
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

// An effect that doubles its input and records the transport it was given before each block.
class TransportEffect : public FakeEffect, public TransportListener {
public:
    struct Seen {
        double bpm;
        bool playing;
        int64_t frame;
    };
    std::vector<Seen> seen;
    void setTransport(double bpm, bool playing, int64_t frame) noexcept override { seen.push_back({bpm, playing, frame}); }
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

TEST(TrackProgram, AnInstrumentFeedsTheEffectsAfterItInOrder) {
    ClipPlayer player(block, 2);
    auto synth = std::make_shared<FakeInstrument>();
    ProcessorBank::instance().set("chainSynth", synth);
    ProcessorBank::instance().set("chainFx1", std::make_shared<FakeEffect>());
    ProcessorBank::instance().set("chainFx2", std::make_shared<FakeEffect>());
    player.setTrackProgram(0, makePluginTrackProgram(std::vector<std::string>{"chainSynth", "chainFx1", "chainFx2"}, block),
                           {synth});
    player.addEventClip(0, 0, 4000, {{0, 0x90, 60, 100}});
    player.play();
    const Rendered out = renderFrames(player, 2048);
    EXPECT_FLOAT_EQ(out.l[1000], 0.5f * 2.0f * 2.0f);
    EXPECT_FLOAT_EQ(out.r[1000], 0.5f * 2.0f * 2.0f);
    EXPECT_THROW(makePluginTrackProgram(std::vector<std::string>{}, block), std::invalid_argument);
}

TEST(TrackProgram, TransportListenersGetTempoPlayStateAndTheBlockStartFrame) {
    ClipPlayer player(block, 2);
    auto fx = std::make_shared<TransportEffect>();
    ProcessorBank::instance().set("transportFx", fx);
    player.setTrackProgram(0, makePluginTrackProgram("transportFx", block), {fx});
    player.setTempo(97.0);
    fx->seen.clear();  // setTrackProgram warms the program

    renderFrames(player, block * 2, block);
    ASSERT_EQ(fx->seen.size(), 2u);
    EXPECT_FALSE(fx->seen[0].playing);
    EXPECT_DOUBLE_EQ(fx->seen[0].bpm, 97.0);

    fx->seen.clear();
    player.play();
    renderFrames(player, block * 3, block);
    ASSERT_EQ(fx->seen.size(), 3u);
    EXPECT_TRUE(fx->seen[0].playing);
    EXPECT_EQ(fx->seen[0].frame, 0);
    EXPECT_EQ(fx->seen[1].frame, static_cast<int64_t>(block));
    EXPECT_EQ(fx->seen[2].frame, static_cast<int64_t>(block * 2));
}

TEST(TrackProgram, DelayCompensationAlignsTracksAndTheDirectPathToTheSlowestOne) {
    ClipPlayer player(block, 2);
    auto slow = std::make_shared<FakeInstrument>();
    auto fast = std::make_shared<FakeInstrument>();
    slow->latency = 256;
    ProcessorBank::instance().set("pdcSlow", slow);
    ProcessorBank::instance().set("pdcFast", fast);
    player.setTrackProgram(0, makePluginTrackProgram("pdcSlow", block), {slow});
    player.setTrackProgram(1, makePluginTrackProgram("pdcFast", block), {fast});
    EXPECT_EQ(player.latency(), 256u);
    EXPECT_EQ(player.compensationDelay(0), 0u);
    EXPECT_EQ(player.compensationDelay(1), 256u);
    EXPECT_EQ(player.compensationDelay(-1), 256u);

    player.addEventClip(0, 1000, 5000, {{0, 0x90, 60, 100}});
    player.addEventClip(1, 1000, 5000, {{0, 0x90, 60, 100}});
    player.addClip(2000, 4096, constantClip(0.25f));
    player.play();
    const Rendered out = renderFrames(player, 4000);
    EXPECT_EQ(peakOf(out.l, 0, 1000), 0.0f);
    EXPECT_FLOAT_EQ(out.l[1000], 0.5f) << "the slow track's sound is 'already' late by its own latency";
    EXPECT_FLOAT_EQ(out.l[1255], 0.5f);
    EXPECT_FLOAT_EQ(out.l[1256], 1.0f) << "the fast track arrives 256 frames later, in step with it";
    EXPECT_FLOAT_EQ(out.l[2255], 1.0f);
    EXPECT_FLOAT_EQ(out.l[2256], 1.25f) << "and so does a clip that goes straight to the master";

    slow->latency = 0;
    player.updateLatencyCompensation();
    EXPECT_EQ(player.latency(), 0u);
    EXPECT_EQ(player.compensationDelay(1), 0u);
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

TEST(PluginHost, AnEffectIsNotCalledOnceItsInputHasBeenSilentLongerThanItsTail) {
    PluginHost host(rate, static_cast<int>(block));
    std::string error;
    auto processor = host.add("chaosTail", WAVY_CHAOS_PATH, 0, error);
    ASSERT_TRUE(processor) << error;
    processor->plugin().setParameter(3, 0.125);  // latency 512, tail 0
    processor->setOutputHold(0);

    std::vector<float> loud(block, 0.5f), quiet(block, 0.0f), l(block), r(block);
    const float* loudIn[2] = {loud.data(), loud.data()};
    const float* quietIn[2] = {quiet.data(), quiet.data()};
    float* out[2] = {l.data(), r.data()};
    EventBuffer events;
    processor->process(loudIn, out, block, events);
    EXPECT_FLOAT_EQ(l[5], 0.5f);
    for (int i = 0; i < 4; ++i) {
        processor->process(quietIn, out, block, events);
    }
    EXPECT_EQ(processor->tailSkippedBlocks(), 0u) << "the plugin's latency (512) has not elapsed yet";
    for (int i = 0; i < 4; ++i) {
        processor->process(quietIn, out, block, events);
    }
    EXPECT_EQ(processor->tailSkippedBlocks(), 4u);
    EXPECT_EQ(processor->plugin().stats().blocks, 5u);

    processor->process(loudIn, out, block, events);
    EXPECT_FLOAT_EQ(l[5], 0.5f) << "audible input wakes the plugin again";
    EXPECT_EQ(processor->plugin().stats().blocks, 6u);

    processor->setTailBypass(false);
    for (int i = 0; i < 10; ++i) {
        processor->process(quietIn, out, block, events);
    }
    EXPECT_EQ(processor->tailSkippedBlocks(), 4u);
}

TEST(PluginHost, ALatencyRestartRequestIsRequeriedOnTheMainThreadAndOnlyActedOnWhenItChanged) {
    PluginHost host(rate, static_cast<int>(block));
    std::string error;
    auto processor = host.add("chaosRestart", WAVY_CHAOS_PATH, 0, error);
    ASSERT_TRUE(processor) << error;
    EXPECT_FALSE(host.pollRestarts());

    processor->plugin().setParameter(3, 0.125);  // the plugin asks for a restart, but its processor has not seen the value yet
    EXPECT_FALSE(host.pollRestarts()) << "an unchanged latency is not a change";

    std::vector<float> in(block, 0.5f), l(block), r(block);
    const float* inPtr[2] = {in.data(), in.data()};
    float* out[2] = {l.data(), r.data()};
    EventBuffer events;
    processor->process(inPtr, out, block, events);
    processor->plugin().setParameter(3, 0.125);  // the plugin hints again, now its latency really is 512
    EXPECT_EQ(processor->latencySamples(), 512u);
    EXPECT_TRUE(host.pollRestarts());
    EXPECT_FALSE(host.pollRestarts()) << "the flags are consumed";
    processor->plugin().setParameter(3, 0.125);
    EXPECT_FALSE(host.pollRestarts()) << "a hint without a change";
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

TEST(PluginHost, ALoudPluginIsHeldAtTheCeilingAndTheGainReductionMeterShowsItThenRecovers) {
    constexpr float ceilingDb = -1.0f;
    PluginHost host(rate, static_cast<int>(block));
    std::string error;
    auto processor = host.add("chaosLoud", WAVY_CHAOS_PATH, 0, error);
    ASSERT_TRUE(processor) << error;

    ClipPlayer player(block, 2);
    player.enableMasterLimiter(rate, ceilingDb, 100.0f);
    player.setTrackProgram(0, makePluginTrackProgram("chaosLoud", block), {processor});
    player.addClip(0, 200000, constantClip(0.25f), 1.0f, {}, 0);
    player.play();

    const auto reductionDb = [&] { return 20.0f * std::log10(player.takeLimiterGain()); };
    const float ceiling = std::pow(10.0f, ceilingDb / 20.0f);

    Rendered out = renderFrames(player, 4096);
    EXPECT_FLOAT_EQ(reductionDb(), 0.0f) << "a well-behaved plugin leaves the meter at 0 dB";
    EXPECT_NEAR(out.l[4000], 0.25f, 1e-5f);

    processor->plugin().setParameter(0, 5.0 / 6.0);  // loud: x1e6
    out = renderFrames(player, 4096);
    EXPECT_LE(peakOf(out.l, 0, out.l.size()), ceiling + 1e-5f);
    EXPECT_LE(peakOf(out.r, 0, out.r.size()), ceiling + 1e-5f);
    EXPECT_GT(peakOf(out.l, 2048, 4096), ceiling * 0.95f) << "held near the ceiling, not crushed to silence";
    EXPECT_LT(reductionDb(), -90.0f) << "0.25 * 1e6 against a -1 dB ceiling is about -108 dB";
    EXPECT_FALSE(processor->plugin().failed());

    processor->plugin().setParameter(0, 0.0);  // back to pass-through
    renderFrames(player, 40000);  // several release time constants
    player.takeLimiterGain();
    out = renderFrames(player, 4096);
    EXPECT_GT(reductionDb(), -0.5f) << "the meter is back near 0 dB";
    EXPECT_NEAR(out.l.back(), 0.25f, 0.005f);
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

TEST(PluginHost, SurgeXtEffectsProcessesTheSurgeXtInstrumentOnTheSameTrack) {
    const std::string synthPath = PluginHost::commonVst3Path("Surge Synth Team/Surge XT.vst3/Contents/x86_64-win/Surge XT.vst3");
    const std::string fxPath =
        PluginHost::commonVst3Path("Surge Synth Team/Surge XT Effects.vst3/Contents/x86_64-win/Surge XT Effects.vst3");
    if (synthPath.empty() || fxPath.empty()) GTEST_SKIP() << "Surge XT and Surge XT Effects are not both installed";
    PluginHost host(rate, static_cast<int>(block));
    std::string error;
    auto synth = host.add("surgeChainSynth", synthPath, 0, error);
    ASSERT_TRUE(synth) << error;
    auto fx = host.add("surgeChainFx", fxPath, 0, error);
    ASSERT_TRUE(fx) << error;
    ASSERT_FALSE(fx->isInstrument());

    ClipPlayer player(block, 2);
    player.setTrackProgram(0, makePluginTrackProgram(std::vector<std::string>{"surgeChainSynth", "surgeChainFx"}, block), {synth, fx});
    player.setTempo(120.0);
    player.addEventClip(0, 0, 48000, {{0, 0x90, 60, 100}, {24000, 0x80, 60, 0}});
    player.play();
    const Rendered out = renderFrames(player, 48000);
    EXPECT_GT(peakOf(out.l, 2000, 24000), 0.001f);
    EXPECT_FALSE(synth->plugin().failed());
    EXPECT_FALSE(fx->plugin().failed());
    for (float v : out.l) {
        ASSERT_TRUE(std::isfinite(v));
    }
}

namespace {

// Feeds a short burst then `silence` frames of silence through an effect, returns the energy of everything after the burst.
double energyAfterBurst(VstProcessor& fx, size_t silence) {
    std::vector<float> in(block), l(block), r(block);
    const float* inPtr[2] = {in.data(), in.data()};
    float* out[2] = {l.data(), r.data()};
    EventBuffer events;
    for (size_t b = 0; b < 8; ++b) {
        for (size_t i = 0; i < block; ++i) {
            in[i] = 0.5f * std::sin(0.05f * static_cast<float>(b * block + i));
        }
        fx.process(inPtr, out, block, events);
    }
    std::fill(in.begin(), in.end(), 0.0f);
    double energy = 0.0;
    for (size_t at = 0; at < silence; at += block) {
        fx.process(inPtr, out, block, events);
        for (size_t i = 0; i < block; ++i) {
            energy += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
        }
    }
    return energy;
}

void expectReverbTailSurvivesBypass(const char* relative, const char* name) {
    const std::string path = PluginHost::commonVst3Path(relative);
    if (path.empty()) GTEST_SKIP() << relative << " not installed";
    PluginHost host(rate, static_cast<int>(block));
    std::string error;
    auto always = host.add(std::string(name) + "A", path, 0, error);
    ASSERT_TRUE(always) << error;
    always->setTailBypass(false);
    auto bypassing = host.add(std::string(name) + "B", path, 0, error);
    ASSERT_TRUE(bypassing) << error;

    const size_t silence = 5 * 48000;
    const double full = energyAfterBurst(*always, silence);
    const double gated = energyAfterBurst(*bypassing, silence);
    ASSERT_GT(full, 0.0) << "the reverb produced no tail to protect";
    EXPECT_GT(gated, 0.95 * full) << "tail bypass cut the reverb's tail (plugin reports tail " << bypassing->tailSamples() << ")";
    EXPECT_GT(bypassing->tailSkippedBlocks() + 1, 0u);
}

}  // namespace

TEST(PluginHost, TailBypassDoesNotCutValhallaSupermassiveWhichReportsATailOfZero) {
    expectReverbTailSurvivesBypass("ValhallaSupermassive.vst3", "valhalla");
}

TEST(PluginHost, TailBypassDoesNotCutRevPlate140WhichReportsATailOfZero) {
    expectReverbTailSurvivesBypass("Rev PLATE-140.vst3", "plate");
}
