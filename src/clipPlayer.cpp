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
    size_t channels = 1;
    std::vector<float> blocks[2];  // clip block k lives in blocks[k & 1], channel c at c * blockSize
    int track = -1;
    std::array<uint16_t, 128> held{};  // event clips: per note, the channels with a note on that this clip has not yet turned off
};

// A ring of the last maxCompensation frames per channel; process() replaces the block by itself delayed by `delay` frames.
struct ClipPlayer::DelayLine {
    explicit DelayLine(size_t channels) : channels(channels), data(channels * ClipPlayer::maxCompensation, 0.0f) {}

    void process(float* const* io, size_t frames, size_t delay) noexcept {
        constexpr size_t size = ClipPlayer::maxCompensation;
        delay = std::min(delay, size - 1);
        for (size_t c = 0; c < channels; ++c) {
            float* ring = data.data() + c * size;
            size_t at = pos;
            for (size_t i = 0; i < frames; ++i) {
                ring[at] = io[c][i];
                io[c][i] = ring[(at + size - delay) % size];
                at = (at + 1) % size;
            }
        }
        pos = (pos + frames) % size;
    }

    size_t channels;
    std::vector<float> data;
    size_t pos = 0;
};

struct ClipPlayer::Track {
    std::unique_ptr<jitnodes::AudioProgram> program;
    std::vector<std::shared_ptr<jitnodes::ExternalProcessor>> processors;
    std::vector<TransportListener*> listeners;  // the processors that want the transport
    std::vector<float> in, out;  // two channels, channel c at c * blockSize
    jitnodes::EventBuffer events;
    DelayLine line{2};
    std::atomic<size_t> delay{0};
};

ClipPlayer::ClipPlayer(size_t blockSize, size_t channels)
    : blockSize_(blockSize), channels_(channels), block_(blockSize * (channels == 2 ? 2 : 1), 0.0f), served_(blockSize) {
    if (blockSize == 0 || blockSize % 16 != 0) throw std::invalid_argument("ClipPlayer: block size must be a multiple of 16");
    if (channels != 1 && channels != 2) throw std::invalid_argument("ClipPlayer: channels must be 1 or 2");
    for (auto& s : slots_) s.store(nullptr);
    bus_.assign(static_cast<size_t>(maxTracks) * 2 * blockSize, 0.0f);
    directLine_ = std::make_unique<DelayLine>(channels);
}

ClipPlayer::~ClipPlayer() = default;

int ClipPlayer::addClip(uint64_t startFrame, uint64_t lengthFrames, std::unique_ptr<jitnodes::AudioProgram> program, float gain,
                        std::vector<ClipEvent> events, int track) {
    if (!program || program->blockSize() != blockSize_ || program->outputCount() < 1 || program->outputCount() > 2 ||
        (track < 0 && program->outputCount() > channels_))
        throw std::invalid_argument("ClipPlayer::addClip: program needs this player's block size and one output (two on a stereo player)");
    if (track >= maxTracks) throw std::invalid_argument("ClipPlayer::addClip: track out of range");
    if (count_ == maxClips) return -1;

    auto clip = std::make_unique<Clip>();
    clip->track = track;
    clip->start = startFrame;
    clip->length = lengthFrames;
    clip->gain = gain;
    clip->events = std::move(events);
    clip->channels = program->outputCount();
    clip->blocks[0].assign(blockSize_ * clip->channels, 0.0f);
    clip->blocks[1].assign(blockSize_ * clip->channels, 0.0f);
    clip->program = std::move(program);
    clip->program->warm();
    clip->program->resetState();

    const int index = static_cast<int>(count_);
    slots_[count_++].store(clip.get(), std::memory_order_release);
    owned_.push_back(std::move(clip));
    return index;
}

