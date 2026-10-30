# Audio in wavy on top of jitnodes

Findings from `docs/building a DAW.docx`, `docs/asio.docx`, `docs/Dealing with WASAPI.docx` (all AI chat transcripts)
checked against the real jitnodes sources (`3rdparty/jitnodes`, branch `audio`, which has no audio code yet).

## What the three documents agree on

- Hard real-time rule: no allocation, locks, I/O or logging in the device callback.
- The graph is edited and compiled on the main thread; the audio thread only runs an immutable, prebuilt program,
  handed over through an atomic swap / SPSC queue; retired programs are freed on the main thread.
- Feedback only through an explicit one-block delay pair (sink/source), keeping the graph acyclic.
- Sample-accurate events by splitting the block at event offsets; parameters as streams that fall back to a constant.
- Driver layer: native WASAPI (exclusive, event-driven, `IAudioClient3`, MMCSS) and ASIO behind one interface.
- ASIO doc adds: int32/int24 <-> float SIMD conversion, `requestReset` handling, crossfade when swapping programs,
  state migration between program versions, and extras (VST3 host node, pitch shift/SRC, parallel DFD scheduling).

## The one thing the chats get wrong for this codebase

They say: call `net.cookDisplay()` from the audio callback. That cannot work, because a cook is not real-time safe:

- `Network` is non-copyable and not designed to be cloned for a "shadow" copy; undo uses snapshots, not live copies.
- `cook` plans per call (`planCook`, `CookTask` vectors), allocates `Geometry`/`AttribArray` (copy-on-write; results are
  `shared_ptr<const Geometry>`), takes shared_ptr refcounts, and runs levels on a `ThreadPool`.
- Dirty-flag caching assumes edits are rare; audio would dirty every time-dependent node every block.

What is real-time safe is the kernel itself: `Kernel::run(void* const* channels, const float* params, size_t paddedCount,
ThreadPool* = nullptr)`, ABI `void(float* const* channels, const float* params, const float* consts, size_t paddedCount)`.
It is pure SIMD over caller-owned, 64-byte aligned, 16-lane-padded arrays and needs nothing else.

## Proposed architecture

Use jitnodes as the authoring model and compiler, not as the audio-thread executor.

```
main thread                                   audio thread (device callback)
-----------                                   ------------------------------
jitnodes Network (editor, undo, assets)       AudioProgram (immutable, preallocated)
   -> AudioCompiler: plan + fuse + compile        for each Step: Kernel::run(chans, params, n)   // pool = nullptr
   -> AudioProgram {steps, buffers, state}        plus hand-written stateful steps
   -> atomic swap (+ crossfade)  ------------>    drain events / param SPSC ring, write device buffer
   <- retired programs freed here
```

- **`AudioProgram`**: flat list of steps in topological order; each step is a compiled `Kernel` plus channel pointers
  into one preallocated, aligned buffer pool (slots assigned by liveness analysis at compile time, so the callback
  never leases or releases). Block size is fixed per program; changing it rebuilds the program.
- **Reuse jitnodes fusion.** `composeKernels` already fuses chains of point-wise nodes into one kernel with intermediates
  in registers. Gain, mix, waveshaper, crossfade, CV scaling, wrangle DSP all fuse. This is the biggest win and costs nothing.
- **Reuse the wrangle** as the user-facing DSP node. Samples are points, channels are attributes (`left`, `right`, `cv`).
  Needed additions, all in the expression layer: constants `sr`, `invSr`, a per-sample `index`/`sampleIndex` load channel,
  and block-level `time`/`blockStart` as parameters (parameters are per-block, which gives block-rate control for free).
- **Params and CV**: a parameter is block-rate (the existing `params` block, written by the main thread via an SPSC ring and
  applied at block start) or sample-rate (a channel). Compile different kernels for wired vs unwired, as the chat says;
  jitnodes' signature cache already keys on channel names, so this is just a different `KernelSpec`.
- **Domain naming**: do not rename `Geometry` to `DataBlock` as the chat suggests. Keep `Geometry` as the carrier at
  compile time (it is what kernels bind to) and expose a thin `AudioBlock` view in wavy. A rename would churn the
  whole library and its editor for a cosmetic gain.

## State, the real gap

The kernel ABI is stateless and fully data-parallel across elements. Audio needs state:

| Need | How it fits |
|---|---|
| Oscillator / LFO phase | Stateless per block: `phase = fract(phase0 + index * inc)`; `phase0` is a param written back by the host from `phase0 + n*inc` after the block. No kernel change. |
| Gain, mix, waveshaper, CV math | Pure point-wise; fuses. |
| FIR, delay line, feedback delay | Read a ring/history channel at offset; history is an extra input channel padded in front of the block (copy last N samples), still point-wise. |
| IIR / biquad / envelope follower (sequential in time) | Not parallel across samples. Two options: (a) hand-written scalar C++ step with a per-node state struct; (b) **map voices/channels to points** and let SIMD run across them, with the time loop inside a new "recurrence" kernel type (loop over samples, state in registers, load/store state per block). (b) is the natural jitnodes generalisation and needs a new `KernelSpec` kind; start with (a). |
| Polyphony | Voices as points (SIMD across voices) for the recurrence kernel, or per-voice hand-written step. |

