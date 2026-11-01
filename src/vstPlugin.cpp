#include "wavy/vstPlugin.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

#include "wavy/spscRing.h"

#include "base/source/fstring.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

namespace wavy {

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

HostApplication& hostApplication() {
    static HostApplication app;
    PluginContextFactory::instance().setPluginContext(&app);
    return app;
}

std::string toUtf8(const TChar* s) { return Steinberg::Vst::StringConvert::convert(std::u16string(reinterpret_cast<const char16_t*>(s))); }

std::array<uint8_t, 16> toArray(const VST3::UID& uid) {
    std::array<uint8_t, 16> a{};
    std::memcpy(a.data(), uid.data(), 16);
    return a;
}

constexpr uint32_t noParam = 0xFFFFFFFFu;
constexpr int midiControllers = 130;  // 0..127 CC, 128 channel pressure, 129 pitch bend (Steinberg::Vst::ControllerNumbers)

// A plugin that dereferences garbage must not take the editor down; the C++ objects live in the caller, so this function has none.
bool guardedProcess(IAudioProcessor* processor, ProcessData& data) noexcept {
    __try {
        processor->process(data);
        return true;
    } __except (1) {
        return false;
    }
}

struct ParamChange {
    uint32_t id;
    double value;
};

class HostHandler final : public IComponentHandler {
public:
    SpscRing<ParamChange, 1024> toProcessor;
    std::atomic<int32_t> restartFlags{0};

    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override {
        toProcessor.push({id, value});
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        restartFlags.fetch_or(flags, std::memory_order_acq_rel);
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, FUnknown)
        QUERY_INTERFACE(_iid, obj, IComponentHandler::iid, IComponentHandler)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        uint32 r = --refs_;
        if (r == 0) delete this;
        return r;
    }

private:
    std::atomic<uint32> refs_{1};
};

}  // namespace

struct VstModule::Impl {
    VST3::Hosting::Module::Ptr module;
    std::vector<VST3::Hosting::ClassInfo> infos;
};

VstModule::VstModule() : impl_(std::make_unique<Impl>()) {}
VstModule::~VstModule() = default;

std::shared_ptr<VstModule> VstModule::load(const std::string& path, std::string& error) {
    hostApplication();
    std::shared_ptr<VstModule> m(new VstModule());
    m->path_ = path;
    m->impl_->module = VST3::Hosting::Module::create(path, error);
    if (!m->impl_->module) {
        if (error.empty()) error = "cannot load " + path;
        return nullptr;
    }
    const auto& factory = m->impl_->module->getFactory();
    m->vendor_ = factory.info().vendor();
    for (const auto& ci : factory.classInfos()) {
        if (ci.category() != kVstAudioEffectClass) continue;
        VstClassInfo info;
        info.name = ci.name();
        info.vendor = ci.vendor().empty() ? m->vendor_ : ci.vendor();
        info.version = ci.version();
        info.subCategories = ci.subCategoriesString();
        info.classId = toArray(ci.ID());
        m->classes_.push_back(std::move(info));
        m->impl_->infos.push_back(ci);
    }
    if (m->classes_.empty()) {
        error = "no audio classes in " + path;
        return nullptr;
    }
    return m;
}

struct VstPlugin::Impl {
    std::shared_ptr<VstModule> module;
    size_t classIndex = 0;
    IPtr<PlugProvider> provider;
    IPtr<IComponent> component;
    IPtr<IAudioProcessor> processor;
    IPtr<IEditController> controller;
    IPtr<IMidiMapping> midiMapping;
    HostHandler* handler = nullptr;  // owned through the controller's reference
    bool controllerIsComponent = false;

    std::vector<VstBusInfo> inputs, outputs;
    int mainInChannels = 0, mainOutChannels = 0;
    bool prepared = false;
    int maxBlock = 0;
    double sampleRate = 0.0;

    ParameterChanges inChanges{256};
    ParameterChanges outChanges{64};
    EventList inEvents;
    EventList outEvents;
    ProcessContext context{};
    std::atomic<bool> transportDirty{false};
    struct Transport {
        double bpm = 120.0;
        bool playing = false;
        int64_t position = 0;
    } transport;
    std::vector<ParamID> midiMap;  // [channel * midiControllers + controller]
    SpscRing<ParamChange, 1024> setParams;
    std::vector<AudioBusBuffers> inBuses, outBuses;
    std::vector<float*> inPtrs, outPtrs;
    int64_t samplePosition = 0;
    bool processing = false;
    std::atomic<uint64_t> blocks{0}, slowBlocks{0}, nonFiniteBlocks{0};
    std::atomic<uint32_t> maxMicros{0};
    std::array<bool, 16 * 128> held{};          // note on/off bookkeeping: VST3 has no all-notes-off event
    std::atomic<bool> releaseHeld{false};       // set by reset(): the next block starts with a note-off for every held note

