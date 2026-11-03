#include "canvases.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>

#include "newui/font.h"

namespace wavy::editor {

namespace {

BLRgba32 rgb(uint32_t c, uint32_t alpha = 0xFF) { return BLRgba32(c | (alpha << 24)); }

const BLRgba32 well = BLRgba32(0xFF111113);
const BLRgba32 panelAlt = BLRgba32(0xFF1E1E21);
const BLRgba32 muted = BLRgba32(0xFF8E9096);
const BLRgba32 active = BLRgba32(0xFF3FB4F0);
const BLRgba32 playhead = BLRgba32(0xFFF2E54A);

const char* badgeOf(ClipKind kind) {
    switch (kind) {
        case ClipKind::sample: return "WAV";
        case ClipKind::notes: return "MIDI";
        default: return "GEN";
    }
}

}  // namespace

void drawText(BLContext& ctx, const std::string& s, float size, float x, float y, BLRgba32 color) {
    static std::map<int, std::unique_ptr<newui::Font>> fonts;
    const int halfPoints = static_cast<int>(std::lround(size * 2.0f));
    auto& font = fonts[halfPoints];
    if (!font) font = std::make_unique<newui::Font>("Segoe UI", static_cast<float>(halfPoints) / 2.0f);
    BLFont* f = font->blFont();
    if (!f) return;
    ctx.set_fill_style(color);
    ctx.fill_utf8_text(BLPoint(x, y), *f, s.c_str(), s.size());
}

RulerCanvas::RulerCanvas(EditorState& state) : state_(state) {
    setVisible(true);
    onMouseDown.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
            state_.session.player().seek(state_.frameAt(p.x));
            return newui::SyncReturn::Handled;
        }));
}

void RulerCanvas::paint(BLContext& ctx) {
    const float w = bounds().width(), h = bounds().height();
    ctx.set_fill_style(panelAlt);
    ctx.fill_rect(BLRect(0, 0, w, h));
    const float bar = state_.pixelsPerBar;
    int n = 1;
    for (float x = 0.0f; x < w; x += bar, ++n) {
        ctx.set_fill_style(BLRgba32(n % 4 == 1 ? 0xFF55555C : 0xFF38383D));
        ctx.fill_rect(BLRect(x, n % 4 == 1 ? 8.0f : 16.0f, 1, h));
        drawText(ctx, std::to_string(n), 9.0f, x + 3.0f, 22.0f, muted);
    }
    const float px = state_.xOf(state_.session.player().position());
    ctx.set_fill_style(playhead);
    ctx.fill_rect(BLRect(px, 0, 1, h));
    ctx.fill_rect(BLRect(px - 4.0f, 0, 9, 3));
}

TimelineCanvas::TimelineCanvas(EditorState& state) : state_(state) {
    setVisible(true);
    onMouseDown.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
            const int track = static_cast<int>(p.y / laneHeight);
            if (track >= 0 && track < static_cast<int>(state_.session.tracks().size())) {
                state_.track = track;
                state_.clip = state_.session.clipAt(track, state_.frameAt(p.x));
                if (state_.changed) state_.changed();
            }
            return newui::SyncReturn::Handled;
        }));
}