A per-node `StateBlock` (aligned, preallocated by the compiler, migrated by node id on program swap) covers all rows.

### As built (phase 3b)

Sequential nodes implement `Node::audioStep()` (an `AudioStep`: state size plus a scalar `process` over the x channel); `delay`
and `biquad` are the first two. History is held inside the step's `StateBlock` (a ring for `delay`, `z1/z2` for `biquad`)
rather than as a padded input channel, because kernels cannot read other points. The compiler splits the graph at a step:
the chain above it is its own kernel stage, the step runs on that stage's x, and the chain below reads the step's buffer as an
ordinary source. Steps can sit inside `mix` branches. An FIR filter is currently a `mix` of `delay`s with gains; a dedicated
node can follow if profiling asks for it.

### As built (phase 2)

`wavy::AudioEngine` (`include/wavy/audioEngine.h`) is the device-independent audio side. A device callback calls `render(out, frames)`
with any frame count; the engine serves the program's fixed-size blocks from a staging buffer.

- **Params**: `setParam(node, index, value)` pushes onto a 1024-slot SPSC ring (`spscRing.h`); the audio thread drains it at the start
  of each block through `AudioProgram::tryParam` (noexcept; unknown nodes or indices are ignored).
- **Swap**: `swapProgram(next)` parks the program in an atomic slot (a program that was never picked up is replaced and freed on the
  main thread). At the next block boundary the audio thread migrates state into it, runs both programs for that block and crossfades
  linearly old to new, then pushes the old program onto a 16-slot retire ring. A swap is only accepted while the ring has room, so the
  audio thread never frees anything; `collectRetired()` on the main thread deletes. Block size and output count must match the engine's.
- **ASIO**: `AudioDevice` (`audioDevice.h`) with an ASIO backend in `src/asioDevice.cpp`, built only when `ASIO_SDK_DIR` (CMake
  cache variable or environment variable) points at the SDK; otherwise `makeAsioDevice()` returns null. Output only for now, using the
  driver's preferred buffer size; float32/64 and int16/24/32 little-endian sample types (others play silence). Driver reset and
  sample-rate messages set `resetRequested()` for the main thread to reopen and rebuild.
- Not done: input channels, a device-driven block size different from the program's (the engine copes, but there is no helper to pick
  one), warming the JIT stack on the device thread, and a hands-on test against a real driver.

## As built (phase 4)

- `jitnodes::EventBuffer` holds up to 256 `AudioEvent`s (offset, status, data1, data2) sorted by offset; `AudioProgram::process(out, &events)` hands it to every step. `AudioStep::processEvents` is the hook; steps split the block at event offsets so each event lands on its exact sample.
- `adsr` gates x with a linear envelope: any note on opens it, it releases when all held notes are off. `voices` is a polyphonic oscillator bank added into x. Params: attack/decay/release in samples, sustain 0-1, `waveform` (0 sine, 1 saw, 2 square, 3 triangle), `sampleRate`; `voiceCount` (1-64) is fixed at compile.
- `wavy::AudioEngine::sendEvent` queues events through a lock-free ring (one producer thread); `wavy::MidiInput` feeds it from a winmm device. Hardware events carry no timestamp mapping yet, so they land at the start of the next block.

## Events, feedback, swaps

- **MIDI/events**: a fixed-capacity `EventBuffer` per block (offset, status, data) delivered to event-consuming steps.
  Stateless steps do not care. Event-driven steps split the block at event offsets and call the kernel per sub-range
  (the kernel takes `paddedCount` and pointers, so shifting channel pointers works, as `Kernel::run` already does for its
  own parallel chunks). Keep sub-ranges multiples of 16 where possible or accept scalar tails via padded copies.
- **Feedback**: `fbSink`/`fbSource` node pair the compiler treats as a cycle break (one block of latency, or `blockSize`
  minimum delay). `Network::connect` rejects cycles today; the pair keeps that rule intact because they are not wired.
- **Program swap**: atomic exchange of a `std::atomic<AudioProgram*>` plus a retire slot/ring freed by the main thread
  (fix the chat's "delete on the audio thread if the slot is full" by using a ring of retire slots sized well above the
  UI rate). Crossfade old->new over one block; migrate `StateBlock`s whose node id and type survive.
- **Threading**: pass `pool = nullptr`, so the callback never touches the thread pool. Multi-core later by splitting
  independent branches across a dedicated real-time worker set (separate from the editor pool), after profiling.
