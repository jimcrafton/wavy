#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "jitnodes/audio/audioProgram.h"
#include "wavy/spscRing.h"

namespace wavy {

// Owns the program that the audio thread runs and the lock-free paths into it. Device independent: a device callback calls render().
//
// Main thread: swapProgram(), setParam(), collectRetired(). Audio thread: render() only; it allocates nothing, locks nothing and never
// frees a program (replaced programs travel back through a retire ring that collectRetired() empties).
class AudioEngine {
public:
    // The initial program fixes the block size and output count; later programs must match both.
    explicit AudioEngine(std::unique_ptr<jitnodes::AudioProgram> program);
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    size_t blockSize() const { return blockSize_; }
    size_t outputCount() const { return outputCount_; }

    // Queues a program for the next block boundary. The audio thread copies the running program's state into it (migrateState), crossfades
    // from the old program over that block and retires the old one. A program that is still waiting when another is swapped in is dropped.
    // Throws std::invalid_argument for a null program or one with a different block size or output count.
    void swapProgram(std::unique_ptr<jitnodes::AudioProgram> next);

    // Queued block-rate parameter write, applied at the start of the next block. False when the ring is full (try again later). Writes
    // for a node or index the running program does not have are ignored.
    bool setParam(jitnodes::NodeId node, size_t index, float value);

    // Frees programs the audio thread has replaced. Returns how many.
    size_t collectRetired();

    // Writes `frames` samples to out[0..outputCount). Any frame count works; a partly consumed block is carried to the next call.
    void render(float* const* out, size_t frames) noexcept;

private:
    struct ParamChange {
        jitnodes::NodeId node;
        uint32_t index;
        float value;
    };

    void renderBlock() noexcept;

    size_t blockSize_ = 0;
    size_t outputCount_ = 0;

    jitnodes::AudioProgram* current_ = nullptr;  // audio thread
    std::atomic<jitnodes::AudioProgram*> pending_{nullptr};
    SpscRing<ParamChange, 1024> params_;
    SpscRing<jitnodes::AudioProgram*, 16> retired_;  // audio -> main

    std::vector<float> block_;  // outputCount_ * blockSize_, the block being served
    std::vector<float> fade_;   // the outgoing program's block while crossfading
    std::vector<float*> blockPtrs_;
    std::vector<float*> fadePtrs_;
    size_t served_ = 0;         // frames of block_ already handed out; blockSize_ means "render another"
};

}  // namespace wavy