void TimelineCanvas::paint(BLContext& ctx) {
    const float w = bounds().width(), h = bounds().height();
    const Session& session = state_.session;
    ctx.set_fill_style(well);
    ctx.fill_rect(BLRect(0, 0, w, h));

    for (size_t t = 0; t < session.tracks().size(); ++t) {
        const float y = static_cast<float>(t) * laneHeight;
        if (static_cast<int>(t) == state_.track) {
            ctx.set_fill_style(BLRgba32(0x143FB4F0));
            ctx.fill_rect(BLRect(0, y, w, laneHeight));
        }
        ctx.set_fill_style(BLRgba32(0xFF2C2C30));
        ctx.fill_rect(BLRect(0, y + laneHeight - 1.0f, w, 1));
    }
    const float bar = state_.pixelsPerBar;
    int n = 0;
    for (float x = 0.0f; x < w; x += bar, ++n) {
        ctx.set_fill_style(BLRgba32(n % 4 == 0 ? 0xFF2E2E33 : 0xFF222226));
        ctx.fill_rect(BLRect(x, 0, 1, h));
    }

    const float ppf = state_.pixelsPerFrame();
    for (size_t i = 0; i < session.clips().size(); ++i) {
        const ClipInfo& clip = session.clips()[i];
        const TrackInfo& track = session.tracks()[static_cast<size_t>(clip.track)];
        const float x = state_.xOf(clip.start), cw = std::max(2.0f, static_cast<float>(clip.length) * ppf);
        const float y = static_cast<float>(clip.track) * laneHeight + 2.0f, ch = laneHeight - 4.0f;
        if (x > w || x + cw < 0.0f) continue;

        ctx.set_fill_style(rgb(track.dark));
        ctx.fill_round_rect(BLRoundRect(x, y, cw, ch, 3, 3));
        const float bodyTop = y + 14.0f, bodyH = ch - 14.0f, mid = bodyTop + bodyH * 0.5f;
        ctx.save();
        ctx.clip_to_rect(BLRect(x, y, cw, ch));

        if (clip.kind == ClipKind::notes) {
            uint8_t lo = 127, hi = 0;
            for (const NoteBox& nb : clip.notes) {
                lo = std::min(lo, nb.pitch);
                hi = std::max(hi, nb.pitch);
            }
            const float span = std::max<float>(1.0f, static_cast<float>(hi - lo));
            ctx.set_fill_style(rgb(track.color));
            for (const NoteBox& nb : clip.notes) {
                const float ny = bodyTop + 4.0f + (bodyH - 12.0f) * (1.0f - static_cast<float>(nb.pitch - lo) / span);
                ctx.fill_rect(BLRect(state_.xOf(clip.start + nb.start), ny, std::max(2.0f, static_cast<float>(nb.length) * ppf), 3.0f));
            }
        } else if (!clip.peaks.empty()) {
            ctx.set_stroke_style(rgb(track.color));
            ctx.set_stroke_width(1.0f);
            const float amp = bodyH * 0.45f;
            const float framesPerPixel = 1.0f / ppf;
            const size_t buckets = clip.peaks.size() / 2;
            const float first = std::max(0.0f, -x), last = std::min(cw, w - x);
            for (float px = first; px < last; px += 1.0f) {
                const size_t b0 = static_cast<size_t>(px * framesPerPixel / static_cast<float>(Session::peakFrames));
                const size_t b1 = std::max(b0 + 1, static_cast<size_t>((px + 1.0f) * framesPerPixel / static_cast<float>(Session::peakFrames)));
                float lo = 0.0f, hi = 0.0f;
                for (size_t b = b0; b < std::min(b1, buckets); ++b) {
                    lo = std::min(lo, clip.peaks[b * 2]);
                    hi = std::max(hi, clip.peaks[b * 2 + 1]);
                }
                ctx.stroke_line(BLPoint(x + px + 0.5f, mid - hi * amp), BLPoint(x + px + 0.5f, mid - lo * amp + 0.5f));
            }
        }

        ctx.set_fill_style(rgb(track.color));
        ctx.fill_rect(BLRect(x, y, cw, 14.0f));
        drawText(ctx, clip.name, 10.0f, x + 4.0f, y + 11.0f, BLRgba32(0xFF101012));
        drawText(ctx, badgeOf(clip.kind), 8.0f, x + cw - 28.0f, y + 10.0f, BLRgba32(0xFF101012));
        ctx.restore();

        if (static_cast<int>(i) == state_.clip) {
            ctx.set_stroke_style(active);
            ctx.set_stroke_width(2.0f);
            ctx.stroke_round_rect(BLRoundRect(x, y, cw, ch, 3, 3));
        }
    }

    const float px = state_.xOf(session.player().position());
    ctx.set_fill_style(playhead);
    ctx.fill_rect(BLRect(px, 0, 1, h));
}