    std::atomic<bool> panic{false};  // set by hardReset(): all-sound-off and all-notes-off controllers at the start of the next block

    void sendPanic() {
        for (int ch = 0; ch < 16; ++ch)
            for (int cc : {120, 123}) {
                ParamID id = midiMap.empty() ? noParam : midiMap[static_cast<size_t>(ch * midiControllers + cc)];
                if (id != noParam) addChange(id, 1.0, 0);
            }
    }

    void releaseAllHeld() {
        for (int ch = 0; ch < 16; ++ch)
            for (int pitch = 0; pitch < 128; ++pitch) {
                bool& h = held[static_cast<size_t>(ch * 128 + pitch)];
                if (!h) continue;
                h = false;
                Event ev{};
                ev.type = Event::kNoteOffEvent;
                ev.noteOff.channel = static_cast<int16>(ch);
                ev.noteOff.pitch = static_cast<int16>(pitch);
                ev.noteOff.noteId = -1;
                inEvents.addEvent(ev);
            }
    }

    Impl() {
        inEvents.setMaxSize(512);
        outEvents.setMaxSize(512);
    }

    ~Impl() {
        if (processor && processing) processor->setProcessing(false);
        if (component && prepared) component->setActive(false);
        if (controller && !controllerIsComponent) controller->setComponentHandler(nullptr);
        midiMapping = nullptr;
        processor = nullptr;
        controller = nullptr;
        component = nullptr;
        provider = nullptr;  // terminates and disconnects through PlugProvider
    }

    void readBuses() {
        auto fill = [&](BusDirection dir, std::vector<VstBusInfo>& out) {
            out.clear();
            int32 n = component->getBusCount(kAudio, dir);
            for (int32 i = 0; i < n; ++i) {
                BusInfo bi{};
                if (component->getBusInfo(kAudio, dir, i, bi) != kResultOk) continue;
                VstBusInfo v;
                v.name = toUtf8(bi.name);
                v.channels = bi.channelCount;
                v.main = bi.busType == kMain;
                v.defaultActive = (bi.flags & BusInfo::kDefaultActive) != 0;
                out.push_back(std::move(v));
            }
        };
        fill(kInput, inputs);
        fill(kOutput, outputs);
    }

    void buildMidiMap() {
        midiMap.assign(16 * midiControllers, noParam);
        if (!midiMapping) return;
        for (int ch = 0; ch < 16; ++ch)
            for (int cc = 0; cc < midiControllers; ++cc) {
                ParamID id = 0;
                if (midiMapping->getMidiControllerAssignment(0, static_cast<int16>(ch), static_cast<CtrlNumber>(cc), id) == kResultTrue)
                    midiMap[ch * midiControllers + cc] = id;
            }
    }

    void fillContext(int frames) {
        context = {};
        context.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kProjectTimeMusicValid;
        if (transport.playing) context.state |= ProcessContext::kPlaying;
        context.sampleRate = sampleRate;
        context.projectTimeSamples = transport.position + samplePosition;
        context.tempo = transport.bpm;
        context.timeSigNumerator = 4;
        context.timeSigDenominator = 4;
        context.projectTimeMusic = static_cast<double>(context.projectTimeSamples) / sampleRate * transport.bpm / 60.0;
        samplePosition += frames;
    }

    void addChange(ParamID id, double value, int32 offset) {
        int32 index = 0;
        if (IParamValueQueue* q = inChanges.addParameterData(id, index)) {
            int32 pointIndex = 0;
            q->addPoint(offset, value, pointIndex);
        }
    }

