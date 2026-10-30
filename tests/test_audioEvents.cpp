#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "jitnodes/jitnodes.h"
#include "wavy/audioEngine.h"

using namespace jitnodes;

namespace {

constexpr size_t block = 128;
constexpr double sampleRate = 48000.0;
constexpr double twoPi = 6.283185307179586;

AudioEvent noteOn(uint32_t offset, uint8_t note, uint8_t velocity = 127) { return {offset, 0x90, note, velocity}; }
AudioEvent noteOff(uint32_t offset, uint8_t note) { return {offset, 0x80, note, 0}; }

// silence (or a constant) -> step -> unity gain, since a step cannot be the program's output node
struct EventChain {
    Network net;
    NodeId source, step, out;

    EventChain(const char* stepType, const char* sourceCode) {
        const NodeId line = net.addNode("line");
        net.node(line).setFloat("count", static_cast<float>(block));
        source = net.addNode("wrangle");
        net.node(source).setString("code", sourceCode);
        net.connect(line, source);
        step = net.addNode(stepType);
        net.connect(source, step);
        out = net.addNode("wrangle");
        net.node(out).setString("code", "x = x * a;");
        net.connect(step, out);
    }

    std::unique_ptr<AudioProgram> compile() {
        auto program = AudioProgram::compile(net, out, {block, {"x"}});
        program->param(out, 0) = 1.0f;
        return program;
    }
};

std::vector<float> run(AudioProgram& program, const std::vector<std::vector<AudioEvent>>& eventsPerBlock) {
    std::vector<float> all(eventsPerBlock.size() * block);
    for (size_t b = 0; b < eventsPerBlock.size(); ++b) {
        EventBuffer events;
        for (const AudioEvent& e : eventsPerBlock[b]) events.push(e);
        float* out[] = {all.data() + b * block};
        program.process(out, &events);
    }
    return all;
}

double sine(double hz, long long samplesSinceOn) { return samplesSinceOn < 0 ? 0.0 : std::sin(twoPi * std::fmod(static_cast<double>(samplesSinceOn) * hz / sampleRate, 1.0)); }

}  // namespace

TEST(EventBuffer, KeepsOffsetOrderAndArrivalOrderAtEqualOffsets) {
    EventBuffer events;
    events.push(noteOn(30, 60));
    events.push(noteOn(10, 61));
    events.push(noteOff(10, 62));
    events.push(noteOn(20, 63));
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[0].data1, 61);
    EXPECT_EQ(events[1].data1, 62);
    EXPECT_EQ(events[2].data1, 63);
    EXPECT_EQ(events[3].data1, 60);
    EXPECT_TRUE(events[0].isNoteOn());
    EXPECT_TRUE(events[1].isNoteOff());
    EXPECT_TRUE((AudioEvent{0, 0x90, 60, 0}).isNoteOff());
}

TEST(EventBuffer, DropsEventsBeyondCapacity) {
    EventBuffer events;
    for (size_t i = 0; i < EventBuffer::capacity; ++i) ASSERT_TRUE(events.push(noteOn(0, 60)));
    EXPECT_FALSE(events.push(noteOn(0, 60)));
    events.clear();
    EXPECT_EQ(events.size(), 0u);
}

TEST(AudioEvents, AdsrFollowsNoteEventsOnTheExactSample) {
    EventChain g("adsr", "x = 1.0;");
    g.net.node(g.step).setFloat("attack", 16.0f);
    g.net.node(g.step).setFloat("decay", 1.0f);
    g.net.node(g.step).setFloat("sustain", 0.5f);
    g.net.node(g.step).setFloat("release", 20.0f);
    auto program = g.compile();

    const std::vector<float> out = run(*program, {{noteOn(10, 60)}, {}, {noteOff(5, 60)}, {}});
    for (size_t i = 0; i < out.size(); ++i) {
        double expected;
        if (i < 10) expected = 0.0;
        else if (i < 26) expected = (static_cast<double>(i) - 9.0) / 16.0;
        else if (i < 2 * block + 5) expected = 0.5;
        else if (i < 2 * block + 25) expected = 0.5 - 0.025 * (static_cast<double>(i) - (2.0 * block + 5.0) + 1.0);
        else expected = 0.0;
        ASSERT_NEAR(out[i], expected, 1e-4) << "sample " << i;
    }
}

