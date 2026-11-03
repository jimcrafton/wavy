#include "session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "jitnodes/jitnodes.h"
#include "wavy/wavLoader.h"

namespace wavy::editor {

namespace {

constexpr double twoPi = 6.283185307179586;

void addPeaks(std::vector<float>& peaks, const float* samples, size_t count) {
    for (size_t at = 0; at < count; at += Session::peakFrames) {
        const size_t end = std::min<size_t>(count, at + Session::peakFrames);
        const auto [lo, hi] = std::minmax_element(samples + at, samples + end);
        peaks.push_back(*lo);
        peaks.push_back(*hi);
    }
}

std::string oscCode(double harmonics) {
    (void)harmonics;
    return "x = sin(6.28318530718 * (a + sampleIndex * b)) + 0.5 * sin(12.56637061436 * (a + sampleIndex * b)) + 0.3 * sin(18.84955592154 * (a + sampleIndex * b));";
}

}  // namespace

void lowpassCoefficients(double sampleRate, float cutoffHz, float q, float out[5]) {
    const double w0 = twoPi * cutoffHz / sampleRate, alpha = std::sin(w0) / (2.0 * q), c = std::cos(w0), a0 = 1.0 + alpha;
    out[0] = static_cast<float>((1.0 - c) / 2.0 / a0);
    out[1] = static_cast<float>((1.0 - c) / a0);
    out[2] = out[0];
    out[3] = static_cast<float>(-2.0 * c / a0);
    out[4] = static_cast<float>((1.0 - alpha) / a0);
}

std::unique_ptr<jitnodes::AudioProgram> makeDroneProgram(double sampleRate, size_t blockSize, GraphInfo* info, const DroneSettings& settings,
                                                         DroneNodes* nodes) {
    using namespace jitnodes;
    Network net;
    NodeId osc[2];
    const double hz[2] = {110.0, 110.0 * std::pow(2.0, settings.detuneCents / 1200.0)};
    NodeId mix = net.addNode("mix");
    for (int i = 0; i < 2; ++i) {
        const NodeId line = net.addNode("line");
        net.node(line).setFloat("count", static_cast<float>(blockSize));
        osc[i] = net.addNode("wrangle");
        net.node(osc[i]).setString("code", oscCode(3));
        net.connect(line, osc[i]);
        net.connect(osc[i], mix, i);
    }
    float coeffs[5];
    lowpassCoefficients(sampleRate, settings.cutoffHz, settings.q, coeffs);
    const NodeId filter = net.addNode("biquad");
    const char* coeffNames[5] = {"b0", "b1", "b2", "a1", "a2"};
    for (int k = 0; k < 5; ++k) net.node(filter).setFloat(coeffNames[k], coeffs[k]);
    net.connect(mix, filter);
    const NodeId gain = net.addNode("wrangle");
    net.node(gain).setString("code", "x = x * a;");
    net.connect(filter, gain);

    auto program = AudioProgram::compile(net, gain, {blockSize, {"x"}});
    for (int i = 0; i < 2; ++i) {
        program->param(osc[i], 1) = static_cast<float>(hz[i] / sampleRate);
        program->addPhase(osc[i], 0, 1);
    }
    program->param(gain, 0) = std::pow(10.0f, settings.levelDb / 20.0f);

    if (nodes) *nodes = {osc[0], osc[1], filter, gain};
    if (info) {
        char b[48];
        info->nodes.clear();
        info->nodes.push_back({"Saw Osc A", "110 Hz", 0, 0, true, false});
        std::snprintf(b, sizeof(b), "%.1f Hz", hz[1]);
        info->nodes.push_back({"Saw Osc B", b, 0, 1, true, false});
        info->nodes.push_back({"Mix", "2 inputs", 1, 0.5f, false, false});
        std::snprintf(b, sizeof(b), "%.0f Hz  Q %.1f", settings.cutoffHz, settings.q);
        info->nodes.push_back({"Biquad LP", b, 2, 0.5f, false, false});
        std::snprintf(b, sizeof(b), "%.1f dB", settings.levelDb);
        info->nodes.push_back({"Gain", b, 3, 0.5f, false, false});
        info->wires = {{0, 2}, {1, 2}, {2, 3}, {3, 4}};
    }
    return program;
}

std::unique_ptr<jitnodes::AudioProgram> makeInstrumentProgram(double sampleRate, size_t blockSize) {
    using namespace jitnodes;
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(blockSize));
    const NodeId silence = net.addNode("wrangle");
    net.node(silence).setString("code", "x = 0;");
    net.connect(line, silence);
    const NodeId voices = net.addNode("voices");
    net.node(voices).setFloat("attack", 240.0f);
    net.node(voices).setFloat("decay", 4800.0f);
    net.node(voices).setFloat("sustain", 0.5f);
    net.node(voices).setFloat("release", 9600.0f);
    net.node(voices).setFloat("waveform", 1.0f);
    net.node(voices).setFloat("sampleRate", static_cast<float>(sampleRate));
    net.connect(silence, voices);
    const NodeId gain = net.addNode("wrangle");
    net.node(gain).setString("code", "x = x * a;");
    net.connect(voices, gain);
    auto program = AudioProgram::compile(net, gain, {blockSize, {"x"}});
    program->param(gain, 0) = 0.2f;
    return program;
}

