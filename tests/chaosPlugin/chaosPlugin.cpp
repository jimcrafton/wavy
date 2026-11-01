// wavyChaos: a VST3 effect that misbehaves on command, for testing how the host and the audio graph cope. Never ship it.
//
// Parameters (normalized 0..1 on the wire):
//   0 mode      7 steps: 0 pass-through, 1 block (sleep), 2 crash (access violation), 3 NaN output, 4 infinite output,
//               5 loud (x1e6), 6 throw a C++ exception
//   1 blockMs   0..1000 ms slept per block in mode 1
//   2 armAfter  0..1000 blocks processed normally after a mode change before the fault starts
//   3 latency   0..4096 samples reported through getLatencySamples()
//   4 gain      0..2, applied in mode 0 and in the modes that still pass audio
// Environment variable WAVY_CHAOS_FAIL: "init" fails initialize(), "setup" fails setupProcessing(), "activate" fails setActive(true).

#include <windows.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

enum ParamId : ParamID { pMode = 0, pBlockMs = 1, pArmAfter = 2, pLatency = 3, pGain = 4 };
constexpr int modeSteps = 6;

std::string failMode() {
    char buf[32] = {};
    DWORD n = GetEnvironmentVariableA("WAVY_CHAOS_FAIL", buf, sizeof buf);
    return n > 0 && n < sizeof buf ? std::string(buf) : std::string();
}

class ChaosProcessor final : public AudioEffect {
public:
    ChaosProcessor() { setControllerClass(INLINE_UID(0x7A1C0001, 0x00000002, 0x00000003, 0x00000004)); }

    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new ChaosProcessor()); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        if (failMode() == "init") return kResultFalse;
        tresult r = AudioEffect::initialize(context);
        if (r != kResultOk) return r;
        addAudioInput(STR16("In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Out"), SpeakerArr::kStereo);
        return kResultOk;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) SMTG_OVERRIDE {
        if (failMode() == "setup") return kResultFalse;
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {
        if (state && failMode() == "activate") return kResultFalse;
        blocks_ = 0;
        return AudioEffect::setActive(state);
    }

    uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE { return latency_; }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE {
        if (data.inputParameterChanges) applyParams(*data.inputParameterChanges);
        ++blocks_;
        const bool armed = mode_ != 0 && blocks_ > armAfter_;

        if (data.numOutputs < 1 || data.numSamples <= 0) return kResultOk;
        AudioBusBuffers& out = data.outputs[0];
        const bool haveIn = data.numInputs > 0 && data.inputs[0].numChannels > 0;
        for (int32 c = 0; c < out.numChannels; ++c) {
            float* o = out.channelBuffers32[c];
            const float* i = haveIn && c < data.inputs[0].numChannels ? data.inputs[0].channelBuffers32[c] : nullptr;
            for (int32 s = 0; s < data.numSamples; ++s) o[s] = i ? i[s] * gain_ : 0.0f;
        }
        if (!armed) return kResultOk;

        switch (mode_) {
            case 1:
                Sleep(static_cast<DWORD>(blockMs_));
                break;
            case 2:
                *static_cast<volatile int*>(nullptr) = 1;
                break;
            case 3:
                for (int32 c = 0; c < out.numChannels; ++c) out.channelBuffers32[c][0] = std::numeric_limits<float>::quiet_NaN();
                break;
            case 4:
                for (int32 c = 0; c < out.numChannels; ++c) out.channelBuffers32[c][0] = std::numeric_limits<float>::infinity();
                break;
            case 5:
                for (int32 c = 0; c < out.numChannels; ++c)
                    for (int32 s = 0; s < data.numSamples; ++s) out.channelBuffers32[c][s] *= 1.0e6f;
                break;
            case 6:
                throw std::runtime_error("wavyChaos: deliberate exception");
            default:
                break;
        }
        return kResultOk;
    }

private:
    void applyParams(IParameterChanges& changes) {
        for (int32 q = 0; q < changes.getParameterCount(); ++q) {
            IParamValueQueue* queue = changes.getParameterData(q);
            if (!queue || queue->getPointCount() == 0) continue;
            int32 offset = 0;
            ParamValue v = 0;
            queue->getPoint(queue->getPointCount() - 1, offset, v);
            switch (queue->getParameterId()) {
                case pMode: {
                    const int m = static_cast<int>(std::lround(v * modeSteps));
                    if (m != mode_) blocks_ = 0;
                    mode_ = m;
                    break;
                }
                case pBlockMs: blockMs_ = static_cast<int>(std::lround(v * 1000.0)); break;
                case pArmAfter: armAfter_ = static_cast<int>(std::lround(v * 1000.0)); break;
                case pLatency: latency_ = static_cast<uint32>(std::lround(v * 4096.0)); break;
                case pGain: gain_ = static_cast<float>(v * 2.0); break;
                default: break;
            }
        }
    }

    int mode_ = 0;
    int blockMs_ = 0;
    int armAfter_ = 0;
    uint32 latency_ = 0;
    float gain_ = 1.0f;
    int blocks_ = 0;
};

class ChaosController final : public EditController {
public:
    static FUnknown* create(void*) { return static_cast<IEditController*>(new ChaosController()); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        tresult r = EditController::initialize(context);
        if (r != kResultOk) return r;
        parameters.addParameter(STR16("Mode"), nullptr, modeSteps, 0, ParameterInfo::kCanAutomate | ParameterInfo::kIsList, pMode);
        parameters.addParameter(STR16("Block ms"), STR16("ms"), 0, 0, ParameterInfo::kCanAutomate, pBlockMs);
        parameters.addParameter(STR16("Arm after"), STR16("blocks"), 0, 0, ParameterInfo::kCanAutomate, pArmAfter);
        parameters.addParameter(STR16("Latency"), STR16("smp"), 0, 0, ParameterInfo::kCanAutomate, pLatency);
        parameters.addParameter(STR16("Gain"), nullptr, 0, 0.5, ParameterInfo::kCanAutomate, pGain);
        return kResultOk;
    }
};

}  // namespace

BEGIN_FACTORY_DEF("wavy", "https://example.invalid", "mailto:nobody@example.invalid")

DEF_CLASS2(INLINE_UID(0x7A1C0000, 0x00000002, 0x00000003, 0x00000004), PClassInfo::kManyInstances, kVstAudioEffectClass, "wavyChaos",
           Vst::kDistributable, "Fx", "1.0.0", kVstVersionString, ChaosProcessor::create)

DEF_CLASS2(INLINE_UID(0x7A1C0001, 0x00000002, 0x00000003, 0x00000004), PClassInfo::kManyInstances, kVstComponentControllerClass,
           "wavyChaos Controller", 0, "", "1.0.0", kVstVersionString, ChaosController::create)

END_FACTORY
