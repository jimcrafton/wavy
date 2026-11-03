#define NOMINMAX
#include "sessionController.h"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "newui/application.h"
#include "newui/controls.h"
#include "newui/layout.h"

namespace wavy::editor {

namespace {

std::string openWavDialog() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"WAV files\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }
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

// Slider position (0..100) <-> Drone setting. Cutoff and resonance are exponential, so the low end has fine control.
float cutoffOf(float v) { return 80.0f * std::pow(100.0f, v / 100.0f); }
float sliderOfCutoff(float hz) { return 100.0f * std::log(hz / 80.0f) / std::log(100.0f); }
float qOf(float v) { return 0.5f * std::pow(20.0f, v / 100.0f); }
float sliderOfQ(float q) { return 100.0f * std::log(q / 0.5f) / std::log(20.0f); }
float centsOf(float v) { return v - 50.0f; }
float sliderOfCents(float c) { return c + 50.0f; }
float dbOf(float v) { return -48.0f + 0.48f * v; }
float sliderOfDb(float db) { return (db + 48.0f) / 0.48f; }

constexpr const char* sliderNames[] = {"clipCutoff", "clipResonance", "clipDetune", "clipLevel"};

}  // namespace

bool AudioOutput::open(const std::string& name) {
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

void AudioOutput::start(Session& session) {
    if (!device) {
        return;
    }
    const uint32_t channels = info.outputChannels;
    device->start([this, &session, channels](float* const* out, uint32_t frames) {
        ++callbacks;
        if (channels >= 2) {
            session.player().render(out, frames);
            for (uint32_t c = 2; c < channels; ++c) {
                std::copy(out[0], out[0] + frames, out[c]);
            }
        } else {
            session.player().render(out[0], frames);
        }
    });
    started = true;
}

void AudioOutput::pause() {
    if (device && started) {
        device->stop();
        started = false;
    }
}

void AudioOutput::close() {
    if (!device) {
        return;
    }
    if (started) {
        device->stop();
    }
    device->close();
    device.reset();
    started = false;
}

SessionController::SessionController(newui::RootView* view, Session& session, AudioOutput& audio)
    : newui::RootController(view), session_(session), audio_(audio), state_(session) {}

bool SessionController::internal_init() {
    if (!newui::RootController::internal_init()) {
        return false;
    }
    ruler_ = fillCanvas<RulerCanvas>("rulerCanvas", state_);
    timeline_ = fillCanvas<TimelineCanvas>("timelineCanvas", state_);
    headers_ = fillCanvas<HeadersCanvas>("trackHeaders", state_);
    patch_ = fillCanvas<PatchCanvas>("detailCanvas", state_);
    bindBrowser();

    // The owner handle is looked up per click: the window may not exist yet when the controller is built.
    state_.openEditor = [this](int track) {
        std::string error;
        session_.setEditorOwner(view()->windowHandle());
        if (!session_.openInstrumentEditor(track, error)) {
            status("Plugin editor: " + error);
        }
    };
    state_.openEffectEditor = [this](int track, int index) {
        std::string error;
        session_.setEditorOwner(view()->windowHandle());
        if (!session_.openEffectEditor(track, index, error)) {
            status("Plugin editor: " + error);
        }
    };
    state_.changed = [this] { selectionChanged(); };

    fillDropDown("toolSelect", {"Select"}, 0);
    fillDropDown("rulerModeSelect", {"Bars+Beats"}, 0);
    fillDropDown("gridSelect", {"1/16"}, 0);
    fillDropDown("snapModeSelect", {"Absolute"}, 0);
    fillDropDown("countInSelect", {"Count-in 1"}, 0);

    bindTransport();
    bindSliders();

    // postDelayed throws until the loop is running, so the timer starts from onStart.
    newui::Application::instance().runLoop().onStart.add(std::function<newui::SyncReturn(newui::RunLoop&)>([this](newui::RunLoop& loop) {
        loop.postDelayed(std::chrono::milliseconds(33), [this] {
            tick();
            return false;
        });
        return newui::SyncReturn::Handled;
    }));
    return true;
}

template <class T, class... Args>
T* SessionController::fillCanvas(const char* name, Args&&... args) {
    // The mockup's canvas hosts carry sample content for review; hide it and fill the host with the real canvas.
    auto* host = control<newui::SubView>(name);
    if (!host) {
        std::fprintf(stderr, "mainWindow.newui: '%s' is missing or not a SubView\n", name);
        return nullptr;
    }
    for (newui::SubView* child : host->childViews()) {
        child->setVisible(false);
    }
    host->setLayout(std::make_unique<newui::FlexLayout>(newui::Orientation::Vertical));
    auto* canvas = new T(std::forward<Args>(args)...);
    canvas->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(1.0f));
    host->addChild(canvas);
    return canvas;
}

void SessionController::fillDropDown(const char* name, const std::vector<std::string>& items, size_t selected) {
    auto* list = control<newui::DropDownList>(name);
    if (!list) {
        return;
    }
    auto model = std::make_unique<newui::StringListModel>();
    for (const std::string& s : items) {
        model->addItem(s);
    }
    list->setModel(std::move(model));
    list->setSelectedIndex(selected);
}

void SessionController::showDrivers(const std::vector<std::string>& drivers, size_t selected) {
    drivers_ = drivers;
    fillDropDown("audioDriverSelect", drivers_, selected);
    auto* list = control<newui::DropDownList>("audioDriverSelect");
    if (!list) {
        return;
    }
    list->onSelectionChanged.add(std::function<newui::SyncReturn(newui::DropDownList&)>([this](newui::DropDownList& d) {
        if (!d.selectedIndex() || *d.selectedIndex() >= drivers_.size()) {
            return newui::SyncReturn::Handled;
        }
        session_.player().stop();
        audio_.open(drivers_[*d.selectedIndex()]);
        std::string text = audio_.status;
        if (audio_.device) {
            if (std::abs(audio_.info.sampleRate - session_.sampleRate()) > 0.5) {
                text += " (sample rate changed: restart to rebuild the clips)";
            }
            audio_.start(session_);
        }
        status(text);
        return newui::SyncReturn::Handled;
    }));
}

void SessionController::bindTransport() {
    auto onClick = [this](const char* name, std::function<void()> fn) {
        if (auto* button = control<newui::Button>(name)) {
            button->onClick.add(std::function<newui::SyncReturn(newui::Control&)>([fn](newui::Control&) {
                fn();
                return newui::SyncReturn::Handled;
            }));
        }
    };
    onClick("playButton", [this] { session_.player().play(); });
    onClick("pauseButton", [this] { session_.player().stop(); });
    onClick("stopButton", [this] {
        if (session_.player().playing()) {
            session_.player().stop();
        } else {
            session_.player().seek(0);
        }
    });
    onClick("rewindButton", [this] { session_.player().seek(0); });
    onClick("forwardButton", [this] { session_.player().seek(barStart() + session_.framesPerBar()); });
    onClick("addSampleButton", [this] {
        const std::string path = openWavDialog();
        if (path.empty()) {
            return;
        }
        std::string error;
        added(session_.addSampleClip(state_.track, barStart(), path, error), error, "the WAV");
    });
    onClick("addNotesButton", [this] {
        std::string error;
        added(session_.addNotesClip(state_.track, barStart(), 4, error), error, "the MIDI clip");
    });
    onClick("addGraphButton", [this] {
        std::string error;
        added(session_.addGraphClip(state_.track, barStart(), 4, error), error, "the graph clip");
    });
}

void SessionController::bindSliders() {
    for (const char* name : sliderNames) {
        if (auto* slider = control<newui::Slider>(name)) {
            slider->setRange(0.0f, 100.0f);
            slider->onValueChanged.add(std::function<newui::SyncReturn(newui::Slider&)>([this](newui::Slider&) {
                sliderChanged();
                return newui::SyncReturn::Handled;
            }));
        }
    }
}

void SessionController::bindBrowser() {
    browserCategories_ = fillCanvas<BrowserCategoriesCanvas>("browserCategories", browser_, [this] {
        if (browserList_) {
            browserList_->reset();
        }
        redrawBrowser();
    });
    // The mockup's list is a ScrollView, which does not lay out a custom canvas; the canvas takes its place beside the categories and
    // scrolls itself.
    auto* mockList = control<newui::SubView>("browserList");
    auto* row = mockList ? dynamic_cast<newui::SubView*>(mockList->parent()) : nullptr;
    if (row) {
        mockList->setVisible(false);
        browserList_ = new BrowserListCanvas(
            browser_,
            [this](const BrowserItem& item) {
                status(item.name + " by " + (item.vendor.empty() ? "an unknown maker" : item.vendor) + " (" + (item.instrument ? "instrument" : "effect") +
                       "). Double-click to " + (item.instrument ? "put it on " : "add it to ") + "the selected track.");
            },
            [this](const BrowserItem& item) { loadPlugin(item); });
        browserList_->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(1.0f));
        row->addChild(browserList_);
    }