int ClipPlayer::addEventClip(int track, uint64_t startFrame, uint64_t lengthFrames, std::vector<ClipEvent> events) {
    if (track < 0 || track >= maxTracks) throw std::invalid_argument("ClipPlayer::addEventClip: track out of range");
    if (count_ == maxClips) return -1;
    auto clip = std::make_unique<Clip>();
    clip->track = track;
    clip->start = startFrame;
    clip->length = lengthFrames;
    clip->events = std::move(events);

    const int index = static_cast<int>(count_);
    slots_[count_++].store(clip.get(), std::memory_order_release);
    owned_.push_back(std::move(clip));
    return index;
}

void ClipPlayer::setTrackProgram(int track, std::unique_ptr<jitnodes::AudioProgram> program,
                                 std::vector<std::shared_ptr<jitnodes::ExternalProcessor>> processors) {
    if (track < 0 || track >= maxTracks) throw std::invalid_argument("ClipPlayer::setTrackProgram: track out of range");
    if (!program || program->blockSize() != blockSize_ || program->outputCount() != 2)
        throw std::invalid_argument("ClipPlayer::setTrackProgram: needs this player's block size and two outputs");
    auto t = std::make_unique<Track>();
    t->in.assign(blockSize_ * 2, 0.0f);
    t->out.assign(blockSize_ * 2, 0.0f);
    t->program = std::move(program);
    t->processors = std::move(processors);
    for (const auto& processor : t->processors) {
        if (auto* listener = dynamic_cast<TransportListener*>(processor.get())) t->listeners.push_back(listener);
    }
    t->program->warm();
    t->program->resetState();
    tracks_[static_cast<size_t>(track)].store(t.get(), std::memory_order_release);
    ownedTracks_.push_back(std::move(t));  // a replaced one stays alive until clear()
    hasTracks_.store(true, std::memory_order_release);
    updateLatencyCompensation();
}

void ClipPlayer::updateLatencyCompensation() {
    size_t latencies[maxTracks] = {};
    size_t longest = 0;
    for (size_t t = 0; t < tracks_.size(); ++t) {
        Track* track = tracks_[t].load(std::memory_order_acquire);
        if (!track) continue;
        for (const auto& processor : track->processors) {
            latencies[t] += processor->latencySamples();
        }
        latencies[t] = std::min(latencies[t], maxCompensation - 1);
        longest = std::max(longest, latencies[t]);
    }
    for (size_t t = 0; t < tracks_.size(); ++t) {
        if (Track* track = tracks_[t].load(std::memory_order_acquire)) track->delay.store(longest - latencies[t], std::memory_order_release);
    }
    directDelay_.store(longest, std::memory_order_release);
    latency_.store(longest, std::memory_order_release);
}

size_t ClipPlayer::compensationDelay(int track) const {
    if (track < 0) return directDelay_.load(std::memory_order_acquire);
    if (track >= maxTracks) return 0;
    const Track* t = tracks_[static_cast<size_t>(track)].load(std::memory_order_acquire);
    return t ? t->delay.load(std::memory_order_acquire) : 0;
}

void ClipPlayer::setTrackVolume(int track, float gain) {
    if (track >= 0 && track < maxTracks) strips_[static_cast<size_t>(track)].volume.store(std::clamp(gain, 0.0f, 4.0f), std::memory_order_release);
}
void ClipPlayer::setTrackPan(int track, float pan) {
    if (track >= 0 && track < maxTracks) strips_[static_cast<size_t>(track)].pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_release);
}
void ClipPlayer::setTrackMute(int track, bool mute) {
    if (track >= 0 && track < maxTracks) strips_[static_cast<size_t>(track)].mute.store(mute, std::memory_order_release);
}
void ClipPlayer::setTrackSolo(int track, bool solo) {
    if (track >= 0 && track < maxTracks) strips_[static_cast<size_t>(track)].solo.store(solo, std::memory_order_release);
}
float ClipPlayer::trackVolume(int track) const {
    return track >= 0 && track < maxTracks ? strips_[static_cast<size_t>(track)].volume.load(std::memory_order_acquire) : 1.0f;
}
float ClipPlayer::trackPan(int track) const {
    return track >= 0 && track < maxTracks ? strips_[static_cast<size_t>(track)].pan.load(std::memory_order_acquire) : 0.0f;
}
bool ClipPlayer::trackMuted(int track) const {
    return track >= 0 && track < maxTracks && strips_[static_cast<size_t>(track)].mute.load(std::memory_order_acquire);
}
bool ClipPlayer::trackSoloed(int track) const {
    return track >= 0 && track < maxTracks && strips_[static_cast<size_t>(track)].solo.load(std::memory_order_acquire);
}

