#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "jitnodes/jitnodes.h"
#include "wavy/audioDevice.h"
#include "wavy/audioEngine.h"

int main(int argc, char** argv) {
    const auto drivers = wavy::listAsioDrivers();
    for (const auto& d : drivers) std::printf("driver: %s\n", d.c_str());
    if (drivers.empty()) return 1;
    const std::string name = argc > 1 ? argv[1] : drivers.front();

    auto device = wavy::makeAsioDevice();
    const wavy::AudioDeviceInfo info = device->open(name);
    std::printf("opened %s: %.0f Hz, %u frames, %u outputs\n", name.c_str(), info.sampleRate, info.blockSize, info.outputChannels);

    using namespace jitnodes;
    constexpr size_t block = 128;
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    const NodeId osc = net.addNode("wrangle");
    net.node(osc).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
    net.connect(line, osc);
    const NodeId gain = net.addNode("wrangle");
    net.node(gain).setString("code", "x = x * a;");
    net.connect(osc, gain);

    auto program = AudioProgram::compile(net, gain, {block, {"x"}});
    program->param(osc, 1) = 440.0f / static_cast<float>(info.sampleRate);
    program->param(gain, 0) = 0.1f;
    program->addPhase(osc, 0, 1);
    program->warm();
    program->resetState();
    wavy::AudioEngine engine(std::move(program));

    const uint32_t channels = info.outputChannels;
    std::atomic<unsigned> callbacks{0};
    std::atomic<float> peak{0.0f};
    device->start([&](float* const* out, uint32_t frames) {
        engine.render(out, frames);  // channel 0
        for (uint32_t c = 1; c < channels; ++c) std::copy(out[0], out[0] + frames, out[c]);
        float p = 0.0f;
        for (uint32_t i = 0; i < frames; ++i) p = (std::max)(p,std::abs(out[0][i]));
        if (p > peak.load()) peak = p;
        ++callbacks;
    });
    // Some drivers (Realtek's) are COM objects that expect the host thread to keep pumping messages.
    const auto end =std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    device->stop();
    device->close();
    std::printf("callbacks: %u, peak: %f\n", callbacks.load(), peak.load());
}
