#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#include "wavy/vstPlugin.h"

// These tests load plugins installed on the machine (Surge XT, Rev PLATE-140) and skip when one is missing.
namespace {

using namespace wavy;

constexpr int block = 128;
constexpr double rate = 48000.0;

std::string vst3Path(const std::string& relative) {
    const char* common = std::getenv("CommonProgramFiles");
    std::filesystem::path p = std::filesystem::path(common ? common : "C:/Program Files/Common Files") / "VST3" / relative;
    return std::filesystem::exists(p) ? p.string() : std::string();
}

std::unique_ptr<VstPlugin> open(const std::string& relative) {
    const std::string path = vst3Path(relative);
    if (path.empty()) return nullptr;
    std::string error;
    auto module = VstModule::load(path, error);
    EXPECT_TRUE(module) << error;
    if (!module) return nullptr;
    auto plugin = VstPlugin::create(module, 0, error);
    EXPECT_TRUE(plugin) << error;
    if (!plugin) return nullptr;
    EXPECT_TRUE(plugin->prepare(rate, block, error)) << error;
    return plugin;
}

struct Rendered {
    std::vector<float> l, r;
};

// Runs `blocks` blocks; `eventsFor(b)` supplies the events of block b, `input` (optional) feeds an effect.
template <class EventsFor>
Rendered render(VstPlugin& p, int blocks, EventsFor eventsFor, const std::vector<float>* input = nullptr) {
    Rendered out;
    std::vector<float> l(block), r(block), silence(block, 0.0f);
    for (int b = 0; b < blocks; ++b) {
        jitnodes::EventBuffer events;
        eventsFor(b, events);
        float* o[2] = {l.data(), r.data()};
        std::vector<float> inL(block, 0.0f), inR(block, 0.0f);
        if (input)
            for (int i = 0; i < block; ++i) inL[static_cast<size_t>(i)] = inR[static_cast<size_t>(i)] = (*input)[static_cast<size_t>(b * block + i)];
        const float* in[2] = {inL.data(), inR.data()};
        p.process(p.inputChannels() ? in : nullptr, o, block, &events);
        out.l.insert(out.l.end(), l.begin(), l.end());
        out.r.insert(out.r.end(), r.begin(), r.end());
    }
    return out;
}

float peakOf(const std::vector<float>& v, size_t from, size_t to) {
    float m = 0.0f;
    for (size_t i = from; i < to && i < v.size(); ++i) m = std::max(m, std::fabs(v[i]));
    return m;
}

bool allFinite(const Rendered& r) {
    return std::all_of(r.l.begin(), r.l.end(), [](float x) { return std::isfinite(x); }) &&
           std::all_of(r.r.begin(), r.r.end(), [](float x) { return std::isfinite(x); });
}

const char* surgeXt = "Surge Synth Team/Surge XT.vst3/Contents/x86_64-win/Surge XT.vst3";
const char* surgeFx = "Surge Synth Team/Surge XT Effects.vst3/Contents/x86_64-win/Surge XT Effects.vst3";
const char* plate = "Rev PLATE-140.vst3";

}  // namespace

TEST(VstPlugin, MissingBinaryFailsWithAMessage) {
    std::string error;
    EXPECT_FALSE(VstModule::load("C:/does/not/exist.vst3", error));
    EXPECT_FALSE(error.empty());
}

TEST(VstPlugin, SurgeXtIsAnInstrumentWithAStereoMainOutputAndNoInput) {
    auto p = open(surgeXt);
    if (!p) GTEST_SKIP() << "Surge XT not installed";
    EXPECT_TRUE(p->info().isInstrument());
    EXPECT_EQ(p->outputChannels(), 2);
    EXPECT_EQ(p->inputChannels(), 0);  // its only input is a sidechain, which stays inactive
    EXPECT_GT(p->parameters().size(), 100u);
}

TEST(VstPlugin, SurgeXtPlaysANoteAndFallsSilentAfterTheRelease) {
    auto p = open(surgeXt);
    if (!p) GTEST_SKIP() << "Surge XT not installed";
    const int blocks = static_cast<int>(rate * 3 / block);
    auto out = render(*p, blocks, [&](int b, jitnodes::EventBuffer& e) {
        if (b == 0) e.push({0, 0x90, 60, 100});
        if (b == 100) e.push({0, 0x80, 60, 0});
    });
    EXPECT_TRUE(allFinite(out));
    EXPECT_GT(peakOf(out.l, 0, static_cast<size_t>(100 * block)), 0.01f);
    EXPECT_LT(peakOf(out.l, out.l.size() - static_cast<size_t>(20 * block), out.l.size()), 1e-4f);
    EXPECT_FALSE(p->failed());
}

