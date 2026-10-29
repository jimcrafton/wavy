#include "wavy/audioEngine.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace wavy {

using jitnodes::AudioProgram;

AudioEngine::AudioEngine(std::unique_ptr<AudioProgram> program) {
    if (!program) throw std::invalid_argument("audio engine: null program");
    blockSize_ = program->blockSize();
    outputCount_ = program->outputCount();
    current_ = program.release();

    block_.assign(outputCount_ * blockSize_, 0.0f);
    fade_.assign(outputCount_ * blockSize_, 0.0f);
    for (size_t i = 0; i < outputCount_; ++i) {
        blockPtrs_.push_back(block_.data() + i * blockSize_);
        fadePtrs_.push_back(fade_.data() + i * blockSize_);
    }
    served_ = blockSize_;
}

AudioEngine::~AudioEngine() {
    delete current_;
    delete pending_.exchange(nullptr);
    collectRetired();
}

void AudioEngine::swapProgram(std::unique_ptr<AudioProgram> next) {
    if (!next) throw std::invalid_argument("audio engine: null program");
    if (next->blockSize() != blockSize_ || next->outputCount() != outputCount_)
        throw std::invalid_argument("audio engine: program block size or output count differs from the engine's");
    delete pending_.exchange(next.release());
}

bool AudioEngine::setParam(jitnodes::NodeId node, size_t index, float value) {
    return params_.push({node, static_cast<uint32_t>(index), value});
}

size_t AudioEngine::collectRetired() {
    size_t freed = 0;
    AudioProgram* program = nullptr;
    while (retired_.pop(program)) {
        delete program;
        ++freed;
    }
    return freed;
}

void AudioEngine::renderBlock() noexcept {
    AudioProgram* fading = nullptr;
    // Only accept a swap when the old program has somewhere to go, so nothing is ever freed here.
    if (!retired_.full()) {
        if (AudioProgram* next = pending_.exchange(nullptr)) {
            next->migrateState(*current_);
            fading = current_;
            current_ = next;
        }
    }

    ParamChange change;
    while (params_.pop(change))
        if (float* p = current_->tryParam(change.node, change.index)) *p = change.value;

    current_->process(blockPtrs_.data());
    if (fading) {
        fading->process(fadePtrs_.data());
        const float step = 1.0f / static_cast<float>(blockSize_);
        for (size_t c = 0; c < outputCount_; ++c) {
            float* to = blockPtrs_[c];
            const float* from = fadePtrs_[c];
            for (size_t i = 0; i < blockSize_; ++i) {
                const float t = (static_cast<float>(i) + 0.5f) * step;
                to[i] = from[i] + (to[i] - from[i]) * t;
            }
        }
        retired_.push(fading);
    }
    served_ = 0;
}

void AudioEngine::render(float* const* out, size_t frames) noexcept {
    size_t done = 0;
    while (done < frames) {
        if (served_ == blockSize_) renderBlock();
        const size_t n = std::min(frames - done, blockSize_ - served_);
        for (size_t c = 0; c < outputCount_; ++c)
            if (out[c]) std::memcpy(out[c] + done, blockPtrs_[c] + served_, n * sizeof(float));
        served_ += n;
        done += n;
    }
}

}  // namespace wavy