    if (auto* search = control<newui::TextField>("browserSearch")) {
        search->model().onChanged.add(std::function<newui::SyncReturn(newui::Model&)>([this, search](newui::Model&) {
            const std::wstring wide = search->text();
            std::string utf8(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
            browser_.setSearch(utf8);
            if (browserList_) {
                browserList_->reset();
            }
            redrawBrowser();
            return newui::SyncReturn::Handled;
        }));
    }
}

void SessionController::redrawBrowser() {
    for (newui::View* v : std::initializer_list<newui::View*>{browserCategories_, browserList_}) {
        if (v) {
            v->redraw();
        }
    }
}

void SessionController::refreshBrowser() {
    std::vector<BrowserItem> items;
    for (const wavy::CatalogEntry& e : catalog_.entries()) {
        items.push_back({e.info.name, e.info.vendor, e.info.subCategories, e.path, e.classIndex, e.info.isInstrument()});
    }
    browser_.setItems(std::move(items));
    if (browserList_) {
        if (browserList_) {
            browserList_->reset();
        }
    }
    redrawBrowser();
}

void SessionController::scanPlugins(const StartOptions& options, std::string& note) {
    const std::string cache = options.pluginCachePath.empty() ? wavy::PluginCatalog::defaultCachePath() : options.pluginCachePath;
    if (!catalog_.load(cache)) {
        note += " The plugin cache was unreadable and is rebuilt.";
    }
    wavy::ScanOptions scan;
    scan.scannerExe = options.scannerPath;
    const wavy::ScanReport report = catalog_.scan(wavy::PluginCatalog::installedModules(), scan);
    std::string error;
    if (!catalog_.save(cache, error)) {
        note += " Plugin cache not saved (" + error + ").";
    }
    refreshBrowser();
    note += " " + std::to_string(browser_.itemCount()) + " plugins";
    if (report.scanned > 0) {
        note += ", " + std::to_string(report.scanned) + " newly scanned";
    }
    if (report.blocked > 0) {
        note += ", " + std::to_string(report.blocked) + " blocked (" + report.problems.front() + ")";
    }
    note += ".";
}

bool SessionController::loadPlugin(const BrowserItem& item) {
    const int track = state_.track;
    const std::string trackName = session_.tracks()[static_cast<size_t>(track)].name;
    session_.player().stop();
    const bool wasStreaming = audio_.started;
    audio_.pause();
    std::string error;
    const bool ok = item.instrument ? session_.setTrackInstrument(track, item.path, error, item.classIndex)
                                    : session_.addTrackEffect(track, item.path, error, item.classIndex);
    if (wasStreaming) {
        audio_.start(session_);
    }
    if (ok) {
        status(item.name + (item.instrument ? " is now the instrument of " : " was added to ") + trackName + ".");
    } else {
        status("Could not load " + item.name + ": " + error);
    }
    redrawAll();
    return ok;
}

void SessionController::loadDemo(const StartOptions& options) {
    std::string error;
    std::string note;
    if (options.loadPlugins) {
        scanPlugins(options, note);
        // Surge XT plays the Arp track when it is installed; otherwise the clip uses the built-in voices.
        const auto surge = catalog_.find("Surge XT", true);
        if (!surge) {
            note += " Surge XT not found: the Arp uses the built-in voices.";
        } else if (!session_.setTrackInstrument(3, surge->path, error, surge->classIndex)) {
            note += " Surge XT failed to load (" + error + "): the Arp uses the built-in voices.";
        } else {
            note += " The Arp track plays Surge XT.";
            const auto surgeFx = catalog_.find("Surge XT Effects", false);
            if (!surgeFx) {
                note += " Surge XT Effects not found.";
            } else if (!session_.addTrackEffect(3, surgeFx->path, error, surgeFx->classIndex)) {
                note += " Surge XT Effects failed to load (" + error + ").";
            } else {
                note += " Surge XT Effects follows it.";
            }

            // After the effects are on the track, so their saved state is restored too.
            std::string stateError;
            const int restored = session_.loadInstrumentStates(options.statePath, stateError);
            if (!stateError.empty()) {
                note += " State not restored (" + stateError + ").";
            } else if (restored > 0) {
                note += " Restored " + std::to_string(restored) + " plugin state" + (restored > 1 ? "s" : "") + ".";
            }
        }
    }
    state_.clip = session_.addGraphClip(2, 0, 4, error);
    if (state_.clip < 0) {
        std::fprintf(stderr, "drone: %s\n", error.c_str());
    }
    if (session_.addNotesClip(3, 2 * session_.framesPerBar(), 4, error) < 0) {
        std::fprintf(stderr, "arp: %s\n", error.c_str());
    }
    if (!options.wavPath.empty() && session_.addSampleClip(0, 0, options.wavPath, error) < 0) {
        std::fprintf(stderr, "%s: %s\n", options.wavPath.c_str(), error.c_str());
    }
    state_.track = 2;
    status(audio_.status + "." + note + " Press Play; pick a track, then add a clip at the playhead.");
    syncSliders();
}

void SessionController::status(const std::string& text) {
    if (auto* label = control<newui::Label>("statusText")) {
        label->setText(text);
    }
}

void SessionController::redrawAll() {
    for (newui::View* v : std::initializer_list<newui::View*>{ruler_, timeline_, headers_, patch_}) {
        if (v) {
            v->redraw();
        }
    }
}

uint64_t SessionController::barStart() const {
    const uint64_t bar = session_.framesPerBar();
    return session_.player().position() / bar * bar;
}

void SessionController::added(int index, const std::string& error, const char* what) {
    if (index < 0) {
        status(std::string("Could not add ") + what + ": " + error);
    } else {
        state_.clip = index;
        const ClipInfo& c = session_.clips()[static_cast<size_t>(index)];
        status("Added " + c.name + " to " + session_.tracks()[static_cast<size_t>(c.track)].name + " at bar " +
               std::to_string(c.start / session_.framesPerBar() + 1));
        selectionChanged();
    }
    redrawAll();
}

void SessionController::setLabel(const char* name, const char* format, float value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), format, static_cast<double>(value));
    if (auto* label = control<newui::Label>(name)) {
        label->setText(buf);
    }
}

