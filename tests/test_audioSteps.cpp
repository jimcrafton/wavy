#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "jitnodes/jitnodes.h"

using namespace jitnodes;

namespace {

constexpr size_t block = 128;
constexpr double sampleRate = 48000.0;
constexpr double twoPi = 6.283185307179586;

struct Chain {
    Network net;
    NodeId line, osc;

    explicit Chain(size_t count = block) {
        line = net.addNode("line");
        net.node(line).setFloat("count", static_cast<float>(count));
        osc = net.addNode("wrangle");
        net.node(osc).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
        net.connect(line, osc);
    }

    NodeId add(const char* type, NodeId from, const char* code = nullptr) {
        const NodeId id = net.addNode(type);
        if (code) net.node(id).setString("code", code);
        net.connect(from, id);
        return id;
    }
};

std::vector<float> run(AudioProgram& program, size_t blocks) {
    std::vector<float> all(blocks * block);
    for (size_t b = 0; b < blocks; ++b) {
        float* out[] = {all.data() + b * block};
        program.process(out);
    }
    return all;
}

double oscAt(double hz, long long i) { return i < 0 ? 0.0 : std::sin(twoPi * std::fmod(static_cast<double>(i) * hz / sampleRate, 1.0)); }

}  // namespace

TEST(AudioSteps, DelayShiftsTheSignalAcrossBlocks) {
    for (const float samples : {0.0f, 37.0f, 300.0f}) {
        Chain g;
        const NodeId delay = g.add("delay", g.osc);
        g.net.node(delay).setFloat("maxSamples", 512.0f);
        const NodeId gain = g.add("wrangle", delay, "x = x * a;");

        auto program = AudioProgram::compile(g.net, gain, {block, {"x"}});
        program->param(gain, 0) = 0.5f;
        program->param(g.osc, 1) = 440.0f / static_cast<float>(sampleRate);
        program->param(delay, 0) = samples;
        program->addPhase(g.osc, 0, 1);
        program->warm();
        program->resetState();

        const std::vector<float> out = run(*program, 12);
        for (size_t i = 0; i < out.size(); ++i)
            ASSERT_NEAR(out[i], 0.5 * oscAt(440.0, static_cast<long long>(i) - static_cast<long long>(samples)), 2e-3) << "delay " << samples << " sample " << i;
    }
}

TEST(AudioSteps, DelayOfABareSourceKeepsHistoryBetweenBlocks) {
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    net.node(line).setFloat("length", static_cast<float>(block - 1));  // x[i] = i
    const NodeId delay = net.addNode("delay");
    net.connect(line, delay);
    const NodeId gain = net.addNode("wrangle");
    net.node(gain).setString("code", "x = x * a;");
    net.connect(delay, gain);

    auto program = AudioProgram::compile(net, gain, {block, {"x"}});
    program->param(gain, 0) = 1.0f;
    program->param(delay, 0) = 5.0f;
    const std::vector<float> out = run(*program, 3);
    for (size_t b = 0; b < 3; ++b)
        for (size_t i = 0; i < block; ++i) {
            const float expected = i >= 5 ? static_cast<float>(i - 5) : (b == 0 ? 0.0f : static_cast<float>(block - 5 + i));
            ASSERT_EQ(out[b * block + i], expected) << "block " << b << " sample " << i;
        }
}

TEST(AudioSteps, BiquadMatchesAScalarReferenceAcrossBlocks) {
    // RBJ low-pass at 1 kHz, Q 0.707.
    const double w0 = twoPi * 1000.0 / sampleRate, alpha = std::sin(w0) / (2.0 * 0.707), cw = std::cos(w0);
    const double a0 = 1.0 + alpha;
    const double b0 = (1.0 - cw) / 2.0 / a0, b1 = (1.0 - cw) / a0, b2 = b0, a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;

    Chain g;
    const NodeId filter = g.add("biquad", g.osc);
    const NodeId gain = g.add("wrangle", filter, "x = x * a;");
    auto program = AudioProgram::compile(g.net, gain, {block, {"x"}});
    program->param(gain, 0) = 1.0f;
    program->param(g.osc, 1) = 3000.0f / static_cast<float>(sampleRate);
    for (size_t k = 0; k < 5; ++k) program->param(filter, k) = static_cast<float>(std::array<double, 5>{b0, b1, b2, a1, a2}[k]);
    program->addPhase(g.osc, 0, 1);

    const std::vector<float> out = run(*program, 40);
    double z1 = 0.0, z2 = 0.0, peak = 0.0;
    for (size_t i = 0; i < out.size(); ++i) {
        const double in = oscAt(3000.0, static_cast<long long>(i));
        const double y = b0 * in + z1;
        z1 = b1 * in - a1 * y + z2;
        z2 = b2 * in - a2 * y;
        ASSERT_NEAR(out[i], y, 2e-3) << "sample " << i;
        if (i > block * 20) peak = std::max(peak, std::fabs(static_cast<double>(out[i])));
    }
    EXPECT_LT(peak, 0.2);  // 3 kHz sits two octaves above the corner
}

