#include <gtest/gtest.h>

#include <cmath>
#include <deque>
#include <string>
#include <vector>

#include "jitnodes/jitnodes.h"

using namespace jitnodes;

namespace {

constexpr size_t block = 128;

// Stereo test double: out = in * gain, optionally swapped and delayed by `delay` samples (a stateful, latency-reporting effect).
class FakeProcessor : public ExternalProcessor {
public:
    float gain = 1.0f;
    bool swap = false;
    size_t delay = 0;
    size_t blocks = 0;
    size_t events = 0;

    void process(const float* const* in, float* const* out, size_t frames, const EventBuffer& eventBuffer) noexcept override {
        ++blocks;
        events += eventBuffer.size();
        for (size_t i = 0; i < frames; ++i) {
            for (size_t c = 0; c < 2; ++c) {
                history_[c].push_back(in[swap ? 1 - c : c][i] * gain);
                float v = 0.0f;
                if (history_[c].size() > delay) {
                    v = history_[c].front();
                    history_[c].pop_front();
                }
                out[c][i] = v;
            }
        }
    }
    size_t latencySamples() const override { return delay; }
    void reset() noexcept override {
        history_[0].clear();
        history_[1].clear();
    }

private:
    std::deque<float> history_[2];
};

std::shared_ptr<FakeProcessor> registerFake(const std::string& name) {
    auto fake = std::make_shared<FakeProcessor>();
    ProcessorBank::instance().set(name, fake);
    return fake;
}

NodeId addHostInput(Network& net, float first = 0.0f, float channels = 2.0f) {
    const NodeId in = net.addNode("hostInput");
    net.node(in).setFloat("count", static_cast<float>(block));
    net.node(in).setFloat("first", first);
    net.node(in).setFloat("channels", channels);
    return in;
}

NodeId addNode(Network& net, const char* type, NodeId from, const char* code = nullptr) {
    const NodeId id = net.addNode(type);
    if (code) {
        net.node(id).setString("code", code);
    }
    net.connect(from, id);
    return id;
}

// Runs `blocks` blocks of host input where channel c of block b holds value(c, b * block + i).
template <class F>
std::vector<std::vector<float>> render(AudioProgram& program, size_t blocks, F value) {
    std::vector<std::vector<float>> out(program.outputCount(), std::vector<float>(blocks * block));
    std::vector<std::vector<float>> in(program.inputCount(), std::vector<float>(block));
    for (size_t b = 0; b < blocks; ++b) {
        std::vector<const float*> inPtr;
        for (size_t c = 0; c < in.size(); ++c) {
            for (size_t i = 0; i < block; ++i) {
                in[c][i] = value(c, b * block + i);
            }
            inPtr.push_back(in[c].data());
        }
        std::vector<float*> outPtr;
        for (auto& o : out) {
            outPtr.push_back(o.data() + b * block);
        }
        program.process(outPtr.data(), nullptr, inPtr.data());
    }
    return out;
}

float ramp(size_t c, size_t i) { return (c == 0 ? 1.0f : -2.0f) * static_cast<float>(i % 1000) * 0.001f; }

}  // namespace

TEST(AudioStereo, HostInputFeedsBothChannelsThroughAKernel) {
    Network net;
    const NodeId in = addHostInput(net);
    const NodeId gain = addNode(net, "wrangle", in, "x = x * a; r = r * a;");
    auto program = AudioProgram::compile(net, gain, {block, {"x", "r"}});
    program->param(gain, 0) = 0.5f;
    EXPECT_EQ(program->inputCount(), 2u);

    const auto out = render(*program, 3, ramp);
    for (size_t i = 0; i < 3 * block; ++i) {
        ASSERT_FLOAT_EQ(out[0][i], 0.5f * ramp(0, i));
        ASSERT_FLOAT_EQ(out[1][i], 0.5f * ramp(1, i));
    }
}

TEST(AudioStereo, MonoHostInputAndSecondChannelPair) {
    Network net;
    const NodeId in = addHostInput(net, 1.0f, 1.0f);
    const NodeId gain = addNode(net, "wrangle", in, "x = x * a;");
    auto program = AudioProgram::compile(net, gain, {block, {"x"}});
    program->param(gain, 0) = 2.0f;
    EXPECT_EQ(program->inputCount(), 2u);

    const auto out = render(*program, 2, ramp);
    for (size_t i = 0; i < 2 * block; ++i) {
        ASSERT_FLOAT_EQ(out[0][i], 2.0f * ramp(1, i));
    }
}