namespace {

constexpr float stripTop = 53.0f, stripHeight = 16.0f;

struct StripRects {
    float muteX, soloX, volumeX, volumeW, panX, panW;
};

StripRects stripRects(float w) {
    constexpr float panWidth = 44.0f;
    return {14.0f, 36.0f, 62.0f, std::max(20.0f, w - 62.0f - panWidth - 16.0f), w - panWidth - 8.0f, panWidth};
}

constexpr float minDb = -60.0f, maxDb = 6.0f;

float gainOfPosition(float pos) { return pos <= 0.0f ? 0.0f : std::pow(10.0f, (minDb + pos * (maxDb - minDb)) / 20.0f); }

float positionOfGain(float gain) {
    return gain <= 0.0f ? 0.0f : std::clamp((20.0f * std::log10(gain) - minDb) / (maxDb - minDb), 0.0f, 1.0f);
}

}  // namespace

bool HeadersCanvas::hitStrip(const newui::Point& p, bool reset) {
    const int track = static_cast<int>(p.y / laneHeight);
    if (track < 0 || track >= static_cast<int>(state_.session.tracks().size())) return false;
    const float localY = p.y - static_cast<float>(track) * laneHeight;
    if (localY < stripTop || localY > stripTop + stripHeight) return false;
    const StripRects r = stripRects(bounds().width());
    ClipPlayer& player = state_.session.player();
    if (p.x >= r.muteX && p.x < r.muteX + 18.0f) {
        if (!reset) player.setTrackMute(track, !player.trackMuted(track));
    } else if (p.x >= r.soloX && p.x < r.soloX + 18.0f) {
        if (!reset) player.setTrackSolo(track, !player.trackSoloed(track));
    } else if (p.x >= r.volumeX && p.x <= r.volumeX + r.volumeW) {
        dragTrack_ = track;
        drag_ = Drag::volume;
        if (reset) player.setTrackVolume(track, 1.0f);
        else dragTo(p.x);
    } else if (p.x >= r.panX && p.x <= r.panX + r.panW) {
        dragTrack_ = track;
        drag_ = Drag::pan;
        if (reset) player.setTrackPan(track, 0.0f);
        else dragTo(p.x);
    } else {
        return false;
    }
    if (reset) drag_ = Drag::none;
    return true;
}

void HeadersCanvas::dragTo(float x) {
    const StripRects r = stripRects(bounds().width());
    ClipPlayer& player = state_.session.player();
    if (drag_ == Drag::volume) {
        player.setTrackVolume(dragTrack_, gainOfPosition(std::clamp((x - r.volumeX) / r.volumeW, 0.0f, 1.0f)));
    } else if (drag_ == Drag::pan) {
        const float pos = std::clamp((x - r.panX) / r.panW, 0.0f, 1.0f) * 2.0f - 1.0f;
        player.setTrackPan(dragTrack_, std::abs(pos) < 0.06f ? 0.0f : pos);
    }
}

HeadersCanvas::HeadersCanvas(EditorState& state) : state_(state) {
    setVisible(true);
    using Handler = std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>;
    onMouseMove.add(Handler([this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
        if (drag_ == Drag::none) return newui::SyncReturn::Handled;
        dragTo(p.x);
        redraw();
        return newui::SyncReturn::Handled;
    }));
    onMouseUp.add(Handler([this](newui::View&, const newui::Point&, std::uint32_t, std::uint32_t) {
        drag_ = Drag::none;
        return newui::SyncReturn::Handled;
    }));
    onMouseLeft.add(Handler([this](newui::View&, const newui::Point&, std::uint32_t, std::uint32_t) {
        drag_ = Drag::none;
        return newui::SyncReturn::Handled;
    }));
    onMouseDblClick.add(Handler([this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
        hitStrip(p, true);
        redraw();
        return newui::SyncReturn::Handled;
    }));
    onMouseDown.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
            const int track = static_cast<int>(p.y / laneHeight);
            if (track >= 0 && track < static_cast<int>(state_.session.tracks().size())) {
                if (hitStrip(p, false)) {
                    redraw();
                    return newui::SyncReturn::Handled;
                }
                state_.track = track;
                if (p.x >= bounds().width() - editButtonWidth) {
                    const float localY = p.y - static_cast<float>(track) * laneHeight;
                    if (localY >= effectButtonTop && state_.session.effectCount(track) > 0 && state_.openEffectEditor) {
                        state_.openEffectEditor(track, 0);
                    } else if (localY < effectButtonTop && !state_.session.instrumentName(track).empty() && state_.openEditor) {
                        state_.openEditor(track);
                    }
                }
                if (state_.changed) state_.changed();
            }
            return newui::SyncReturn::Handled;
        }));
}

