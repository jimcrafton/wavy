# Editor (phase 5)

wavy's editor is a DAW UI built on newui over `wavyCore`, borrowing Ableton Live's layout: a browser on the left, a
transport bar on top, the arrangement in the centre, and a tabbed bottom dock (Mixer / Piano Roll / Audio Edit / Devices).
Single window, no floating windows. The jitnodes node graph is not the main UI: it is the engine behind a track's device chain (each track and each
device compiles to a jitnodes `Network` -> `AudioProgram`). Reuse jitnodes' `GraphCanvas` later for a "patch" view of a
custom device, not for the main window.

Screens are HTML mockups in `editor/mockups/`, converted to `editor/ui/*.newui` with `html-mockup-to-newui` (skills in
`D:\code\jitnodes\.claude\skills`, `NEWUI_DIR=D:\code\newui`), never hand-edited.

## Minimum feature set

1. **Arrangement / timeline**: track headers (name, S, M, R, I, volume, pan); audio and MIDI clip regions with trim, split,
   fade and slip-edit tools; global ruler switchable between Bars+Beats, SMPTE timecode, Samples and Min:Sec; grid and
   snap (absolute/relative, 1/4, 1/8, ... , triplet and dotted); automation lanes that unfold under a track.
2. **Mixer**: a channel strip per track plus aux returns and master; insert slots (load, reorder, bypass); sends to aux
   tracks; peak and RMS/LUFS meters with clip indicators; input and output routing selectors per strip.
3. **Transport**: play, stop, record, pause, rewind, fast-forward; large position readout; tempo (automatable), time
   signature, key; metronome on/off with volume, count-in bars (pre-roll).
4. **Detail editors**: piano roll (draw, quantize, velocity/length/pitch); audio clip editor (phase alignment, transient
   detection, warp/stretch, pitch correction); automation breakpoint editing.
5. **Browser**: file system navigator for samples, loops and MIDI; plugin/instrument manager by type or manufacturer with
   drag onto tracks; audition player with auto-play synced to project BPM.

## UI rules

- Dark only: backgrounds `#121212`-`#1c1c1e`. Colour means state: red = record/clip, yellow/orange = solo, cyan = selection
  or focus; the playhead is a 1 px neon-yellow line with a triangle handle in the ruler.
- Track headers are a fixed, resizable column (default 240 px, mockup) locked to the timeline lanes; a chevron per header
  expands its automation lanes.
- Channel strips are narrow (76 px): colour bar by track type (audio grey, MIDI blue, aux green, master red), phase and
  link buttons, insert list (bypass on hover), input selector, R/M/S, fader rail with the meter beside it (not behind),
  level readout, output selector.
- Every fader, pan and parameter resets to default on double-click; knobs use infinite drag (cursor captured).
- Smart tool over a region: top half selects, bottom half grabs/moves, corners fade.
- Canvases (ruler, timeline, meters) are drawn with Blend2D from newui; the audio callback never touches UI state
  (parameter/meter traffic goes through `SpscRing`s, as in `AudioEngine`).

## Next-generation behaviours

Keep Live's rigid single-screen layout; upgrade the behaviour. Each item is a later slice, not part of the first build.

- **Focus bar** (`focusLabel`, top bar): selected track name and active tool.
- **Intent-driven browser** (5g): selecting an audio track filters the browser to audio effects and loops, a MIDI track to
  instruments. Hover-audition plays the sample on the cue output at project tempo and draws a transient ghost-waveform over
  the row; no click needed.
- **Module deck** (the dock's Devices tab, `deviceChain`): each device is a direct-manipulation view (an EQ is just its
  curve; touching a node pops Q/gain micro-sliders under the cursor and collapses on release). Plugin backgrounds react to
  the audio (compressor gain-reduction shadow) from the same meter ring. Needs each device to publish a small telemetry
  struct to the UI through an `SpscRing`.
- **Inline micro-waveforms**: clips draw a semi-transparent mini waveform or MIDI outline behind the name, from a cached
  per-clip peak pyramid (also used for zoomed-out arrangement drawing).
- **XY scrubbing**: a modifier or secondary drag on empty lane space zooms time horizontally and scales track height
  vertically, so the ruler is never needed.
- **Decided**: the arrangement ships first. Live's Session (clip-launcher) grid and scenes follow later as a second view
  over the same tracks and clips (Live shares tracks and mixer between its two views); keep the `Project` model free of
  anything arrangement-only so Session can be added.