    void translate(const jitnodes::AudioEvent& e, int frames) {
        const int32 offset = static_cast<int32>(std::min<uint32_t>(e.offset, static_cast<uint32_t>(frames - 1)));
        const int ch = e.status & 0x0F;
        Event ev{};
        ev.busIndex = 0;
        ev.sampleOffset = offset;
        ev.ppqPosition = 0;
        ev.flags = 0;
        switch (e.status & 0xF0) {
            case 0x90:
                if (e.data2 != 0) {
                    ev.type = Event::kNoteOnEvent;
                    ev.noteOn.channel = static_cast<int16>(ch);
                    ev.noteOn.pitch = e.data1;
                    ev.noteOn.velocity = e.data2 / 127.0f;
                    ev.noteOn.noteId = -1;
                    ev.noteOn.tuning = 0.0f;
                    ev.noteOn.length = 0;
                    held[static_cast<size_t>(ch * 128 + (e.data1 & 0x7F))] = true;
                    inEvents.addEvent(ev);
                    return;
                }
                [[fallthrough]];
            case 0x80:
                ev.type = Event::kNoteOffEvent;
                ev.noteOff.channel = static_cast<int16>(ch);
                ev.noteOff.pitch = e.data1;
                ev.noteOff.velocity = e.data2 / 127.0f;
                ev.noteOff.noteId = -1;
                ev.noteOff.tuning = 0.0f;
                held[static_cast<size_t>(ch * 128 + (e.data1 & 0x7F))] = false;
                inEvents.addEvent(ev);
                return;
            case 0xA0:
                ev.type = Event::kPolyPressureEvent;
                ev.polyPressure.channel = static_cast<int16>(ch);
                ev.polyPressure.pitch = e.data1;
                ev.polyPressure.pressure = e.data2 / 127.0f;
                ev.polyPressure.noteId = -1;
                inEvents.addEvent(ev);
                return;
            case 0xB0: {
                ParamID id = midiMap.empty() ? noParam : midiMap[ch * midiControllers + (e.data1 & 0x7F)];
                if (id != noParam) addChange(id, e.data2 / 127.0, offset);
                return;
            }
            case 0xD0: {
                ParamID id = midiMap.empty() ? noParam : midiMap[ch * midiControllers + 128];
                if (id != noParam) addChange(id, e.data1 / 127.0, offset);
                return;
            }
            case 0xE0: {
                ParamID id = midiMap.empty() ? noParam : midiMap[ch * midiControllers + 129];
                int value14 = (e.data2 << 7) | e.data1;
                if (id != noParam) addChange(id, value14 / 16383.0, offset);
                return;
            }
            default:
                return;
        }
    }
};

VstPlugin::VstPlugin() : impl_(std::make_unique<Impl>()) {}
VstPlugin::~VstPlugin() = default;

std::unique_ptr<VstPlugin> VstPlugin::create(std::shared_ptr<VstModule> module, size_t classIndex, std::string& error) {
    if (!module || classIndex >= module->classes().size()) {
        error = "bad class index";
        return nullptr;
    }
    hostApplication();
    std::unique_ptr<VstPlugin> p(new VstPlugin());
    Impl& d = *p->impl_;
    d.module = module;
    d.classIndex = classIndex;
    d.provider = owned(new PlugProvider(module->impl().module->getFactory(), module->impl().infos[classIndex], true));
    if (!d.provider->initialize()) {
        error = "plugin '" + module->classes()[classIndex].name + "' failed to initialize";
        return nullptr;
    }
    d.component = d.provider->getComponentPtr();
    d.controller = d.provider->getControllerPtr();
    if (!d.component || !(d.processor = FUnknownPtr<IAudioProcessor>(d.component))) {
        error = "plugin has no audio processor";
        return nullptr;
    }
    d.controllerIsComponent = d.controller && FUnknownPtr<IEditController>(d.component).getInterface() == d.controller.get();

    if (d.controller) {
        d.handler = new HostHandler();
        d.controller->setComponentHandler(d.handler);
        d.handler->release();  // the controller holds its own reference now

        // Bring the controller's view of the parameters in line with the component's initial state.
        MemoryStream stream;
        if (d.component->getState(&stream) == kResultOk) {
            stream.seek(0, IBStream::kIBSeekSet, nullptr);
            d.controller->setComponentState(&stream);
        }
        d.midiMapping = FUnknownPtr<IMidiMapping>(d.controller);
    }
    d.readBuses();
    d.buildMidiMap();
    return p;
}

const VstClassInfo& VstPlugin::info() const { return impl_->module->classes()[impl_->classIndex]; }
const std::vector<VstBusInfo>& VstPlugin::inputBuses() const { return impl_->inputs; }
const std::vector<VstBusInfo>& VstPlugin::outputBuses() const { return impl_->outputs; }

