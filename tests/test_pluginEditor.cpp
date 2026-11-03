#include <windows.h>

#include <gtest/gtest.h>

#include "wavy/pluginEditor.h"
#include "wavy/pluginHost.h"

using namespace wavy;

namespace {

void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace

TEST(PluginEditor, SurgeXtEditorOpensAndClosesWithItsPlugin) {
    const std::string path = PluginHost::commonVst3Path("Surge Synth Team/Surge XT.vst3/Contents/x86_64-win/Surge XT.vst3");
    if (path.empty()) GTEST_SKIP() << "Surge XT is not installed";

    PluginHost host(48000.0, 128);
    std::string error;
    auto processor = host.add("surge", path, 0, error);
    ASSERT_TRUE(processor) << error;

    auto editor = PluginEditorWindow::open(processor->plugin(), error);
    ASSERT_TRUE(editor) << error;
    EXPECT_TRUE(editor->isOpen());
    pump();
    editor->bringToFront();
    pump();

    editor->close();
    EXPECT_FALSE(editor->isOpen());
    pump();

    auto again = PluginEditorWindow::open(processor->plugin(), error);
    ASSERT_TRUE(again) << error;
    again.reset();
    host.remove("surge");
}
