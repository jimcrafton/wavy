#include <gtest/gtest.h>

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "wavy/vstPlugin.h"

// Fault handling around hosted plugins, driven by tests/chaosPlugin (wavyChaos.vst3, built next to the tests).
namespace {

using namespace wavy;

constexpr int block = 128;
constexpr double rate = 48000.0;

// Parameter ids of wavyChaos and the normalized value that selects each mode (7 steps).
enum : uint32_t { pMode = 0, pBlockMs = 1, pArmAfter = 2, pLatency = 3, pGain = 4 };
enum Mode { passThrough = 0, blocking = 1, crash = 2, nanOutput = 3, infOutput = 4, loud = 5, throwing = 6 };
double modeValue(Mode m) { return m / 6.0; }

struct Fixture {
    std::shared_ptr<VstModule> module;
    std::unique_ptr<VstPlugin> plugin;
    std::string error;

    bool open() {
        module = VstModule::load(WAVY_CHAOS_PATH, error);
        if (!module) return false;
        plugin = VstPlugin::create(module, 0, error);
        return plugin != nullptr;
    }
    bool prepare() { return plugin->prepare(rate, block, error); }

    // One block of constant 0.5 on both channels; returns the left output.
    std::vector<float> run() {
        std::vector<float> inL(block, 0.5f), inR(block, 0.5f), l(block, -9.0f), r(block, -9.0f);
        const float* in[2] = {inL.data(), inR.data()};
        float* out[2] = {l.data(), r.data()};
        plugin->process(in, out, block, nullptr);
        return l;
    }
    void runBlocks(int n) {
        for (int i = 0; i < n; ++i) run();
    }
    void arm(Mode m, int afterBlocks = 0) {
        plugin->setParameter(pArmAfter, afterBlocks / 1000.0);
        plugin->setParameter(pMode, modeValue(m));
    }
};

float peak(const std::vector<float>& v) {
    float m = 0.0f;
    for (float x : v) m = std::max(m, std::fabs(x));
    return m;
}

struct ScopedEnv {
    explicit ScopedEnv(const char* value) { SetEnvironmentVariableA("WAVY_CHAOS_FAIL", value); }
    ~ScopedEnv() { SetEnvironmentVariableA("WAVY_CHAOS_FAIL", nullptr); }
};

}  // namespace

TEST(VstChaos, PassesAudioThroughWhenBehaving) {
    Fixture f;
    ASSERT_TRUE(f.open()) << f.error;
    ASSERT_TRUE(f.prepare()) << f.error;
    f.plugin->setParameter(pGain, 0.5);  // gain 1.0
    auto out = f.run();
    EXPECT_FLOAT_EQ(out[0], 0.5f);
    EXPECT_FLOAT_EQ(out[block - 1], 0.5f);
    EXPECT_FALSE(f.plugin->failed());
}

TEST(VstChaos, BlockingProcessIsMeasuredAndCountedAsSlow) {
    Fixture f;
    ASSERT_TRUE(f.open() && f.prepare()) << f.error;
    f.plugin->setParameter(pBlockMs, 0.03);  // 30 ms against a 2.7 ms block
    f.arm(blocking);
    f.runBlocks(5);
    auto s = f.plugin->stats();
    EXPECT_EQ(s.blocks, 5u);
    EXPECT_EQ(s.slowBlocks, 5u);
    EXPECT_GT(s.maxMicros, 15000u);
    EXPECT_FALSE(f.plugin->failed()) << "slow is not a fault; policy belongs to the caller";
}

TEST(VstChaos, FastProcessIsNotCountedAsSlow) {
    Fixture f;
    ASSERT_TRUE(f.open() && f.prepare()) << f.error;
    f.runBlocks(20);
    EXPECT_EQ(f.plugin->stats().slowBlocks, 0u);
}

TEST(VstChaos, AccessViolationMarksThePluginFailedAndSilencesIt) {
    Fixture f;
    ASSERT_TRUE(f.open() && f.prepare()) << f.error;
    f.arm(crash, 3);
    f.runBlocks(3);
    EXPECT_FALSE(f.plugin->failed());
    auto out = f.run();  // the 4th block faults
    EXPECT_TRUE(f.plugin->failed());
    EXPECT_EQ(peak(out), 0.0f);
    EXPECT_EQ(peak(f.run()), 0.0f) << "a failed plugin stays silent without being called again";
}

