#include "wavy/wavLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace wavy {

namespace {

uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

float sampleAt(const unsigned char* p, uint16_t format, uint16_t bits) {
    if (format == 3) {
        if (bits == 32) {
            float f;
            std::memcpy(&f, p, 4);
            return f;
        }
        double d;
        std::memcpy(&d, p, 8);
        return static_cast<float>(d);
    }
    switch (bits) {
        case 8: return (static_cast<int>(p[0]) - 128) / 128.0f;
        case 16: return static_cast<int16_t>(u16(p)) / 32768.0f;
        case 24: return (static_cast<int32_t>((p[0] << 8) | (p[1] << 16) | (static_cast<uint32_t>(p[2]) << 24)) >> 8) / 8388608.0f;
        default: return static_cast<int32_t>(u32(p)) / 2147483648.0f;
    }
}

}  // namespace

std::shared_ptr<jitnodes::SampleBuffer> loadWav(const std::string& path, double targetRate) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error(path + " is not a WAVE file");

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const unsigned char* data = nullptr;
    size_t dataBytes = 0;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        const uint32_t size = u32(&bytes[at + 4]);
        const unsigned char* body = &bytes[at + 8];
        const size_t avail = std::min<size_t>(size, bytes.size() - (at + 8));
        if (std::memcmp(&bytes[at], "fmt ", 4) == 0 && avail >= 16) {
            format = u16(body);
            channels = u16(body + 2);
            rate = u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xFFFE && avail >= 26) format = u16(body + 24);  // extensible: the sub-format's leading tag
        } else if (std::memcmp(&bytes[at], "data", 4) == 0) {
            data = body;
            dataBytes = avail;
        }
        at += 8 + size + (size & 1);
    }
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool flt = format == 3 && (bits == 32 || bits == 64);
    if (!data || channels == 0 || rate == 0 || !(pcm || flt)) throw std::runtime_error(path + ": unsupported WAVE format");

    const size_t stride = static_cast<size_t>(bits / 8) * channels;
    const size_t frames = dataBytes / stride;
    std::vector<float> mono(frames);
    for (size_t f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (uint16_t c = 0; c < channels; ++c) sum += sampleAt(data + f * stride + static_cast<size_t>(c) * (bits / 8), format, bits);
        mono[f] = sum / channels;
    }

    auto out = std::make_shared<jitnodes::SampleBuffer>();
    out->sampleRate = rate;
    if (targetRate > 0.0 && std::abs(targetRate - rate) > 0.5 && frames > 1) {
        const double step = rate / targetRate;
        const size_t n = static_cast<size_t>(static_cast<double>(frames) / step);
        out->frames.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const double pos = static_cast<double>(i) * step;
            const size_t a = static_cast<size_t>(pos);
            const size_t b = std::min(a + 1, frames - 1);
            const float t = static_cast<float>(pos - static_cast<double>(a));
            out->frames[i] = mono[a] + (mono[b] - mono[a]) * t;
        }
        out->sampleRate = targetRate;
    } else {
        out->frames = std::move(mono);
    }
    return out;
}

}  // namespace wavy
