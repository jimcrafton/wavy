#include "wavy/audioDevice.h"

#include <stdexcept>

#ifdef WAVY_HAS_ASIO

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

namespace wavy {
namespace {

class AsioDevice;
AsioDevice* active = nullptr;  // ASIO callbacks carry no user data

int16_t toInt16(float v) { return static_cast<int16_t>(std::lrintf(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); }
int32_t toInt32(float v) { return static_cast<int32_t>(std::llrint(std::clamp(static_cast<double>(v), -1.0, 1.0) * 2147483647.0)); }

class AsioDevice final : public AudioDevice {
public:
    ~AsioDevice() override { close(); }

    AudioDeviceInfo open(const std::string& driver) override {
        close();
        std::string name = driver;
        if (!loadAsioDriver(name.data())) throw std::runtime_error("asio: cannot load driver '" + driver + "'");
        loaded_ = true;
        ASIODriverInfo info{};
        info.sysRef = GetDesktopWindow();
        if (ASIOInit(&info) != ASE_OK) fail("ASIOInit failed: " + std::string(info.errorMessage));
        initialised_ = true;

        long inputs = 0, outputs = 0, minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
        ASIOSampleRate rate = 0;
        if (ASIOGetChannels(&inputs, &outputs) != ASE_OK || outputs <= 0) fail("driver has no output channels");
        if (ASIOGetBufferSize(&minSize, &maxSize, &preferred, &granularity) != ASE_OK) fail("ASIOGetBufferSize failed");
        if (ASIOGetSampleRate(&rate) != ASE_OK || rate <= 0) fail("ASIOGetSampleRate failed");

        outputs_ = static_cast<size_t>(outputs);
        blockSize_ = static_cast<uint32_t>(preferred);
        buffers_.assign(outputs_, ASIOBufferInfo{});
        for (size_t c = 0; c < outputs_; ++c) {
            buffers_[c].isInput = ASIOFalse;
            buffers_[c].channelNum = static_cast<long>(c);
        }

        callbacks_.bufferSwitch = &AsioDevice::bufferSwitch;
        callbacks_.sampleRateDidChange = &AsioDevice::sampleRateChanged;
        callbacks_.asioMessage = &AsioDevice::message;
        callbacks_.bufferSwitchTimeInfo = &AsioDevice::bufferSwitchTimeInfo;
        active = this;
        if (ASIOCreateBuffers(buffers_.data(), outputs, preferred, &callbacks_) != ASE_OK) fail("ASIOCreateBuffers failed");
        buffersCreated_ = true;

        types_.assign(outputs_, ASIOSTFloat32LSB);
        for (size_t c = 0; c < outputs_; ++c) {
            ASIOChannelInfo ci{};
            ci.channel = static_cast<long>(c);
            ci.isInput = ASIOFalse;
            if (ASIOGetChannelInfo(&ci) != ASE_OK) fail("ASIOGetChannelInfo failed");
            types_[c] = ci.type;
        }
        outputReady_ = ASIOOutputReady() == ASE_OK;
        planar_.assign(outputs_, nullptr);
        scratch_.assign(outputs_ * blockSize_, 0.0f);
        for (size_t c = 0; c < outputs_; ++c) planar_[c] = scratch_.data() + c * blockSize_;
        resetRequested_ = false;
        return {static_cast<double>(rate), blockSize_, static_cast<uint32_t>(outputs_)};
    }

    void start(AudioCallback callback) override {
        if (!buffersCreated_) throw std::runtime_error("asio: device is not open");
        callback_ = std::move(callback);
        if (ASIOStart() != ASE_OK) throw std::runtime_error("asio: ASIOStart failed");
        started_ = true;
    }

    void stop() override {
        if (!started_) return;
        ASIOStop();
        started_ = false;
        callback_ = nullptr;
    }

    void close() override {
        stop();
        if (buffersCreated_) ASIODisposeBuffers();
        if (initialised_) ASIOExit();
        if (loaded_ && asioDrivers) asioDrivers->removeCurrentDriver();
        buffersCreated_ = initialised_ = loaded_ = false;
        if (active == this) active = nullptr;
    }

    bool resetRequested() const override { return resetRequested_.load(); }

private:
    [[noreturn]] void fail(const std::string& what) {
        close();
        throw std::runtime_error("asio: " + what);
    }

    void fill(long index) {
        if (callback_) callback_(planar_.data(), blockSize_);
        else std::fill(scratch_.begin(), scratch_.end(), 0.0f);
        for (size_t c = 0; c < outputs_; ++c) {
            const float* src = planar_[c];
            void* dst = buffers_[c].buffers[index];
            switch (types_[c]) {
            case ASIOSTFloat32LSB: std::memcpy(dst, src, blockSize_ * sizeof(float)); break;
            case ASIOSTFloat64LSB:
                for (uint32_t i = 0; i < blockSize_; ++i) static_cast<double*>(dst)[i] = src[i];
                break;
            case ASIOSTInt16LSB:
                for (uint32_t i = 0; i < blockSize_; ++i) static_cast<int16_t*>(dst)[i] = toInt16(src[i]);
                break;
            case ASIOSTInt32LSB:
                for (uint32_t i = 0; i < blockSize_; ++i) static_cast<int32_t*>(dst)[i] = toInt32(src[i]);
                break;
            case ASIOSTInt24LSB: {
                auto* bytes = static_cast<uint8_t*>(dst);
                for (uint32_t i = 0; i < blockSize_; ++i) {
                    const int32_t v = toInt32(src[i]) >> 8;
                    bytes[3 * i] = static_cast<uint8_t>(v);
                    bytes[3 * i + 1] = static_cast<uint8_t>(v >> 8);
                    bytes[3 * i + 2] = static_cast<uint8_t>(v >> 16);
                }
                break;
            }
            default: std::memset(dst, 0, blockSize_ * 4); break;  // unsupported (big-endian, packed 16/18/20/24 in 32): silence
            }
        }
        if (outputReady_) ASIOOutputReady();
    }

    static void bufferSwitch(long index, ASIOBool) {
        if (active) active->fill(index);
    }
    static ASIOTime* bufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool) {
        if (active) active->fill(index);
        return time;
    }
    static void sampleRateChanged(ASIOSampleRate) {
        if (active) active->resetRequested_ = true;
    }
    static long message(long selector, long value, void*, double*) {
        switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest || value == kAsioSupportsTimeInfo ? 1 : 0;
        case kAsioResetRequest:
            if (active) active->resetRequested_ = true;
            return 1;
        case kAsioEngineVersion: return 2;
        case kAsioSupportsTimeInfo: return 1;
        default: return 0;
        }
    }

    ASIOCallbacks callbacks_{};
    std::vector<ASIOBufferInfo> buffers_;
    std::vector<ASIOSampleType> types_;
    std::vector<float*> planar_;
    std::vector<float> scratch_;
    AudioCallback callback_;
    size_t outputs_ = 0;
    uint32_t blockSize_ = 0;
    bool loaded_ = false, initialised_ = false, buffersCreated_ = false, started_ = false, outputReady_ = false;
    std::atomic<bool> resetRequested_{false};
};

}  // namespace

std::vector<std::string> listAsioDrivers() {
    std::vector<std::string> names;
    AsioDrivers drivers;
    char storage[32][32];
    char* pointers[32];
    for (auto& s : storage) pointers[&s - storage] = s;
    const long count = drivers.getDriverNames(pointers, 32);
    for (long i = 0; i < count; ++i) names.emplace_back(pointers[i]);
    return names;
}

std::unique_ptr<AudioDevice> makeAsioDevice() { return std::make_unique<AsioDevice>(); }

}  // namespace wavy

#else

namespace wavy {

std::vector<std::string> listAsioDrivers() { return {}; }
std::unique_ptr<AudioDevice> makeAsioDevice() { return nullptr; }

}  // namespace wavy

#endif