TEST(VstChaos, CppExceptionIsContainedLikeACrash) {
    Fixture f;
    ASSERT_TRUE(f.open() && f.prepare()) << f.error;
    f.arm(throwing);
    auto out = f.run();
    EXPECT_TRUE(f.plugin->failed());
    EXPECT_EQ(peak(out), 0.0f);
}

TEST(VstChaos, PrepareAgainClearsTheFailureAndAHealthyModeWorks) {
    Fixture f;
    ASSERT_TRUE(f.open() && f.prepare()) << f.error;
    f.arm(crash);
    f.run();
    ASSERT_TRUE(f.plugin->failed());
    f.arm(passThrough);
    ASSERT_TRUE(f.prepare()) << f.error;
    EXPECT_FALSE(f.plugin->failed());
    f.plugin->setParameter(pGain, 0.5);
    EXPECT_FLOAT_EQ(f.run()[0], 0.5f);
}

TEST(VstChaos, NaNAndInfinityAreScrubbedFromTheOutput) {
    for (Mode m : {nanOutput, infOutput}) {
        Fixture f;
        ASSERT_TRUE(f.open() && f.prepare()) << f.error;
        f.arm(m);
        for (int i = 0; i < 4; ++i) {
            auto out = f.run();
            for (float x : out) ASSERT_TRUE(std::isfinite(x));
        }
        EXPECT_EQ(f.plugin->stats().nonFiniteBlocks, 4u);
        EXPECT_FALSE(f.plugin->failed());
    }
}

TEST(VstChaos, ExtremeLoudnessPassesThroughUnclamped) {
    Fixture f;
    ASSERT_TRUE(f.open() && f.prepare()) << f.error;
    f.arm(loud);
    auto out = f.run();
    EXPECT_GT(peak(out), 1.0e5f) << "documents that the host does not limit; the mix/master stage must";
    EXPECT_FALSE(f.plugin->failed());
}

TEST(VstChaos, FaultStartsOnlyAfterTheArmedNumberOfBlocks) {
    Fixture f;
    ASSERT_TRUE(f.open() && f.prepare()) << f.error;
    f.arm(nanOutput, 10);
    f.runBlocks(10);
    EXPECT_EQ(f.plugin->stats().nonFiniteBlocks, 0u);
    f.runBlocks(1);
    EXPECT_EQ(f.plugin->stats().nonFiniteBlocks, 1u);
}

TEST(VstChaos, ReportedLatencyFollowsTheParameter) {
    Fixture f;
    ASSERT_TRUE(f.open() && f.prepare()) << f.error;
    EXPECT_EQ(f.plugin->latencySamples(), 0);
    f.plugin->setParameter(pLatency, 0.25);
    f.run();
    EXPECT_EQ(f.plugin->latencySamples(), 1024);
}

TEST(VstChaos, SetupFailuresAreReportedNotThrown) {
    {
        ScopedEnv env("init");
        Fixture f;
        EXPECT_FALSE(f.open());
        EXPECT_FALSE(f.error.empty());
    }
    {
        ScopedEnv env("setup");
        Fixture f;
        ASSERT_TRUE(f.open()) << f.error;
        EXPECT_FALSE(f.prepare());
        EXPECT_FALSE(f.error.empty());
        EXPECT_FALSE(f.plugin->prepared());
    }
    {
        ScopedEnv env("activate");
        Fixture f;
        ASSERT_TRUE(f.open()) << f.error;
        EXPECT_FALSE(f.prepare());
        EXPECT_FALSE(f.plugin->prepared());
    }
}

TEST(VstChaos, ProcessingAnUnpreparedPluginIsHarmless) {
    Fixture f;
    ASSERT_TRUE(f.open()) << f.error;
    ASSERT_EQ(f.plugin->outputChannels(), 0);  // nothing to write before prepare(), so the call must simply return
    std::vector<float> inL(block, 0.5f), inR(block, 0.5f), l(block, -9.0f), r(block, -9.0f);
    const float* in[2] = {inL.data(), inR.data()};
    float* out[2] = {l.data(), r.data()};
    f.plugin->process(in, out, block, nullptr);
    EXPECT_EQ(f.plugin->stats().blocks, 0u);
    EXPECT_FALSE(f.plugin->failed());
}
