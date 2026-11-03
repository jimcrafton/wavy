#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace wavy {

// Safety limiter for the master output: the gain drops instantly so no sample exceeds the ceiling, then recovers exponentially. It is
// linked across channels (one gain for all) and has no lookahead, so it adds no latency; it is a guard against runaway plugins, not a
// mastering tool. Non-finite samples become silence. process() is allocation-free; configure() is for the main thread before streaming.
class MasterLimiter {
public:
    void configure(double sampleRate, float ceilingDb = -1.0f, float releaseMs = 100.0f) {
        ceiling_ = std::pow(10.0f, ceilingDb / 20.0f);
        releaseCoefficient_ = 1.0f - std::exp(-1.0f / (static_cast<float>(sampleRate) * releaseMs * 0.001f));
        gain_ = 1.0f;
        enabled_ = true;
    }
    bool enabled() const { return enabled_; }

    void reset() { gain_ = 1.0f; }

    // Lowest gain applied since the last call (1 = never limited).
    float takeLowestGain() { return lowestGain_.exchange(1.0f, std::memory_order_acq_rel); }

    // `channels` planar buffers of `frames` samples, at `data + c * stride`.
    void process(float* data, size_t channels, size_t stride, size_t frames) noexcept {
        float lowest = 1.0f;
        for (size_t i = 0; i < frames; ++i) {
            float peak = 0.0f;
            for (size_t c = 0; c < channels; ++c) {
                float& s = data[c * stride + i];
                if (!std::isfinite(s)) {
                    s = 0.0f;
                }
                peak = (std::max)(peak, std::abs(s));
            }
            const float needed = peak > ceiling_ ? ceiling_ / peak : 1.0f;
            if (needed < gain_) {
                gain_ = needed;
            } else {
                gain_ += (1.0f - gain_) * releaseCoefficient_;
                gain_ = (std::min)(gain_, needed);
            }
            if (gain_ < 1.0f) {
                for (size_t c = 0; c < channels; ++c) {
                    data[c * stride + i] *= gain_;
                }
                lowest = (std::min)(lowest, gain_);
            }
        }
        if (lowest < lowestGain_.load(std::memory_order_relaxed)) {
            lowestGain_.store(lowest, std::memory_order_release);
        }
    }

private:
    bool enabled_ = false;
    float ceiling_ = 0.891f;
    float releaseCoefficient_ = 0.0f;
    float gain_ = 1.0f;
    std::atomic<float> lowestGain_{1.0f};
};

}  // namespace wavy
