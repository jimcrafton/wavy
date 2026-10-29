#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wavy::testing {

// Mono IEEE-float WAV (format 3), enough to listen to what a test rendered.
inline bool writeWavFloat(const std::string& path, const std::vector<float>& samples, uint32_t sampleRate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    const uint32_t riffSize = 36 + dataBytes;
    const uint16_t format = 3, channels = 1, bits = 32, blockAlign = 4;
    const uint32_t fmtSize = 16, byteRate = sampleRate * blockAlign;
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riffSize, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtSize, 4, 1, f);
    std::fwrite(&format, 2, 1, f);
    std::fwrite(&channels, 2, 1, f);
    std::fwrite(&sampleRate, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&blockAlign, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&dataBytes, 4, 1, f);
    std::fwrite(samples.data(), sizeof(float), samples.size(), f);
    return std::fclose(f) == 0;
}

}  // namespace wavy::testing
