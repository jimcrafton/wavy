#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

#include "jitnodes/jitnodes.h"
#include "wavFile.h"
#include "wavy/clipPlayer.h"
#include "wavy/wavLoader.h"

using namespace jitnodes;
using namespace wavy;

namespace {

constexpr size_t block = 128;
constexpr double twoPi = 6.283185307179586;

std::shared_ptr<SampleBuffer> ramp(size_t n) {
    auto buffer = std::make_shared<SampleBuffer>();
    buffer->sampleRate = 48000.0;
    for (size_t i = 0; i < n; ++i) buffer->frames.push_back(0.001f * static_cast<float>(i + 1));
    return buffer;
}

// Renders `total` frames in callbacks of `chunk` frames, the way a device would.
std::vector<float> render(ClipPlayer& player, size_t total, size_t chunk) {
    std::vector<float> all(total);
    for (size_t at = 0; at < total; at += chunk) player.render(all.data() + at, std::min(chunk, total - at));
    return all;
}

std::unique_ptr<AudioProgram> oscProgram(float hz) {
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    const NodeId osc = net.addNode("wrangle");
    net.node(osc).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
    net.connect(line, osc);
    const NodeId gain = net.addNode("wrangle");
    net.node(gain).setString("code", "x = x * a;");
    net.connect(osc, gain);
    auto program = AudioProgram::compile(net, gain, {block, {"x"}});
    program->param(osc, 1) = hz / 48000.0f;
    program->param(gain, 0) = 0.5f;
    program->addPhase(osc, 0, 1);
    return program;
}

}  // namespace

TEST(WavLoader, ReadsAFloatFileAndResamples) {
    const std::string path = (std::filesystem::temp_directory_path() / "wavyLoaderTest.wav").string();
    std::vector<float> samples(480);
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = std::sin(0.05f * static_cast<float>(i));
    ASSERT_TRUE(wavy::testing::writeWavFloat(path, samples, 48000));

    auto same = loadWav(path);
    ASSERT_EQ(same->frames.size(), samples.size());
    EXPECT_EQ(same->sampleRate, 48000.0);
    EXPECT_FLOAT_EQ(same->frames[100], samples[100]);

    auto half = loadWav(path, 24000.0);
    EXPECT_EQ(half->frames.size(), 240u);
    EXPECT_NEAR(half->frames[50], samples[100], 1e-5);
    std::filesystem::remove(path);
    EXPECT_THROW(loadWav(path), std::runtime_error);
}

TEST(ClipPlayer, SampleClipStartsOnItsFrameWhateverTheCallbackSize) {
    SampleBank::instance().set("clipRamp", ramp(500));
    for (const size_t chunk : {size_t{64}, size_t{100}, size_t{128}, size_t{333}}) {
        ClipPlayer player(block);
        ASSERT_EQ(player.addClip(300, 500, makeSampleProgram("clipRamp", block)), 0);
        player.play();
        const std::vector<float> out = render(player, 1000, chunk);
        for (size_t i = 0; i < out.size(); ++i) {
            const float expected = (i >= 300 && i < 800) ? 0.001f * static_cast<float>(i - 300 + 1) : 0.0f;
            ASSERT_NEAR(out[i], expected, 1e-6) << "frame " << i << " chunk " << chunk;
        }
        EXPECT_EQ(player.position(), 1000u);
    }
}

TEST(ClipPlayer, OverlappingClipsAreSummedAndLengthCutsAClipShort) {
    SampleBank::instance().set("clipRamp", ramp(500));
    ClipPlayer player(block);
    player.addClip(0, 500, makeSampleProgram("clipRamp", block));
    player.addClip(100, 200, makeSampleProgram("clipRamp", block), 0.5f);
    player.play();
    const std::vector<float> out = render(player, 600, 128);
    for (size_t i = 0; i < out.size(); ++i) {
        float expected = i < 500 ? 0.001f * static_cast<float>(i + 1) : 0.0f;
        if (i >= 100 && i < 300) expected += 0.5f * 0.001f * static_cast<float>(i - 100 + 1);
        ASSERT_NEAR(out[i], expected, 1e-6) << "frame " << i;
    }
}

TEST(ClipPlayer, StopHoldsThePlayheadAndResumeContinuesFromIt) {
    SampleBank::instance().set("clipRamp", ramp(500));
    ClipPlayer player(block);
    player.addClip(0, 500, makeSampleProgram("clipRamp", block));
    std::vector<float> first(200), silent(300), second(100);
    player.play();
    player.render(first.data(), first.size());
    player.stop();
    player.render(silent.data(), silent.size());
    EXPECT_EQ(player.position(), 200u);
    EXPECT_FLOAT_EQ(*std::max_element(silent.begin(), silent.end()), 0.0f);
    player.play();
    player.render(second.data(), second.size());
    for (size_t i = 0; i < second.size(); ++i) ASSERT_NEAR(second[i], 0.001f * static_cast<float>(200 + i + 1), 1e-6) << i;
    EXPECT_EQ(player.position(), 300u);
}

TEST(ClipPlayer, SeekReplaysClipsFromTheirStart) {
    SampleBank::instance().set("clipRamp", ramp(500));
    ClipPlayer player(block);
    player.addClip(100, 500, makeSampleProgram("clipRamp", block));
    player.play();
    render(player, 400, 128);
    player.seek(250);  // 150 frames into the clip
    const std::vector<float> out = render(player, 64, 64);
    for (size_t i = 0; i < out.size(); ++i) ASSERT_NEAR(out[i], 0.001f * static_cast<float>(150 + i + 1), 1e-6) << i;
    player.seek(0);
    const std::vector<float> again = render(player, 300, 100);
    EXPECT_FLOAT_EQ(again[99], 0.0f);
    EXPECT_NEAR(again[100], 0.001f, 1e-6);
}

