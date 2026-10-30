#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "canvases.h"
#include "newui/application.h"
#include "newui/bundle.h"
#include "newui/controls.h"
#include "newui/frame.h"
#include "newui/layout.h"
#include "newui/rootview.h"
#include "session.h"
#include "wavy/audioDevice.h"
#ifdef WAVY_GUI_AUTOMATION
#include "newui/automation.h"
#endif

extern void registerReflectionData();

using namespace wavy;
using namespace wavy::editor;

namespace {

// The mockup's canvas hosts carry sample content for review; hide it and fill the host with the real canvas.
template <class T>
T* fill(newui::RootView& root, const char* name, EditorState& state) {
    auto* host = dynamic_cast<newui::SubView*>(root.findView(name));
    if (!host) {
        std::fprintf(stderr, "mainWindow.newui: '%s' is missing or not a SubView\n", name);
        return nullptr;
    }
    for (newui::SubView* child : host->childViews()) child->setVisible(false);
    host->setLayout(std::make_unique<newui::FlexLayout>(newui::Orientation::Vertical));
    auto* canvas = new T(state);
    canvas->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(1.0f));
    host->addChild(canvas);
    return canvas;
}

std::atomic<unsigned> audioCallbacks{0};

// The ASIO device and its status text; switching drivers closes the old one before the new one opens.
struct Audio {
    std::unique_ptr<AudioDevice> device;
    AudioDeviceInfo info;
    std::string status = "Audio: off";
    bool started = false;

    bool open(const std::string& name) {
        close();
        device = makeAsioDevice();
        if (!device) {
            status = "Audio: built without the ASIO SDK";
            return false;
        }
        try {
            info = device->open(name);
            status = "Audio: " + name + ", " + std::to_string(static_cast<int>(info.sampleRate)) + " Hz, " + std::to_string(info.blockSize) +
                     " frames, " + std::to_string(info.outputChannels) + " outputs";
            return true;
        } catch (const std::exception& e) {
            status = "Audio: could not open " + name + ": " + e.what();
            device.reset();
            return false;
        }
    }

    void start(Session& session) {
        if (!device) return;
        const uint32_t channels = info.outputChannels;
        device->start([&session, channels](float* const* out, uint32_t frames) {
            ++audioCallbacks;
            session.player().render(out[0], frames);
            for (uint32_t c = 1; c < channels; ++c) std::copy(out[0], out[0] + frames, out[c]);
        });
        started = true;
    }

    void close() {
        if (!device) return;
        if (started) device->stop();
        device->close();
        device.reset();
        started = false;
    }
};

void fillDropDown(newui::RootView& root, const char* name, const std::vector<std::string>& items, size_t selected) {
    auto* list = dynamic_cast<newui::DropDownList*>(root.findView(name));
    if (!list) return;
    auto model = std::make_unique<newui::StringListModel>();
    for (const std::string& s : items) model->addItem(s);
    list->setModel(std::move(model));
    list->setSelectedIndex(selected);
}

template <class T>
T* find(newui::RootView& root, const char* name) {
    return dynamic_cast<T*>(root.findView(name));
}

std::string openWavDialog() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"WAV files\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    char utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return utf8;
}

std::string positionText(const Session& session, uint64_t frame) {
    const uint64_t bar = frame / session.framesPerBar(), inBar = frame % session.framesPerBar();
    const uint64_t beat = inBar / session.framesPerBeat(), tick = inBar % session.framesPerBeat() * 100 / session.framesPerBeat();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%03llu.%llu.%02llu", static_cast<unsigned long long>(bar + 1), static_cast<unsigned long long>(beat + 1),
                  static_cast<unsigned long long>(tick));
    return buf;
}

struct Window {
    newui::RootView& root;
    Session& session;
    EditorState& state;
    RulerCanvas* ruler = nullptr;
    TimelineCanvas* timeline = nullptr;
    HeadersCanvas* headers = nullptr;
    PatchCanvas* patch = nullptr;

    void status(const std::string& text) {
        if (auto* label = find<newui::Label>(root, "statusText")) label->setText(text);
    }

    void redrawAll() {
        for (newui::View* v : std::initializer_list<newui::View*>{ruler, timeline, headers, patch})
            if (v) v->redraw();
    }

    uint64_t barStart() const {
        const uint64_t bar = session.framesPerBar();
        return session.player().position() / bar * bar;
    }