std::vector<VstParamInfo> VstPlugin::parameters() const {
    std::vector<VstParamInfo> out;
    if (!impl_->controller) return out;
    int32 n = impl_->controller->getParameterCount();
    out.reserve(static_cast<size_t>(n));
    for (int32 i = 0; i < n; ++i) {
        ParameterInfo pi{};
        if (impl_->controller->getParameterInfo(i, pi) != kResultOk) continue;
        VstParamInfo v;
        v.id = pi.id;
        v.title = toUtf8(pi.title);
        v.units = toUtf8(pi.units);
        v.stepCount = pi.stepCount;
        v.defaultNormalized = pi.defaultNormalizedValue;
        v.readOnly = (pi.flags & ParameterInfo::kIsReadOnly) != 0;
        v.hidden = (pi.flags & ParameterInfo::kIsHidden) != 0;
        v.automatable = (pi.flags & ParameterInfo::kCanAutomate) != 0;
        out.push_back(std::move(v));
    }
    return out;
}

bool VstPlugin::prepare(double sampleRate, int maxBlock, std::string& error) {
    Impl& d = *impl_;
    if (d.prepared) {
        if (d.processing) d.processor->setProcessing(false);
        d.component->setActive(false);
        d.processing = false;
        d.prepared = false;
    }
    if (d.processor->canProcessSampleSize(kSample32) != kResultTrue) {
        error = "plugin does not support 32-bit float processing";
        return false;
    }
    d.readBuses();

    // Ask for stereo on the main buses; the plugin may answer with another layout, which we read back below.
    std::vector<SpeakerArrangement> inArr, outArr;
    for (size_t i = 0; i < d.inputs.size(); ++i) {
        SpeakerArrangement a = 0;
        d.processor->getBusArrangement(kInput, static_cast<int32>(i), a);
        if (d.inputs[i].main && i == 0) a = SpeakerArr::kStereo;
        inArr.push_back(a);
    }
    for (size_t i = 0; i < d.outputs.size(); ++i) {
        SpeakerArrangement a = 0;
        d.processor->getBusArrangement(kOutput, static_cast<int32>(i), a);
        if (d.outputs[i].main && i == 0) a = SpeakerArr::kStereo;
        outArr.push_back(a);
    }
    d.processor->setBusArrangements(inArr.empty() ? nullptr : inArr.data(), static_cast<int32>(inArr.size()),
                                    outArr.empty() ? nullptr : outArr.data(), static_cast<int32>(outArr.size()));

    d.mainInChannels = d.mainOutChannels = 0;
    for (size_t i = 0; i < d.inputs.size(); ++i) {
        SpeakerArrangement a = 0;
        d.processor->getBusArrangement(kInput, static_cast<int32>(i), a);
        const bool use = i == 0 && d.inputs[i].main;
        d.component->activateBus(kAudio, kInput, static_cast<int32>(i), use ? 1 : 0);
        if (use) d.mainInChannels = SpeakerArr::getChannelCount(a);
    }
    for (size_t i = 0; i < d.outputs.size(); ++i) {
        SpeakerArrangement a = 0;
        d.processor->getBusArrangement(kOutput, static_cast<int32>(i), a);
        const bool use = i == 0 && d.outputs[i].main;
        d.component->activateBus(kAudio, kOutput, static_cast<int32>(i), use ? 1 : 0);
        if (use) d.mainOutChannels = SpeakerArr::getChannelCount(a);
    }
    if (d.mainOutChannels == 0) {
        error = "plugin has no main audio output";
        return false;
    }

    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = maxBlock;
    setup.sampleRate = sampleRate;
    if (d.processor->setupProcessing(setup) != kResultOk) {
        error = "setupProcessing failed";
        return false;
    }
    if (d.component->setActive(true) != kResultOk) {
        error = "setActive failed";
        return false;
    }
    d.prepared = true;
    d.maxBlock = maxBlock;
    d.sampleRate = sampleRate;
    d.samplePosition = 0;

    // Per-bus arrays cover every bus index; only the main buses carry channels.
    d.inBuses.assign(d.inputs.size(), AudioBusBuffers{});
    d.outBuses.assign(d.outputs.size(), AudioBusBuffers{});
    d.inPtrs.assign(static_cast<size_t>(std::max(d.mainInChannels, 1)), nullptr);
    d.outPtrs.assign(static_cast<size_t>(d.mainOutChannels), nullptr);
    if (!d.inBuses.empty() && d.mainInChannels > 0) {
        d.inBuses[0].numChannels = d.mainInChannels;
        d.inBuses[0].channelBuffers32 = d.inPtrs.data();
    }
    d.outBuses[0].numChannels = d.mainOutChannels;
    d.outBuses[0].channelBuffers32 = d.outPtrs.data();

    d.processor->setProcessing(true);
    d.processing = true;
    failed_.store(false, std::memory_order_release);
    resetStats();
    return true;
}