TEST(AudioStereo, MissingHostInputReadsAsSilence) {
    Network net;
    const NodeId in = addHostInput(net);
    const NodeId gain = addNode(net, "wrangle", in, "x = x + 1; r = r + 2;");
    auto program = AudioProgram::compile(net, gain, {block, {"x", "r"}});
    std::vector<float> l(block), r(block);
    float* out[] = {l.data(), r.data()};
    program->process(out);
    EXPECT_FLOAT_EQ(l[5], 1.0f);
    EXPECT_FLOAT_EQ(r[5], 2.0f);
}

TEST(AudioStereo, VstNodeForwardsStereoAndKeepsHistoryAcrossBlocks) {
    auto fake = registerFake("fx");
    fake->gain = 0.5f;
    fake->swap = true;
    fake->delay = 200;

    Network net;
    const NodeId in = addHostInput(net);
    const NodeId vst = addNode(net, "vst", in);
    net.node(vst).setString("plugin", "fx");
    const NodeId tail = addNode(net, "wrangle", vst, "x = x * a; r = r * a;");
    auto program = AudioProgram::compile(net, tail, {block, {"x", "r"}});
    program->param(tail, 0) = 2.0f;

    const auto out = render(*program, 6, ramp);
    for (size_t i = 0; i < 6 * block; ++i) {
        const float left = i < 200 ? 0.0f : ramp(1, i - 200);
        const float right = i < 200 ? 0.0f : ramp(0, i - 200);
        ASSERT_NEAR(out[0][i], left, 1e-6f) << i;
        ASSERT_NEAR(out[1][i], right, 1e-6f) << i;
    }
    EXPECT_EQ(fake->blocks, 6u);
}

TEST(AudioStereo, VstNodeAsMiddleOfAMonoChainDuplicatesTheSignal) {
    registerFake("fxDup");

    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    const NodeId saw = addNode(net, "wrangle", line, "x = sampleIndex * 0.01;");
    const NodeId vst = addNode(net, "vst", saw);
    net.node(vst).setString("plugin", "fxDup");
    const NodeId tail = addNode(net, "wrangle", vst, "x = x; r = r;");
    auto program = AudioProgram::compile(net, tail, {block, {"x", "r"}});
    std::vector<float> l(block), r(block);
    float* out[] = {l.data(), r.data()};
    program->process(out);
    for (size_t i = 0; i < block; ++i) {
        ASSERT_FLOAT_EQ(l[i], static_cast<float>(i) * 0.01f);
        ASSERT_FLOAT_EQ(r[i], static_cast<float>(i) * 0.01f);
    }
}

TEST(AudioStereo, MixSumsBothChannelsAndFeedsMonoBranchesToBothSides) {
    Network net;
    const NodeId stereoIn = addHostInput(net);
    const NodeId stereoGain = addNode(net, "wrangle", stereoIn, "x = x * a; r = r * a;");

    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    const NodeId mono = addNode(net, "wrangle", line, "x = 0.25;");

    const NodeId mix = net.addNode("mix");
    net.connect(stereoGain, mix, 0);
    net.connect(mono, mix, 1);
    const NodeId master = addNode(net, "wrangle", mix, "x = x * a; r = r * a;");

    auto program = AudioProgram::compile(net, master, {block, {"x", "r"}});
    program->param(stereoGain, 0) = 0.5f;
    program->param(master, 0) = 1.0f;

    const auto out = render(*program, 2, ramp);
    for (size_t i = 0; i < 2 * block; ++i) {
        ASSERT_FLOAT_EQ(out[0][i], 0.5f * ramp(0, i) + 0.25f);
        ASSERT_FLOAT_EQ(out[1][i], 0.5f * ramp(1, i) + 0.25f);
    }
}

TEST(AudioStereo, MixOfMonoBranchesStaysMono) {
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    const NodeId a = addNode(net, "wrangle", line, "x = 0.25;");
    const NodeId b = addNode(net, "wrangle", line, "x = 0.5;");
    const NodeId mix = net.addNode("mix");
    net.connect(a, mix, 0);
    net.connect(b, mix, 1);
    auto program = AudioProgram::compile(net, mix, {block, {"x"}});
    std::vector<float> l(block);
    float* out[] = {l.data()};
    program->process(out);
    EXPECT_FLOAT_EQ(l[3], 0.75f);
    EXPECT_THROW(AudioProgram::compile(net, mix, {block, {"x", "r"}}), std::runtime_error);
}

