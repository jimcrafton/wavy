# VST3 host (phase 6) - sketch

Status: sketch, nothing built. Decisions needed from the user are in "Open questions".

## Scope

- **VST3 only.** VST2 is deprecated and its SDK is no longer distributable. CLAP could be added later behind the same `PluginInstance` interface; do not design for it now.
- Effects (inserts) and instruments (MIDI in, audio out). Float32 processing only.
- Windows x64 first (the project is Windows-only so far).

## Licensing

Steinberg released VST 3.8 under the **MIT license** (press release 2025-10-29), ending the old proprietary/GPLv3 dual model. So, unlike ASIO (see `docs/audioIntegration.md`), no signed agreement is needed; we keep the copyright notice and add attribution.
Verify against the `LICENSE` file in the SDK checkout before relying on this. Plan: git submodule `3rdparty/vst3sdk` (github.com/steinbergmedia/vst3sdk, tag 3.8.x), built with `SMTG_ENABLE_VST3_HOSTING_EXAMPLES` off; we compile only what we need from `public.sdk/source/vst/hosting` (`Module`, `PluginFactory`, `HostApplication`, `ParameterChanges`, `EventList`, `ProcessData`, `ProcessContext`, `ComponentHandler`) plus `base` and `pluginterfaces`.

## What a host has to do (the VST3 model in one page)

A plugin binary (`.vst3` bundle/DLL) exports a **factory**. Each class in it is a pair of objects, which may live in one class or two:

- **IComponent + IAudioProcessor**: the DSP. Realtime side.
- **IEditController** (+ optional **IPlugView**): parameters and GUI. Main-thread side.

(2D/3D analogy: the processor is the render thread, the controller is the scene-graph/UI side; they are linked only by messages, like a render proxy and its editor object. The host is the thing that carries those messages.)

Lifecycle for one instance (all main thread except `process`):

1. `Module::create(path)` -> factory -> `createInstance` for the component, then the controller (`getControllerClassId`, or the component itself implements it).
2. `initialize(hostContext)` on both, where our host context implements `IHostApplication` (name, `createInstance` for `IMessage`/`IAttributeList`).
3. Connect them with `IConnectionPoint` (host-mediated message passing; the host owns the proxy so messages are not thread-unsafe).
4. Negotiate buses: `getBusCount`, `setBusArrangements` (we want stereo in/out, plugin may refuse), `activateBus`.
5. `setupProcessing({sampleRate, maxSamplesPerBlock, kSample32, kRealtime})`, `setActive(true)`, then `setProcessing(true)` from the audio thread.
6. Audio thread, per block: fill `ProcessData` (input/output `AudioBusBuffers`, `inputEvents`, `inputParameterChanges`, `outputParameterChanges`, `processContext` with tempo/time signature/sample position/playing flag) and call `IAudioProcessor::process`.
7. Teardown reverses it (`setProcessing(false)`, `setActive(false)`, disconnect, `terminate`, release). Destroying must not happen on the audio thread.

Things that bite hosts:

- **Parameter flow is bidirectional.** GUI edits arrive via `IComponentHandler::beginEdit/performEdit/endEdit` (on the UI thread) and must be queued to the audio thread as `inputParameterChanges`. Plugin-side changes (automation readback, presets) come back in `outputParameterChanges`. We already have the SPSC ring pattern for this (`AudioEngine::setParam`).
- **`restartComponent(flags)`** can arrive from the plugin at any time (latency changed, bus layout changed, parameter titles changed, state reset). Defer to the main thread, and for IO/latency changes redo the setup and swap programs, which our engine already does with state migration.
- **Latency** (`getLatencySamples`) feeds plug-in delay compensation: every other path to the master needs a delay of the difference. **Tail** (`getTailSamples`) tells us when we can stop processing an effect after the input goes silent.
- **State**: `IComponent::getState` + `IEditController::getState` into an `IBStream` (a memory stream). Needed for undo, save/load and for recompiling a project without losing the plugin's settings.
- **Editor**: `IPlugView::attached(HWND, "HWND")` into a native child window; the plugin asks to resize through `IPlugFrame::resizeView`; also `onSize`, DPI (`IPlugViewContentScaleSupport`), and `canResize`. This needs an HWND from newui (see risks).
- **Events**: note on/off, poly pressure, etc. as `Event` structs with `sampleOffset`, plus MIDI CC mapped through `IMidiMapping` to parameter changes (VST3 has no raw CC events).
- Plugins are third-party code: they may allocate or take locks in `process`, and may crash. Wrap `process` in an SEH guard (`__try`) that marks the instance dead and outputs silence. Set FTZ/DAZ on the audio thread.

