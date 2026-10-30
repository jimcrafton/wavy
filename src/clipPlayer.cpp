#include "wavy/clipPlayer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "jitnodes/jitnodes.h"

namespace wavy {

struct ClipPlayer::Clip {
    uint64_t start = 0;
    uint64_t length = 0;
    float gain = 1.0f;
    std::unique_ptr<jitnodes::AudioProgram> program;
    std::vector<ClipEvent> events;
    size_t nextEvent = 0;
    int64_t loaded = -1;  // highest clip block rendered; -1 = none, the program starts from reset state
    std::vector<float> blocks[2];  // clip block k lives in blocks[k & 1]
};

ClipPlayer::ClipPlayer(size_t blockSize) : blockSize_(blockSize), block_(blockSize, 0.0f), served_(blockSize) {
    if (blockSize == 0 || blockSize % 16 != 0) throw std::invalid_argument("ClipPlayer: block size must be a multiple of 16");
    for (auto& s : slots_) s.store(nullptr);
}

ClipPlayer::~ClipPlayer() = default;

int ClipPlayer::addClip(uint64_t startFrame, uint64_t lengthFrames, std::unique_ptr<jitnodes::AudioProgram> program, float gain,
                        std::vector<ClipEvent> events) {
    if (!program || program->blockSize() != blockSize_ || program->outputCount() != 1)
        throw std::invalid_argument("ClipPlayer::addClip: program needs this player's block size and one output");
    if (count_ == maxClips) return -1;

    auto clip = std::make_unique<Clip>();
    clip->start = startFrame;
    clip->length = lengthFrames;
    clip->gain = gain;
    clip->events = std::move(events);
    clip->blocks[0].assign(blockSize_, 0.0f);
    clip->blocks[1].assign(blockSize_, 0.0f);
    clip->program = std::move(program);
    clip->program->warm();
    clip->program->resetState();

    const int index = static_cast<int>(count_);
    slots_[count_++].store(clip.get(), std::memory_order_release);
    owned_.push_back(std::move(clip));
    return index;
}

void ClipPlayer::clear() {
    for (auto& s : slots_) s.store(nullptr, std::memory_order_release);
    owned_.clear();
    count_ = 0;
}

void ClipPlayer::ensureBlock(Clip& clip, int64_t block, size_t blockSize) noexcept {
    jitnodes::EventBuffer events;
    while (clip.loaded < block) {
        if (clip.loaded < 0) {
            clip.program->resetState();
            clip.nextEvent = 0;
        }
        const int64_t j = clip.loaded + 1;
        const uint64_t from = static_cast<uint64_t>(j) * blockSize;
        const uint64_t to = from + blockSize;
        events.clear();
        while (clip.nextEvent < clip.events.size() && clip.events[clip.nextEvent].frame < to) {
            const ClipEvent& e = clip.events[clip.nextEvent++];
            if (e.frame >= from) events.push({static_cast<uint32_t>(e.frame - from), e.status, e.data1, e.data2});
        }
        float* out[] = {clip.blocks[j & 1].data()};
        clip.program->process(out, &events);
        clip.loaded = j;
    }
}

void ClipPlayer::mixClip(Clip& clip) noexcept {
    const uint64_t end = nextStart_ + blockSize_;
    const uint64_t from = std::max(nextStart_, clip.start);
    const uint64_t to = std::min(end, clip.start + clip.length);
    if (from >= to) return;
    ensureBlock(clip, static_cast<int64_t>((to - 1 - clip.start) / blockSize_), blockSize_);
    for (uint64_t t = from; t < to; ++t) {
        const uint64_t c = t - clip.start;
        block_[t - nextStart_] += clip.blocks[(c / blockSize_) & 1][c % blockSize_] * clip.gain;
    }
}

bool ClipPlayer::setParam(int clip, jitnodes::NodeId node, size_t index, float value) {
    return params_.push({clip, node, static_cast<uint32_t>(index), value});
}

void ClipPlayer::renderBlock() noexcept {
    ParamChange change;
    while (params_.pop(change)) {
        if (change.clip < 0 || static_cast<size_t>(change.clip) >= maxClips) continue;
        Clip* clip = slots_[static_cast<size_t>(change.clip)].load(std::memory_order_acquire);
        if (!clip) continue;
        if (float* p = clip->program->tryParam(change.node, change.index)) *p = change.value;
    }
    std::fill(block_.begin(), block_.end(), 0.0f);
    for (auto& slot : slots_) {
        Clip* clip = slot.load(std::memory_order_acquire);
        if (clip) mixClip(*clip);
    }
    nextStart_ += blockSize_;
    served_ = 0;
}

void ClipPlayer::render(float* out, size_t frames) noexcept {
    const int64_t target = seek_.exchange(-1, std::memory_order_acq_rel);
    if (target >= 0) {
        nextStart_ = static_cast<uint64_t>(target);
        served_ = blockSize_;
        for (auto& slot : slots_) {
            Clip* clip = slot.load(std::memory_order_acquire);
            if (clip) clip->loaded = -1;
        }
        position_.store(nextStart_, std::memory_order_release);
    }

    if (!playing_.load(std::memory_order_acquire)) {
        if (wasPlaying_) {
            nextStart_ -= blockSize_ - served_;  // drop the unplayed rest of the block so a resume continues from the playhead
            served_ = blockSize_;
            wasPlaying_ = false;
            position_.store(nextStart_, std::memory_order_release);
        }
        std::fill(out, out + frames, 0.0f);
        return;
    }
    wasPlaying_ = true;

    float peak = 0.0f;
    size_t done = 0;
    while (done < frames) {
        if (served_ == blockSize_) renderBlock();
        const size_t n = std::min(frames - done, blockSize_ - served_);
        for (size_t i = 0; i < n; ++i) {
            const float v = block_[served_ + i];
            out[done + i] = v;
            peak = std::max(peak, std::abs(v));
        }
        served_ += n;
        done += n;
    }
    position_.store(nextStart_ - (blockSize_ - served_), std::memory_order_release);
    if (peak > peak_.load(std::memory_order_relaxed)) peak_.store(peak, std::memory_order_release);
}

std::unique_ptr<jitnodes::AudioProgram> makeSampleProgram(const std::string& sampleName, size_t blockSize) {
    using namespace jitnodes;
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(blockSize));
    const NodeId player = net.addNode("samplePlayer");
    net.node(player).setString("sample", sampleName);
    net.connect(line, player);
    const NodeId gain = net.addNode("wrangle");
    net.node(gain).setString("code", "x = x * a;");
    net.connect(player, gain);
    auto program = AudioProgram::compile(net, gain, {blockSize, {"x"}});
    program->param(gain, 0) = 1.0f;
    return program;
}

}  // namespace wavy