void ClipPlayer::applyStrip(int track, float* left, float* right, bool anySolo) noexcept {
    Strip& s = strips_[static_cast<size_t>(track)];
    const bool audible = !s.mute.load(std::memory_order_acquire) && (!anySolo || s.solo.load(std::memory_order_acquire));
    const float volume = audible ? s.volume.load(std::memory_order_acquire) : 0.0f;
    const float pan = right ? s.pan.load(std::memory_order_acquire) : 0.0f;
    const float targetLeft = volume * (pan > 0.0f ? 1.0f - pan : 1.0f);
    const float targetRight = volume * (pan < 0.0f ? 1.0f + pan : 1.0f);
    if (targetLeft == 1.0f && targetRight == 1.0f && s.lastLeft == 1.0f && s.lastRight == 1.0f) return;
    const float stepLeft = (targetLeft - s.lastLeft) / static_cast<float>(blockSize_);
    const float stepRight = (targetRight - s.lastRight) / static_cast<float>(blockSize_);
    float gl = s.lastLeft, gr = s.lastRight;
    for (size_t i = 0; i < blockSize_; ++i) {
        gl += stepLeft;
        gr += stepRight;
        left[i] *= gl;
        if (right) right[i] *= gr;
    }
    s.lastLeft = targetLeft;
    s.lastRight = targetRight;
}

bool ClipPlayer::hasTrackProgram(int track) const {
    return track >= 0 && track < maxTracks && tracks_[static_cast<size_t>(track)].load(std::memory_order_acquire) != nullptr;
}

void ClipPlayer::clear() {
    for (auto& s : slots_) s.store(nullptr, std::memory_order_release);
    for (auto& t : tracks_) t.store(nullptr, std::memory_order_release);
    hasTracks_.store(false, std::memory_order_release);
    owned_.clear();
    ownedTracks_.clear();
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
        float* base = clip.blocks[j & 1].data();
        float* out[] = {base, base + blockSize};
        clip.program->process(out, &events);
        clip.loaded = j;
    }
}