## Proposed wavy architecture

New files, all under `wavy` (not jitnodes: jitnodes stays pure DSP):

| Piece | Responsibility |
|---|---|
| `PluginScanner` | Walks `C:\Program Files\Common Files\VST3` (+ user dirs), gets class infos (name, vendor, category Fx/Instrument, class id) **in a child process** so a bad plugin cannot take the editor down; caches results on disk with file timestamps. |
| `PluginModule` | One loaded binary (ref-counted; `Module` from the SDK). |
| `PluginInstance` | One component+controller pair, the lifecycle above. Methods: `prepare(sampleRate, maxBlock)`, `process(in, out, frames, events, ctx) noexcept`, `setParam(id, normalized)` (ring), `latency()`, `saveState()/loadState()`, `openEditor(parentHwnd)`. |
| `HostContext` | `IHostApplication`, `IComponentHandler` (+`2`), `IPlugFrame`, the param queue; one per instance. |
| `PluginHost` | Session-level registry of live `PluginInstance`s by id; implements the jitnodes `ExternalProcessor` registry; owns lifetime (destroy on the main thread via the retire pattern). |
| `TrackProgram` (new) | Per track: sums the track's clip outputs, feeds events, runs the track's node graph (with `vst` nodes), outputs stereo to the master. |

### Where plugins sit: a `vst` node in the jitnodes graph (decided: option B)

The user decided plugins are **nodes**, wired like any other (an instrument node fed by note events, an effect node between a filter and the master), because that is the point of the node-graph design. (Option A, a hard-coded C++ insert chain outside the graphs, was rejected.)

Today the jitnodes audio side is **mono and closed**: steps process one `x` channel, `mix` sums only `x`, programs have no audio input, and a step is a pure function over its own `StateBlock`. A plugin needs stereo buffers, events in, an audio input from upstream, a latency, and it is a long-lived object owned by someone else. The jitnodes changes:

| # | Change (in jitnodes, `audio` branch) | Why |
|---|---|---|
| J1 | **Multichannel steps and mix.** A step declares its channels (`{"x","r"}` = L/R); the program passes pointers to each; `mix`/`mixKernels` sum a list of channels, not just `x`. Mono graphs keep working (one channel). | Plugins are stereo. Also gives pan/width/stereo gain nodes for free. |
| J2 | **`hostInput` source node**: audio the caller supplies per block (`process(in, out, events)`), restored like other sources. | An effect node, and track-level graphs that process the sum of a track's clips. |
| J3 | **`ExternalProcessor` interface + registry** (`audio/externalProcessor.h`, like `SampleBank`): abstract `process(in, out, frames, events)`, `latencySamples()`, `tailSamples()`, `reset()`. A `vst` node has a `plugin` param naming a registered processor; its step forwards to it. jitnodes never sees the VST3 SDK; wavy's `PluginInstance` implements the interface. | Keeps jitnodes SDK-free and testable with a fake processor. |
| J4 | **Latency in the compiler.** A step reports `latencySamples()`; where branches meet at a `mix`, the compiler inserts `delay` steps on the shorter ones (the PDC idea from the JUCE lessons). A latency change = recompile and swap. | Plugin delay compensation. |
| J5 | **Lifetime rules.** The processor is owned outside the program (wavy's `PluginHost` registry), so recompiling or swapping a program never reloads or resets it; no state migration needed, the instance simply continues. A processor may appear in only one running program (compile throws otherwise, same rule as a node reachable twice). | Plugin state (and e.g. Surge XT's patch) survives graph edits. |

**The key design question: who owns the instance, a clip or a track?** Clips are separate programs that `ClipPlayer` resets at each clip's start and stops at its end. A synth must not be reset at every clip start, and its release tail must ring past the clip end. So:

