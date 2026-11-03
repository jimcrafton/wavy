#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "browserCanvases.h"
#include "canvases.h"
#include "newui/controllers.h"
#include "newui/rootcontroller.h"
#include "pluginBrowser.h"
#include "session.h"
#include "wavy/audioDevice.h"
#include "wavy/pluginScanner.h"

namespace wavy::editor {

// The ASIO device and its status text; switching drivers closes the old one before the new one opens.
struct AudioOutput {
    std::unique_ptr<AudioDevice> device;
    AudioDeviceInfo info;
    std::string status = "Audio: off";
    bool started = false;
    std::atomic<unsigned> callbacks{0};

    bool open(const std::string& name);
    void start(Session& session);
    // Stops streaming but keeps the device open, so plugins can be added to the session; start() resumes.
    void pause();
    void close();
};

// Controller of the main window: wires the loaded mainWindow view tree (canvases, transport, add-clip buttons, Drone sliders, driver
// dropdown, UI timer) to the Session. The view and the session outlive it.
class SessionController : public newui::RootController {
public:
    SessionController(newui::RootView* view, Session& session, AudioOutput& audio);

    // Fills the session with the demo content, loads the plugins and restores their state; call after initialize().
    struct StartOptions {
        bool loadPlugins = true;
        std::string wavPath;
        std::string statePath;
        std::string pluginCachePath;  // empty = %LOCALAPPDATA%\wavy\plugins.cache
        std::string scannerPath;      // empty = wavyScan.exe next to the editor
    };
    void loadDemo(const StartOptions& options);

    // Loads the plugin onto the selected track: an instrument replaces the track's instrument, an effect joins its chain. Audio is
    // paused meanwhile. This is what a double-click in the browser does.
    bool loadPlugin(const BrowserItem& item);
    PluginBrowser& browser() { return browser_; }
    BrowserListCanvas* browserList() const { return browserList_; }
    BrowserCategoriesCanvas* browserCategories() const { return browserCategories_; }
    const wavy::PluginCatalog& catalog() const { return catalog_; }

    // Fills the driver dropdown; picking an entry reopens the audio device on that driver.
    void showDrivers(const std::vector<std::string>& drivers, size_t selected);

    void status(const std::string& text);
    template <class T>
    T* control(const std::string& name) {
        return dynamic_cast<T*>(view()->findView(name));
    }

    Session& session() { return session_; }
    EditorState& state() { return state_; }
    RulerCanvas* ruler() const { return ruler_; }
    TimelineCanvas* timeline() const { return timeline_; }
    HeadersCanvas* headers() const { return headers_; }
    PatchCanvas* patch() const { return patch_; }

protected:
    bool internal_init() override;

private:
    template <class T, class... Args>
    T* fillCanvas(const char* name, Args&&... args);
    void bindBrowser();
    void refreshBrowser();
    void redrawBrowser();
    void scanPlugins(const StartOptions& options, std::string& note);
    void fillDropDown(const char* name, const std::vector<std::string>& items, size_t selected);
    void bindTransport();
    void bindSliders();
    void redrawAll();
    uint64_t barStart() const;
    void added(int index, const std::string& error, const char* what);
    void setLabel(const char* name, const char* format, float value);
    void showSettings(const DroneSettings& settings);
    void syncSliders();
    void sliderChanged();
    void selectionChanged();
    void tick();

    Session& session_;
    AudioOutput& audio_;
    EditorState state_;
    RulerCanvas* ruler_ = nullptr;
    TimelineCanvas* timeline_ = nullptr;
    HeadersCanvas* headers_ = nullptr;
    PatchCanvas* patch_ = nullptr;
    wavy::PluginCatalog catalog_;
    PluginBrowser browser_;
    BrowserListCanvas* browserList_ = nullptr;
    BrowserCategoriesCanvas* browserCategories_ = nullptr;
    bool updatingSliders_ = false;
    std::chrono::steady_clock::time_point lastEdit_{};
    std::vector<std::string> drivers_;
};

}  // namespace wavy::editor
