#pragma once

#include <cstddef>
#include <string>

namespace wavy {

class AudioEngine;

// One MIDI input device feeding an engine's event ring. Channel messages (note, controller, pitch bend...) arrive on the driver's
// callback thread and are queued with sendEvent at the start of the next block; system messages are ignored. Because the ring has one
// producer, use one MidiInput per engine and do not call sendEvent from another thread while it is open.
class MidiInput {
public:
    MidiInput() = default;
    ~MidiInput();
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    static size_t deviceCount();
    static std::string deviceName(size_t index);

    // The engine must outlive the open device. Returns false when the device cannot be opened (not present, or in use).
    bool open(AudioEngine& engine, size_t deviceIndex);
    void close();
    bool isOpen() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

}  // namespace wavy
