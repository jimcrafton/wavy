#include "wavy/midiInput.h"

#include <cstdint>

#include "wavy/audioEngine.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>

namespace wavy {

namespace {

void CALLBACK onMidi(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    if (message != MIM_DATA) return;
    const uint8_t status = static_cast<uint8_t>(param1 & 0xFF);
    if (status < 0x80 || status >= 0xF0) return;
    reinterpret_cast<AudioEngine*>(instance)->sendEvent(status, static_cast<uint8_t>((param1 >> 8) & 0x7F), static_cast<uint8_t>((param1 >> 16) & 0x7F));
}

}  // namespace

MidiInput::~MidiInput() { close(); }

size_t MidiInput::deviceCount() { return midiInGetNumDevs(); }

std::string MidiInput::deviceName(size_t index) {
    MIDIINCAPSA caps{};
    if (midiInGetDevCapsA(static_cast<UINT>(index), &caps, sizeof(caps)) != MMSYSERR_NOERROR) return {};
    return caps.szPname;
}

bool MidiInput::open(AudioEngine& engine, size_t deviceIndex) {
    close();
    HMIDIIN in = nullptr;
    if (midiInOpen(&in, static_cast<UINT>(deviceIndex), reinterpret_cast<DWORD_PTR>(&onMidi), reinterpret_cast<DWORD_PTR>(&engine), CALLBACK_FUNCTION) !=
        MMSYSERR_NOERROR)
        return false;
    if (midiInStart(in) != MMSYSERR_NOERROR) {
        midiInClose(in);
        return false;
    }
    handle_ = in;
    return true;
}

void MidiInput::close() {
    if (!handle_) return;
    HMIDIIN in = static_cast<HMIDIIN>(handle_);
    midiInStop(in);
    midiInReset(in);
    midiInClose(in);
    handle_ = nullptr;
}

}  // namespace wavy

#else

namespace wavy {

MidiInput::~MidiInput() = default;
size_t MidiInput::deviceCount() { return 0; }
std::string MidiInput::deviceName(size_t) { return {}; }
bool MidiInput::open(AudioEngine&, size_t) { return false; }
void MidiInput::close() {}

}  // namespace wavy

#endif
