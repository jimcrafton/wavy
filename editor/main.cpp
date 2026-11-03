#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "newui/application.h"
#include "newui/bundle.h"
#include "newui/controls.h"
#include "newui/frame.h"
#include "newui/rootview.h"
#include "session.h"
#include "sessionController.h"
#include "wavy/audioDevice.h"
#ifdef WAVY_GUI_AUTOMATION
#include "newui/automation.h"
#endif

extern void registerReflectionData();

using namespace wavy;
using namespace wavy::editor;

namespace {

bool selfCheck(SessionController& controller) {
    Session& session = controller.session();
    EditorState& state = controller.state();
    int failed = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "check failed: %s\n", what);
            ++failed;
        }
    };
    expect(controller.ruler() && controller.timeline() && controller.headers() && controller.patch(), "the four canvases are attached");
    expect(controller.control<newui::Button>("playButton") != nullptr, "the transport buttons exist");
    expect(session.clips().size() >= 2, "the demo session has clips");

    // Play through the engine without a device and look at what comes out.
    session.player().play();
    std::vector<float> out(48000);
    for (size_t at = 0; at < out.size(); at += 480) {
        session.player().render(out.data() + at, 480);
    }
    float peak = 0.0f;
    for (float v : out) {
        peak = std::max(peak, std::abs(v));
    }
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
            for (int x = 0; x < 900; ++x) {
                if (row[x] != background.value) {
                    drawn = true;
                    break;
                }
            }
        }
        expect(drawn, what);
    };
    for (newui::SubView* canvas : std::initializer_list<newui::SubView*>{controller.ruler(), controller.timeline(), controller.headers(), controller.patch()}) {
        canvas->setBounds(newui::Rect(0, 0, 900, 300));
    }
    paintsSomething(controller.timeline(), BLRgba32(0xFF111113), "the timeline draws clips");
    paintsSomething(controller.ruler(), BLRgba32(0xFF1E1E21), "the ruler draws bar numbers");
    paintsSomething(controller.headers(), BLRgba32(0xFF1E1E21), "the headers draw track names");
    paintsSomething(controller.patch(), BLRgba32(0xFF111113), "the patch view draws the graph");
    return failed == 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool checkOnly = false;
    std::string automationPipe, driver, wavPath, statePath, pluginCachePath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--check") {
            checkOnly = true;
        } else if (arg == "--automation" && i + 1 < argc) {
            automationPipe = argv[++i];
        } else if (arg == "--driver" && i + 1 < argc) {
            driver = argv[++i];
        } else if (arg == "--state" && i + 1 < argc) {
            statePath = argv[++i];
        } else if (arg == "--plugin-cache" && i + 1 < argc) {
            pluginCachePath = argv[++i];
        } else {
            wavPath = arg;
        }
    }
    registerReflectionData();
    if (statePath.empty()) {
        char* base = nullptr;
        size_t baseSize = 0;
        _dupenv_s(&base, &baseSize, "LOCALAPPDATA");
        statePath = (base ? std::string(base) + "\\wavy\\" : std::string()) + "instruments.state";
        std::free(base);
    }

    AudioOutput audio;
    audio.info.sampleRate = 48000.0;
    const std::vector<std::string> drivers = listAsioDrivers();
    if (!checkOnly) {
        if (!drivers.empty()) {
            audio.open(driver.empty() ? drivers.front() : driver);
        } else {
            audio.status = "Audio: no ASIO driver found (built without ASIO_SDK_DIR, or none installed)";
        }
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
    SessionController controller(&frame.rootView(), session, audio);
    controller.initialize();
    controller.loadDemo({!checkOnly, wavPath, statePath, pluginCachePath, {}});

    if (checkOnly) {
        return selfCheck(controller) ? 0 : 1;
    }

    const std::string current = driver.empty() && !drivers.empty() ? drivers.front() : driver;
    const auto at = std::find(drivers.begin(), drivers.end(), current);
    controller.showDrivers(drivers, at == drivers.end() ? 0 : static_cast<size_t>(at - drivers.begin()));

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
    std::string saveError;
    if (!session.saveInstrumentStates(statePath, saveError) && saveError != "built without VST3 hosting") {
        std::fprintf(stderr, "could not save the patch: %s\n", saveError.c_str());
    }
    return 0;
}