TEST(AudioSteps, StepsWorkInsideMixBranchesAndKeepTheirOwnState) {
    Chain g;
    const NodeId delay = g.add("delay", g.osc);
    g.net.node(delay).setFloat("maxSamples", 256.0f);
    const NodeId osc2 = g.add("wrangle", g.line, "x = sin(6.28318530718 * (a + sampleIndex * b));");
    const NodeId filter = g.add("biquad", osc2);  // unity passthrough
    const NodeId mix = g.net.addNode("mix");
    g.net.connect(delay, mix, 0);
    g.net.connect(filter, mix, 1);
    const NodeId master = g.add("wrangle", mix, "x = x * a;");

    auto program = AudioProgram::compile(g.net, master, {block, {"x"}});
    program->param(master, 0) = 0.5f;
    program->param(g.osc, 1) = 440.0f / static_cast<float>(sampleRate);
    program->param(osc2, 1) = 660.0f / static_cast<float>(sampleRate);
    program->param(delay, 0) = 130.0f;
    program->addPhase(g.osc, 0, 1);
    program->addPhase(osc2, 0, 1);

    const std::vector<float> out = run(*program, 10);
    for (size_t i = 0; i < out.size(); ++i) {
        const double expected = 0.5 * (oscAt(440.0, static_cast<long long>(i) - 130) + oscAt(660.0, static_cast<long long>(i)));
        ASSERT_NEAR(out[i], expected, 3e-3) << "sample " << i;
    }
}

TEST(AudioSteps, StepNodesCookLikeTheProgramRuns) {
    Chain g(256);
    const NodeId ramp = g.add("wrangle", g.line, "x = sampleIndex;");
    const NodeId delay = g.add("delay", ramp);
    g.net.node(delay).setFloat("samples", 3.0f);
    const auto geo = g.net.cook(delay);
    ASSERT_EQ(geo->pointCount(), 256u);
    for (size_t i = 0; i < 256; ++i) ASSERT_EQ(geo->x()[i], i >= 3 ? static_cast<float>(i - 3) : 0.0f);
}

TEST(AudioSteps, MigratedStateContinuesPhaseAndDelayHistoryInARecompiledProgram) {
    constexpr float samples = 37.0f;
    for (const float maxSamples : {512.0f, 64.0f}) {
        Chain g;
        const NodeId delay = g.add("delay", g.osc);
        g.net.node(delay).setFloat("maxSamples", 256.0f);
        const NodeId gain = g.add("wrangle", delay, "x = x * a;");

        auto configure = [&](AudioProgram& p) {
            p.param(gain, 0) = 0.5f;
            p.param(g.osc, 1) = 440.0f / static_cast<float>(sampleRate);
            p.param(delay, 0) = samples;
            p.addPhase(g.osc, 0, 1);
        };
        auto before = AudioProgram::compile(g.net, gain, {block, {"x"}});
        configure(*before);
        const std::vector<float> first = run(*before, 5);

        g.net.node(delay).setFloat("maxSamples", maxSamples);
        auto after = AudioProgram::compile(g.net, gain, {block, {"x"}});
        configure(*after);
        after->migrateState(*before);
        const std::vector<float> second = run(*after, 5);

        for (size_t i = 0; i < second.size(); ++i) {
            const long long n = static_cast<long long>(first.size() + i);
            ASSERT_NEAR(second[i], 0.5 * oscAt(440.0, n - static_cast<long long>(samples)), 2e-3) << "max " << maxSamples << " sample " << i;
        }
    }
}

TEST(AudioSteps, FeedbackPairEchoesWithOneBlockOfLatency) {
    constexpr float feedback = 0.5f;
    Chain g;
    const NodeId source = g.net.addNode("fbSource");
    g.net.node(source).setString("name", "echo");
    const NodeId tail = g.add("wrangle", source, "x = x * a;");
    const NodeId mix = g.net.addNode("mix");
    g.net.connect(g.osc, mix, 0);
    g.net.connect(tail, mix, 1);
    const NodeId sink = g.add("fbSink", mix);
    g.net.node(sink).setString("name", "echo");
    const NodeId master = g.add("wrangle", sink, "x = x * a;");

    auto program = AudioProgram::compile(g.net, master, {block, {"x"}});
    program->param(master, 0) = 1.0f;
    program->param(tail, 0) = feedback;
    program->param(g.osc, 1) = 440.0f / static_cast<float>(sampleRate);
    program->addPhase(g.osc, 0, 1);
    program->warm();
    program->resetState();

    const size_t blocks = 12;
    std::vector<float> out = run(*program, blocks);
    std::vector<double> expected(blocks * block);
    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = oscAt(440.0, static_cast<long long>(i)) + (i >= block ? feedback * expected[i - block] : 0.0);
    for (size_t i = 0; i < out.size(); ++i) ASSERT_NEAR(out[i], expected[i], 3e-3) << "sample " << i;

    program->resetState();
    const std::vector<float> again = run(*program, blocks);
    for (size_t i = 0; i < out.size(); ++i) ASSERT_EQ(again[i], out[i]) << "sample " << i;
}