std::vector<wavy::ClipEvent> makeArpEvents(uint64_t framesPerBeat, int bars, std::vector<NoteBox>* noteBoxes) {
    static const uint8_t pattern[] = {60, 63, 67, 72, 67, 63, 60, 55};
    std::vector<wavy::ClipEvent> events;
    const uint64_t eighth = framesPerBeat / 2;
    const int steps = bars * 8;
    for (int i = 0; i < steps; ++i) {
        const uint64_t on = static_cast<uint64_t>(i) * eighth;
        const uint64_t length = eighth * 9 / 10;
        const uint8_t pitch = pattern[i % 8], velocity = (i % 4 == 0) ? 110 : 85;
        events.push_back({on, 0x90, pitch, velocity});
        events.push_back({on + length, 0x80, pitch, 0});
        if (noteBoxes) noteBoxes->push_back({on, length, pitch, velocity});
    }
    return events;
}

Session::Session(double sampleRate)
    : sampleRate_(sampleRate),
#ifdef WAVY_HAS_VST3
      plugins_(sampleRate, static_cast<int>(blockSize)),
#endif
      player_(blockSize, 2) {
    tracks_ = {{"Drums", 0xd98a5f, 0x4a3022}, {"Bass", 0x6c8cf0, 0x27335e}, {"Pad", 0x5fc7a0, 0x1f4a3a}, {"Arp", 0xe05fa8, 0x5a2442}};
#ifdef WAVY_HAS_VST3
    instruments_.resize(tracks_.size());
    chains_.resize(tracks_.size());
#endif
    player_.setTempo(bpm_);
    player_.enableMasterLimiter(sampleRate_);
}

#ifdef WAVY_HAS_VST3
namespace {

// Whether class `classIndex` of the module at `path` is an instrument, read from the factory before anything is changed.
bool classIsInstrument(const std::string& path, size_t classIndex, bool& instrument, std::string& error) {
    auto module = wavy::VstModule::load(path, error);
    if (!module) return false;
    if (classIndex >= module->classes().size()) {
        error = "no such plugin class";
        return false;
    }
    instrument = module->classes()[classIndex].isInstrument();
    return true;
}

}  // namespace
#endif

bool Session::setTrackInstrument(int track, const std::string& path, std::string& error, size_t classIndex) {
#ifdef WAVY_HAS_VST3
    if (track < 0 || static_cast<size_t>(track) >= tracks_.size()) {
        error = "no such track";
        return false;
    }
    bool instrument = false;
    if (!classIsInstrument(path, classIndex, instrument, error)) return false;
    if (!instrument) {
        error = "the plugin is not an instrument";
        return false;
    }
    const std::string name = "track" + std::to_string(track);
    auto& chain = chains_[static_cast<size_t>(track)];
    const bool replacing = !instruments_[static_cast<size_t>(track)].empty();
    // The old instrument's window is closed first: it must not outlive the plugin it shows.
    editors_.erase(name);
    auto processor = plugins_.add(name, path, classIndex, error);
    if (!processor) return false;
    if (!replacing) chain.insert(chain.begin(), name);
    if (!rebuildTrack(track, error)) {
        if (!replacing) {
            chain.erase(chain.begin());
            plugins_.remove(name);
        }
        return false;
    }
    instruments_[static_cast<size_t>(track)] = std::filesystem::path(path).stem().string();
    return true;
#else
    (void)track;
    (void)path;
    (void)classIndex;
    error = "built without VST3 hosting";
    return false;
#endif
}