bool VstPlugin::prepared() const { return impl_->prepared; }
int VstPlugin::inputChannels() const { return impl_->mainInChannels; }
int VstPlugin::outputChannels() const { return impl_->mainOutChannels; }
int VstPlugin::latencySamples() const { return impl_->processor ? static_cast<int>(impl_->processor->getLatencySamples()) : 0; }
uint32_t VstPlugin::tailSamples() const { return impl_->processor ? impl_->processor->getTailSamples() : 0; }

bool VstPlugin::setParameter(uint32_t id, double normalized) {
    Impl& d = *impl_;
    if (!d.controller) return false;
    normalized = std::clamp(normalized, 0.0, 1.0);
    d.controller->setParamNormalized(id, normalized);
    return d.setParams.push({id, normalized});
}

double VstPlugin::getParameter(uint32_t id) const { return impl_->controller ? impl_->controller->getParamNormalized(id) : 0.0; }

std::string VstPlugin::parameterText(uint32_t id, double normalized) const {
    if (!impl_->controller) return {};
    String128 text{};
    if (impl_->controller->getParamStringByValue(id, normalized, text) != kResultOk) return {};
    return toUtf8(text);
}

void VstPlugin::setTransport(double bpm, bool playing, int64_t samplePosition) {
    Impl& d = *impl_;
    d.transport = {bpm, playing, samplePosition};
    d.samplePosition = 0;
}

void VstPlugin::process(const float* const* in, float* const* out, int frames, const jitnodes::EventBuffer* events) noexcept {
    Impl& d = *impl_;
    const int outCh = d.mainOutChannels;
    if (!d.prepared || failed_.load(std::memory_order_relaxed) || frames <= 0 || frames > d.maxBlock) {
        for (int c = 0; c < outCh; ++c)
            if (out[c] && frames > 0) std::memset(out[c], 0, sizeof(float) * static_cast<size_t>(frames));
        return;
    }

    d.inChanges.clearQueue();
    d.outChanges.clearQueue();
    d.inEvents.clear();
    d.outEvents.clear();

    if (d.releaseHeld.exchange(false, std::memory_order_acq_rel)) d.releaseAllHeld();
    if (d.panic.exchange(false, std::memory_order_acq_rel)) d.sendPanic();

    ParamChange pc;
    while (d.setParams.pop(pc)) d.addChange(pc.id, pc.value, 0);
    if (d.handler)
        while (d.handler->toProcessor.pop(pc)) d.addChange(pc.id, pc.value, 0);
    if (events)
        for (const auto& e : *events) d.translate(e, frames);

    for (int c = 0; c < d.mainInChannels; ++c) d.inPtrs[static_cast<size_t>(c)] = const_cast<float*>(in ? in[c] : nullptr);
    for (int c = 0; c < outCh; ++c) d.outPtrs[static_cast<size_t>(c)] = out[c];

    d.fillContext(frames);

    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = frames;
    data.numInputs = d.mainInChannels > 0 ? static_cast<int32>(d.inBuses.size()) : 0;
    data.numOutputs = static_cast<int32>(d.outBuses.size());
    data.inputs = data.numInputs ? d.inBuses.data() : nullptr;
    data.outputs = d.outBuses.data();
    data.inputParameterChanges = &d.inChanges;
    data.outputParameterChanges = &d.outChanges;
    data.inputEvents = &d.inEvents;
    data.outputEvents = &d.outEvents;
    data.processContext = &d.context;

    const auto start = std::chrono::steady_clock::now();
    const bool ok = guardedProcess(d.processor, data);
    const auto micros = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());

    d.blocks.fetch_add(1, std::memory_order_relaxed);
    if (micros > static_cast<uint64_t>(frames) * 1000000ull / static_cast<uint64_t>(d.sampleRate)) d.slowBlocks.fetch_add(1, std::memory_order_relaxed);
    uint32_t prevMax = d.maxMicros.load(std::memory_order_relaxed);
    const auto clipped = static_cast<uint32_t>(std::min<uint64_t>(micros, 0xFFFFFFFFull));
    while (clipped > prevMax && !d.maxMicros.compare_exchange_weak(prevMax, clipped, std::memory_order_relaxed)) {}

    if (!ok) {
        failed_.store(true, std::memory_order_release);
        for (int c = 0; c < outCh; ++c) std::memset(out[c], 0, sizeof(float) * static_cast<size_t>(frames));
        return;
    }
    // A plugin must never push NaN/Inf into the mix: one bad sample poisons every filter downstream.
    bool bad = false;
    for (int c = 0; c < outCh; ++c)
        for (int i = 0; i < frames; ++i)
            if (!std::isfinite(out[c][i])) {
                out[c][i] = 0.0f;
                bad = true;
            }
    if (bad) d.nonFiniteBlocks.fetch_add(1, std::memory_order_relaxed);
}