    void added(int index, const std::string& error, const char* what) {
        if (index < 0) {
            status(std::string("Could not add ") + what + ": " + error);
        } else {
            state.clip = index;
            const ClipInfo& c = session.clips()[static_cast<size_t>(index)];
            status("Added " + c.name + " to " + session.tracks()[static_cast<size_t>(c.track)].name + " at bar " +
                   std::to_string(c.start / session.framesPerBar() + 1));
            if (state.changed) state.changed();
        }
        redrawAll();
    }

    void addSample(const std::string& path) {
        std::string error;
        added(session.addSampleClip(state.track, barStart(), path, error), error, "the WAV");
    }
    void addNotes() {
        std::string error;
        added(session.addNotesClip(state.track, barStart(), 4, error), error, "the MIDI clip");
    }
    void addGraph() {
        std::string error;
        added(session.addGraphClip(state.track, barStart(), 4, error), error, "the graph clip");
    }

    // Slider position (0..100) <-> Drone setting. Cutoff and resonance are exponential, so the low end has fine control.
    static float cutoffOf(float v) { return 80.0f * std::pow(100.0f, v / 100.0f); }
    static float sliderOfCutoff(float hz) { return 100.0f * std::log(hz / 80.0f) / std::log(100.0f); }
    static float qOf(float v) { return 0.5f * std::pow(20.0f, v / 100.0f); }
    static float sliderOfQ(float q) { return 100.0f * std::log(q / 0.5f) / std::log(20.0f); }
    static float centsOf(float v) { return v - 50.0f; }
    static float sliderOfCents(float c) { return c + 50.0f; }
    static float dbOf(float v) { return -48.0f + 0.48f * v; }
    static float sliderOfDb(float db) { return (db + 48.0f) / 0.48f; }

    bool updatingSliders = false;
    std::chrono::steady_clock::time_point lastEdit{};

    void setLabel(const char* name, const char* format, float value) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), format, static_cast<double>(value));
        if (auto* label = find<newui::Label>(root, name)) label->setText(buf);
    }

    void showSettings(const DroneSettings& s) {
        setLabel("clipCutoffValue", s.cutoffHz >= 1000.0f ? "%.0f Hz" : "%.0f Hz", s.cutoffHz);
        setLabel("clipResonanceValue", "%.1f", s.q);
        setLabel("clipDetuneValue", "%+.1f c", s.detuneCents);
        setLabel("clipLevelValue", "%.1f dB", s.levelDb);
    }

    // Moves the sliders to the selected Drone's settings, or disables them for any other clip.
    void syncSliders() {
        const bool drone = state.clip >= 0 && session.clips()[static_cast<size_t>(state.clip)].kind == ClipKind::graph;
        const DroneSettings s = drone ? session.clips()[static_cast<size_t>(state.clip)].drone : DroneSettings{};
        updatingSliders = true;
        const struct { const char* name; float value; } sliders[] = {{"clipCutoff", sliderOfCutoff(s.cutoffHz)}, {"clipResonance", sliderOfQ(s.q)},
                                                                   {"clipDetune", sliderOfCents(s.detuneCents)}, {"clipLevel", sliderOfDb(s.levelDb)}};
        for (const auto& item : sliders)
            if (auto* slider = find<newui::Slider>(root, item.name)) {
                slider->setValue(item.value);
                slider->setEnabled(drone);
            }
        updatingSliders = false;
        showSettings(s);
    }

    void sliderChanged() {
        if (updatingSliders || state.clip < 0) return;
        auto value = [this](const char* name) {
            auto* slider = find<newui::Slider>(root, name);
            return slider ? slider->value() : 0.0f;
        };
        DroneSettings s;
        s.cutoffHz = cutoffOf(value("clipCutoff"));
        s.q = qOf(value("clipResonance"));
        s.detuneCents = centsOf(value("clipDetune"));
        s.levelDb = dbOf(value("clipLevel"));
        if (!session.setDrone(state.clip, s)) return;
        showSettings(s);
        lastEdit = std::chrono::steady_clock::now();
        if (patch) patch->redraw();
    }

    void bindSliders() {
        for (const char* name : {"clipCutoff", "clipResonance", "clipDetune", "clipLevel"})
            if (auto* slider = find<newui::Slider>(root, name)) {
                slider->setRange(0.0f, 100.0f);
                slider->onValueChanged.add(std::function<newui::SyncReturn(newui::Slider&)>([this](newui::Slider&) {
                    sliderChanged();
                    return newui::SyncReturn::Handled;
                }));
            }
    }

    void tick() {
        // The preview waveform re-renders once the knobs have been still for a moment.
        if (state.clip >= 0 && session.clips()[static_cast<size_t>(state.clip)].previewDirty &&
            std::chrono::steady_clock::now() - lastEdit > std::chrono::milliseconds(250))
            session.refreshPreview(state.clip);
        if (auto* label = find<newui::Label>(root, "positionLabel")) label->setText(positionText(session, session.player().position()));
        if (auto* label = find<newui::Label>(root, "sampleRateLabel")) label->setText("cb " + std::to_string(audioCallbacks.load()));
        if (auto* label = find<newui::Label>(root, "cpuLabel")) {
            const float peak = session.player().takePeak();
            char buf[32];
            std::snprintf(buf, sizeof(buf), peak > 0.0f ? "Peak %.1f dB" : "Peak -inf", 20.0f * std::log10(std::max(peak, 1e-9f)));
            label->setText(buf);
        }
        redrawAll();
    }

    void wire(newui::RunLoop& loop) {
        auto onClick = [this](const char* name, std::function<void()> fn) {
            if (auto* b = find<newui::Button>(root, name))
                b->onClick.add(std::function<newui::SyncReturn(newui::Control&)>([fn](newui::Control&) {
                    fn();
                    return newui::SyncReturn::Handled;
                }));
        };
        onClick("playButton", [this] { session.player().play(); });
        onClick("pauseButton", [this] { session.player().stop(); });
        onClick("stopButton", [this] {
            if (session.player().playing()) session.player().stop();
            else session.player().seek(0);
        });
        onClick("rewindButton", [this] { session.player().seek(0); });
        onClick("forwardButton", [this] { session.player().seek(barStart() + session.framesPerBar()); });
        onClick("addSampleButton", [this] {
            const std::string path = openWavDialog();
            if (!path.empty()) addSample(path);
        });
        onClick("addNotesButton", [this] { addNotes(); });
        onClick("addGraphButton", [this] { addGraph(); });
        state.changed = [this] {
            if (state.clip >= 0) {
                const ClipInfo& c = session.clips()[static_cast<size_t>(state.clip)];
                if (auto* label = find<newui::Label>(root, "detailTitle")) label->setText(c.name + " - " + (c.kind == ClipKind::graph ? "graph clip" : c.kind == ClipKind::notes ? "MIDI clip" : "audio clip"));
                if (auto* label = find<newui::Label>(root, "clipName")) label->setText(c.name);
            }
            syncSliders();
            redrawAll();
        };
        bindSliders();
        syncSliders();
        // postDelayed throws until the loop is running, so the timer starts from onStart.
        loop.onStart.add(std::function<newui::SyncReturn(newui::RunLoop&)>([this](newui::RunLoop& l) {
            l.postDelayed(std::chrono::milliseconds(33), [this] {
                tick();
                return false;
            });
            return newui::SyncReturn::Handled;
        }));
    }
};

