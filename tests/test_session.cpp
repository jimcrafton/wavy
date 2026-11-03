#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "../editor/session.h"
#include "wavy/pluginHost.h"

using namespace wavy;
using namespace wavy::editor;

namespace {

std::vector<char> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(SessionState, MissingFileRestoresNothingAndGarbageIsRejected) {
    Session session(48000.0);
    std::string error;
    const auto dir = std::filesystem::temp_directory_path() / "wavySessionState";
    std::filesystem::create_directories(dir);
    EXPECT_EQ(session.loadInstrumentStates((dir / "missing.state").string(), error), 0);
    EXPECT_TRUE(error.empty());

    const auto bad = dir / "bad.state";
    std::ofstream(bad, std::ios::binary) << "not a state file";
    EXPECT_EQ(session.loadInstrumentStates(bad.string(), error), 0);
    EXPECT_FALSE(error.empty());
}

TEST(SessionState, EffectStateIsSavedAndRestoredAlongsideTheInstrument) {
    const std::string synth = PluginHost::commonVst3Path("Surge Synth Team/Surge XT.vst3/Contents/x86_64-win/Surge XT.vst3");
    const std::string fx = PluginHost::commonVst3Path("Surge Synth Team/Surge XT Effects.vst3/Contents/x86_64-win/Surge XT Effects.vst3");
    if (synth.empty() || fx.empty()) GTEST_SKIP() << "Surge XT and Surge XT Effects are not both installed";
    const auto dir = std::filesystem::temp_directory_path() / "wavySessionState";
    std::filesystem::create_directories(dir);
    const auto first = dir / "withEffect.state", second = dir / "withEffectAgain.state";

    std::string error;
    {
        Session a(48000.0);
        ASSERT_TRUE(a.setTrackInstrument(3, synth, error)) << error;
        ASSERT_TRUE(a.addTrackEffect(3, fx, error)) << error;
        ASSERT_TRUE(a.saveInstrumentStates(first.string(), error)) << error;
    }
    {
        Session b(48000.0);
        ASSERT_TRUE(b.setTrackInstrument(3, synth, error)) << error;
        EXPECT_EQ(b.loadInstrumentStates(first.string(), error), 1) << "the effect is not on the track yet: only the instrument restores";
        ASSERT_TRUE(b.addTrackEffect(3, fx, error)) << error;
        EXPECT_EQ(b.loadInstrumentStates(first.string(), error), 2) << error;
        ASSERT_TRUE(b.saveInstrumentStates(second.string(), error)) << error;
    }
    EXPECT_EQ(readAll(first), readAll(second));
}

TEST(SessionState, SurgeXtPatchSurvivesSaveAndLoadIntoANewSession) {
    const std::string path = PluginHost::commonVst3Path("Surge Synth Team/Surge XT.vst3/Contents/x86_64-win/Surge XT.vst3");
    if (path.empty()) GTEST_SKIP() << "Surge XT not installed";
    const auto dir = std::filesystem::temp_directory_path() / "wavySessionState";
    std::filesystem::create_directories(dir);
    const auto first = dir / "first.state", second = dir / "second.state";

    std::string error;
    {
        Session a(48000.0);
        ASSERT_TRUE(a.setTrackInstrument(3, path, error)) << error;
        ASSERT_TRUE(a.saveInstrumentStates(first.string(), error)) << error;
    }
    {
        Session b(48000.0);
        ASSERT_TRUE(b.setTrackInstrument(3, path, error)) << error;
        EXPECT_EQ(b.loadInstrumentStates(first.string(), error), 1) << error;
        ASSERT_TRUE(b.saveInstrumentStates(second.string(), error)) << error;
        // A state saved for another track or plugin is skipped.
        Session c(48000.0);
        EXPECT_EQ(c.loadInstrumentStates(first.string(), error), 0);
    }
    const auto a = readAll(first), b = readAll(second);
    EXPECT_GT(a.size(), 100u);
    EXPECT_EQ(a, b);
}
