#pragma once

#include <memory>
#include <string>

#include "wavy/vstPlugin.h"

namespace wavy {

// A top-level window that hosts a plugin's own editor (IPlugView) and follows its resize requests. Main thread only, on a thread that
// pumps messages. The plugin must outlive the window: close or destroy the window before the plugin.
class PluginEditorWindow {
public:
    // Null with `error` set when the plugin has no editor or the window cannot be made. Shown on return. `owner` is an optional HWND
    // (any window of the host's top-level window): the editor then stays above it, like a floating tool window.
    static std::unique_ptr<PluginEditorWindow> open(VstPlugin& plugin, std::string& error, void* owner = nullptr);
    ~PluginEditorWindow();
    PluginEditorWindow(const PluginEditorWindow&) = delete;
    PluginEditorWindow& operator=(const PluginEditorWindow&) = delete;

    // False once the user closed the window.
    bool isOpen() const;
    void bringToFront();
    void close();

    struct Impl;

private:
    PluginEditorWindow();
    std::unique_ptr<Impl> impl_;
};

}  // namespace wavy
