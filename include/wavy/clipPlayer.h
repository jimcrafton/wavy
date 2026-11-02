#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "jitnodes/audio/audioProgram.h"
#include "jitnodes/audio/externalProcessor.h"
#include "wavy/spscRing.h"

namespace wavy {

// A MIDI-style message at a frame relative to the start of its clip.
struct ClipEvent {
    uint64_t frame = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

// Plays clips placed on a timeline: each clip is a compiled AudioProgram (a sample player, a generator graph, or an instrument fed by
// the clip's events) that starts at `startFrame`, is reset there and runs for `lengthFrames`. Clips overlapping in time are summed.
//
// Main thread: addClip(), clear() (not while streaming), play(), stop(), seek(), position(), takePeak(). Audio thread: render() only; it
// allocates nothing and locks nothing. A clip added while streaming is picked up at the next block.
class ClipPlayer {
public:
    static constexpr size_t maxClips = 64;

    // `blockSize` is the block size of every clip program (a multiple of 16). `channels` (1 or 2) is the player's output width: a mono clip
    // feeds every channel, a stereo clip (outputs x, r) needs a stereo player.
    explicit ClipPlayer(size_t blockSize = 128, size_t channels = 1);
    ~ClipPlayer();
    ClipPlayer(const ClipPlayer&) = delete;
    ClipPlayer& operator=(const ClipPlayer&) = delete;

    size_t blockSize() const { return blockSize_; }
    size_t channels() const { return channels_; }

    // `program` must have this player's block size and one output (or two, on a stereo player); it is warmed here. `events` must be sorted by frame. Returns the
    // clip's index, or -1 when the player is full. Throws std::invalid_argument for a mismatched program.
    //
    // `track` (-1 = none) names a track that has, or later gets, a program from setTrackProgram(): the clip's audio is then summed into
    // that track's input instead of the master mix.
    int addClip(uint64_t startFrame, uint64_t lengthFrames, std::unique_ptr<jitnodes::AudioProgram> program, float gain = 1.0f,
                std::vector<ClipEvent> events = {}, int track = -1);

    // A clip that is only events (a MIDI clip): they go to the program of `track`, which must be a valid track number. Sorted by frame,
    // relative to the clip start. Notes still held when the clip ends or the playhead moves are released through the track's processors.
    int addEventClip(int track, uint64_t startFrame, uint64_t lengthFrames, std::vector<ClipEvent> events);

    static constexpr int maxTracks = 16;

    // Gives `track` a long-lived stereo program (two outputs, one block size, hostInput reading the track's summed clips). Unlike a clip's
    // program it is never reset at a clip boundary, runs for as long as the player is asked to render (also while stopped, so a plugin's
    // tail rings out) and is mixed into the master. `processors` are the external processors inside it; the player calls their reset() at
    // stop and seek. Throws std::invalid_argument for a bad track or program. Not while streaming.
    void setTrackProgram(int track, std::unique_ptr<jitnodes::AudioProgram> program,
                         std::vector<std::shared_ptr<jitnodes::ExternalProcessor>> processors = {});
    bool hasTrackProgram(int track) const;

    // Frees every clip and track program. Only while the device is not streaming.
    void clear();
    size_t clipCount() const { return count_; }

    // Queues a block-rate parameter write for `clip` (an index addClip returned), applied at the start of the next block. False when the
    // ring is full (try again later). Writes for a node or index the clip's program does not have are ignored. One producer thread.
    bool setParam(int clip, jitnodes::NodeId node, size_t index, float value);

    void play() { playing_.store(true, std::memory_order_release); }
    void stop() { playing_.store(false, std::memory_order_release); }
    bool playing() const { return playing_.load(std::memory_order_acquire); }
    // Moves the playhead to `frame` at the next block; clips before it replay from their start.
    void seek(uint64_t frame) { seek_.store(static_cast<int64_t>(frame), std::memory_order_release); }
    // Frames played so far in this transport position (the playhead).
    uint64_t position() const { return position_.load(std::memory_order_acquire); }
    // Largest absolute output sample since the last call.
    float takePeak() { return peak_.exchange(0.0f, std::memory_order_acq_rel); }

    // Writes `frames` mono samples (silence while stopped). Any frame count works. On a stereo player this writes the left channel.
    void render(float* out, size_t frames) noexcept;

    // Writes `frames` samples to out[0..channels()).
    void render(float* const* out, size_t frames) noexcept;

private:
    struct Clip;
    struct Track;
    void renderChannels(float* const* out, size_t outCount, size_t frames) noexcept;
    void renderBlock(bool playing) noexcept;
    void mixClip(Clip& clip) noexcept;
    void runTracks(bool release) noexcept;
    static void ensureBlock(Clip& clip, int64_t block, size_t blockSize) noexcept;

    size_t blockSize_;
    size_t channels_;
    std::array<std::atomic<Clip*>, maxClips> slots_{};
    std::vector<std::unique_ptr<Clip>> owned_;  // main thread
    size_t count_ = 0;

    std::array<std::atomic<Track*>, maxTracks> tracks_{};
    std::vector<std::unique_ptr<Track>> ownedTracks_;  // main thread
    std::atomic<bool> hasTracks_{false};
    bool releaseTracks_ = false;  // audio thread: reset the tracks' processors at the next block

    struct ParamChange {
        int32_t clip;
        jitnodes::NodeId node;
        uint32_t index;
        float value;
    };
    SpscRing<ParamChange, 1024> params_;

    std::atomic<bool> playing_{false};
    std::atomic<int64_t> seek_{-1};
    std::atomic<uint64_t> position_{0};
    std::atomic<float> peak_{0.0f};

    std::vector<float> block_;  // the mixed block being served, channel c at c * blockSize_
    size_t served_;             // frames of block_ already handed out; blockSize_ means "render another"
    uint64_t nextStart_ = 0;    // timeline frame of the next block to render
    bool wasPlaying_ = false;
};

// line -> samplePlayer(`sampleName`, from the SampleBank) -> gain: a program for a clip that plays a whole sample once.
std::unique_ptr<jitnodes::AudioProgram> makeSampleProgram(const std::string& sampleName, size_t blockSize);

}  // namespace wavy