void HeadersCanvas::paint(BLContext& ctx) {
    const float w = bounds().width(), h = bounds().height();
    ctx.set_fill_style(panelAlt);
    ctx.fill_rect(BLRect(0, 0, w, h));
    const auto& tracks = state_.session.tracks();
    for (size_t t = 0; t < tracks.size(); ++t) {
        const float y = static_cast<float>(t) * laneHeight;
        ctx.set_fill_style(BLRgba32(static_cast<int>(t) == state_.track ? 0xFF2A2F36 : 0xFF212124));
        ctx.fill_rect(BLRect(0, y, w, laneHeight - 1.0f));
        ctx.set_fill_style(rgb(tracks[t].color));
        ctx.fill_rect(BLRect(0, y, 5, laneHeight - 1.0f));
        drawText(ctx, std::to_string(t + 1) + " " + tracks[t].name, 12.0f, 14.0f, y + 20.0f, BLRgba32(0xFFE4E4E6));
        const std::string instrument = state_.session.instrumentName(static_cast<int>(t));
        if (!instrument.empty()) {
            drawText(ctx, instrument, 10.0f, 14.0f, y + 38.0f, BLRgba32(0xFF8E8E94));
            ctx.set_fill_style(BLRgba32(0xFF3A3F47));
            ctx.fill_rect(BLRect(w - editButtonWidth + 4.0f, y + 6.0f, editButtonWidth - 10.0f, 20.0f));
            drawText(ctx, "Edit", 10.0f, w - editButtonWidth + 10.0f, y + 20.0f, BLRgba32(0xFFE4E4E6));
        }
        {
            const StripRects r = stripRects(w);
            const ClipPlayer& player = state_.session.player();
            const int id = static_cast<int>(t);
            const float sy = y + stripTop;
            auto button = [&](float x, const char* label, bool on, uint32_t onColor) {
                ctx.set_fill_style(BLRgba32(on ? onColor : 0xFF3A3F47));
                ctx.fill_rect(BLRect(x, sy, 18.0f, stripHeight));
                drawText(ctx, label, 10.0f, x + 5.0f, sy + 12.0f, BLRgba32(on ? 0xFF101012 : 0xFFE4E4E6));
            };
            button(r.muteX, "M", player.trackMuted(id), 0xFFF2B84A);
            button(r.soloX, "S", player.trackSoloed(id), 0xFF3FB4F0);

            ctx.set_fill_style(BLRgba32(0xFF111113));
            ctx.fill_rect(BLRect(r.volumeX, sy, r.volumeW, stripHeight));
            ctx.set_fill_style(player.trackMuted(id) ? BLRgba32(0xFF55555C) : rgb(tracks[t].color));
            ctx.fill_rect(BLRect(r.volumeX, sy, r.volumeW * positionOfGain(player.trackVolume(id)), stripHeight));
            const float volume = player.trackVolume(id);
            char text[16];
            if (volume <= 0.0f) std::snprintf(text, sizeof(text), "-inf");
            else std::snprintf(text, sizeof(text), "%.1f dB", 20.0f * std::log10(volume));
            drawText(ctx, text, 9.0f, r.volumeX + 4.0f, sy + 12.0f, BLRgba32(0xFFE4E4E6));

            ctx.set_fill_style(BLRgba32(0xFF111113));
            ctx.fill_rect(BLRect(r.panX, sy, r.panW, stripHeight));
            const float pan = player.trackPan(id), centre = r.panX + r.panW * 0.5f;
            ctx.set_fill_style(BLRgba32(0xFF8E9096));
            ctx.fill_rect(BLRect(std::min(centre, centre + pan * r.panW * 0.5f), sy, std::max(1.0f, std::abs(pan) * r.panW * 0.5f), stripHeight));
            ctx.set_fill_style(BLRgba32(0xFFE4E4E6));
            ctx.fill_rect(BLRect(centre - 0.5f, sy, 1.0f, stripHeight));
        }
        if (state_.session.effectCount(static_cast<int>(t)) > 0) {
            ctx.set_fill_style(BLRgba32(0xFF3A3F47));
            ctx.fill_rect(BLRect(w - editButtonWidth + 4.0f, y + effectButtonTop + 2.0f, editButtonWidth - 10.0f, 20.0f));
            drawText(ctx, "FX", 10.0f, w - editButtonWidth + 14.0f, y + effectButtonTop + 16.0f, BLRgba32(0xFFE4E4E6));
        }
    }
}

