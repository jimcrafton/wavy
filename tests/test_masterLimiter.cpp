#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "jitnodes/jitnodes.h"
#include "wavy/clipPlayer.h"
#include "wavy/masterLimiter.h"

using namespace jitnodes;
using namespace wavy;

namespace {

constexpr size_t block = 128;

std::unique_ptr<AudioProgram> osc(float amplitude) {
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    const NodeId o = net.addNode("wrangle");
    net.node(o).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
    net.connect(line, o);
    const NodeId gain = net.addNode("wrangle");
    net.node(gain).setString("code", "x = x * a;");
    net.connect(o, gain);
    auto program = AudioProgram::compile(net, gain, {block, {"x"}});
    program->param(o, 1) = 440.0f / 48000.0f;
    program->param(gain, 0) = amplitude;
    program->addPhase(o, 0, 1);
    return program;
}

std::vector<float> render(ClipPlayer& player, size_t total) {
    std::vector<float> all(total);
    for (size_t at = 0; at < total; at += 100) {
        player.render(all.data() + at, std::min<size_t>(100, total - at));
    }
    return all;
}

float peakOf(const std::vector<float>& v) {
    float peak = 0.0f;
    for (float s : v) {
        peak = std::max(peak, std::abs(s));
    }
    return peak;
}

}  // namespace

TEST(MasterLimiter, LoudClipNeverExceedsTheCeiling) {
    ClipPlayer player(block);
    player.enableMasterLimiter(48000.0, -1.0f);
    player.addClip(0, 20000, osc(1.0f), 8.0f);
    player.play();
    const std::vector<float> out = render(player, 20000);
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    EXPECT_LE(peakOf(out), ceiling + 1e-6f);
    EXPECT_GT(peakOf(out), ceiling * 0.95f) << "the limiter should hold the signal near the ceiling, not crush it";
    EXPECT_LT(player.takeLimiterGain(), 0.2f);
}

TEST(MasterLimiter, QuietSignalPassesUntouchedAndWithoutALimiterNothingChanges) {
    ClipPlayer limited(block), plain(block);
    limited.enableMasterLimiter(48000.0, -1.0f);
    limited.addClip(0, 8000, osc(0.5f));
    plain.addClip(0, 8000, osc(0.5f));
    limited.play();
    plain.play();
    EXPECT_EQ(render(limited, 8000), render(plain, 8000));
    EXPECT_EQ(limited.takeLimiterGain(), 1.0f);

    ClipPlayer loud(block);
    loud.addClip(0, 4000, osc(1.0f), 8.0f);
    loud.play();
    EXPECT_GT(peakOf(render(loud, 4000)), 5.0f) << "without enableMasterLimiter the player must not limit";
}

TEST(MasterLimiter, GainRecoversAfterALoudPassage) {
    ClipPlayer player(block);
    player.enableMasterLimiter(48000.0, -1.0f, 50.0f);
    player.addClip(0, 2000, osc(1.0f), 8.0f);
    player.addClip(2000, 30000, osc(0.5f));
    player.play();
    const std::vector<float> out = render(player, 32000);
    const std::vector<float> tail(out.end() - 2000, out.end());
    EXPECT_NEAR(peakOf(tail), 0.5f, 0.01f) << "after the release time the quiet clip is back at full level";
}

TEST(MasterLimiter, NonFiniteSamplesBecomeSilenceAndStereoIsLinked) {
    MasterLimiter limiter;
    limiter.configure(48000.0, -1.0f);
    std::vector<float> data(2 * 8, 0.0f);
    data[0] = std::numeric_limits<float>::quiet_NaN();
    data[1] = std::numeric_limits<float>::infinity();
    data[8 + 2] = 4.0f;  // right channel only
    data[2] = 0.5f;      // left channel, same frame
    limiter.process(data.data(), 2, 8, 8);
    EXPECT_EQ(data[0], 0.0f);
    EXPECT_EQ(data[1], 0.0f);
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    EXPECT_NEAR(data[8 + 2], ceiling, 1e-5f);
    EXPECT_NEAR(data[2], 0.5f * ceiling / 4.0f, 1e-5f) << "the left channel is turned down by the same gain";
}