- **Plugins used as instruments or inserts live in a long-lived *track program***: a jitnodes graph that runs for as long as the transport (or the audio engine) runs. Its inputs are `hostInput` (the summed output of the track's clip programs) and the track's note events; its body is nodes, including `vst` nodes; its output goes to the master mix. This is also where per-track volume/pan/mute are nodes.
- A **`vst` node inside a Graph clip** (a plugin as part of one clip's patch) is also allowed; the instance is then owned by that clip's program, created with the clip, reset with it, and its tail is cut at the clip end. Fine for effects on one sample; use the track program for synths.
- Overlapping clips on one track keep working because they sum into the track program's `hostInput`; Notes clips send events into the track program rather than into their own `voices` node.

So the wavy-side structure becomes: clip programs (as now) -> per-track sum -> **track program** -> master program. `ClipPlayer` grows a per-track layer; `AudioEngine`'s stereo path and program swap are reused for the track programs.

**Cooked (interpreter) path.** `Node::cook` runs a whole signal at once (offline previews such as the Drone waveform). A `vst` node's cook either passes the input through (preview ignores the plugin, cheapest) or runs a *second* temporary instance in blocks. Start with pass-through and say so in the node's docs.

**Plugin parameters.** A plugin has hundreds or thousands; they are not all node params. Plan: the `vst` node exposes a handful of chosen "macro" params as ordinary node params (so sliders and graph modulation work), the rest go through the host's parameter ring by `ParamID`. Later: a `vstParam` node that turns a graph signal (an LFO, an envelope) into sample-accurate parameter changes, which VST3 supports natively; that is the node-graph payoff.

**Not changed:** the VST3 SDK stays out of jitnodes and out of the audio-thread rules (a plugin's `process` is third-party code; guard it as described under Risks).

### Integration with what exists

- MIDI/instruments: `ClipEvent` and `AudioEngine::sendEvent` already carry `status/data1/data2` plus a sample offset inside the block, which maps 1:1 onto VST3 `Event::sampleOffset`. A plugin instrument replaces the `voices` step as an event consumer.
- Param automation: reuse the `SpscRing<ParamChange>` pattern; the plugin side needs `ParamID` (uint32) and a normalized double.
- Block size: our programs are fixed at a multiple of 16 (128 today); `maxSamplesPerBlock` for the plugin is that block size. Fixed small blocks are fine for plugins.
- Program swap/recompile: the plugin instance must outlive a swap (owned by `PluginHost`, not by the program; rule J5), so recompiling a graph never reloads a plugin.
- Retire ring: instances are destroyed on the main thread via the same retire pattern as programs.

## First target: Surge XT

The user is installing Surge XT (surge-synthesizer.github.io), so it is the first real plugin. It should be at `C:\Program Files\Common Files\VST3\Surge Synth Team\Surge XT.vst3` (check after install; the installer also ships a separate "Surge XT Effects" plugin, a ready-made effect for 6b). Why it is a good test, and what to expect (from my knowledge of Surge XT, verify with `vstProbe`):

- An instrument with MIDI in; also a good stress test: a very large parameter list, large state blobs, MPE support, a resizable editor with its own zoom setting.
- Likely exposes several output buses (main plus per-scene outputs): exactly the "extra buses" pitfall above, so we activate only the main stereo bus.
- The plugin is GPL, but we only load it at runtime and do not link or bundle it, so it does not affect wavy's licence. Do not ship it with wavy without checking its terms.
- Offline test (6a): load it, send a note-on at offset 0, render N blocks, assert non-silent, finite output, and silence after note-off plus release. Needs the user's machine; skip the test if the plugin is absent. The SDK samples stay the CI fixtures.

Since the first plugin is an instrument, the order below is adjusted: after 6a go to the instrument step so the first audible result is Surge XT playing the Arp notes, then the effect insert.

## Fault-injection plugin (built)

`tests/chaosPlugin` builds `wavyChaos.vst3`, a stereo effect that misbehaves on command so the host and later the jitnodes `vst` node can be tested against crashes and stalls without depending on third-party plugins. Parameters: mode (0 pass-through, 1 block via `Sleep`, 2 access violation, 3 NaN, 4 Inf, 5 loud x1e6, 6 C++ exception), block milliseconds, "arm after N blocks", reported latency, gain. The environment variable `WAVY_CHAOS_FAIL` (`init`, `setup`, `activate`) makes that lifecycle step fail. Tests are in `tests/test_vstChaos.cpp`.

Host behaviour that is verified today: crashes and exceptions are caught (SEH guard) and the plugin goes silent with `failed()` set, until `prepare()` is called again; NaN/Inf samples are replaced by 0 and counted; slow `process()` calls are counted (`stats().slowBlocks`, `maxMicros`) but not otherwise acted on; setup failures come back as errors, not exceptions. What to decide in 6c, on the jitnodes side: the policy when a plugin is slow (a blocked plugin blocks the whole audio thread, so real protection needs either skipping the plugin for a while, a CPU budget that bypasses it, or the out-of-process host of 6g), a limiter on the master for loud output, and the `restartComponent`-from-audio-thread case.

## Phased plan

- **6a Load and process offline.** Add the SDK submodule; `PluginModule` + `PluginInstance`; gtests that load the SDK's own samples (`again`, `adelay`, built as test fixtures, so CI needs no third-party plugin), push a sine through, compare with the known gain, render to WAV. Also a console `vstProbe.exe <file.vst3>` that lists classes, buses, parameters, latency.
- **6b jitnodes prerequisites (J1, J2, J3 interface only), no plugin yet.** Multichannel steps and `mix`, `hostInput`, `ExternalProcessor` + registry; tested with a fake processor (gain, delay with latency) in jitnodes' macro tests and wavy gtests. Stereo path in `AudioEngine`/`ClipPlayer`. Risk-check the existing 197 tests after each step.
  - **As built (2026-11-01, 6b DONE):** stereo is two channels, `x` (left) and `r` (right). `AudioStep` gained `channels()` (1 or 2), `prepare(blockSize)`, `resource()` and `processChannels(ch, ...)`; a two-channel step fed a mono signal reads r = x, and a mono step fed stereo is rejected at compile (J4 will need a stereo `delay`). `mixKernels(branches, {"x","r"})` sums both channels, a mono branch in a stereo mix feeds both sides, and the `mix` node's `cook` does the same. `hostInput` (`first`, `channels` 1 or 2) reads host channels given to `AudioProgram::process(out, events, in)`; `inputCount()` tells the host how many. `ExternalProcessor` + `ProcessorBank` (`audio/externalProcessor.h`) and the `vst` node (`plugin` = bank name; cook passes the signal through); the same processor in two nodes of one program is rejected. `ClipPlayer(blockSize, channels)` and `render(float* const*, frames)` for a stereo player (mono clips are centred); `AudioEngine` already took any output count. Tests: `tests/test_audioStereo.cpp` (11, fake processor with gain/swap/delay/events), `ClipPlayerStereo` (3). Not done: jitnodes' own macro tests for these, multi-output ports (sidechain, still unchecked), latency/PDC (J4).
- **6c `vst` node + instrument (Surge XT audible).** The `vst` node, `PluginHost`, `TrackProgram` with events in; Event list from `ClipEvent`/`sendEvent`, `IMidiMapping`; the Arp notes drive Surge XT through ASIO; audio-thread SEH guard.
  - **As built (2026-11-01, 6c DONE):**
    - `PluginHost` (`wavy/pluginHost.h`) loads and prepares plugins and publishes each as a `VstProcessor` in `ProcessorBank` under a name. `VstProcessor` is the `ExternalProcessor` adapter: an instrument gets no input, an effect gets the block's input, and `reset()` releases held notes.
    - Slow-plugin policy (`SlowPolicy`, default 3 slow blocks in a row, then 400 blocks skipped): it reads `VstPlugin::stats().slowBlocks`, bypasses (silence for an instrument, input passed through for an effect), calls `reset()` when the skip starts and tries the plugin again afterwards. A failed (crashed) plugin is bypassed the same way. Counters: `skippedBlocks()`, `skipEpisodes()`. The block that was too slow is still late; this only stops the next ones. Real isolation is still 6g.
    - `ClipPlayer` track layer: `setTrackProgram(track, program, processors)` gives a track a long-lived stereo program (`makePluginTrackProgram`: `hostInput` -> `vst` -> gain). Clips added with `track = n` sum into that track's `hostInput`; `addEventClip(track, ...)` is a MIDI clip with no program of its own whose events go to the track program at their frame offsets. A held note is released with a note off when its clip ends. Stop and seek call `reset()` on the track's processors, and track programs keep running while stopped, so a synth's release tail and an effect's delay ring out.
    - Editor: the player is stereo (mono clips centred); `Session::setTrackInstrument(track, path)` loads Surge XT on the Arp track when it is installed (otherwise the Arp clip keeps the built-in `voices`), and the ASIO callback renders both channels.
    - Tests `tests/test_pluginHost.cpp`: fake instrument/effect through the track layer, wavyChaos slow and crash policy, and Surge XT playing a MIDI clip through `ClipPlayer` (skips when not installed).
    - Known limits: no master limiter yet (a loud plugin passes through; NaN/Inf are scrubbed by `VstPlugin`); no `restartComponent`-from-audio-thread mode in wavyChaos; `setTrackProgram` warms the program, which runs the plugin for one silent block; the editor has no UI for choosing an instrument; per-track volume/pan/mute are not wired to the gain node yet; an instrument ignores audio clips on its track.
- **6c' Effect nodes and PDC (J4, J5 rules).** An effect `vst` node on a track program (Surge XT Effects), latency reporting and compensation at `mix`, `ProcessContext` with tempo and playhead, param ring by `ParamID`.
- **6d Plugin editor window.** `IPlugView` in a native window; resize and DPI.
- **6e State, latency, tail.** Save/load blobs, PDC across tracks, tail-aware bypass, `restartComponent` handling.
- **6f Scanner and browser.** Out-of-process scan + cache, browser panel by type/manufacturer (per `docs/editor.md` 5g).
- **6g Crash isolation (maybe).** Out-of-process plugin host with shared-memory audio and the same rings. Large; do not start until in-process hosting is solid. In-process is how most hosts begin, but a crash takes the whole DAW down, so the guard in 6c is the stopgap.

## Lessons from JUCE's hosting experience (researched 2026-11-01)

Not using JUCE (decided). These come from JUCE forum threads and bug reports about its host side, plus a few outside host projects; most threads are old (several years), so they show recurring failure *classes*, not current JUCE behaviour. Where a point is my own inference it says so.

**Good ideas to adopt**

1. **Compile the routing into a render sequence with latency-compensation delays inserted on connections.** This is what `AudioProcessorGraph` does so parallel paths line up when each node reports its latency. It is the same model as our `AudioProgram` compile, so PDC belongs in the track/strip compile step: each strip reports latency, the compiler adds `delay` steps to the shorter paths.
2. **Bypass must keep the latency.** JUCE's `processBlockBypassed` default does not, so bypassing a latent plugin shifts the signal in time unless the plugin overrides it, and some hosts never call it. For wavy: bypass = a delay of the plugin's reported latency (we already have a `delay` step), never a plain copy.
3. **Explicit, owned format registration.** JUCE 8 removed `addDefaultFormats()`, which broke hosts that relied on it. Keep `PluginInstance` our own thin interface so SDK or framework churn stays behind it (also what lets CLAP slot in later).
4. **Out-of-process scanning with a persisted blacklist** (see the scanner bullets below).
5. **Bus layout set explicitly, then (re)prepared**: JUCE hosts that call `setBusesLayout` then `setRateAndBufferSizeDetails` get predictable channel counts.

**Pitfalls they hit, and what we do instead**

1. **`restartComponent` raised from the audio thread.** A JUCE-wrapped plugin asked for a latency restart from inside its process call and Studio One rejected it as belonging on the UI thread. Our rule has two halves: our host never calls anything restart-ish from `process`, and incoming `restartComponent` from plugins is only *queued* and handled on the main thread, never inline in the callback.
2. **Spurious or early restart flags.** JUCE-built plugins have sent `kLatencyChanged` on every display update and on activation; one native host treated that as "reload everything" and could never run those plugins (openxlr #205). We treat the flag as "re-query": compare `getLatencySamples()` and the bus info with the cached values and only recompile when something actually changed.
3. **Plugins that never send the notification.** Korg's VST3s did not call `restartComponent` on preset changes, so JUCE kept stale parameter values. We do not rely on flags alone: re-read parameter values after any `setState`/preset load we trigger, and refresh visible parameters on a UI timer.
4. **Conflating "the user moved a knob" with "the host refreshed the value".** In JUCE's model, pushing preset-driven changes to the host can be recorded as automation. Our parameter ring tags each change with its origin (gesture between `beginEdit`/`endEdit`, automation playback, host refresh) so automation recording only ever sees gestures. Also keep processor state and controller state separate, as VST3 does.
5. **Extra buses.** A graph hosting HALion exposed all 32 channels while Ableton showed stereo, and unused buses showed as "Discrete" rather than disabled; disabling one crashed the host. We activate only the main stereo bus (`activateBus(..., false)` for the rest), do layout changes only while the component is inactive (`setActive(false)` first), and put a multi-out instrument in the test set early.
6. **Latency change mid-stream forcing a graph rebuild on the wrong thread.** JUCE users report PDC needing a rebuild and the graph not being fully thread-safe. Our answer is already the engine's swap: a latency change recompiles the strip off the audio thread and swaps with state migration; the audio thread never mutates the routing.
7. **Scanning runs plugin code.** JUCE's directory scanner loads every plugin and crashes the host on bad ones; in-process scanning also tripped thread-affinity asserts because VST3 wants factory calls on a specific thread. We: scan in `wavyScan.exe`, with VST3 calls on that process's main thread; read filenames cheaply first and load only when needed; write a "now scanning X" marker *before* loading it so a hard crash is attributable (my inference, standard technique), persist the blacklist, and cache by file timestamp.
8. **Out-of-process hosting is a UI problem more than an audio problem.** One outside project (a plugin bridge, notes on Hugging Face) tried full out-of-process hosting, found the plugin GUI in a separate window broke fullscreen and cross-process GUI sharing was fragile, and moved to a hybrid: a short-lived helper test-loads each plugin, passing ones run in-process, failures are blocked. Caveat from the same notes: a plugin that crashes later in `process` still takes the host down. So the realistic ladder for wavy is: scan out of process, host in process, SEH guard on `process`, project autosave before loading an unknown plugin, and a "quarantine on crash" list. True bridging (6g) stays optional and large; decide with the user.
9. **Editor resize is best-effort.** Plugins ask to resize via `IPlugFrame`, hosts may refuse, and some editors are fixed-size. Support `resizeView`, but design the editor window to cope with a plugin that ignores us. (Thin evidence; test with real plugins.)

Sources: [restart/latency on the audio thread (Studio One)](https://forum.juce.com/t/vst3-latency-reporting-in-presonus-studio-one-fails/28249), [spurious latency flag](https://forum.juce.com/t/vst3-parameter-name-changes-dont-work/28313), [host rejecting latency-on-activation, openxlr #205](https://github.com/emaspa/openxlr/issues/205), [Korg preset restart](https://forum.juce.com/t/hosting-korg-vst3s-not-calling-vst3hostcontext-restartcomponent/56367), [automation vs host refresh](https://forum.juce.com/t/vst3-parameter-updates-automation-vs-host-refresh/67373/9), [bus layout of hosted plugin](https://forum.juce.com/t/setting-buses-layout-of-hosted-plugin/55262), [graph PDC and changing latency](https://forum.juce.com/t/audioprocessorgraph-pdc-and-changing-latency/18121), [bypass and latency](https://forum.juce.com/t/assertion-suggestion-in-audioprocessor-processblockbypassed/31945), [PDC in parallel paths](https://forum.juce.com/t/delay-compensation-in-an-audioprocessorgraph-with-internal-parallel-signal-paths/54081), [scan crash protection](https://forum.juce.com/t/vst3-plugin-scanning-crash-protection/58485), [out-of-process vs in-process notes](https://huggingface.co/RAM2118/pluginbridge/blob/main/FAILED-APPROACHES.md).

## Using JUCE itself (decided: no)

What JUCE has that maps onto this sketch (`juce_audio_processors`): `AudioPluginFormatManager`, `VST3PluginFormat`, `KnownPluginList`, `PluginDirectoryScanner`, `AudioPluginInstance`, `AudioProcessorGraph`, and the `AudioPluginHost` example app (a complete reference host with a plugin list, graph editor and editor windows).

**Licence is the blocker for linking it.** JUCE is dual licensed AGPLv3 / commercial. Only the core modules (`juce_core`, `juce_events`, `juce_audio_basics`, `juce_audio_devices`, ...) are ISC; `juce_audio_processors` (the hosting code) is not, so using it makes wavy AGPLv3 or requires a paid JUCE licence (tiers and prices found were dated, check current ones). The VST3 SDK is MIT, so wavy can host plugins with no such strings attached.

**Technical mismatch too.** JUCE's hosting assumes its own `MessageManager`/event loop and `AudioBuffer`/`MidiBuffer` types, and its VST3 wrapper asserts that many calls happen on the JUCE message thread. wavy's UI is newui with its own run loop, so we would be running two frameworks.

**Recommendation: do not link JUCE; use it as a reference.** Read, do not copy (copying AGPL code pulls the licence in):

- `juce_VST3PluginFormat*` and `juce_VST3Common.h`: how a real host sequences `initialize`/`setBusArrangements`/`setupProcessing`/`setActive`, which calls must be on the message thread, parameter and `restartComponent` handling, and the accumulated per-plugin workarounds. JUCE forum threads about specific plugins are a good list of quirks to test for.
- **Scanning**: JUCE's own forum reports show in-process scanning crashing the host and tripping thread-affinity asserts (VST3 wants its factory calls on the main thread). Its fix is the one already in this sketch: the host re-launches itself as a scanner subprocess (`PluginScannerSubprocess` in `AudioPluginHost`'s `HostStartup.cpp`, coordinated by `CustomPluginScanner` in `MainHostWindow.cpp`), the child replies with one XML description per plugin, and the parent blacklists whatever crashed. Note one forum poster's caveat: re-launching "yourself" does not work when the host is itself a plugin; irrelevant to wavy, which is an exe, but it means a small dedicated `wavyScan.exe` is the cleaner choice.
- `AudioPluginHost` UI for ideas on the plugin list/browser (fits 6f).

The better first reference is the SDK's own host sample (`public.sdk/samples/vst-hosting/audiohost` and `editorhost`, plus the `hosting` classes listed above), which is MIT and does exactly the lifecycle in steps 1-7. `editorhost` is also the reference for the `IPlugView` window parenting (risk 1).

Sources: [JUCE forum: VST3 plugin scanning crash protection](https://forum.juce.com/t/vst3-plugin-scanning-crash-protection/58485), [VST3 scan crash in AudioPluginHost](https://forum.juce.com/t/vst3-scan-crash-in-audiopluginhost/59786), [JUCE: help understanding licenses](https://forum.juce.com/t/help-understanding-licenses/69205), [JUCE on Wikipedia](https://en.wikipedia.org/wiki/JUCE), [VST 3 under MIT (Sound On Sound)](https://www.soundonsound.com/news/steinberg-adopt-mit-license-vst3).
The module-by-module licence split and the class names come from the search snippets plus my own knowledge of JUCE; confirm in the JUCE checkout's `LICENSE.md` before depending on them.

## Risks

1. **Editor window parenting.** newui must expose or create a native child HWND we can hand to `IPlugView::attached`, and forward focus and keyboard properly. Check newui first (it is the unknown with the least information).
2. **Plugin misbehaviour** (crashes, long `process`, allocation on the audio thread). Mitigation: SEH guard, per-instance CPU timing, drop to silence.
3. **Bus/layout variety** (mono in/stereo out, sidechain, multi-out instruments). v1: one stereo main in/out, ignore extra buses (leave them inactive).
4. **Threading contracts** of `IComponentHandler` and `IPlugFrame` are easy to get subtly wrong; test with several real plugins early, not only the SDK samples.
5. **Licences of individual plugins** are not our concern, but bundling any in a release is.

## Open questions

1. ~~Option A or B?~~ Answered: B, plugins are nodes. Open sub-question: does jitnodes' `Network` support nodes with more than one output port (needed for multi-out instruments and sidechain)? Check before 6b.
2. ~~Which plugins?~~ Answered: Surge XT first (instrument). Still open: a second, third-party effect to test with, ideally a commercial one, since the SDK samples and Surge are friendlier than most.
3. Is crash isolation (out-of-process) a day-one requirement, or acceptable as 6g?
4. Order versus the editor work: do stereo and 6a now, ahead of the audibility tweaks and clip editing?