TEST(AudioEvents, AdsrStaysOpenUntilEveryHeldNoteIsReleased) {
    EventChain g("adsr", "x = 1.0;");
    g.net.node(g.step).setFloat("attack", 1.0f);
    g.net.node(g.step).setFloat("decay", 1.0f);
    g.net.node(g.step).setFloat("sustain", 1.0f);
    g.net.node(g.step).setFloat("release", 1.0f);
    auto program = g.compile();

    const std::vector<float> out = run(*program, {{noteOn(0, 60), noteOn(4, 64), noteOff(8, 60), noteOff(40, 64)}});
    for (size_t i = 0; i < 40; ++i) ASSERT_NEAR(out[i], 1.0f, 1e-5) << "sample " << i;
    for (size_t i = 41; i < block; ++i) ASSERT_NEAR(out[i], 0.0f, 1e-5) << "sample " << i;
}

TEST(AudioEvents, VoicesPlayPolyphonicNotesAtTheirOffsets) {
    EventChain g("voices", "x = 0.0;");
    for (const char* name : {"attack", "decay", "release"}) g.net.node(g.step).setFloat(name, 1.0f);
    g.net.node(g.step).setFloat("sustain", 1.0f);
    auto program = g.compile();

    const std::vector<float> out = run(*program, {{noteOn(20, 69), noteOn(50, 81)}, {noteOff(7, 69)}, {}});
    for (size_t i = 0; i < out.size(); ++i) {
        const long long s = static_cast<long long>(i);
        const double a = (s >= 20 && s < static_cast<long long>(block) + 7) ? sine(440.0, s - 20) : 0.0;
        ASSERT_NEAR(out[i], a + sine(880.0, s - 50), 2e-3) << "sample " << i;
    }
}

TEST(AudioEvents, VoicesStealTheOldestHeldNoteWhenFull) {
    EventChain g("voices", "x = 0.0;");
    for (const char* name : {"attack", "decay", "release"}) g.net.node(g.step).setFloat(name, 1.0f);
    g.net.node(g.step).setFloat("sustain", 1.0f);
    g.net.node(g.step).setFloat("voiceCount", 2.0f);
    auto program = g.compile();

    const auto hz = [](int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); };
    const std::vector<float> out = run(*program, {{noteOn(0, 60), noteOn(10, 62), noteOn(20, 64)}});
    for (size_t i = 0; i < block; ++i) {
        const long long s = static_cast<long long>(i);
        const double expected = (s < 20 ? sine(hz(60), s) : 0.0) + (s >= 10 ? sine(hz(62), s - 10) : 0.0) + (s >= 20 ? sine(hz(64), s - 20) : 0.0);
        ASSERT_NEAR(out[i], expected, 2e-3) << "sample " << i;
    }
}

TEST(AudioEvents, VoicesKeepPlayingThroughAMigratedProgram) {
    EventChain a("voices", "x = 0.0;");
    for (const char* name : {"attack", "decay", "release"}) a.net.node(a.step).setFloat(name, 1.0f);
    a.net.node(a.step).setFloat("sustain", 1.0f);
    EventChain b("voices", "x = 0.0;");
    for (const char* name : {"attack", "decay", "release"}) b.net.node(b.step).setFloat(name, 1.0f);
    b.net.node(b.step).setFloat("sustain", 1.0f);

    auto first = a.compile();
    auto second = b.compile();
    ASSERT_EQ(a.step, b.step);
    const std::vector<float> head = run(*first, {{noteOn(0, 69)}});
    second->migrateState(*first);
    const std::vector<float> tail = run(*second, {{}});
    for (size_t i = 0; i < block; ++i) ASSERT_NEAR(tail[i], sine(440.0, static_cast<long long>(block + i)), 2e-3) << "sample " << i;
    EXPECT_NEAR(head[10], sine(440.0, 10), 2e-3);
}

TEST(AudioEngineEvents, SendEventReachesTheProgramAtItsFrameOffset) {
    EventChain g("voices", "x = 0.0;");
    for (const char* name : {"attack", "decay", "release"}) g.net.node(g.step).setFloat(name, 1.0f);
    g.net.node(g.step).setFloat("sustain", 1.0f);
    wavy::AudioEngine engine(g.compile());

    ASSERT_TRUE(engine.sendEvent(0x90, 69, 127, 33));
    ASSERT_TRUE(engine.sendEvent(0x80, 69, 0, 100000));  // clamped to the last frame of the block
    std::vector<float> out(2 * block);
    float* planes[] = {out.data()};
    engine.render(planes, 2 * block);

    for (size_t i = 0; i < block - 1; ++i) {
        const long long s = static_cast<long long>(i);
        ASSERT_NEAR(out[i], s >= 33 ? sine(440.0, s - 33) : 0.0, 2e-3) << "sample " << i;
    }
    for (size_t i = block; i < 2 * block; ++i) ASSERT_NEAR(out[i], 0.0f, 1e-5) << "sample " << i;
}
