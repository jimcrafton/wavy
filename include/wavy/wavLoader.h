#pragma once

#include <memory>
#include <string>

#include "jitnodes/audio/sampleBank.h"

namespace wavy {

// Reads a RIFF/WAVE file (8/16/24/32-bit PCM or 32/64-bit float, any channel count) into a mono buffer (channels averaged). When
// targetRate is above zero the result is linearly resampled to it. Throws std::runtime_error for a missing or unsupported file.
std::shared_ptr<jitnodes::SampleBuffer> loadWav(const std::string& path, double targetRate = 0.0);

}  // namespace wavy