void ClipPlayer::mixClip(Clip& clip) noexcept {
    const uint64_t end = nextStart_ + blockSize_;
    const uint64_t from = std::max(nextStart_, clip.start);
    const uint64_t to = std::min(end, clip.start + clip.length);
    if (from >= to) return;
    Track* track = clip.track >= 0 ? tracks_[static_cast<size_t>(clip.track)].load(std::memory_order_acquire) : nullptr;

    if (!clip.program) {
        if (!track) return;
        const auto first = std::lower_bound(clip.events.begin(), clip.events.end(), from - clip.start,
                                            [](const ClipEvent& e, uint64_t frame) { return e.frame < frame; });
        for (auto it = first; it != clip.events.end() && it->frame < to - clip.start; ++it) {
            const uint32_t offset = static_cast<uint32_t>(clip.start + it->frame - nextStart_);
            track->events.push({offset, it->status, it->data1, it->data2});
            const uint8_t type = it->status & 0xF0, channel = it->status & 0x0F;
            if (type == 0x90 && it->data2 != 0) clip.held[it->data1 & 0x7F] |= static_cast<uint16_t>(1u << channel);
            else if (type == 0x80 || type == 0x90) clip.held[it->data1 & 0x7F] &= static_cast<uint16_t>(~(1u << channel));
        }
        if (clip.start + clip.length <= end) {
            // The clip ends inside this block: release what it left sounding.
            const uint32_t offset = static_cast<uint32_t>(clip.start + clip.length - nextStart_ - 1);
            for (uint8_t note = 0; note < 128; ++note) {
                for (uint8_t channel = 0; channel < 16 && clip.held[note]; ++channel) {
                    if (clip.held[note] & (1u << channel)) track->events.push({offset, static_cast<uint8_t>(0x80 | channel), note, 0});
                }
                clip.held[note] = 0;
            }
        }
        return;
    }

    ensureBlock(clip, static_cast<int64_t>((to - 1 - clip.start) / blockSize_), blockSize_);
    float* dest = track ? track->in.data() : clip.track >= 0 ? bus_.data() + static_cast<size_t>(clip.track) * 2 * blockSize_ : block_.data();
    const size_t destChannels = track ? 2 : channels_;
    for (size_t ch = 0; ch < destChannels; ++ch) {
        const size_t source = std::min(ch, clip.channels - 1);
        for (uint64_t t = from; t < to; ++t) {
            const uint64_t c = t - clip.start;
            dest[ch * blockSize_ + (t - nextStart_)] += clip.blocks[(c / blockSize_) & 1][source * blockSize_ + c % blockSize_] * clip.gain;
        }
    }
}

bool ClipPlayer::setParam(int clip, jitnodes::NodeId node, size_t index, float value) {
    return params_.push({clip, node, static_cast<uint32_t>(index), value});
}

void ClipPlayer::runTracks(bool release, bool playing, bool anySolo) noexcept {
    const double bpm = bpm_.load(std::memory_order_acquire);
    const int64_t blockStart = static_cast<int64_t>(playing ? nextStart_ - blockSize_ : nextStart_);
    for (int t = 0; t < maxTracks; ++t) {
        Track* track = tracks_[static_cast<size_t>(t)].load(std::memory_order_acquire);
        if (!track) continue;
        if (release) {
            for (auto& processor : track->processors) {
                processor->reset();
            }
        }
        for (TransportListener* listener : track->listeners) {
            listener->setTransport(bpm, playing, blockStart);
        }
        float* out[] = {track->out.data(), track->out.data() + blockSize_};
        const float* in[] = {track->in.data(), track->in.data() + blockSize_};
        track->program->process(out, &track->events, in);
        track->line.process(out, blockSize_, track->delay.load(std::memory_order_acquire));
        applyStrip(t, out[0], out[1], anySolo);
        for (size_t i = 0; i < blockSize_; ++i) {
            if (channels_ == 2) {
                block_[i] += out[0][i];
                block_[blockSize_ + i] += out[1][i];
            } else {
                block_[i] += 0.5f * (out[0][i] + out[1][i]);
            }
        }
    }
}

