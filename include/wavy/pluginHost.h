#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "jitnodes/audio/audioProgram.h"
#include "jitnodes/audio/externalProcessor.h"
#include "wavy/vstPlugin.h"

namespace wavy {

// What a VstProcessor does when its plugin keeps missing the block deadline: after `slowLimit` slow blocks in a row the plugin is
// skipped (silence for an instrument, the input passed through for an effect) for `skipBlocks` blocks, then tried again.
struct SlowPolicy {
    uint32_t slowLimit = 3;
    uint32_t skipBlocks = 400;
};

// The ExternalProcessor a `vst` node drives: forwards blocks to a VstPlugin, tracks its health and applies the slow-plugin policy.
// Audio thread: process(), reset(). Everything else main thread.
class VstProcessor : public jitnodes::ExternalProcessor {
public:
    explicit VstProcessor(std::unique_ptr<VstPlugin> plugin, SlowPolicy policy = {});

    void process(const float* const* in, float* const* out, size_t frames, const jitnodes::EventBuffer& events) noexcept override;
    size_t latencySamples() const override;
    size_t tailSamples() const override;
    void reset() noexcept override;

    VstPlugin& plugin() { return *plugin_; }
    bool isInstrument() const { return plugin_->inputChannels() == 0; }
    // Blocks played as a bypass because the plugin was too slow, or because it had failed.
    uint64_t skippedBlocks() const { return skipped_.load(std::memory_order_relaxed); }
    uint64_t skipEpisodes() const { return episodes_.load(std::memory_order_relaxed); }

private:
    std::unique_ptr<VstPlugin> plugin_;
    SlowPolicy policy_;
    uint64_t slowSeen_ = 0;
    uint32_t slowRun_ = 0;
    uint32_t skipLeft_ = 0;
    std::atomic<uint64_t> skipped_{0};
    std::atomic<uint64_t> episodes_{0};
};

// Owns the plugin instances of a session and publishes each one in the jitnodes ProcessorBank under its name, so `vst` nodes can find it.
// A program that names a processor keeps it alive, so removing one never frees an instance a running program still calls. Main thread.
class PluginHost {
public:
    PluginHost(double sampleRate, int blockSize) : sampleRate_(sampleRate), blockSize_(blockSize) {}
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Loads the .vst3 at `path` (class `classIndex`), prepares it for this host's rate and block size and registers it as `name`.
    // Null with `error` set on failure. A second add with the same name replaces the first.
    std::shared_ptr<VstProcessor> add(const std::string& name, const std::string& path, size_t classIndex, std::string& error,
                                      SlowPolicy policy = {});
    std::shared_ptr<VstProcessor> find(const std::string& name) const;
    void remove(const std::string& name);

    double sampleRate() const { return sampleRate_; }
    int blockSize() const { return blockSize_; }

    // Installed location of a plugin below the machine's `Common Files/VST3` folder, or empty when it is not there.
    static std::string commonVst3Path(const std::string& relative);

private:
    double sampleRate_;
    int blockSize_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<VstProcessor>> processors_;
    std::map<std::string, std::shared_ptr<VstModule>> modules_;
};

// hostInput (the summed clips of the track, stereo) -> vst(`processor`) -> gain: the long-lived program of a track with a plugin.
// An instrument ignores the input. Outputs x and r. Throws std::runtime_error when the processor is not registered.
std::unique_ptr<jitnodes::AudioProgram> makePluginTrackProgram(const std::string& processor, size_t blockSize, jitnodes::NodeId* gain = nullptr);

}  // namespace wavy