bool Session::addTrackEffect(int track, const std::string& path, std::string& error, size_t classIndex) {
#ifdef WAVY_HAS_VST3
    if (track < 0 || static_cast<size_t>(track) >= tracks_.size()) {
        error = "no such track";
        return false;
    }
    bool instrument = false;
    if (!classIsInstrument(path, classIndex, instrument, error)) return false;
    if (instrument) {
        error = "the plugin is an instrument, not an effect";
        return false;
    }
    auto& chain = chains_[static_cast<size_t>(track)];
    const std::string name = "track" + std::to_string(track) + "fx" + std::to_string(effectCounter_++);
    auto processor = plugins_.add(name, path, classIndex, error);
    if (!processor) return false;
    chain.push_back(name);
    if (!rebuildTrack(track, error)) {
        chain.pop_back();
        plugins_.remove(name);
        return false;
    }
    effectStems_[name] = std::filesystem::path(path).stem().string();
    return true;
#else
    (void)track;
    (void)path;
    (void)classIndex;
    error = "built without VST3 hosting";
    return false;
#endif
}

int Session::effectCount(int track) const {
#ifdef WAVY_HAS_VST3
    if (track < 0 || static_cast<size_t>(track) >= chains_.size()) return 0;
    const auto& chain = chains_[static_cast<size_t>(track)];
    return static_cast<int>(chain.size()) - (instruments_[static_cast<size_t>(track)].empty() ? 0 : 1);
#else
    (void)track;
    return 0;
#endif
}

