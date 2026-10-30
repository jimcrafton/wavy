#pragma once

#include <functional>
#include <string>

#include "newui/newui.h"
#include "newui/rootview.h"
#include "newui/subview.h"
#include "newui/view.h"
#include "session.h"

namespace wavy::editor {

constexpr float laneHeight = 72.0f;

// What the canvases share: the session, what is selected and the horizontal scale.
struct EditorState {
    Session& session;
    int track = 0;
    int clip = -1;
    float pixelsPerBar = 56.0f;
    std::function<void()> changed;  // selection changed; the window redraws everything

    explicit EditorState(Session& s) : session(s) {}
    float pixelsPerFrame() const { return pixelsPerBar / static_cast<float>(session.framesPerBar()); }
    float xOf(uint64_t frame) const { return static_cast<float>(frame) * pixelsPerFrame(); }
    uint64_t frameAt(float x) const { return x <= 0.0f ? 0 : static_cast<uint64_t>(x / pixelsPerFrame()); }
};

void drawText(BLContext& ctx, const std::string& s, float size, float x, float y, BLRgba32 color);

// Bar numbers over the timeline; a click moves the playhead.
class RulerCanvas : public newui::SubView {
public:
    explicit RulerCanvas(EditorState& state);
    void paint(BLContext& ctx) override;

private:
    EditorState& state_;
};

// One lane per track with the clips drawn by kind: waveform peaks, note boxes, or a rendered preview of a graph clip.
class TimelineCanvas : public newui::SubView {
public:
    explicit TimelineCanvas(EditorState& state);
    void paint(BLContext& ctx) override;

private:
    EditorState& state_;
};

// Track names and colours, one row per lane; a click selects the track.
class HeadersCanvas : public newui::SubView {
public:
    explicit HeadersCanvas(EditorState& state);
    void paint(BLContext& ctx) override;

private:
    EditorState& state_;
};

// The node graph of the selected graph clip, read-only.
class PatchCanvas : public newui::SubView {
public:
    explicit PatchCanvas(EditorState& state);
    void paint(BLContext& ctx) override;

private:
    EditorState& state_;
};

}  // namespace wavy::editor