bool selfCheck(Window& w, Session& session, EditorState& state) {
    int failed = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "check failed: %s\n", what);
            ++failed;
        }
    };
    expect(w.ruler && w.timeline && w.headers && w.patch, "the four canvases are attached");
    expect(find<newui::Button>(w.root, "playButton") != nullptr, "the transport buttons exist");
    expect(session.clips().size() >= 2, "the demo session has clips");

    // Play through the engine without a device and look at what comes out.
    session.player().play();
    std::vector<float> out(48000);
    for (size_t at = 0; at < out.size(); at += 480) session.player().render(out.data() + at, 480);
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::abs(v));
    expect(peak > 0.01f && peak < 1.0f, "the demo clips make sound");
    expect(session.player().position() == out.size(), "the playhead advanced");
    session.player().stop();
    session.player().seek(0);

    state.clip = 0;
    state.track = session.clips()[0].track;
    auto paintsSomething = [&](newui::SubView* canvas, BLRgba32 background, const char* what) {
        BLImage image(900, 300, BL_FORMAT_PRGB32);
        BLContext ctx(image);
        canvas->paint(ctx);
        ctx.end();
        BLImageData data;
        image.get_data(&data);
        bool drawn = false;
        for (int y = 0; y < 300 && !drawn; ++y) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(data.pixel_data) + static_cast<size_t>(y) * static_cast<size_t>(data.stride));
            for (int x = 0; x < 900; ++x)
                if (row[x] != background.value) {
                    drawn = true;
                    break;
                }
        }
        expect(drawn, what);
    };
    for (newui::SubView* canvas : std::initializer_list<newui::SubView*>{w.ruler, w.timeline, w.headers, w.patch})
        canvas->setBounds(newui::Rect(0, 0, 900, 300));
    paintsSomething(w.timeline, BLRgba32(0xFF111113), "the timeline draws clips");
    paintsSomething(w.ruler, BLRgba32(0xFF1E1E21), "the ruler draws bar numbers");
    paintsSomething(w.headers, BLRgba32(0xFF1E1E21), "the headers draw track names");
    paintsSomething(w.patch, BLRgba32(0xFF111113), "the patch view draws the graph");
    return failed == 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool checkOnly = false;
    std::string automationPipe, driver, wavPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--check") checkOnly = true;
        else if (arg == "--automation" && i + 1 < argc) automationPipe = argv[++i];
        else if (arg == "--driver" && i + 1 < argc) driver = argv[++i];
        else wavPath = arg;
    }
    registerReflectionData();

    Audio audio;
    audio.info.sampleRate = 48000.0;
    const std::vector<std::string> drivers = listAsioDrivers();
    if (!checkOnly) {
        if (!drivers.empty()) audio.open(driver.empty() ? drivers.front() : driver);
        else audio.status = "Audio: no ASIO driver found (built without ASIO_SDK_DIR, or none installed)";
        std::fprintf(stderr, "%s\n", audio.status.c_str());
    }
    const double sampleRate = audio.device ? audio.info.sampleRate : 48000.0;

    newui::Frame frame;
    newui::Application& app = newui::Application::instance();
    app.setName("wavyEditor");
    app.setFrame(&frame);
    frame.setName("mainWindow");
    if (!newui::Bundle::instance().loadFrame(frame)) {
        std::fprintf(stderr, "could not load Resources/mainWindow.newui\n");
        return 1;
    }

    Session session(sampleRate);
    EditorState state(session);
    Window window{frame.rootView(), session, state};
    window.ruler = fill<RulerCanvas>(window.root, "rulerCanvas", state);
    window.timeline = fill<TimelineCanvas>(window.root, "timelineCanvas", state);
    window.headers = fill<HeadersCanvas>(window.root, "trackHeaders", state);
    window.patch = fill<PatchCanvas>(window.root, "detailCanvas", state);

    std::string error;
    state.clip = session.addGraphClip(2, 0, 4, error);
    if (state.clip < 0) std::fprintf(stderr, "drone: %s\n", error.c_str());
    if (session.addNotesClip(3, 2 * session.framesPerBar(), 4, error) < 0) std::fprintf(stderr, "arp: %s\n", error.c_str());
    if (!wavPath.empty() && session.addSampleClip(0, 0, wavPath, error) < 0) std::fprintf(stderr, "%s: %s\n", wavPath.c_str(), error.c_str());
    state.track = 2;
    window.status(audio.status + ". Press Play; pick a track, then add a clip at the playhead.");

    if (checkOnly) return selfCheck(window, session, state) ? 0 : 1;

    fillDropDown(window.root, "toolSelect", {"Select"}, 0);
    fillDropDown(window.root, "rulerModeSelect", {"Bars+Beats"}, 0);
    fillDropDown(window.root, "gridSelect", {"1/16"}, 0);
    fillDropDown(window.root, "snapModeSelect", {"Absolute"}, 0);
    fillDropDown(window.root, "countInSelect", {"Count-in 1"}, 0);
    {
        const std::string current = driver.empty() && !drivers.empty() ? drivers.front() : driver;
        const auto at = std::find(drivers.begin(), drivers.end(), current);
        fillDropDown(window.root, "audioDriverSelect", drivers, at == drivers.end() ? 0 : static_cast<size_t>(at - drivers.begin()));
        if (auto* list = dynamic_cast<newui::DropDownList*>(window.root.findView("audioDriverSelect")))
            list->onSelectionChanged.add(std::function<newui::SyncReturn(newui::DropDownList&)>([&](newui::DropDownList& d) {
                if (!d.selectedIndex() || *d.selectedIndex() >= drivers.size()) return newui::SyncReturn::Handled;
                session.player().stop();
                audio.open(drivers[*d.selectedIndex()]);
                std::string text = audio.status;
                if (audio.device) {
                    if (std::abs(audio.info.sampleRate - session.sampleRate()) > 0.5) text += " (sample rate changed: restart to rebuild the clips)";
                    audio.start(session);
                }
                window.status(text);
                return newui::SyncReturn::Handled;
            }));
    }

    window.wire(app.runLoop());
    audio.start(session);
#ifdef WAVY_GUI_AUTOMATION
    if (!automationPipe.empty()) {
        newui::Automation::expose(frame.rootView());
        newui::Automation::start(automationPipe, app.runLoop());
    }
#endif
    app.run();
#ifdef WAVY_GUI_AUTOMATION
    newui::Automation::stop();
#endif
    audio.close();
    return 0;
}
