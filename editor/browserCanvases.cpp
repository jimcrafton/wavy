#include "browserCanvases.h"

#include <algorithm>

namespace wavy::editor {

namespace {

const BLRgba32 ink = BLRgba32(0xFFE4E4E6);
const BLRgba32 dim = BLRgba32(0xFF8E9096);

using MouseFn = std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>;
using WheelFn = std::function<newui::SyncReturn(newui::View&, const newui::Point&, float)>;

float clampScroll(float scroll, size_t rows, float height) {
    return std::clamp(scroll, 0.0f, std::max(0.0f, static_cast<float>(rows) * browserRowHeight - height));
}

}  // namespace

BrowserCategoriesCanvas::BrowserCategoriesCanvas(PluginBrowser& browser, std::function<void()> changed)
    : browser_(browser), changed_(std::move(changed)) {
    setVisible(true);
    onMouseDown.add(MouseFn([this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
        const int row = static_cast<int>((p.y + scroll_) / browserRowHeight);
        if (row >= 0 && static_cast<size_t>(row) < browser_.categories().size()) {
            browser_.selectCategory(static_cast<size_t>(row));
            if (changed_) {
                changed_();
            }
        }
        return newui::SyncReturn::Handled;
    }));
    onMouseWheel.add(WheelFn([this](newui::View&, const newui::Point&, float delta) {
        scroll_ = clampScroll(scroll_ - (delta > 0 ? 3.0f : -3.0f) * browserRowHeight, browser_.categories().size(), bounds().height());
        redraw();
        return newui::SyncReturn::Handled;
    }));
}

void BrowserCategoriesCanvas::paint(BLContext& ctx) {
    const float w = bounds().width(), h = bounds().height();
    ctx.set_fill_style(BLRgba32(0xFF1E1E21));
    ctx.fill_rect(BLRect(0, 0, w, h));
    scroll_ = clampScroll(scroll_, browser_.categories().size(), h);
    ctx.save();
    ctx.clip_to_rect(BLRect(0, 0, w, h));
    for (size_t i = 0; i < browser_.categories().size(); ++i) {
        const PluginBrowser::Category& c = browser_.categories()[i];
        const float y = static_cast<float>(i) * browserRowHeight - scroll_;
        if (y + browserRowHeight < 0.0f || y > h) {
            continue;
        }
        if (i == browser_.selectedCategory() && browser_.search().empty()) {
            ctx.set_fill_style(BLRgba32(0xFF2A4658));
            ctx.fill_rect(BLRect(0, y, w, browserRowHeight));
        }
        if (i == 2) {
            ctx.set_fill_style(BLRgba32(0xFF38383D));
            ctx.fill_rect(BLRect(4, y, w - 8, 1));
        }
        ctx.save();
        ctx.clip_to_rect(BLRect(0, y, w - 22.0f, browserRowHeight));
        drawText(ctx, c.label, 11.0f, 6.0f, y + 14.0f, ink);
        ctx.restore();
        drawText(ctx, std::to_string(c.count), 9.0f, w - 20.0f, y + 14.0f, dim);
    }
    ctx.restore();
}

BrowserListCanvas::BrowserListCanvas(PluginBrowser& browser, ItemFn selected, ItemFn load)
    : browser_(browser), selected_(std::move(selected)), load_(std::move(load)) {
    setVisible(true);
    onMouseDown.add(MouseFn([this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
        const int row = rowAt(p.y);
        if (row >= 0) {
            row_ = row;
            if (selected_) {
                selected_(browser_.visible()[static_cast<size_t>(row)]);
            }
            redraw();
        }
        return newui::SyncReturn::Handled;
    }));
    onMouseDblClick.add(MouseFn([this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
        const int row = rowAt(p.y);
        if (row >= 0 && load_) {
            load_(browser_.visible()[static_cast<size_t>(row)]);
        }
        return newui::SyncReturn::Handled;
    }));
    onMouseWheel.add(WheelFn([this](newui::View&, const newui::Point&, float delta) {
        scroll_ = clampScroll(scroll_ - (delta > 0 ? 3.0f : -3.0f) * browserRowHeight, browser_.visible().size(), bounds().height());
        redraw();
        return newui::SyncReturn::Handled;
    }));
}

int BrowserListCanvas::rowAt(float y) const {
    const int row = static_cast<int>((y + scroll_) / browserRowHeight);
    return row >= 0 && static_cast<size_t>(row) < browser_.visible().size() ? row : -1;
}

void BrowserListCanvas::reset() {
    scroll_ = 0.0f;
    row_ = -1;
    redraw();
}

void BrowserListCanvas::paint(BLContext& ctx) {
    const float w = bounds().width(), h = bounds().height();
    ctx.set_fill_style(BLRgba32(0xFF1E1E21));
    ctx.fill_rect(BLRect(0, 0, w, h));
    const auto& items = browser_.visible();
    scroll_ = clampScroll(scroll_, items.size(), h);
    if (items.empty()) {
        drawText(ctx, browser_.itemCount() == 0 ? "No plugins found" : "Nothing matches", 11.0f, 6.0f, 18.0f, dim);
        return;
    }
    ctx.save();
    ctx.clip_to_rect(BLRect(0, 0, w, h));
    for (size_t i = 0; i < items.size(); ++i) {
        const float y = static_cast<float>(i) * browserRowHeight - scroll_;
        if (y + browserRowHeight < 0.0f || y > h) {
            continue;
        }
        if (static_cast<int>(i) == row_) {
            ctx.set_fill_style(BLRgba32(0xFF2A4658));
            ctx.fill_rect(BLRect(0, y, w, browserRowHeight));
        }
        ctx.save();
        ctx.clip_to_rect(BLRect(0, y, w - 2.0f, browserRowHeight));
        drawText(ctx, items[i].name, 11.0f, 6.0f, y + 14.0f, ink);
        ctx.restore();
    }
    ctx.restore();
}

}  // namespace wavy::editor
