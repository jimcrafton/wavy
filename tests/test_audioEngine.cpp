#include <gtest/gtest.h>

#include <cmath>
#include <thread>
#include <vector>

#include "jitnodes/jitnodes.h"
#include "wavy/audioEngine.h"
#include "wavy/audioDevice.h"
#include "wavy/spscRing.h"

using namespace jitnodes;
using wavy::AudioEngine;

namespace {

constexpr size_t block = 64;
constexpr double twoPi = 6.283185307179586;

struct Osc {
    Network net;
    NodeId line, osc, gain;
    std::unique_ptr<AudioProgram> program;

    explicit Osc(float hz, float level) {
        line = net.addNode("line");
        net.node(line).setFloat("count", static_cast<float>(block));
        osc = net.addNode("wrangle");
        net.node(osc).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
        net.connect(line, osc);
        gain = net.addNode("wrangle");
        net.node(gain).setString("code", "x = x * a;");
        net.connect(osc, gain);
        program = AudioProgram::compile(net, gain, {block, {"x"}});
        program->param(osc, 1) = hz / 48000.0f;
        program->param(gain, 0) = level;
        program->addPhase(osc, 0, 1);
        program->warm();
        program->resetState();
    }
};

std::vector<float> render(AudioEngine& engine, size_t frames, size_t chunk) {
    std::vector<float> all(frames);
    for (size_t at = 0; at < frames; at += chunk) {
        float* out[] = {all.data() + at};
        engine.render(out, std::min(chunk, frames - at));
    }
    return all;
}

}  // namespace

TEST(SpscRing, KeepsOrderAndReportsFullAndEmpty) {
    wavy::SpscRing<int, 4> ring;
    int v = 0;
    EXPECT_FALSE(ring.pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(ring.push(i));
    EXPECT_TRUE(ring.full());
    EXPECT_FALSE(ring.push(99));
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_TRUE(ring.push(7));
}

TEST(SpscRing, TransfersAMillionValuesBetweenThreads) {
    wavy::SpscRing<int, 256> ring;
    constexpr int count = 1000000;
    std::thread producer([&] {
        for (int i = 0; i < count;)
            if (ring.push(i)) ++i;
    });
    int expected = 0, v = 0;
    while (expected < count)
        if (ring.pop(v)) ASSERT_EQ(v, expected++);
    producer.join();
}

TEST(AudioEngine, ServesAnyCallbackSizeAsOneContinuousSignal) {
    for (const size_t chunk : {size_t{64}, size_t{17}, size_t{256}, size_t{1}}) {
        Osc g(440.0f, 0.5f);
        AudioEngine engine(std::move(g.program));
        const std::vector<float> out = render(engine, 1024, chunk);
        for (size_t i = 0; i < out.size(); ++i)
            ASSERT_NEAR(out[i], 0.5 * std::sin(twoPi * std::fmod(static_cast<double>(i) * 440.0 / 48000.0, 1.0)), 2e-3) << "chunk " << chunk << " sample " << i;
    }
}

TEST(AudioEngine, QueuedParamsApplyAtTheNextBlockAndBadOnesAreIgnored) {
    Osc g(440.0f, 0.5f);
    const NodeId gain = g.gain;
    AudioEngine engine(std::move(g.program));
    render(engine, block, block);
    EXPECT_TRUE(engine.setParam(gain, 0, 0.25f));
    EXPECT_TRUE(engine.setParam(gain, 9, 1.0f));
    EXPECT_TRUE(engine.setParam(9999, 0, 1.0f));
    const std::vector<float> out = render(engine, block, block);
    for (size_t i = 0; i < block; ++i) {
        const double i0 = static_cast<double>(block + i);
        ASSERT_NEAR(out[i], 0.25 * std::sin(twoPi * std::fmod(i0 * 440.0 / 48000.0, 1.0)), 2e-3) << i;
    }
}

TEST(AudioEngine, ParamRingReportsFullWhenTheAudioThreadIsNotDraining) {
    Osc g(440.0f, 0.5f);
    AudioEngine engine(std::move(g.program));
    size_t accepted = 0;
    while (engine.setParam(g.gain, 0, 0.5f)) ++accepted;
    EXPECT_GE(accepted, 1u);
    EXPECT_LT(accepted, 100000u);
}

TEST(AudioEngine, SwapCrossfadesOverOneBlockAndContinuesThePhase) {
    Osc a(440.0f, 0.5f);
    AudioEngine engine(std::move(a.program));
    render(engine, 3 * block, block);

    Osc b(440.0f, 1.0f);  // same frequency, double the level: after migration the phase continues
    engine.swapProgram(std::move(b.program));
    const std::vector<float> out = render(engine, 3 * block, block);

    for (size_t i = 0; i < out.size(); ++i) {
        const double s = std::sin(twoPi * std::fmod(static_cast<double>(3 * block + i) * 440.0 / 48000.0, 1.0));
        const double level = i < block ? 0.5 + 0.5 * (static_cast<double>(i) + 0.5) / block : 1.0;
        ASSERT_NEAR(out[i], level * s, 3e-3) << i;
    }
    EXPECT_EQ(engine.collectRetired(), 1u);
    EXPECT_EQ(engine.collectRetired(), 0u);
}

TEST(AudioEngine, KeepsOnlyTheNewestPendingSwapAndRejectsMismatchedPrograms) {
    Osc a(440.0f, 0.5f);
    AudioEngine engine(std::move(a.program));
    Osc b(440.0f, 0.2f), c(440.0f, 0.8f);
    engine.swapProgram(std::move(b.program));
    engine.swapProgram(std::move(c.program));
    const std::vector<float> out = render(engine, 2 * block, block);
    EXPECT_NEAR(out[2 * block - 1], 0.8 * std::sin(twoPi * std::fmod(static_cast<double>(2 * block - 1) * 440.0 / 48000.0, 1.0)), 3e-3);
    EXPECT_EQ(engine.collectRetired(), 1u);

    EXPECT_THROW(engine.swapProgram(nullptr), std::invalid_argument);
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 32.0f);
    const NodeId w = net.addNode("wrangle");
    net.node(w).setString("code", "x = x;");
    net.connect(line, w);
    EXPECT_THROW(engine.swapProgram(AudioProgram::compile(net, w, {32, {"x"}})), std::invalid_argument);
}

TEST(AudioDevice, AsioIsOptionalAndListingNeverThrows) {
    const auto drivers = wavy::listAsioDrivers();
    const auto device = wavy::makeAsioDevice();
#ifdef WAVY_HAS_ASIO
    EXPECT_NE(device, nullptr);
#else
    EXPECT_EQ(device, nullptr);
    EXPECT_TRUE(drivers.empty());
#endif
}
