#include "session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>

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

Session::Session(double sampleRate) : sampleRate_(sampleRate), player_(blockSize) {
    tracks_ = {{"Drums", 0xd98a5f, 0x4a3022}, {"Bass", 0x6c8cf0, 0x27335e}, {"Pad", 0x5fc7a0, 0x1f4a3a}, {"Arp", 0xe05fa8, 0x5a2442}};
}

int Session::finish(ClipInfo info, std::unique_ptr<jitnodes::AudioProgram> program, float gain, std::vector<wavy::ClipEvent> events,
                    std::string& error) {
    try {
        if (player_.addClip(info.start, info.length, std::move(program), gain, std::move(events)) < 0) {
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
        return finish(std::move(info), makeInstrumentProgram(sampleRate_, blockSize), 1.0f, std::move(events), error);
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