PatchCanvas::PatchCanvas(EditorState& state) : state_(state) { setVisible(true); }

void PatchCanvas::paint(BLContext& ctx) {
    const float w = bounds().width(), h = bounds().height();
    ctx.set_fill_style(well);
    ctx.fill_rect(BLRect(0, 0, w, h));
    const Session& session = state_.session;
    const bool have = state_.clip >= 0 && static_cast<size_t>(state_.clip) < session.clips().size();
    if (!have || session.clips()[static_cast<size_t>(state_.clip)].kind != ClipKind::graph) {
        drawText(ctx, "Select a graph clip to see its nodes", 12.0f, 16.0f, 28.0f, muted);
        return;
    }
    const ClipInfo& clip = session.clips()[static_cast<size_t>(state_.clip)];
    drawText(ctx, clip.name + " - graph", 12.0f, 12.0f, 20.0f, BLRgba32(0xFFE4E4E6));

    constexpr float nodeW = 120.0f, nodeH = 52.0f, stepX = 170.0f, stepY = 78.0f, originX = 20.0f, originY = 36.0f;
    auto at = [&](const GraphNodeInfo& n) { return BLPoint(static_cast<double>(originX + n.x * stepX), static_cast<double>(originY + n.y * stepY)); };

    ctx.set_stroke_style(BLRgba32(0xFF6A6A74));
    ctx.set_stroke_width(2.0f);
    for (const auto& [from, to] : clip.graph.wires) {
        const BLPoint a = at(clip.graph.nodes[static_cast<size_t>(from)]), b = at(clip.graph.nodes[static_cast<size_t>(to)]);
        BLPath path;
        path.move_to(a.x + nodeW, a.y + nodeH * 0.5);
        const double reach = (b.x - a.x - static_cast<double>(nodeW)) * 0.5;
        path.cubic_to(a.x + nodeW + reach, a.y + nodeH * 0.5, b.x - reach, b.y + nodeH * 0.5, b.x, b.y + nodeH * 0.5);
        ctx.stroke_path(path);
    }
    for (const GraphNodeInfo& n : clip.graph.nodes) {
        const BLPoint p = at(n);
        ctx.set_fill_style(BLRgba32(0xFF26262B));
        ctx.fill_round_rect(BLRoundRect(p.x, p.y, nodeW, nodeH, 4, 4));
        ctx.set_fill_style(BLRgba32(n.source ? 0xFF1F4A3A : n.modulator ? 0xFF2E2450 : 0xFF34343B));
        ctx.fill_round_rect(BLRoundRect(p.x, p.y, nodeW, 18, 4, 4));
        ctx.set_stroke_style(BLRgba32(0xFF4A4A52));
        ctx.set_stroke_width(1.0f);
        ctx.stroke_round_rect(BLRoundRect(p.x, p.y, nodeW, nodeH, 4, 4));
        drawText(ctx, n.title, 11.0f, static_cast<float>(p.x) + 7.0f, static_cast<float>(p.y) + 13.0f, BLRgba32(0xFFE4E4E6));
        drawText(ctx, n.detail, 9.0f, static_cast<float>(p.x) + 7.0f, static_cast<float>(p.y) + 36.0f, muted);
    }
}

}  // namespace wavy::editor