- **Warm the JIT**: `Kernel` commits 256 KB of stack per thread before first run. Do this on the audio thread once at
  startup (before streaming), not lazily in the first callback.

## Device layer

One interface, two backends, behind `wavy::AudioDevice` (open/start/stop, sample rate, block size, channel counts,
a callback `void(const float* const* in, float* const* out, uint32_t frames)` with planar float):

1. **ASIO is the primary backend** (decision: it is what professional DAWs and pro interfaces use). The SDK is not vendored (see
   licensing below): build the first device against `ASIO_SDK_DIR`. Handle `kAsioResetRequest` / buffer-size changes by tearing
   down and rebuilding the program on the main thread; convert driver sample types (int16/24/32 LSB/MSB, float32/64)
   <-> planar float with SIMD, the loops the ASIO doc sketches. ASIO has no device enumeration beyond the registry,
   so list drivers from `HKLM\SOFTWARE\ASIO`.
2. **WASAPI later, as a fallback** for machines with no ASIO driver (exclusive + event-driven + `IAudioClient3`, MMCSS
   `"Pro Audio"`). Same `AudioDevice` interface, so it costs little to add. ASIO4ALL-style wrappers also cover this.
3. Defer miniaudio/RtAudio unless a third platform is wanted.

### ASIO licensing (decided: Steinberg proprietary agreement)

Steinberg also offers the SDK under GPLv3 (Oct 2025; [heise](https://heise.de/-10963451)), which would force wavy to be
GPLv3. We use the proprietary *ASIO SDK Licensing Agreement* instead. Terms read from v2.2 (Oct 2005); check the version
shipped with the SDK you download (2.3):

- **The SDK must not be redistributed** (§2.2, "including the Internet"), so it is never committed to git and never
  vendored. Each developer downloads it from Steinberg and points CMake at it with `ASIO_SDK_DIR` (cache variable or
  environment variable). The ASIO backend is compiled only when it is set, so a checkout without the SDK still builds.
  `3rdparty/asio/` is git-ignored as a safety net.
- **Before any release** that uses the SDK: send Steinberg the signed agreement (§2.5). Free, no royalties (§4).
- **Attribution (§3)** in shipped builds: documentation line "ASIO is a trademark and software of Steinberg Media
  Technologies GmbH"; ASIO logo on packaging/promotional material; about box text "ASIO Technology by Steinberg Media
  Technologies GmbH".
- Third parties publishing wavy under their own name must accept §2.1-2.3 and §3 (§2.4).
- Provided as-is, no indemnity (§5-7); Steinberg may terminate after an uncured material breach (14 days); German law,
  Hamburg courts.

Do not copy SDK headers or sources into `docs/`, tests or examples either.

## Caveats about the source docs

They are unreviewed chat output. Sample code is illustrative and has bugs; do not copy it. Known examples: the arena
`releaseBuffer` pushes onto the same stack it leases from (breaks with out-of-order release), `std::pow`/`std::sin` per
sample per voice in the "RT" synth, the discard-slot fallback `delete`s on the audio thread, and the asm sketches use
the camelCase asmjit API (this repo is v1.21 snake_case). Treat them as problem statements, not designs.

## Suggested phases

1. **Offline**: `AudioProgram` compiler from a jitnodes `Network` of audio nodes (osc via `index`, gain, mix, wrangle)
   rendering N blocks to a WAV in a test, compared against the scalar interpreter. No device yet. Adds `sr`/`index` symbols.
2. **Device**: `AudioDevice` + ASIO backend, callback runs a program, SPSC param ring, program swap with crossfade.
3. **State**: `StateBlock`, delay/FIR, scalar IIR steps, feedback pair, state migration on swap.
4. **Events**: `EventBuffer`, MIDI in, sample-accurate split, ADSR, polyphonic voices.
5. **Editor**: newui audio node palette, meters (audio->UI ring), transport/timeline.
6. **WASAPI fallback backend**, then optional: recurrence kernel type, VST3 host node, SRC/pitch.

## Open decisions

- Where the code lives (decided direction): jitnodes is enhanced on its `audio` branch, which `3rdparty/jitnodes` tracks.
  Suggested split: kernel/graph-level additions in jitnodes (`sr`/`index` symbols, state blocks, recurrence kernels,
  feedback pair, event buffer type, an `AudioProgram` compiler since it needs `Network`/fusion internals);
  devices (ASIO), transport, MIDI input, editor and app in wavy. Open: does `AudioProgram` belong in jitnodes or wavy?
- Resolved: ASIO under the proprietary agreement, SDK kept out of git (see above). Still to do: submit the signed
  agreement to Steinberg before the first release.
- Fixed block size (64/128/256) vs device-driven; suggest fixed per program, rebuilt on change.
- Float only (matches kernels) vs also double; suggest float.
