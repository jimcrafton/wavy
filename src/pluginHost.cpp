#include "wavy/pluginHost.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "jitnodes/jitnodes.h"

namespace wavy {

VstProcessor::VstProcessor(std::unique_ptr<VstPlugin> plugin, SlowPolicy policy) : plugin_(std::move(plugin)), policy_(policy) {
    slowSeen_ = plugin_->stats().slowBlocks;
    knownLatency_ = plugin_->latencySamples();
}

bool VstProcessor::pollRestart() {
    const int32_t flags = plugin_->takeRestartFlags();
    if (flags & (VstPlugin::reloadComponent | VstPlugin::ioChanged)) ++unhandledRestarts_;
    if (!(flags & VstPlugin::latencyChanged)) return false;
    const int latency = plugin_->latencySamples();
    const bool changed = latency != knownLatency_;
    knownLatency_ = latency;
    return changed;
}

bool PluginHost::pollRestarts() {
    std::vector<std::shared_ptr<VstProcessor>> all;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : processors_) {
            all.push_back(entry.second);
        }
    }
    bool latencyChanged = false;
    for (auto& processor : all) {
        latencyChanged = processor->pollRestart() || latencyChanged;
    }
    return latencyChanged;
}

void VstProcessor::process(const float* const* in, float* const* out, size_t frames, const jitnodes::EventBuffer& events) noexcept {
    const bool bypass = skipLeft_ > 0 || plugin_->failed();
    if (bypass) {
        if (skipLeft_ > 0) --skipLeft_;
        skipped_.fetch_add(1, std::memory_order_relaxed);
        for (size_t c = 0; c < 2; ++c) {
            if (in && !isInstrument()) std::memcpy(out[c], in[c], frames * sizeof(float));
            else std::fill(out[c], out[c] + frames, 0.0f);
        }
        return;
    }

    if (in && !isInstrument() && tailBypass_.load(std::memory_order_relaxed)) {
        bool silent = true;
        for (size_t c = 0; c < 2 && silent; ++c) {
            for (size_t i = 0; i < frames; ++i) {
                if (std::fabs(in[c][i]) > 1e-6f) {
                    silent = false;
                    break;
                }
            }
        }
        silentFrames_ = silent ? silentFrames_ + frames : 0;
        const uint32_t tail = plugin_->tailSamples();
        // A reported tail is only a claim (several reverbs say 0), so the plugin must also have stopped sounding by itself.
        if (silent && tail != 0xFFFFFFFFu && outSilentFrames_ > outputHold_ &&
            silentFrames_ > static_cast<uint64_t>(tail) + static_cast<uint64_t>(std::max(plugin_->latencySamples(), 0))) {
            tailSkipped_.fetch_add(1, std::memory_order_relaxed);
            for (size_t c = 0; c < 2; ++c) {
                std::fill(out[c], out[c] + frames, 0.0f);
            }
            return;
        }
    }

    plugin_->process(isInstrument() ? nullptr : in, out, static_cast<int>(frames), &events);

    if (in && !isInstrument()) {
        bool quiet = true;
        for (size_t c = 0; c < 2 && quiet; ++c) {
            for (size_t i = 0; i < frames; ++i) {
                if (std::fabs(out[c][i]) > 1e-6f) {
                    quiet = false;
                    break;
                }
            }
        }
        outSilentFrames_ = quiet ? outSilentFrames_ + frames : 0;
    }

    const uint64_t slow = plugin_->stats().slowBlocks;
    if (slow != slowSeen_) {
        slowSeen_ = slow;
        if (++slowRun_ >= policy_.slowLimit) {
            slowRun_ = 0;
            skipLeft_ = policy_.skipBlocks;
            episodes_.fetch_add(1, std::memory_order_relaxed);
            // Drop what the plugin was playing so that it does not wake up with hanging notes.
            plugin_->reset();
        }
    } else {
        slowRun_ = 0;
    }
}

size_t VstProcessor::latencySamples() const { return static_cast<size_t>(std::max(plugin_->latencySamples(), 0)); }

size_t VstProcessor::tailSamples() const { return plugin_->tailSamples(); }

void VstProcessor::reset() noexcept { plugin_->reset(); }

PluginHost::~PluginHost() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : processors_) {
        jitnodes::ProcessorBank::instance().set(entry.first, nullptr);
    }
}

std::string PluginHost::commonVst3Path(const std::string& relative) {
    char* common = nullptr;
    size_t length = 0;
    _dupenv_s(&common, &length, "CommonProgramFiles");
    const std::filesystem::path p = std::filesystem::path(common ? common : "C:/Program Files/Common Files") / "VST3" / relative;
    std::free(common);
    return std::filesystem::exists(p) ? p.string() : std::string();
}

std::shared_ptr<VstProcessor> PluginHost::add(const std::string& name, const std::string& path, size_t classIndex, std::string& error,
                                              SlowPolicy policy) {
    std::shared_ptr<VstModule> module;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = modules_.find(path);
        if (it != modules_.end()) module = it->second;
    }
    if (!module) {
        module = VstModule::load(path, error);
        if (!module) return nullptr;
    }
    auto plugin = VstPlugin::create(module, classIndex, error);
    if (!plugin) return nullptr;
    if (!plugin->prepare(sampleRate_, blockSize_, error)) return nullptr;
    auto processor = std::make_shared<VstProcessor>(std::move(plugin), policy);

    std::lock_guard<std::mutex> lock(mutex_);
    modules_[path] = module;
    processors_[name] = processor;
    jitnodes::ProcessorBank::instance().set(name, processor);
    return processor;
}

std::shared_ptr<VstProcessor> PluginHost::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = processors_.find(name);
    return it == processors_.end() ? nullptr : it->second;
}

void PluginHost::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (processors_.erase(name) > 0) jitnodes::ProcessorBank::instance().set(name, nullptr);
}

std::unique_ptr<jitnodes::AudioProgram> makePluginTrackProgram(const std::string& processor, size_t blockSize, jitnodes::NodeId* gainNode) {
    return makePluginTrackProgram(std::vector<std::string>{processor}, blockSize, gainNode);
}

std::unique_ptr<jitnodes::AudioProgram> makePluginTrackProgram(const std::vector<std::string>& chain, size_t blockSize,
                                                               jitnodes::NodeId* gainNode) {
    using namespace jitnodes;
    if (chain.empty()) throw std::invalid_argument("makePluginTrackProgram: empty chain");
    Network net;
    const NodeId in = net.addNode("hostInput");
    net.node(in).setFloat("count", static_cast<float>(blockSize));
    net.node(in).setFloat("first", 0.0f);
    net.node(in).setFloat("channels", 2.0f);
    NodeId previous = in;
    for (const std::string& processor : chain) {
        const NodeId vst = net.addNode("vst");
        net.node(vst).setString("plugin", processor);
        net.connect(previous, vst);
        previous = vst;
    }
    const NodeId gain = net.addNode("wrangle");
    net.node(gain).setString("code", "x = x * a; r = r * a;");
    net.connect(previous, gain);
    auto program = AudioProgram::compile(net, gain, {blockSize, {"x", "r"}});
    program->param(gain, 0) = 1.0f;
    if (gainNode) *gainNode = gain;
    return program;
}

}  // namespace wavy