void SessionController::showSettings(const DroneSettings& s) {
    setLabel("clipCutoffValue", "%.0f Hz", s.cutoffHz);
    setLabel("clipResonanceValue", "%.1f", s.q);
    setLabel("clipDetuneValue", "%+.1f c", s.detuneCents);
    setLabel("clipLevelValue", "%.1f dB", s.levelDb);
}

// Moves the sliders to the selected Drone's settings, or disables them for any other clip.
void SessionController::syncSliders() {
    const bool drone = state_.clip >= 0 && session_.clips()[static_cast<size_t>(state_.clip)].kind == ClipKind::graph;
    const DroneSettings s = drone ? session_.clips()[static_cast<size_t>(state_.clip)].drone : DroneSettings{};
    updatingSliders_ = true;
    const float values[] = {sliderOfCutoff(s.cutoffHz), sliderOfQ(s.q), sliderOfCents(s.detuneCents), sliderOfDb(s.levelDb)};
    for (size_t i = 0; i < 4; ++i) {
        if (auto* slider = control<newui::Slider>(sliderNames[i])) {
            slider->setValue(values[i]);
            slider->setEnabled(drone);
        }
    }
    updatingSliders_ = false;
    showSettings(s);
}

void SessionController::sliderChanged() {
    if (updatingSliders_ || state_.clip < 0) {
        return;
    }
    auto value = [this](const char* name) {
        auto* slider = control<newui::Slider>(name);
        return slider ? slider->value() : 0.0f;
    };
    DroneSettings s;
    s.cutoffHz = cutoffOf(value("clipCutoff"));
    s.q = qOf(value("clipResonance"));
    s.detuneCents = centsOf(value("clipDetune"));
    s.levelDb = dbOf(value("clipLevel"));
    if (!session_.setDrone(state_.clip, s)) {
        return;
    }
    showSettings(s);
    lastEdit_ = std::chrono::steady_clock::now();
    if (patch_) {
        patch_->redraw();
    }
}