| Component | Default | Hover | Active |
|---|---|---|---|
| Track volume fader | value (-3.2 dB) on a dark strip | 10% brighter, fine scale ticks | cursor hidden, infinite vertical drag |
| Insert rack | compact signal-flow block | bypass toggle and wet/dry thumb | click expands to the device's full view |
| Timeline audio clip | waveform in a solid box | micro-handles for fade in/out and gain | dragging an edge slip-edits (moves audio inside the clip) |

## Clips

A clip is a region plus a source, and the source is a jitnodes node graph. The timeline draws a clip by what its graph
produces; the engine treats every clip the same way.

```
Clip { track, start, length, contentOffset, loop, fades, source }
source = Sample   (file reader node; a template graph)
       | Notes    (event list; a template graph that emits events)
       | Graph    (user Network: generators, parameters, promoted knobs)
```

- Every graph has an **output kind**: audio or events. A Notes clip emits events into the track's instrument (`voices`,
  `adsr`, `AudioEngine::sendEvent`). An audio Graph clip is a generator (oscillators, noise, granular). An event Graph
  clip is a generative sequencer (arpeggiator, Euclidean pattern). "A generator driven by MIDI" = a Notes clip feeding an
  instrument.
- The instrument stays on the **track**. A per-clip synth would give each clip its own state and CPU cost. A Graph clip
  may still hold a full voice for a one-off sound.
- **Engine**: topology is fixed per `AudioProgram` and edits swap programs. Each track compiles to one program holding its
  device chain; a clip instance activates at a sample offset inside a block and gets `resetState` at that point. Moving or
  trimming a clip changes the time mapping only (no recompile). Editing a clip's graph recompiles, then `migrateState` and
  the one-block crossfade. Clip graphs read time as `sampleIndex` plus a clip-relative offset, so trim, loop and slip-edit
  never touch the graph. Adjacent clips with a fade keep two instances alive for the length of the fade.
- **Drawing**: audio and audio-Graph clips are rendered offline with `cook` into a peak pyramid (shared with the
  micro-waveforms); event clips draw note outlines. A graph that reads live input has no preview and shows a placeholder.
