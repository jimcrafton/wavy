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
#include "wavy/clipPlayer.h"
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
class VstProcessor : public jitnodes::ExternalProcessor, public TransportListener {
public:
    explicit VstProcessor(std::unique_ptr<VstPlugin> plugin, SlowPolicy policy = {});

    void setTransport(double bpm, bool playing, int64_t frame) noexcept override { plugin_->setTransport(bpm, playing, frame); }

    void process(const float* const* in, float* const* out, size_t frames, const jitnodes::EventBuffer& events) noexcept override;
    size_t latencySamples() const override;
    size_t tailSamples() const override;
    void reset() noexcept override;

    // Effects only: once the input has been silent for longer than the plugin's tail (plus its latency) the plugin is not called and
    // the output is silence, until the input is audible again. A plugin that reports an infinite tail is always called. On by default.
    void setTailBypass(bool on) { tailBypass_.store(on, std::memory_order_relaxed); }
    // The bypass also needs the plugin's own output to have been silent (<= 1e-6) for this many frames, because many reverbs report a
    // tail of 0 while still ringing. Default 10 s at 48 kHz: ValhallaSupermassive stays silent for its delay time (up to seconds) before the tail starts. Main thread, before streaming.
    void setOutputHold(uint64_t frames) { outputHold_ = frames; }
    uint64_t tailSkippedBlocks() const { return tailSkipped_.load(std::memory_order_relaxed); }

    // Main thread. Handles what the plugin asked for through restartComponent since the last call: a latency change is re-queried (the
    // flag is only a hint, so an unchanged value does nothing) and returned as true so the caller can recompute delay compensation.
    // Flags that need the plugin re-prepared (io/reload) are counted in unhandledRestarts() and left to the caller.
    bool pollRestart();
    uint32_t unhandledRestarts() const { return unhandledRestarts_; }

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
    std::atomic<bool> tailBypass_{true};
    std::atomic<uint64_t> tailSkipped_{0};
    uint64_t silentFrames_ = 0;  // audio thread: consecutive frames of silent input
    uint64_t outSilentFrames_ = 0;  // audio thread: consecutive frames of silent output
    uint64_t outputHold_ = 480000;
    int knownLatency_ = 0;       // main thread
    uint32_t unhandledRestarts_ = 0;
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
    // Main thread: pollRestart() on every plugin. True when any plugin's latency changed.
    bool pollRestarts();

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
// The same with several processors in series: hostInput -> chain[0] -> chain[1] ... -> gain. Typically an instrument followed by effects;
// an instrument later in the chain would ignore what comes before it. Throws std::invalid_argument for an empty chain.
std::unique_ptr<jitnodes::AudioProgram> makePluginTrackProgram(const std::vector<std::string>& chain, size_t blockSize,
                                                               jitnodes::NodeId* gain = nullptr);

}  // namespace wavy