void SessionController::selectionChanged() {
    if (state_.clip >= 0) {
        const ClipInfo& c = session_.clips()[static_cast<size_t>(state_.clip)];
        if (auto* label = control<newui::Label>("detailTitle")) {
            label->setText(c.name + " - " + (c.kind == ClipKind::graph ? "graph clip" : c.kind == ClipKind::notes ? "MIDI clip" : "audio clip"));
        }
        if (auto* label = control<newui::Label>("clipName")) {
            label->setText(c.name);
        }
    }
    syncSliders();
    redrawAll();
}

void SessionController::tick() {
    session_.pollPlugins();
    // The preview waveform re-renders once the knobs have been still for a moment.
    if (state_.clip >= 0 && session_.clips()[static_cast<size_t>(state_.clip)].previewDirty &&
        std::chrono::steady_clock::now() - lastEdit_ > std::chrono::milliseconds(250)) {
        session_.refreshPreview(state_.clip);
    }
    if (auto* label = control<newui::Label>("positionLabel")) {
        label->setText(positionText(session_, session_.player().position()));
    }
    if (auto* label = control<newui::Label>("sampleRateLabel")) {
        label->setText("cb " + std::to_string(audio_.callbacks.load()));
    }
    if (auto* label = control<newui::Label>("cpuLabel")) {
        const float peak = session_.player().takePeak();
        char buf[32];
        std::snprintf(buf, sizeof(buf), peak > 0.0f ? "Peak %.1f dB" : "Peak -inf", 20.0f * std::log10(std::max(peak, 1e-9f)));
        label->setText(buf);
    }
    redrawAll();
}

}  // namespace wavy::editor
