#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "jitnodes/jitnodes.h"
#include "wavFile.h"

using namespace jitnodes;

namespace {

constexpr size_t block = 128;
constexpr uint32_t sampleRate = 48000;

struct OscGainGraph {
    Network net;
    NodeId osc, gain;

    OscGainGraph() {
        const NodeId line = net.addNode("line");
        net.node(line).setFloat("count", static_cast<float>(block));
        osc = net.addNode("wrangle");
        net.node(osc).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
        gain = net.addNode("wrangle");
        net.node(gain).setString("code", "x = x * a;");
        net.connect(line, osc);
        net.connect(osc, gain);
    }
};

std::vector<float> render(AudioProgram& program, NodeId osc, NodeId gain, float hz, float level, size_t blocks) {
    const float step = hz / static_cast<float>(sampleRate);
    float phase = 0.0f;
    std::vector<float> all(blocks * block);
    program.param(gain, 0) = level;
    program.param(osc, 1) = step;
    for (size_t b = 0; b < blocks; ++b) {
        program.param(osc, 0) = phase;
        float* out[] = {all.data() + b * block};
        program.process(out);
        phase = std::fmod(phase + step * static_cast<float>(block), 1.0f);
    }
    return all;
}

}  // namespace

TEST(AudioProgram, RendersOscThroughGainToWav) {
    OscGainGraph g;
    auto program = AudioProgram::compile(g.net, g.gain, {block, {"x"}});
    program->warm();

    const float hz = 440.0f, level = 0.5f;
    const size_t blocks = sampleRate / 4 / block;
    const std::vector<float> samples = render(*program, g.osc, g.gain, hz, level, blocks);

    // The program is checked against the scalar interpreter in jitnodes' tests; here the absolute signal, on a float phase.
    float peak = 0.0f;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double phase = std::fmod(static_cast<double>(i) * hz / sampleRate, 1.0);
        const float expected = level * static_cast<float>(std::sin(6.283185307179586 * phase));
        ASSERT_NEAR(samples[i], expected, 2e-3f) << "sample " << i;
        peak = std::max(peak, std::fabs(samples[i]));
    }
    EXPECT_NEAR(peak, level, 1e-2f);

    const auto path = std::filesystem::temp_directory_path() / "wavy_osc_gain.wav";
    ASSERT_TRUE(wavy::testing::writeWavFloat(path.string(), samples, sampleRate));
    EXPECT_EQ(std::filesystem::file_size(path), 44 + samples.size() * sizeof(float));
    std::filesystem::remove(path);
}

TEST(AudioProgram, ParametersTakeEffectAtTheNextBlock) {
    OscGainGraph g;
    auto program = AudioProgram::compile(g.net, g.gain, {block, {"x"}});
    std::vector<float> out(block);
    float* channels[] = {out.data()};

    program->param(g.osc, 0) = 0.25f;  // sin(pi/2) at sample 0
    program->param(g.osc, 1) = 0.0f;
    program->param(g.gain, 0) = 1.0f;
    program->process(channels);
    EXPECT_NEAR(out[0], 1.0f, 1e-3f);

    program->param(g.gain, 0) = 0.0f;
    program->process(channels);
    EXPECT_EQ(out[0], 0.0f);
}

TEST(AudioProgram, AddPhaseKeepsTheOscillatorContinuousAndResets) {
    OscGainGraph g;
    auto program = AudioProgram::compile(g.net, g.gain, {block, {"x"}});
    const float hz = 440.0f, level = 0.5f;
    program->param(g.gain, 0) = level;
    program->param(g.osc, 1) = hz / static_cast<float>(sampleRate);
    program->addPhase(g.osc, 0, 1);
    EXPECT_THROW(program->addPhase(g.osc, 1, 1), std::runtime_error);

    auto run = [&](size_t blocks) {
        std::vector<float> all(blocks * block);
        for (size_t b = 0; b < blocks; ++b) {
            float* out[] = {all.data() + b * block};
            program->process(out);
        }
        return all;
    };

    const std::vector<float> first = run(400);  // long enough for a float phase to drift
    for (size_t i = 0; i < first.size(); ++i) {
        const double phase = std::fmod(static_cast<double>(i) * hz / sampleRate, 1.0);
        ASSERT_NEAR(first[i], level * static_cast<float>(std::sin(6.283185307179586 * phase)), 2e-3f) << "sample " << i;
    }

    program->resetState();
    const std::vector<float> again = run(2);
    for (size_t i = 0; i < again.size(); ++i) ASSERT_NEAR(again[i], first[i], 1e-6f) << "sample " << i;
}

TEST(AudioProgram, StateBlocksAreAlignedZeroedAndKeyedByNode) {
    OscGainGraph g;
    auto program = AudioProgram::compile(g.net, g.gain, {block, {"x"}});
    StateBlock& s = program->state(g.gain, 100);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(s.data()) % 64, 0u);
    for (size_t i = 0; i < s.size(); ++i) EXPECT_EQ(s.as<unsigned char>()[i], 0);
    s.as<unsigned char>()[7] = 9;
    EXPECT_EQ(&program->state(g.gain, 100), &s);
    EXPECT_THROW(program->state(g.gain, 64), std::runtime_error);
    program->resetState();
    EXPECT_EQ(s.as<unsigned char>()[7], 0);
}

TEST(AudioProgram, MixesTwoOscillatorsThroughMasterGain) {
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    NodeId osc[2];
    const NodeId mix = net.addNode("mix");
    for (int i = 0; i < 2; ++i) {
        osc[i] = net.addNode("wrangle");
        net.node(osc[i]).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
        net.connect(line, osc[i]);
        net.connect(osc[i], mix, i);
    }
    const NodeId master = net.addNode("wrangle");
    net.node(master).setString("code", "x = x * a;");
    net.connect(mix, master);

    auto program = AudioProgram::compile(net, master, {block, {"x"}});
    program->warm();

    const float hz[2] = {440.0f, 660.0f}, level = 0.4f;
    float phase[2] = {0.0f, 0.0f};
    program->param(master, 0) = level;
    const size_t blocks = sampleRate / 10 / block;
    std::vector<float> samples(blocks * block);
    for (size_t b = 0; b < blocks; ++b) {
        for (int i = 0; i < 2; ++i) {
            program->param(osc[i], 0) = phase[i];
            program->param(osc[i], 1) = hz[i] / static_cast<float>(sampleRate);
            phase[i] = std::fmod(phase[i] + hz[i] / static_cast<float>(sampleRate) * static_cast<float>(block), 1.0f);
        }
        float* out[] = {samples.data() + b * block};
        program->process(out);
    }

    for (size_t i = 0; i < samples.size(); ++i) {
        double expected = 0.0;
        for (float f : hz) expected += std::sin(6.283185307179586 * std::fmod(static_cast<double>(i) * f / sampleRate, 1.0));
        ASSERT_NEAR(samples[i], level * static_cast<float>(expected), 4e-3f) << "sample " << i;
    }
}

TEST(AudioProgram, RejectsBadBlockSize) {
    OscGainGraph g;
    EXPECT_THROW(AudioProgram::compile(g.net, g.gain, {100, {"x"}}), std::runtime_error);
    EXPECT_THROW(AudioProgram::compile(g.net, g.gain, {block, {"missing"}}), std::runtime_error);
}