TEST(AudioStereo, InterpreterMixMatchesTheCompiledProgram) {
    Network net;
    const NodeId stereoIn = addHostInput(net);
    const NodeId stereoGain = addNode(net, "wrangle", stereoIn, "x = x + 1; r = r + 2;");
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    const NodeId mono = addNode(net, "wrangle", line, "x = 0.25;");
    const NodeId mix = net.addNode("mix");
    net.connect(stereoGain, mix, 0);
    net.connect(mono, mix, 1);

    auto program = AudioProgram::compile(net, mix, {block, {"x", "r"}});
    std::vector<float> l(block), r(block);
    float* out[] = {l.data(), r.data()};
    program->process(out);

    const GeometryHandle cooked = net.cook(mix);
    const float* cx = cooked->x();
    const float* cr = static_cast<const float*>(cooked->findAttrib(AttribClass::point, "r")->data());
    for (size_t i = 0; i < block; ++i) {
        ASSERT_FLOAT_EQ(cx[i], l[i]);
        ASSERT_FLOAT_EQ(cr[i], r[i]);
    }
}

TEST(AudioStereo, VstNodeCookPassesTheSignalThrough) {
    registerFake("fxCook");
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 32.0f);
    const NodeId saw = addNode(net, "wrangle", line, "x = sampleIndex;");
    const NodeId vst = addNode(net, "vst", saw);
    net.node(vst).setString("plugin", "fxCook");
    const GeometryHandle cooked = net.cook(vst);
    for (size_t i = 0; i < 32; ++i) {
        ASSERT_FLOAT_EQ(cooked->x()[i], static_cast<float>(i));
    }
}

TEST(AudioStereo, RejectsUnknownProcessorSharedProcessorAndMonoStepAfterStereo) {
    {
        Network net;
        const NodeId in = addHostInput(net);
        const NodeId vst = addNode(net, "vst", in);
        net.node(vst).setString("plugin", "doesNotExist");
        const NodeId tail = addNode(net, "wrangle", vst, "x = x;");
        EXPECT_THROW(AudioProgram::compile(net, tail, {block, {"x"}}), std::invalid_argument);
    }
    {
        registerFake("shared");
        Network net;
        const NodeId in = addHostInput(net);
        const NodeId first = addNode(net, "vst", in);
        net.node(first).setString("plugin", "shared");
        const NodeId second = addNode(net, "vst", first);
        net.node(second).setString("plugin", "shared");
        const NodeId tail = addNode(net, "wrangle", second, "x = x;");
        EXPECT_THROW(AudioProgram::compile(net, tail, {block, {"x"}}), std::runtime_error);
    }
    {
        registerFake("stereoFx");
        Network net;
        const NodeId in = addHostInput(net);
        const NodeId vst = addNode(net, "vst", in);
        net.node(vst).setString("plugin", "stereoFx");
        const NodeId delay = addNode(net, "delay", vst);
        const NodeId tail = addNode(net, "wrangle", delay, "x = x;");
        EXPECT_THROW(AudioProgram::compile(net, tail, {block, {"x"}}), std::runtime_error);
    }
    {
        Network net;
        const NodeId in = addHostInput(net, 0.0f, 3.0f);
        const NodeId tail = addNode(net, "wrangle", in, "x = x;");
        EXPECT_THROW(AudioProgram::compile(net, tail, {block, {"x"}}), std::runtime_error);
    }
}

TEST(AudioStereo, VstNodeReceivesTheBlocksEvents) {
    auto fake = registerFake("fxEvents");
    Network net;
    const NodeId in = addHostInput(net);
    const NodeId vst = addNode(net, "vst", in);
    net.node(vst).setString("plugin", "fxEvents");
    const NodeId tail = addNode(net, "wrangle", vst, "x = x;");
    auto program = AudioProgram::compile(net, tail, {block, {"x"}});
    EventBuffer events;
    events.push({10, 0x90, 60, 100});
    events.push({20, 0x80, 60, 0});
    float* out[] = {nullptr};
    program->process(out, &events);
    EXPECT_EQ(fake->events, 2u);
}
