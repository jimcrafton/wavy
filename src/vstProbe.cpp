// vstProbe: lists the VST3 modules installed on this machine, or loads one and reports its classes, buses, parameters and latency.
//   vstProbe                         list installed modules and their audio classes
//   vstProbe <file.vst3> [class]     load, prepare at 48 kHz / 128 and print details
//   vstProbe <file.vst3> [class] -n  also play a middle C note for 2 seconds offline and report the peak

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "public.sdk/source/vst/hosting/module.h"
#include "wavy/vstPlugin.h"

using namespace wavy;

namespace {

void listInstalled() {
    for (const auto& path : VST3::Hosting::Module::getModulePaths()) {
        std::string error;
        auto module = VstModule::load(path, error);
        if (!module) {
            std::printf("%s\n  ERROR: %s\n", path.c_str(), error.c_str());
            continue;
        }
        std::printf("%s\n", path.c_str());
        for (size_t i = 0; i < module->classes().size(); ++i) {
            const auto& c = module->classes()[i];
            std::printf("  [%zu] %s | %s | %s | %s\n", i, c.name.c_str(), c.vendor.c_str(), c.version.c_str(), c.subCategories.c_str());
        }
    }
}

void printBuses(const char* label, const std::vector<VstBusInfo>& buses) {
    for (size_t i = 0; i < buses.size(); ++i)
        std::printf("  %s bus %zu: %s, %d ch, %s, %s\n", label, i, buses[i].name.c_str(), buses[i].channels,
                    buses[i].main ? "main" : "aux", buses[i].defaultActive ? "default active" : "default off");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        listInstalled();
        return 0;
    }
    std::string path = argv[1];
    size_t classIndex = 0;
    bool note = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-n") == 0) note = true;
        else classIndex = static_cast<size_t>(std::atoi(argv[i]));
    }

    std::string error;
    auto module = VstModule::load(path, error);
    if (!module) {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
        return 1;
    }
    auto plugin = VstPlugin::create(module, classIndex, error);
    if (!plugin) {
        std::fprintf(stderr, "create failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s (%s) %s [%s]\n", plugin->info().name.c_str(), plugin->info().vendor.c_str(), plugin->info().version.c_str(),
                plugin->info().subCategories.c_str());
    printBuses("in ", plugin->inputBuses());
    printBuses("out", plugin->outputBuses());

    auto params = plugin->parameters();
    std::printf("  %zu parameters\n", params.size());
    for (size_t i = 0; i < params.size() && i < 12; ++i)
        std::printf("    #%u %s [%s] steps=%d default=%.3f%s%s\n", params[i].id, params[i].title.c_str(), params[i].units.c_str(),
                    params[i].stepCount, params[i].defaultNormalized, params[i].readOnly ? " readonly" : "",
                    params[i].hidden ? " hidden" : "");

    constexpr int block = 128;
    if (!plugin->prepare(48000.0, block, error)) {
        std::fprintf(stderr, "prepare failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  prepared: %d in / %d out channels, latency %d samples, tail %u\n", plugin->inputChannels(), plugin->outputChannels(),
                plugin->latencySamples(), plugin->tailSamples());

    if (note) {
        const int blocks = 48000 * 2 / block;
        std::vector<float> l(block), r(block), inL(block, 0.0f), inR(block, 0.0f);
        float* out[2] = {l.data(), r.data()};
        const float* in[2] = {inL.data(), inR.data()};
        float peak = 0.0f, tailPeak = 0.0f;
        bool finite = true;
        for (int b = 0; b < blocks; ++b) {
            jitnodes::EventBuffer events;
            if (b == 0) events.push({0, 0x90, 60, 100});
            if (b == blocks / 2) events.push({0, 0x80, 60, 0});
            plugin->process(plugin->inputChannels() ? in : nullptr, out, block, &events);
            for (int i = 0; i < block; ++i) {
                finite = finite && std::isfinite(l[static_cast<size_t>(i)]) && std::isfinite(r[static_cast<size_t>(i)]);
                const float m = std::max(std::fabs(l[static_cast<size_t>(i)]), std::fabs(r[static_cast<size_t>(i)]));
                peak = std::max(peak, m);
                if (b > blocks - 20) tailPeak = std::max(tailPeak, m);
            }
        }
        std::printf("  note test: peak %.4f, last-20-blocks peak %.6f, finite=%s, failed=%s\n", peak, tailPeak, finite ? "yes" : "NO",
                    plugin->failed() ? "YES" : "no");
    }
    return 0;
}