void ClipPlayer::renderBlock(bool playing) noexcept {
    ParamChange change;
    while (params_.pop(change)) {
        if (change.clip < 0 || static_cast<size_t>(change.clip) >= maxClips) continue;
        Clip* clip = slots_[static_cast<size_t>(change.clip)].load(std::memory_order_acquire);
        if (!clip || !clip->program) continue;
        if (float* p = clip->program->tryParam(change.node, change.index)) *p = change.value;
    }
    std::fill(block_.begin(), block_.end(), 0.0f);
    std::fill(bus_.begin(), bus_.end(), 0.0f);
    for (auto& slot : tracks_) {
        Track* track = slot.load(std::memory_order_acquire);
        if (!track) continue;
        std::fill(track->in.begin(), track->in.end(), 0.0f);
        track->events.clear();
    }
    if (playing) {
        for (auto& slot : slots_) {
            Clip* clip = slot.load(std::memory_order_acquire);
            if (clip) mixClip(*clip);
        }
        nextStart_ += blockSize_;
    }
    bool anySolo = false;
    for (const Strip& strip : strips_) {
        anySolo = anySolo || strip.solo.load(std::memory_order_acquire);
    }
    for (int t = 0; t < maxTracks; ++t) {
        float* left = bus_.data() + static_cast<size_t>(t) * 2 * blockSize_;
        float* right = channels_ == 2 ? left + blockSize_ : nullptr;
        applyStrip(t, left, right, anySolo);
        for (size_t i = 0; i < blockSize_; ++i) {
            block_[i] += left[i];
            if (right) block_[blockSize_ + i] += right[i];
        }
    }
    if (hasTracks_.load(std::memory_order_acquire)) {
        float* direct[] = {block_.data(), block_.data() + blockSize_};
        directLine_->process(direct, blockSize_, directDelay_.load(std::memory_order_acquire));
    }
    runTracks(releaseTracks_, playing, anySolo);
    releaseTracks_ = false;
    if (limiter_.enabled()) {
        limiter_.process(block_.data(), channels_, blockSize_, blockSize_);
    }
    served_ = 0;
}

void ClipPlayer::render(float* out, size_t frames) noexcept {
    float* outs[] = {out};
    renderChannels(outs, 1, frames);
}

void ClipPlayer::render(float* const* out, size_t frames) noexcept { renderChannels(out, channels_, frames); }

void ClipPlayer::renderChannels(float* const* out, size_t outCount, size_t frames) noexcept {
    auto releaseHeldNotes = [this] {
        releaseTracks_ = true;
        for (auto& slot : slots_) {
            Clip* clip = slot.load(std::memory_order_acquire);
            if (clip) clip->held.fill(0);
        }
    };

    const int64_t target = seek_.exchange(-1, std::memory_order_acq_rel);
    if (target >= 0) {
        nextStart_ = static_cast<uint64_t>(target);
        served_ = blockSize_;
        for (auto& slot : slots_) {
            Clip* clip = slot.load(std::memory_order_acquire);
            if (clip) clip->loaded = -1;
        }
        releaseHeldNotes();
        position_.store(nextStart_, std::memory_order_release);
    }

    const bool playing = playing_.load(std::memory_order_acquire);
    if (!playing) {
        if (wasPlaying_) {
            nextStart_ -= blockSize_ - served_;  // drop the unplayed rest of the block so a resume continues from the playhead
            served_ = blockSize_;
            wasPlaying_ = false;
            releaseHeldNotes();
            position_.store(nextStart_, std::memory_order_release);
        }
        // Track programs keep running while stopped so that a plugin's release tails and delays ring out.
        if (!hasTracks_.load(std::memory_order_acquire)) {
            for (size_t ch = 0; ch < outCount; ++ch) {
                std::fill(out[ch], out[ch] + frames, 0.0f);
            }
            return;
        }
    } else if (!wasPlaying_) {
        served_ = blockSize_;  // drop the rest of an idle block
        wasPlaying_ = true;
    }

    const size_t written = std::min(outCount, channels_);
    float peak = 0.0f;
    size_t done = 0;
    while (done < frames) {
        if (served_ == blockSize_) renderBlock(playing);
        const size_t n = std::min(frames - done, blockSize_ - served_);
        for (size_t ch = 0; ch < written; ++ch) {
            const float* source = block_.data() + ch * blockSize_ + served_;
            for (size_t i = 0; i < n; ++i) {
                out[ch][done + i] = source[i];
                peak = std::max(peak, std::abs(source[i]));
            }
        }
        for (size_t ch = written; ch < outCount; ++ch) {
            std::fill(out[ch] + done, out[ch] + done + n, 0.0f);
        }
        served_ += n;
        done += n;
    }
    if (playing) position_.store(nextStart_ - (blockSize_ - served_), std::memory_order_release);
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