TEST(ClipPlayer, GraphClipRestartsItsOscillatorAtTheClipStart) {
    ClipPlayer player(block);
    player.addClip(1000, 4000, oscProgram(440.0f));
    player.play();
    const std::vector<float> out = render(player, 3000, 256);
    EXPECT_FLOAT_EQ(out[999], 0.0f);
    for (size_t i = 0; i < 1500; ++i) {
        const double expected = 0.5 * std::sin(twoPi * std::fmod(static_cast<double>(i) * 440.0 / 48000.0, 1.0));
        ASSERT_NEAR(out[1000 + i], expected, 2e-4) << i;
    }
}

TEST(ClipPlayer, NotesClipDrivesAnInstrumentAtItsEventFrames) {
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    const NodeId silence = net.addNode("wrangle");
    net.node(silence).setString("code", "x = 0;");
    net.connect(line, silence);
    const NodeId voices = net.addNode("voices");
    net.connect(silence, voices);
    const NodeId out = net.addNode("wrangle");
    net.node(out).setString("code", "x = x * a;");
    net.connect(voices, out);
    auto program = AudioProgram::compile(net, out, {block, {"x"}});
    program->param(out, 0) = 1.0f;

    ClipPlayer player(block);
    player.addClip(2000, 6000, std::move(program), 1.0f, {{500, 0x90, 69, 100}, {3000, 0x80, 69, 0}});
    player.play();
    const std::vector<float> rendered = render(player, 8000, 200);
    float before = 0.0f, during = 0.0f;
    for (size_t i = 0; i < 2500; ++i) before = std::max(before, std::abs(rendered[i]));
    for (size_t i = 3000; i < 4000; ++i) during = std::max(during, std::abs(rendered[i]));
    EXPECT_FLOAT_EQ(before, 0.0f);
    EXPECT_GT(during, 0.1f);
}

TEST(ClipPlayer, RejectsMismatchedProgramsAndFullPlayers) {
    SampleBank::instance().set("clipRamp", ramp(500));
    ClipPlayer player(block);
    EXPECT_THROW(player.addClip(0, 10, nullptr), std::invalid_argument);
    EXPECT_THROW(player.addClip(0, 10, makeSampleProgram("clipRamp", 64)), std::invalid_argument);
    for (size_t i = 0; i < ClipPlayer::maxClips; ++i) ASSERT_GE(player.addClip(0, 10, makeSampleProgram("clipRamp", block)), 0);
    EXPECT_EQ(player.addClip(0, 10, makeSampleProgram("clipRamp", block)), -1);
    player.clear();
    EXPECT_EQ(player.clipCount(), 0u);
}

TEST(ClipPlayer, DroneShapeMixIntoAStepIntoAGainCompilesAndSounds) {
    Network net;
    NodeId osc[2];
    const NodeId mix = net.addNode("mix");
    for (int i = 0; i < 2; ++i) {
        const NodeId line = net.addNode("line");
        net.node(line).setFloat("count", static_cast<float>(block));
        osc[i] = net.addNode("wrangle");
        net.node(osc[i]).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
        net.connect(line, osc[i]);
        net.connect(osc[i], mix, i);
    }
    const NodeId filter = net.addNode("biquad");
    net.node(filter).setFloat("b0", 0.5f);
    net.connect(mix, filter);
    const NodeId gain = net.addNode("wrangle");
    net.node(gain).setString("code", "x = x * a;");
    net.connect(filter, gain);
    auto program = AudioProgram::compile(net, gain, {block, {"x"}});
    EXPECT_NO_THROW(program->param(osc[0], 1) = 0.01f);
    EXPECT_NO_THROW(program->param(osc[1], 1) = 0.011f);
    EXPECT_NO_THROW(program->addPhase(osc[0], 0, 1));
    EXPECT_NO_THROW(program->addPhase(osc[1], 0, 1));
    EXPECT_NO_THROW(program->param(gain, 0) = 1.0f);
}

TEST(ClipPlayer, ParameterWritesApplyAtTheNextBlockOfThatClipOnly) {
    auto constant = [](NodeId& node) {
        Network net;
        const NodeId line = net.addNode("line");
        net.node(line).setFloat("count", static_cast<float>(block));
        node = net.addNode("wrangle");
        net.node(node).setString("code", "x = a;");
        net.connect(line, node);
        auto program = AudioProgram::compile(net, node, {block, {"x"}});
        program->param(node, 0) = 1.0f;
        return program;
    };
    NodeId first, second;
    ClipPlayer player(block);
    ASSERT_EQ(player.addClip(0, 1000, constant(first)), 0);
    ASSERT_EQ(player.addClip(0, 1000, constant(second)), 1);
    player.play();
    std::vector<float> out = render(player, 128, 128);
    EXPECT_FLOAT_EQ(out[10], 2.0f);
    ASSERT_TRUE(player.setParam(1, second, 0, 0.25f));
    ASSERT_TRUE(player.setParam(7, second, 0, 9.0f));  // no such clip: ignored
    ASSERT_TRUE(player.setParam(0, 99, 0, 9.0f));      // no such node: ignored
    out = render(player, 256, 128);
    EXPECT_FLOAT_EQ(out[0], 1.25f);
    EXPECT_FLOAT_EQ(out[255], 1.25f);
}
