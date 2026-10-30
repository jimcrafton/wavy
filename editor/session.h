#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jitnodes/audio/audioProgram.h"
#include "wavy/clipPlayer.h"

namespace wavy::editor {

enum class ClipKind { sample, notes, graph };

struct NoteBox {
    uint64_t start = 0;  // frames from the clip start
    uint64_t length = 0;
    uint8_t pitch = 60;
    uint8_t velocity = 100;
};

struct GraphNodeInfo {
    std::string title;
    std::string detail;
    float x = 0.0f;
    float y = 0.0f;
    bool source = false;
    bool modulator = false;
};

// What the Patch view draws for a graph clip: the nodes of the network the clip was compiled from.
struct GraphInfo {
    std::vector<GraphNodeInfo> nodes;
    std::vector<std::pair<int, int>> wires;  // indices into nodes, from -> to
};

// The Drone clip's knobs. The second oscillator sits `detuneCents` above 110 Hz, so the default is the original 110.6 Hz.
struct DroneSettings {
    float cutoffHz = 600.0f;
    float q = 0.9f;
    float detuneCents = 9.4f;
    float levelDb = -18.4f;
};

struct DroneNodes {
    jitnodes::NodeId oscA = 0, oscB = 0, filter = 0, gain = 0;
};

struct ClipInfo {
    int track = 0;
    ClipKind kind = ClipKind::sample;
    std::string name;
    uint64_t start = 0;
    uint64_t length = 0;
    std::vector<float> peaks;  // min,max pairs, one per Session::peakFrames
    std::vector<NoteBox> notes;
    GraphInfo graph;
    DroneSettings drone;
    DroneNodes droneNodes;
    bool previewDirty = false;
};

struct TrackInfo {
    std::string name;
    uint32_t color = 0;  // 0xRRGGBB
    uint32_t dark = 0;
};

// The demo arrangement: a fixed set of tracks and the clips on them, mirrored into a ClipPlayer. No undo, no files.
class Session {
public:
    static constexpr uint64_t peakFrames = 512;
    static constexpr size_t blockSize = 128;

    explicit Session(double sampleRate);

    double sampleRate() const { return sampleRate_; }
    double bpm() const { return bpm_; }
    uint64_t framesPerBar() const { return static_cast<uint64_t>(sampleRate_ * 240.0 / bpm_); }
    uint64_t framesPerBeat() const { return framesPerBar() / 4; }

    const std::vector<TrackInfo>& tracks() const { return tracks_; }
    const std::vector<ClipInfo>& clips() const { return clips_; }
    wavy::ClipPlayer& player() { return player_; }
    const wavy::ClipPlayer& player() const { return player_; }

    // Each returns the new clip's index, or -1 with `error` filled in. The new clip starts on `startFrame` and sits on `track`.
    int addSampleClip(int track, uint64_t startFrame, const std::string& path, std::string& error);
    int addGraphClip(int track, uint64_t startFrame, int bars, std::string& error);
    int addNotesClip(int track, uint64_t startFrame, int bars, std::string& error);

    // Clip under the timeline frame on a track, or -1.
    int clipAt(int track, uint64_t frame) const;

    // Changes a graph clip's knobs while it plays: the new values reach the audio thread through the player's parameter ring, and the
    // preview waveform is marked stale. False for a clip that is not a Drone.
    bool setDrone(int clip, const DroneSettings& settings);
    // Re-renders a stale preview offline.
    void refreshPreview(int clip);

private:
    void renderPreview(ClipInfo& info) const;
    int finish(ClipInfo info, std::unique_ptr<jitnodes::AudioProgram> program, float gain, std::vector<wavy::ClipEvent> events,
               std::string& error);

    double sampleRate_;
    double bpm_ = 120.0;
    std::vector<TrackInfo> tracks_;
    std::vector<ClipInfo> clips_;
    wavy::ClipPlayer player_;
    int sampleCount_ = 0;
};

// The clip graph for a drone: two detuned oscillators with a few harmonics, mixed, low-passed, then a gain.
std::unique_ptr<jitnodes::AudioProgram> makeDroneProgram(double sampleRate, size_t blockSize, GraphInfo* info = nullptr,
                                                         const DroneSettings& settings = {}, DroneNodes* nodes = nullptr);

// RBJ low-pass biquad: b0 b1 b2 a1 a2, normalised by a0.
void lowpassCoefficients(double sampleRate, float cutoffHz, float q, float out[5]);

// Arpeggio over a minor chord, one note per eighth, as events relative to the clip start. `noteBoxes` receives the same notes.
std::vector<wavy::ClipEvent> makeArpEvents(uint64_t framesPerBeat, int bars, std::vector<NoteBox>* noteBoxes);

// line -> silence -> voices -> gain: the instrument a notes clip plays.
std::unique_ptr<jitnodes::AudioProgram> makeInstrumentProgram(double sampleRate, size_t blockSize);

}  // namespace wavy::editor
