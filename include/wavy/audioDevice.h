#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wavy {

// Planar float output: out[c] holds `frames` samples for device output channel c. Runs on the device's real-time thread.
using AudioCallback = std::function<void(float* const* out, uint32_t frames)>;

struct AudioDeviceInfo {
    double sampleRate = 0.0;
    uint32_t blockSize = 0;
    uint32_t outputChannels = 0;
};

class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    // Opens the driver and creates its buffers; the block size is the driver's preferred one. Throws std::runtime_error on failure.
    virtual AudioDeviceInfo open(const std::string& driver) = 0;
    // Streaming begins; the callback is installed before the call and stays set until stop().
    virtual void start(AudioCallback callback) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
    // True after the driver asked for a reset (sample rate or buffer size changed): close and reopen, then rebuild the program.
    virtual bool resetRequested() const = 0;
};

// Installed ASIO drivers, read from the registry. Empty when wavy was built without ASIO_SDK_DIR as well.
std::vector<std::string> listAsioDrivers();

// Null when wavy was built without the ASIO SDK (configure with -DASIO_SDK_DIR=<path>).
std::unique_ptr<AudioDevice> makeAsioDevice();

}  // namespace wavy