#ifdef WAVY_HAS_VST3
bool Session::rebuildTrack(int track, std::string& error) {
    const auto& chain = chains_[static_cast<size_t>(track)];
    std::vector<std::shared_ptr<jitnodes::ExternalProcessor>> processors;
    for (const std::string& name : chain) {
        processors.push_back(plugins_.find(name));
    }
    try {
        player_.setTrackProgram(track, wavy::makePluginTrackProgram(chain, blockSize), std::move(processors));
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}
#endif

void Session::pollPlugins() {
#ifdef WAVY_HAS_VST3
    if (plugins_.pollRestarts()) player_.updateLatencyCompensation();
#endif
}

std::string Session::instrumentName(int track) const {
#ifdef WAVY_HAS_VST3
    if (track >= 0 && static_cast<size_t>(track) < instruments_.size()) return instruments_[static_cast<size_t>(track)];
#else
    (void)track;
#endif
    return {};
}

bool Session::openInstrumentEditor(int track, std::string& error) {
#ifdef WAVY_HAS_VST3
    if (track < 0 || static_cast<size_t>(track) >= tracks_.size()) {
        error = "no such track";
        return false;
    }
    return openEditorFor("track" + std::to_string(track), error);
#else
    (void)track;
    error = "built without VST3 hosting";
    return false;
#endif
}

bool Session::openEffectEditor(int track, int index, std::string& error) {
#ifdef WAVY_HAS_VST3
    if (track < 0 || static_cast<size_t>(track) >= tracks_.size() || index < 0 || index >= effectCount(track)) {
        error = "no such effect";
        return false;
    }
    const auto& chain = chains_[static_cast<size_t>(track)];
    const size_t first = instruments_[static_cast<size_t>(track)].empty() ? 0 : 1;
    return openEditorFor(chain[first + static_cast<size_t>(index)], error);
#else
    (void)track;
    (void)index;
    error = "built without VST3 hosting";
    return false;
#endif
}

#ifdef WAVY_HAS_VST3
bool Session::openEditorFor(const std::string& processorName, std::string& error) {
    auto processor = plugins_.find(processorName);
    if (!processor) {
        error = "this track has no plugin";
        return false;
    }
    auto& slot = editors_[processorName];
    if (slot && slot->isOpen()) {
        slot->bringToFront();
        return true;
    }
    slot = wavy::PluginEditorWindow::open(processor->plugin(), error, editorOwner_);
    return slot != nullptr;
}
#endif

namespace {

constexpr uint32_t stateMagic = 0x54535657;  // "WVST"
constexpr uint32_t stateVersion = 2;

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

bool get32(const std::vector<uint8_t>& in, size_t& pos, uint32_t& v) {
    if (pos + 4 > in.size()) return false;
    v = 0;
    for (size_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(in[pos + i]) << (8 * i);
    pos += 4;
    return true;
}

}  // namespace

bool Session::saveInstrumentStates(const std::string& path, std::string& error) {
#ifdef WAVY_HAS_VST3
    std::vector<uint8_t> file;
    put32(file, stateMagic);
    put32(file, stateVersion);
    size_t countAt = file.size();
    put32(file, 0);
    uint32_t count = 0;
    auto putString = [&file](const std::string& s) {
        put32(file, static_cast<uint32_t>(s.size()));
        file.insert(file.end(), s.begin(), s.end());
    };
    for (size_t t = 0; t < tracks_.size(); ++t) {
        for (const std::string& processorName : chains_[t]) {
            auto processor = plugins_.find(processorName);
            if (!processor) {
                continue;
            }
            const std::vector<uint8_t> blob = processor->plugin().saveState();
            if (blob.empty()) {
                continue;
            }
            const auto effect = effectStems_.find(processorName);
            putString(processorName);
            putString(effect != effectStems_.end() ? effect->second : instruments_[t]);
            put32(file, static_cast<uint32_t>(blob.size()));
            file.insert(file.end(), blob.begin(), blob.end());
            ++count;
        }
    }
    for (size_t i = 0; i < 4; ++i) file[countAt + i] = static_cast<uint8_t>(count >> (8 * i));

    std::error_code ec;
    const std::filesystem::path target(path);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!out) {
        error = "short write to " + path;
        return false;
    }
    return true;
#else
    (void)path;
    error = "built without VST3 hosting";
    return false;
#endif
}

int Session::loadInstrumentStates(const std::string& path, std::string& error) {
#ifdef WAVY_HAS_VST3
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return 0;
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    size_t pos = 0;
    uint32_t magic = 0, version = 0, count = 0;
    if (!get32(file, pos, magic) || magic != stateMagic || !get32(file, pos, version) || version < 1 || version > stateVersion ||
        !get32(file, pos, count)) {
        error = path + " is not a wavy state file";
        return 0;
    }
    auto getString = [&](std::string& s) {
        uint32_t size = 0;
        if (!get32(file, pos, size) || pos + size > file.size()) {
            return false;
        }
        s.assign(file.begin() + static_cast<std::ptrdiff_t>(pos), file.begin() + static_cast<std::ptrdiff_t>(pos + size));
        pos += size;
        return true;
    };
    int restored = 0;
    for (uint32_t i = 0; i < count; ++i) {
        // Version 1 keyed an entry by track number (instruments only); version 2 by processor name, so effects fit too.
        std::string processorName, stem;
        if (version == 1) {
            uint32_t track = 0;
            if (!get32(file, pos, track)) {
                break;
            }
            processorName = "track" + std::to_string(track);
        } else if (!getString(processorName)) {
            break;
        }
        uint32_t blobSize = 0;
        if (!getString(stem) || !get32(file, pos, blobSize) || pos + blobSize > file.size()) {
            break;
        }
        const std::vector<uint8_t> blob(file.begin() + static_cast<std::ptrdiff_t>(pos), file.begin() + static_cast<std::ptrdiff_t>(pos + blobSize));
        pos += blobSize;

        std::string loaded;
        for (size_t t = 0; t < tracks_.size() && loaded.empty(); ++t) {
            for (const std::string& name : chains_[t]) {
                if (name == processorName) {
                    const auto effect = effectStems_.find(name);
                    loaded = effect != effectStems_.end() ? effect->second : instruments_[t];
                }
            }
        }
        if (loaded != stem) {
            continue;
        }
        auto processor = plugins_.find(processorName);
        if (processor && processor->plugin().loadState(blob)) {
            ++restored;
        }
    }
    return restored;
#else
    (void)path;
    error = "built without VST3 hosting";
    return 0;
#endif
}

int Session::finish(ClipInfo info, std::unique_ptr<jitnodes::AudioProgram> program, float gain, std::vector<wavy::ClipEvent> events,
                    std::string& error) {
    try {
        const int added = program ? player_.addClip(info.start, info.length, std::move(program), gain, std::move(events), info.track)
                                  : player_.addEventClip(info.track, info.start, info.length, std::move(events));
        if (added < 0) {
            error = "no room for another clip";
            return -1;
        }
    } catch (const std::exception& e) {
        error = e.what();
        return -1;
    }
    clips_.push_back(std::move(info));
    return static_cast<int>(clips_.size()) - 1;
}

int Session::addSampleClip(int track, uint64_t startFrame, const std::string& path, std::string& error) {
    try {
        auto buffer = wavy::loadWav(path, sampleRate_);
        if (buffer->frames.empty()) {
            error = "the file has no audio";
            return -1;
        }
        const std::string key = "sessionSample" + std::to_string(sampleCount_++);
        jitnodes::SampleBank::instance().set(key, buffer);
        ClipInfo info;
        info.track = track;
        info.kind = ClipKind::sample;
        info.name = std::filesystem::path(path).stem().string();
        info.start = startFrame;
        info.length = buffer->frames.size();
        addPeaks(info.peaks, buffer->frames.data(), buffer->frames.size());
        return finish(std::move(info), wavy::makeSampleProgram(key, blockSize), 1.0f, {}, error);
    } catch (const std::exception& e) {
        error = e.what();
        return -1;
    }
}

int Session::addGraphClip(int track, uint64_t startFrame, int bars, std::string& error) {
    try {
        ClipInfo info;
        info.track = track;
        info.kind = ClipKind::graph;
        info.name = "Drone";
        info.start = startFrame;
        info.length = static_cast<uint64_t>(bars) * framesPerBar();
        auto program = makeDroneProgram(sampleRate_, blockSize, &info.graph, info.drone, &info.droneNodes);
        renderPreview(info);
        return finish(std::move(info), std::move(program), 1.0f, {}, error);
    } catch (const std::exception& e) {
        error = e.what();
        return -1;
    }
}

void Session::renderPreview(ClipInfo& info) const {
    // The same graph rendered offline into the peaks the timeline draws.
    auto preview = makeDroneProgram(sampleRate_, blockSize, nullptr, info.drone);
    preview->warm();
    preview->resetState();
    std::vector<float> rendered(info.length / blockSize * blockSize);
    for (size_t at = 0; at < rendered.size(); at += blockSize) {
        float* out[] = {rendered.data() + at};
        preview->process(out);
    }
    info.peaks.clear();
    addPeaks(info.peaks, rendered.data(), rendered.size());
    info.previewDirty = false;
}

bool Session::setDrone(int clip, const DroneSettings& settings) {
    if (clip < 0 || static_cast<size_t>(clip) >= clips_.size() || clips_[static_cast<size_t>(clip)].kind != ClipKind::graph) return false;
    ClipInfo& info = clips_[static_cast<size_t>(clip)];
    info.drone = settings;
    const DroneNodes& n = info.droneNodes;
    float coeffs[5];
    lowpassCoefficients(sampleRate_, settings.cutoffHz, settings.q, coeffs);
    for (size_t k = 0; k < 5; ++k) player_.setParam(clip, n.filter, k, coeffs[k]);
    player_.setParam(clip, n.oscB, 1, static_cast<float>(110.0 * std::pow(2.0, settings.detuneCents / 1200.0) / sampleRate_));
    player_.setParam(clip, n.gain, 0, std::pow(10.0f, settings.levelDb / 20.0f));

    // Keep what the Patch view shows in step with the knobs.
    char b[48];
    std::snprintf(b, sizeof(b), "%.1f Hz", 110.0 * std::pow(2.0, settings.detuneCents / 1200.0));
    info.graph.nodes[1].detail = b;
    std::snprintf(b, sizeof(b), "%.0f Hz  Q %.1f", settings.cutoffHz, settings.q);
    info.graph.nodes[3].detail = b;
    std::snprintf(b, sizeof(b), "%.1f dB", settings.levelDb);
    info.graph.nodes[4].detail = b;
    info.previewDirty = true;
    return true;
}

void Session::refreshPreview(int clip) {
    if (clip < 0 || static_cast<size_t>(clip) >= clips_.size()) return;
    ClipInfo& info = clips_[static_cast<size_t>(clip)];
    if (info.kind == ClipKind::graph && info.previewDirty) renderPreview(info);
}

int Session::addNotesClip(int track, uint64_t startFrame, int bars, std::string& error) {
    try {
        ClipInfo info;
        info.track = track;
        info.kind = ClipKind::notes;
        info.name = "Arp";
        info.start = startFrame;
        info.length = static_cast<uint64_t>(bars) * framesPerBar();
        auto events = makeArpEvents(framesPerBeat(), bars, &info.notes);
        // On a track with a plugin the clip is only events; the plugin's track program plays them.
        auto program = player_.hasTrackProgram(track) ? nullptr : makeInstrumentProgram(sampleRate_, blockSize);
        return finish(std::move(info), std::move(program), 1.0f, std::move(events), error);
    } catch (const std::exception& e) {
        error = e.what();
        return -1;
    }
}

int Session::clipAt(int track, uint64_t frame) const {
    for (size_t i = clips_.size(); i-- > 0;) {
        const ClipInfo& c = clips_[i];
        if (c.track == track && frame >= c.start && frame < c.start + c.length) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace wavy::editor
