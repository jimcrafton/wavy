#pragma once

#include <functional>

#include "canvases.h"
#include "pluginBrowser.h"

namespace wavy::editor {

constexpr float browserRowHeight = 20.0f;

// The browser's left column: Instruments, Audio Effects, then the manufacturers. A click selects the entry.
class BrowserCategoriesCanvas : public newui::SubView {
public:
    BrowserCategoriesCanvas(PluginBrowser& browser, std::function<void()> changed);
    void paint(BLContext& ctx) override;

private:
    PluginBrowser& browser_;
    std::function<void()> changed_;
    float scroll_ = 0.0f;
};

// The plugins of the selected entry (or of the search). A click selects a row, a double-click loads it. The wheel scrolls.
class BrowserListCanvas : public newui::SubView {
public:
    using ItemFn = std::function<void(const BrowserItem&)>;
    BrowserListCanvas(PluginBrowser& browser, ItemFn selected, ItemFn load);
    void paint(BLContext& ctx) override;
    // The list or the filter changed: back to the top, nothing selected.
    void reset();

private:
    int rowAt(float y) const;

    PluginBrowser& browser_;
    ItemFn selected_;
    ItemFn load_;
    float scroll_ = 0.0f;
    int row_ = -1;
};

}  // namespace wavy::editor