TEST(AudioSteps, FeedbackSourceReadsThePreviousBlockEvenAfterTheSink) {
    Chain g;
    const NodeId sink = g.add("fbSink", g.osc);
    g.net.node(sink).setString("name", "tap");
    const NodeId early = g.net.addNode("fbSource");
    g.net.node(early).setString("name", "tap");
    const NodeId late = g.net.addNode("fbSource");
    g.net.node(late).setString("name", "tap");
    const NodeId after = g.add("wrangle", sink, "x = x * a;");  // a stage that runs after the sink
    const NodeId lateGain = g.add("wrangle", late, "x = x * a;");
    const NodeId mix = g.net.addNode("mix");
    g.net.connect(after, mix, 0);
    g.net.connect(lateGain, mix, 1);
    const NodeId early2 = g.add("wrangle", early, "x = x * a;");
    g.net.connect(early2, mix, 2);

    auto program = AudioProgram::compile(g.net, mix, {block, {"x"}});
    program->param(after, 0) = 0.0f;
    program->param(lateGain, 0) = 1.0f;
    program->param(early2, 0) = 1.0f;
    program->param(g.osc, 1) = 440.0f / static_cast<float>(sampleRate);
    program->addPhase(g.osc, 0, 1);

    const std::vector<float> out = run(*program, 6);
    for (size_t i = 0; i < out.size(); ++i)
        ASSERT_NEAR(out[i], i >= block ? 2.0 * oscAt(440.0, static_cast<long long>(i - block)) : 0.0, 3e-3) << "sample " << i;
}

TEST(AudioSteps, RejectsUnpairedOrDuplicateFeedback) {
    {
        Chain g;
        const NodeId sink = g.add("fbSink", g.osc);
        const NodeId master = g.add("wrangle", sink, "x = x * a;");
        EXPECT_THROW(AudioProgram::compile(g.net, master, {block, {"x"}}), std::runtime_error);
    }
    {
        Chain g;
        const NodeId source = g.net.addNode("fbSource");
        const NodeId master = g.add("wrangle", source, "x = x * a;");
        EXPECT_THROW(AudioProgram::compile(g.net, master, {block, {"x"}}), std::runtime_error);
    }
    {
        Chain g;
        const NodeId first = g.add("fbSink", g.osc);
        const NodeId second = g.add("fbSink", first);
        const NodeId source = g.net.addNode("fbSource");
        const NodeId mix = g.net.addNode("mix");
        g.net.connect(second, mix, 0);
        g.net.connect(source, mix, 1);
        EXPECT_THROW(AudioProgram::compile(g.net, mix, {block, {"x"}}), std::runtime_error);
    }
}

TEST(AudioSteps, RejectsStepsWithoutInputOrAsOutput) {
    Chain g;
    const NodeId delay = g.add("delay", g.osc);
    EXPECT_THROW(AudioProgram::compile(g.net, delay, {block, {"x"}}), std::runtime_error);

    const NodeId open = g.net.addNode("biquad");
    const NodeId gain = g.add("wrangle", open, "x = x * a;");
    EXPECT_THROW(AudioProgram::compile(g.net, gain, {block, {"x"}}), std::runtime_error);

    const NodeId shared = g.net.addNode("mix");
    g.net.connect(delay, shared, 0);
    g.net.connect(delay, shared, 1);
    EXPECT_THROW(AudioProgram::compile(g.net, shared, {block, {"x"}}), std::runtime_error);
}

TEST(AudioSteps, FirMatchesAScalarReferenceAcrossBlocks) {
    for (const size_t taps : {1u, 7u, 200u}) {  // 200 taps is longer than a block
        std::vector<float> c(taps);
        std::string text;
        for (size_t k = 0; k < taps; ++k) {
            c[k] = static_cast<float>(std::cos(0.37 * static_cast<double>(k)) / static_cast<double>(taps));
            text += (k ? "," : "") + std::to_string(c[k]);
        }
        Chain g;
        const NodeId fir = g.add("fir", g.osc);
        g.net.node(fir).setString("coeffs", text);
        const NodeId gain = g.add("wrangle", fir, "x = x * a;");
        auto program = AudioProgram::compile(g.net, gain, {block, {"x"}});
        program->param(gain, 0) = 1.0f;
        program->param(g.osc, 1) = 1500.0f / static_cast<float>(sampleRate);
        program->addPhase(g.osc, 0, 1);

        const std::vector<float> out = run(*program, 10);
        for (size_t i = 0; i < out.size(); ++i) {
            double y = 0.0;
            for (size_t k = 0; k < taps && k <= i; ++k) y += std::stod(std::to_string(c[k])) * oscAt(1500.0, static_cast<long long>(i - k));
            ASSERT_NEAR(out[i], y, 2e-3) << taps << " taps, sample " << i;
        }
    }
}

TEST(AudioSteps, FirRejectsBadCoefficients) {
    for (const char* bad : {"", "1,abc"}) {
        Chain g;
        const NodeId fir = g.add("fir", g.osc);
        g.net.node(fir).setString("coeffs", bad);
        const NodeId gain = g.add("wrangle", fir, "x = x * a;");
        EXPECT_THROW(AudioProgram::compile(g.net, gain, {block, {"x"}}), std::exception) << bad;
    }
}