- **Promoted parameters**: a Graph clip exposes a few parameters as knobs in the clip panel (like Live's Clip View) and as
  envelope lines on the clip; everything else needs the Patch view.
- **Overlap**: clips may layer on a track (a mix per track); overlaps draw as sub-lanes. Decided in principle, confirm
  before 5c.
- **Dock editors**, one per source kind over the same clip model: Piano Roll (Notes), Audio Edit (Sample), Patch (Graph,
  reusing jitnodes' `GraphCanvas`). Double-click a clip opens its editor.

| Clip | Timeline drawing | Badge | Opens |
|---|---|---|---|
| Sample | waveform, fade handles | WAV | Audio Edit |
| Notes | note outlines | MIDI | Piano Roll |
| Graph, audio | rendered waveform, parameter envelope lines | GEN | Patch |
| Graph, events | note preview or pattern glyph | SEQ | Patch |

## Beyond Live: explored ideas

Mockups in `editor/mockups/variants/` (exploration only, not converted). `mainWindowAll.html` combines all of them with
numbered callouts; `zoomLevels.html` shows the zoom idea.

1. **Semantic zoom**: one canvas, three levels of detail. Arrangement (clip glyph) -> content (notes, pattern steps) ->
   graph (nodes with scopes on the wires and a probe panel). Double-click still jumps to the dock editor.
2. **Signal lanes**: a track unfolds into time-aligned lanes for each parameter, the clip's graph, and the output level
   (post-fader history); the "show lanes" toggles in the edit bar pick which. One mechanism for automation, modulation and
   telemetry.
3. **Modulation clips**: an LFO, envelope or generative curve is a Graph clip with control-value output, placed on a
   parameter lane and linked to its target; trim, loop and move work as for any clip. Depth lives in the inspector.
4. **Variations**: a clip holds alternates (A/B/C: different patterns or parameter snapshots) switched from the clip header
   or inspector, and auditioned in the loop. Gives Session-style experimentation without a second grid.
5. **Selection inspector**: right panel follows the selection (clip promoted parameters, variations, modulation, track and
   channel), so the dock is only the detail editor; the browser collapses to a rail.
6. **Live feedback** (`activityToggle`): clips under the playhead get a highlight, per-track CPU heat strip, event dots on
   sequencer clips, level history filled up to the playhead with the offline preview drawn dim ahead of it, gain
   reduction as a red band. Fed by the meter/telemetry `SpscRing`s.
7. **Unfolded graph strip**: the clip's nodes sit under its lane aligned to time (see `mainWindowUnfold.html`).

### Audio terms for a 2D/3D background

| Audio | Closest 2D/3D idea |
|---|---|
| Track | a layer, or an object in the scene hierarchy |
| Clip | an animation strip (NLA / Trax clip): a time range with content, movable and loopable |
| Playhead, position | the current frame |
| Sample rate (48 kHz) | frame rate, but 48,000 per second |
| Block / buffer (512 frames) | a render tile: the engine computes this many samples at once, and must finish before the next is due |
| Automation lane | an F-curve on one parameter |
| Modulation (LFO) | a driver or noise modifier on a curve |
| Device chain / inserts | a modifier stack; each device processes the result of the one before |
| Node graph (jitnodes) | a shader or geometry node tree |
| Mixer, sends, returns | compositing and render passes: tracks are summed, sends feed a shared effect (like a shared light or post effect) |
| Waveform | a 1D image of amplitude over time; peaks are its mipmap |
| MIDI | keyframe-like events (note on/off, velocity) that a synth turns into sound |
| Latency / real time | the viewport must hit its frame time every time, or you hear a glitch (a dropped frame is audible) |

## Reference: Live 12 layout (from the Live 12 manual, "Live Concepts")

Web search returned no usable screenshots (manual pages only have figure captions), so this is the structure from text.
Ideas worth copying into later slices:

- **Control Bar** sections, left to right: browser toggle; tempo, time signature, metronome; scale; Follow toggle and
  arrangement position; transport (play/stop, record); automation arm, overdub, capture MIDI; loop and punch in/out;
  draw mode, computer-keyboard MIDI, key/MIDI map, sample rate, CPU; view selector. Ours is close; missing so far: Follow,
  loop/punch region, draw mode, sample-rate readout.
- **Status bar** at the bottom: errors, background-analysis progress, and in the note editor the hovered note's position,
  pitch and velocity.
- **Browser**: headphones toggle for auto-preview, "show similar files" on a sample.
- **Detail view**: Clip View (clip panel left, editor right) and Device View can be stacked, not only tabbed. Consider
  allowing both at once in the dock.
- **Overview** strip above the arrangement for whole-song zoom/navigation; per-track "back to arrangement" buttons are
  Session-related.
- **Mixer** sections (sends, I/O, crossfader) are individually show/hide.

## Reference screenshots (`.claude/attachments/paste-20261029-*.png`, third-party, not for the repo)

Seven images: Ableton Live 12 (arrangement with piano-roll clips, Device View with Meld/Roar/Saturator, MIDI editor) and
Nenad Milosevic's Live redesign (Session view, mixer strips, device slots). What to take from them:

- **Control bar as labelled groups**, each with a small caps heading (TEMPO, SIGNATURE, LOCATORS, TRANSPORT, ENVELOPES,
  LOOP, UTILITIES, STATUS, VIEW), separated by whitespace rather than boxes; status group shows CPU, disk, MIDI, track
  and external-sync activity as tiny blinking indicators. Our transport row should be grouped and headed the same way,
  and gets: Link/Tap, locators (set/prev/next), loop and punch, key/scale, sample rate, I/O + S/R/M/E/D/X view toggles
  (sends, returns, mixer, envelopes, delay, crossfade) at the far right.
- **Arrangement**: an overview strip at the top (whole song, with the visible window outlined); ruler with bar numbers
  and a second time-in-seconds ruler at the bottom; loop brace over the ruler; clip colour = track colour, with
  note/waveform drawn inside in a darker shade, and clip name tabs ("Main", "Var1") on the top edge; a clip's selected
  region is tinted grey-blue. **Track headers are on the right** in Live's arrangement and carry: collapse chevron,
  name, input type, input channel, monitor In/Auto/Off, output, track number button (arm/activate, yellow), S, arm, volume
  readout, pan, and a thin level meter; folded tracks shrink to a single row of the track colour. Return tracks (A Delay,
  B Reverb) and Main sit at the bottom with a Post toggle. (We put headers on the left; either works, decide once.)
- **Strips**: colour bar on top, name, clip/scene slots, two send knobs (Delay/Reverb) with dB readout, a yellow peak/vol
  readout, pan knob with L/R label, a mono vertical meter with dB scale (0, 12, 24 ... 72) and a yellow triangle
  fader marker, track number button (yellow = active, blue = send on), S, arm. Strips have In/Out/Monitor selectors at the
  top and a Track Delay field at the bottom. Active/armed state is yellow, selection cyan, record red.
- **Device slots**: a vertical list per track; each device is a row with a green power dot (grey = bypassed), name, a
  mini-visual (distortion transfer curve, EQ curve, compressor shadow, spectrum, directivity) and small inline controls.
  Empty slots are plain grey rows. Our "module deck" and the rack share one device-view component.
- **Device view** (bottom dock): devices side by side in horizontal panels, each with title bar (power dot, name,
  preset name, hot-swap/save icons) and a big interactive graph (EQ eight, compressor transfer/gain-reduction trace,
  envelopes, LFOs, modulation matrix, filter response); knobs are small, yellow arc on grey, value below. Selected
  band number buttons across the top of EQ. "Drop audio effects here" empty state at the right end.
- **MIDI editor** in the dock: left panel (Clip, transform tools, velocity-shaper curve), keyboard strip, note grid with
  highlight-scale shading, velocity lane at the bottom, Fold/Scale/Highlight Scale toggles, Notes/Envelopes/MPE tabs, grid
  division (1/8) at the right of its header.
- **Browser**: categories (Sounds, Drums, Instruments, Audio Effects ...), places (Packs, User Library, Current Project),
  favourites, a search box, a result list with CPU-usage percentages next to effects, and a waveform strip with a headphones
  toggle at its bottom for audition.
- **Palette and type**: dark grey (not black) panels `~#2b2b2b`-`#3a3a3a` with darker wells, one warm yellow accent
  (`#ffc400`-ish) for active state, cyan for focus/value, green for meters, small caps labels, 11 px sans. Our current
  mockup is darker and bluer than this; decide before the colours get wired into code.
- **Status bar** at the bottom shows the hovered/selected object (Time Selection start/end/length) and a quick-access
  strip of recent devices (EQ, Comp, Reverb) at bottom right; Workspace/Display Zoom/Detail Level controls bottom left
  in the redesign.

## Visual effects (newui)

Palette decided: dark but lighter than first drafted (`#1a1a1d` window, `#252528` panels, `#151517` wells), **cool blue
`#3fb4f0` for active/selected**, red record/clip, orange solo, green meters, yellow only for the playhead.

- **Gradients**: panels and wells carry a subtle top-to-bottom gradient in the mockup (`linear-gradient` written inline;
  the converter does not resolve one held in a CSS variable) and convert to `Fill`/`Gradient` on the view style.
- **Elevation shadows**: `ViewStyle::elevation` is in the `.newui` schema but the converter never writes it, so code sets it
  by view name after loading (dock, browser, popups, dragged clips) rather than hand-editing the generated file.
- **Glow and drop shadow** exist only on `gfx::ShapeStyle` (vector shapes), so they apply to what we draw ourselves on
  the canvases: playhead glow, selected-clip outline glow, clip drop shadow while dragging, clip/peak LED glow on meters,
  EQ/compressor curve glow in the device graphs. They cost an offscreen box blur per shape, so cache them per redraw
  region and keep them off anything that repaints every frame except the playhead and meters, and measure.
- **Opacity**: muted and out-of-loop clips at reduced opacity; ghost waveforms in the browser audition.
- Native buttons and toggles use the OS theme (UxTheme), so their colours cannot be styled from the mockup; custom
  transport/track buttons (R, M, S, play) will be small owner-drawn views to get the blue/red/orange states and glow.

## Mockups

| File | Views (by `name`) |
|---|---|
| `mainWindow.html` | Transport: `rewindButton`, `playButton`, `stopButton`, `pauseButton`, `recordButton`, `forwardButton`, `positionLabel`, `focusLabel`, `bpmField`, `timeSigField`, `keyField`, `metronomeToggle`, `clickVolume`, `countInSelect`, `rulerModeSelect`, `gridSelect`, `snapToggle`, `snapModeSelect`, `toolSelect`, `mixerTab`, `pianoRollTab`, `audioEditTab`, `deviceTab`, `cpuLabel`. Browser: `browserTabs`, `browserPath`, `browserList`, `auditionButton`, `autoPlayToggle`, `auditionLabel`. Arrangement: `rulerCanvas`, `trackHeaders`, `timelineCanvas`. Dock: `detailTitle`, `detailCanvas`, `mixerHost`, `deviceChain`. Status: `statusText` |
| `mixerView.html` (fragment, loaded into `mixerHost` in the dock) | `mixerStrips` (one strip per track built in code from the sample strip), `masterStrip` |

`mainWindow.html` now shows the clip model: four tracks with Sample, Notes, Graph (audio) and Graph (events) clips, a
folded track, a bass automation lane, loop brace, playhead, a selected Graph clip with its promoted parameters, a Patch
tab (clip graph) and the device chain. The edit toolbar (tool, ruler, grid, snap) moved from the control bar to a slim bar
at the top of the arrangement because the control bar overflowed 1440 px. The clips, rulers and node boxes are sample
children for review; the real app draws them on canvases. Two variants to compare live in `editor/mockups/variants/`
(not converted yet, convert the chosen one over `mainWindow`):

- `mainWindowUnfold.html`: a Graph clip unfolds in place under its lane as a time-aligned signal-flow strip (nodes as chips,
  parameter envelope drawn over time), like an automation lane; the dock shows the module deck (EQ curve, compressor,
  reverb panels).
- `mainWindowInspector.html`: browser collapses to a 44 px rail, track headers shrink (routing moves out), and a right
  inspector follows the selection (clip promoted params, track I/O, channel fader/sends/meter), so the dock is only the
  detail editor.

Canvases (`rulerCanvas`, `timelineCanvas`, `detailCanvas`) and the scrolling host rows (`trackHeaders`, `browserList`,
`mixerStrips`) are placeholders filled in code; the sample rows in the mockup show the intended look.

### Mockup status (2026-10-30)

| File | State |
|---|---|
| `mockups/mainWindow.html` | The baseline. Shows the clip model (Sample, Notes, Graph audio, Graph events), a folded track, a bass automation lane, loop brace, playhead, selected Graph clip, Patch tab, device chain. Converted to `ui/mainWindow.newui`, 0 validator errors |
| `mockups/mixerView.html` | Unchanged since slice 5a, converted |
| `mockups/variants/mainWindowUnfold.html` | Exploration. Graph clip unfolds under its lane; dock shows the module deck. Not converted |
| `mockups/variants/mainWindowInspector.html` | Exploration. Browser rail, slim headers, right inspector. Not converted |
| `mockups/variants/mainWindowAll.html` | Exploration. Everything from "Beyond Live" in one window with numbered callouts and a legend. Not converted |
| `mockups/variants/zoomLevels.html` | Exploration. The semantic-zoom idea as three frames. Not converted |

- **Variants are for deciding, not shipping.** Nothing from them is in the editor until the user picks. When one is chosen,
  fold it into `mainWindow.html` (the converted file) and reconvert; keep the others for reference.
- **Open decisions**: which "Beyond Live" ideas go into the first build (suggested: inspector, collapsed browser rail,
  unfolded graph strip, variations; later: modulation clips, live feedback, semantic zoom); whether clips may layer on a
  track; track headers on the left (assumed, not confirmed).
- **Why the variants are not converted**: they use `position:absolute` sample content (clips, nodes, `svg` waveforms) that
  the real app draws on canvases, so converting them would only produce placeholder views.
- **View a mockup**: open the `.html` in a browser, or screenshot it headless:
  `msedge --headless --disable-gpu --hide-scrollbars --window-size=1440,900 --screenshot=out.png file:///D:/code/wavy/editor/mockups/<file>.html`
  (Edge is at `C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe`).
- **Mockup conventions learned**: set `box-sizing: border-box` on `body` and on fixed-height header rows or lanes drift;
  the control bar must fit 1440 px (the edit toolbar lives at the top of the arrangement for that reason); keep `id`s on
  anything the app will look up.

## Slices

1. **5a Shell**: mockups, conversion, `wavyEditor` exe loading the frame and checking every named view (`--check`).
2. **5b Project model**: `Project` (tracks, clips, tempo map, markers) with undo and save/load; headers and mixer strips
   built from it.
3. **5c Arrangement**: ruler (all four modes), grid and snap, clip drawing, selection, move/trim/split/fade, zoom/scroll.
4. **5d Transport and sound**: position, play/stop/record, metronome, count-in; the project renders through
   `AudioEngine` (clip playback via jitnodes sources, track chains compiled to `AudioProgram`, swap on edit).
5. **5e Mixer**: strips, inserts, sends/returns, meters (audio->UI peak/RMS ring), routing.
6. **5f Detail editors and automation**: piano roll, audio clip editor, automation lanes.
7. **5g Browser**: file navigator, plugin manager, audition.

Plugin hosting (VST3) is phase 6; until then inserts are wavy's own jitnodes devices.
