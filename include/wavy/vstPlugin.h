#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "jitnodes/audio/eventBuffer.h"

namespace wavy {

struct VstClassInfo {
    std::string name;
    std::string vendor;
    std::string version;
    std::string subCategories;  // e.g. "Instrument|Synth", "Fx|Reverb"
    std::array<uint8_t, 16> classId{};

    bool isInstrument() const { return subCategories.find("Instrument") != std::string::npos; }
};

struct VstBusInfo {
    std::string name;
    int channels = 0;
    bool main = true;
    bool defaultActive = true;
};

struct VstParamInfo {
    uint32_t id = 0;
    std::string title;
    std::string units;
    int stepCount = 0;  // 0 = continuous
    double defaultNormalized = 0.0;
    bool readOnly = false;
    bool hidden = false;
    bool automatable = false;
};

// One loaded .vst3 binary: its factory and the audio classes in it. Instances keep the module alive.
class VstModule {
public:
    // Null with `error` set when the binary cannot be loaded.
    static std::shared_ptr<VstModule> load(const std::string& path, std::string& error);
    ~VstModule();
    VstModule(const VstModule&) = delete;
    VstModule& operator=(const VstModule&) = delete;

    const std::string& path() const { return path_; }
    const std::string& vendor() const { return vendor_; }
    const std::vector<VstClassInfo>& classes() const { return classes_; }

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    VstModule();
    std::string path_;
    std::string vendor_;
    std::vector<VstClassInfo> classes_;
    std::unique_ptr<Impl> impl_;
};

// One plugin instance (component + edit controller).
//
// Main thread: create(), prepare(), setParameter(), getParameter(), saveState(), loadState(), destruction. Audio thread: process() and
// reset() only, and only between prepare() and the next prepare()/destruction. process() allocates only when a block carries more
// distinct parameters than a warmed-up queue holds, and never locks. A plugin that faults inside process() is marked failed() and
// outputs silence from then on.
class VstPlugin {
public:
    // Null with `error` set on failure. The instance is initialised and connected but not yet processing.
    static std::unique_ptr<VstPlugin> create(std::shared_ptr<VstModule> module, size_t classIndex, std::string& error);
    ~VstPlugin();
    VstPlugin(const VstPlugin&) = delete;
    VstPlugin& operator=(const VstPlugin&) = delete;

    const VstClassInfo& info() const;
    const std::vector<VstBusInfo>& inputBuses() const;
    const std::vector<VstBusInfo>& outputBuses() const;
    std::vector<VstParamInfo> parameters() const;

    // Activates the main stereo input (when the plugin has one) and main stereo output, deactivates every other bus, and starts
    // processing. Float32 only. May be called again with other settings. Returns false with `error` set.
    bool prepare(double sampleRate, int maxBlock, std::string& error);
    bool prepared() const;
    int inputChannels() const;   // main input bus after prepare(); 0 for an instrument
    int outputChannels() const;  // main output bus after prepare()

    int latencySamples() const;
    uint32_t tailSamples() const;  // 0xFFFFFFFF = infinite

    // Sets the controller's value now and queues it for the processor's next block. `normalized` is 0..1.
    bool setParameter(uint32_t id, double normalized);
    double getParameter(uint32_t id) const;
    std::string parameterText(uint32_t id, double normalized) const;

    // Transport for the plugin's process context (tempo sync, arpeggiators); applied from the next block.
    void setTransport(double bpm, bool playing, int64_t samplePosition);

    // Processes `frames` (<= maxBlock) samples. `in` has inputChannels() pointers (null for an instrument), `out` outputChannels() ones;
    // buffers are not aliased. Events (note on/off, poly pressure, CC, pitch bend, channel pressure) are delivered at their offsets.
    void process(const float* const* in, float* const* out, int frames, const jitnodes::EventBuffer* events) noexcept;
    bool failed() const { return failed_.load(std::memory_order_acquire); }

    // Health counters since prepare() (or resetStats()); readable from any thread.
    struct Stats {
        uint64_t blocks = 0;
        uint64_t slowBlocks = 0;       // process() took longer than the block's duration at the sample rate
        uint64_t nonFiniteBlocks = 0;  // blocks where the plugin wrote NaN/Inf; those samples are replaced by 0
        uint32_t maxMicros = 0;        // slowest process() call
    };
    Stats stats() const;
    void resetStats();

    // Audio thread: releases every held note at the start of the next block (voices then ring out their release). VST3 has no
    // all-notes-off event, so the host tracks note on/off itself.
    void reset() noexcept;
    // Main thread, not while process() runs: deactivates and reactivates the plugin, which cuts voices and clears delay lines and
    // reverb tails at once. Audio resumes with the next process() call.
    bool hardReset();

    // Component + controller state as one blob. Not while process() is running.
    std::vector<uint8_t> saveState();
    bool loadState(const std::vector<uint8_t>& blob);

    // Flags from IComponentHandler::restartComponent since the last call (main thread). Act on them there, never in process().
    int32_t takeRestartFlags();

    struct Impl;

private:
    VstPlugin();
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> failed_{false};
};

}  // namespace wavy