TEST(VstPlugin, NoteOffsetsInsideABlockAreSampleAccurate) {
    auto p = open(surgeXt);
    if (!p) GTEST_SKIP() << "Surge XT not installed";
    constexpr uint32_t offset = 96;
    auto out = render(*p, 40, [&](int b, jitnodes::EventBuffer& e) {
        if (b == 3) e.push({offset, 0x90, 64, 110});
    });
    const size_t noteStart = static_cast<size_t>(3 * block + offset);
    EXPECT_LT(peakOf(out.l, 0, noteStart), 1e-6f) << "sound before the note's frame";
    EXPECT_GT(peakOf(out.l, noteStart, noteStart + 4 * block), 0.001f);
}

TEST(VstPlugin, ResetReleasesHeldNotesSoTheSoundDecays) {
    auto p = open(surgeXt);
    if (!p) GTEST_SKIP() << "Surge XT not installed";
    render(*p, 20, [&](int b, jitnodes::EventBuffer& e) {
        if (b == 0) e.push({0, 0x90, 60, 100});
    });
    p->reset();
    auto out = render(*p, static_cast<int>(rate * 3 / block), [](int, jitnodes::EventBuffer&) {});
    EXPECT_LT(peakOf(out.l, out.l.size() - static_cast<size_t>(20 * block), out.l.size()), 1e-4f);
}

TEST(VstPlugin, HardResetCutsRingingVoicesAtOnce) {
    auto p = open(surgeXt);
    if (!p) GTEST_SKIP() << "Surge XT not installed";
    render(*p, 20, [&](int b, jitnodes::EventBuffer& e) {
        if (b == 0) e.push({0, 0x90, 60, 100});
    });
    ASSERT_TRUE(p->hardReset());
    auto out = render(*p, 10, [](int, jitnodes::EventBuffer&) {});
    EXPECT_LT(peakOf(out.l, 0, out.l.size()), 1e-4f);
}

TEST(VstPlugin, EffectProcessesAudioAndReportsItsLatency) {
    auto p = open(plate);
    if (!p) GTEST_SKIP() << "Rev PLATE-140 not installed";
    EXPECT_EQ(p->inputChannels(), 2);
    EXPECT_EQ(p->outputChannels(), 2);
    EXPECT_EQ(p->latencySamples(), 54) << "update this if a plugin update changes it";

    const int blocks = 200;
    std::vector<float> sine(static_cast<size_t>(blocks * block));
    for (size_t i = 0; i < sine.size(); ++i) sine[i] = 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * static_cast<float>(i) / static_cast<float>(rate));
    auto out = render(*p, blocks, [](int, jitnodes::EventBuffer&) {}, &sine);
    EXPECT_TRUE(allFinite(out));
    EXPECT_GT(peakOf(out.l, static_cast<size_t>(50 * block), out.l.size()), 0.05f);
}

TEST(VstPlugin, ParameterRoundTripsThroughTheController) {
    auto p = open(plate);
    if (!p) GTEST_SKIP() << "Rev PLATE-140 not installed";
    uint32_t id = 0;
    for (const auto& pi : p->parameters())
        if (pi.title == "Pre Delay") id = pi.id;
    ASSERT_TRUE(p->setParameter(id, 0.25));
    EXPECT_NEAR(p->getParameter(id), 0.25, 1e-6);
    EXPECT_FALSE(p->parameterText(id, 0.25).empty());
    // The queued change reaches the processor at the next block without a fault.
    render(*p, 4, [](int, jitnodes::EventBuffer&) {});
    EXPECT_FALSE(p->failed());
}

TEST(VstPlugin, StateSavedAndLoadedRestoresAParameter) {
    auto p = open(plate);
    if (!p) GTEST_SKIP() << "Rev PLATE-140 not installed";
    uint32_t id = 0;
    for (const auto& pi : p->parameters())
        if (pi.title == "Pre Delay") id = pi.id;
    p->setParameter(id, 0.8);
    render(*p, 4, [](int, jitnodes::EventBuffer&) {});
    auto blob = p->saveState();
    ASSERT_FALSE(blob.empty());

    p->setParameter(id, 0.1);
    render(*p, 4, [](int, jitnodes::EventBuffer&) {});
    ASSERT_NEAR(p->getParameter(id), 0.1, 1e-6);

    ASSERT_TRUE(p->loadState(blob));
    EXPECT_NEAR(p->getParameter(id), 0.8, 0.02);
}

TEST(VstPlugin, SurgeXtEffectsLoadsAndProcessesSilenceWithoutAFault) {
    auto p = open(surgeFx);
    if (!p) GTEST_SKIP() << "Surge XT Effects not installed";
    EXPECT_EQ(p->inputChannels(), 2);
    auto out = render(*p, 20, [](int, jitnodes::EventBuffer&) {});
    EXPECT_TRUE(allFinite(out));
    EXPECT_FALSE(p->failed());
}