VstPlugin::Stats VstPlugin::stats() const {
    const Impl& d = *impl_;
    return {d.blocks.load(std::memory_order_relaxed), d.slowBlocks.load(std::memory_order_relaxed),
            d.nonFiniteBlocks.load(std::memory_order_relaxed), d.maxMicros.load(std::memory_order_relaxed)};
}

void VstPlugin::resetStats() {
    Impl& d = *impl_;
    d.blocks = 0;
    d.slowBlocks = 0;
    d.nonFiniteBlocks = 0;
    d.maxMicros = 0;
}

void VstPlugin::reset() noexcept {
    Impl& d = *impl_;
    if (!d.prepared || !d.processing) return;
    d.releaseHeld.store(true, std::memory_order_release);
    d.samplePosition = 0;
}

bool VstPlugin::hardReset() {
    Impl& d = *impl_;
    if (!d.prepared) return true;
    // Some synths (Surge XT) keep their voices across a deactivate/activate cycle; reloading the plugin's own state clears them.
    const std::vector<uint8_t> state = saveState();
    d.processor->setProcessing(false);
    d.component->setActive(false);
    const bool ok = d.component->setActive(true) == kResultOk;
    if (!state.empty()) loadState(state);
    d.processor->setProcessing(true);
    d.releaseHeld.store(true, std::memory_order_release);
    d.panic.store(true, std::memory_order_release);
    d.samplePosition = 0;
    return ok;
}

std::vector<uint8_t> VstPlugin::saveState() {
    Impl& d = *impl_;
    std::vector<uint8_t> blob;
    auto append32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) blob.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto appendStream = [&](MemoryStream& s) {
        const char* data = s.getData();
        const auto size = static_cast<uint32_t>(s.getSize());
        append32(size);
        if (data) blob.insert(blob.end(), data, data + size);
    };
    MemoryStream comp;
    if (d.component->getState(&comp) != kResultOk) return {};
    MemoryStream ctrl;
    if (d.controller && !d.controllerIsComponent) d.controller->getState(&ctrl);
    appendStream(comp);
    appendStream(ctrl);
    return blob;
}

bool VstPlugin::loadState(const std::vector<uint8_t>& blob) {
    Impl& d = *impl_;
    size_t pos = 0;
    auto read32 = [&](uint32_t& v) {
        if (pos + 4 > blob.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(blob[pos + static_cast<size_t>(i)]) << (8 * i);
        pos += 4;
        return true;
    };
    uint32_t compSize = 0, ctrlSize = 0;
    if (!read32(compSize) || pos + compSize > blob.size()) return false;
    std::vector<uint8_t> comp(blob.begin() + static_cast<std::ptrdiff_t>(pos), blob.begin() + static_cast<std::ptrdiff_t>(pos + compSize));
    pos += compSize;
    if (!read32(ctrlSize) || pos + ctrlSize > blob.size()) return false;
    std::vector<uint8_t> ctrl(blob.begin() + static_cast<std::ptrdiff_t>(pos), blob.begin() + static_cast<std::ptrdiff_t>(pos + ctrlSize));

    MemoryStream cs(comp.data(), static_cast<TSize>(comp.size()));
    if (d.component->setState(&cs) != kResultOk) return false;
    if (d.controller) {
        cs.seek(0, IBStream::kIBSeekSet, nullptr);
        d.controller->setComponentState(&cs);
        if (!ctrl.empty() && !d.controllerIsComponent) {
            MemoryStream ks(ctrl.data(), static_cast<TSize>(ctrl.size()));
            d.controller->setState(&ks);
        }
    }
    return true;
}

int32_t VstPlugin::takeRestartFlags() { return impl_->handler ? impl_->handler->restartFlags.exchange(0, std::memory_order_acq_rel) : 0; }

}  // namespace wavy
