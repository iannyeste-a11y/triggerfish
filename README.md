# Radium Importer

`Triggerfish` is a Windows-first desktop app for importing layered `.radium` presets and providing a practical sound-design environment for working with them.

The project is intentionally focused on two real workflows:

1. One-shot layered events
   - Example: footsteps, creature movement, impacts
   - One trigger should fire the active layers once, in sync
   - A layer may choose one authored region or source variation per trigger

2. Continuous evolving beds
   - Example: fire, machinery, texture beds
   - Layers can sustain while held and later stop on release

This is not a clone of any third-party tool and it is not trying to reproduce every control in any reference manual.

## Platform

- Runtime target: native Windows 10 desktop app
- Typical development setup: WSL workspace on a Windows 10 host
- Real builds, playback checks, and plugin validation must be done natively on Windows

## Current State

The app is now beyond parser-only work. It currently supports:

- importing `.radium` preset files from the fixture corpus
- reading SQLite-based `.radium` containers
- extracting embedded audio and reconstructing an internal preset/layer model
- app-native project save/load through `.riproj`
- adding local audio files to layers
- isolated layer audition
- real-time streaming playback from the virtual keyboard with live parameter feedback
- record-last and render/export WAV output (offline path preserved)
- manual authored trigger regions
- focused waveform editing with zoom and draggable region boundaries
- per-layer gain, mute, solo, reverse, and layer-level VST3 insert slots
- real-time gain, mute, and solo changes heard immediately during playback
- basic VST3 plugin assignment, editor opening, per-layer processing, and project persistence

## What Is Working Now

### Import and Playback

- `.radium` fixtures import without the earlier preview-device failures
- embedded source audio is decoded and mapped into the app model
- stereo sources now remain stereo
- mono sources remain mono
- imported presets can be auditioned layer-by-layer
- imported presets can be triggered from the virtual keyboard using a real-time streaming engine
- keyboard triggers pitch-shift by note
- parameter changes (gain, mute, solo) are heard immediately during playback
- playback uses callback-driven WASAPI streaming instead of pre-rendered static buffers

### App-Native Editing

- local audio can be added to layers
- projects can be saved and reloaded as `.riproj`
- imported sessions and local-authored content can coexist
- gain changes on visible layers update playback in real time during streaming

### Region Workflow

- selecting a layer shows a focused waveform editor
- the focused waveform is larger and denser than the strip waveform
- `Shift + mouse wheel` zooms the focused waveform
- left-click starts playback from the clicked point
- left-drag creates a loop preview selection
- right-drag creates a trigger region
- authored trigger regions show visible boundaries
- authored trigger region boundaries can be dragged to resize the region
- keyboard triggers can randomly choose among authored regions on a layer
- no-immediate-repeat behavior is applied for authored multi-region layers

### Recording and Export

- layer audition writes and previews temporary WAV output
- full preset trigger output can be recorded
- render/export writes WAV output
- output path and recording flow are usable enough for real testing, though not yet polished

### VST3 Inserts

The project now has a working VST3 path even though the original v1 scope did not plan for plugin hosting.

Current VST3 behavior:

- there is a saved global VST3 folder path
- the path persists across sessions in `artifacts/ui/app_config.txt`
- each layer has 5 insert slots: `A` through `E`
- each slot is assigned from a dropdown
- insert buttons toggle the selected slot’s plugin UI on or off
- only one plugin UI is intended to be open at a time
- multiple insert slots can be populated on a layer
- known-good plugins can process layer audition audio
- known-good plugins can process virtual-keyboard playback
- plugin assignment and saved plugin state are persisted in `.riproj`

## How The App Works Right Now

### Main UI

The current Win32 app is organized around:

- preset header and status area
- 5 visible layer strips at a time
- vertical scrolling through the imported layer set
- a focused layer editor area
- one-shot / continuous trigger mode toggle
- render/export and record controls
- octave-ranged virtual keyboard

### Layer Strips

Each visible layer strip currently exposes:

- layer name / source label
- mute
- solo
- gain
- auto-split button
- waveform strip

Notes:

- mute/solo/gain are present and useful
- solo is reliable for isolating layer behavior
- auto-split exists, but it is still only a helper and not yet dependable enough to be the primary region-authoring workflow

### Focused Layer Mode

When a layer is selected, the focused section is the main editing area.

Current focused-layer workflow:

- inspect the larger waveform
- zoom with `Shift + mouse wheel`
- left-click to audition from a point
- left-drag to audition a loop span
- right-drag to create a trigger region
- drag region start/end handles to refine timing
- use layer insert slots to assign and open VST3 effects

### Triggering

The virtual keyboard is currently the main trigger surface.

What it does now:

- clicking a keyboard key starts real-time streaming playback of the active preset stack
- clicking the same key again stops playback
- authored trigger regions can be chosen per layer at trigger time
- gain, mute, and solo changes take effect immediately during playback — no re-trigger needed
- playback uses a lock-free callback architecture similar to professional DAWs
- isolated layer preview and full preset trigger are both working for real use

### Playback Architecture

The app now has two playback paths:

1. **Real-time streaming** (keyboard triggers, primary listening path)
   - uses `StreamingMixer` with per-block `render_block()` callbacks
   - WASAPI shared-mode streaming thread requests audio in small chunks
   - `LiveParams` (gain, pan, mute, solo) are read atomically each block
   - UI thread writes to `LiveParams` with no locks — changes are heard within milliseconds
   - recording captures the streamed output for "Record Last"

2. **Offline rendering** (export/render path)
   - uses the original `PlaybackEngine::render_one_shot()` / `render_continuous()`
   - produces a complete buffer for 24-bit / 48 kHz WAV export
   - used by "Render / Export" and direct WAV output

### Project Files

The internal project format is `.riproj`.

It currently stores:

- project trigger mode
- authored layer overrides
- added local sources
- authored trigger regions
- per-layer VST3 insert assignments and saved plugin state
- enough layer information to reopen and keep working

## Purpose Of The Current Build

If the project had to be resumed later with no conversation history, the current build should be understood as:

- a working `.radium` importer
- a working layered playback editor for one-shot work with real-time parameter feedback
- a usable manual region authoring tool
- a real-time streaming playback engine with DAW-like live parameter control
- an offline render/export engine for 24-bit / 48 kHz WAV output
- a partly working continuous-capable playback mode
- a partly working VST3 host with real per-layer insert processing

The most valuable working path today is:

1. Import a `.radium` preset
2. Solo/select layers
3. Manually define trigger regions in focused mode
4. Adjust gain and layer balance
5. Add VST3 inserts where needed
6. Trigger from the virtual keyboard
7. Record or export the result

## Important Known Limitations

### Playback

- real-time parameter feedback works for gain, mute, and solo
- pan is present in the streaming engine but not yet exposed in the UI
- built-in FX (reverb, delay, bass boost) are currently only applied at trigger time, not updated live
- VST3 plugin processing is still per-trigger (offline), not yet integrated into the real-time streaming path
- continuous mode uses the streaming engine but has not had the same level of polish as one-shot

### Auto-Split

Auto-split is still not reliable enough to trust as the primary authoring path.

Known issues:

- it may miss obvious hits
- it may start regions late
- it may merge nearby hits incorrectly
- it varies too much across different real-world layer material

Conclusion:

- manual region editing is the real workflow for now
- auto-split should be treated as an optional helper only

### VST3 Stability

The VST3 path is real and useful, but it is still the roughest subsystem in the app.

Known issues:

- some plugins work consistently
- some plugins work only in certain actions
- some plugin swaps still expose unstable behavior depending on the exact plugin
- plugin load/open/teardown is still much less robust than a mature DAW host
- plugin performance and UI responsiveness can feel heavy

Important architectural note:

- plugin enumeration is partly moved into a helper worker process
- actual editor lifetime and audio processing are still not fully isolated from the main app
- this means plugin-host robustness is improved from earlier builds, but not yet DAW-grade

### Built-In FX

The older built-in FX path is no longer the priority path.

Current practical rule:

- `Reverse` remains exposed and useful
- real layer effect work should be treated as VST3-first now
- the older built-in DSP should be considered fallback code, not polished sound-design tooling

### Continuous Workflow

Continuous mode exists in the engine and uses the same streaming architecture, but it has not had the same level of polish as one-shot work.

This means:

- one-shot workflow is the main stable path today
- continuous-bed authoring still needs a dedicated polish pass later

## Known Good / Known Rough Areas

### Known Good

- `.radium` import
- embedded media extraction
- layer audition
- one-shot keyboard triggering
- stereo preservation
- manual trigger-region authoring
- focused waveform zoom/editing
- `.riproj` save/load
- real-time gain/mute/solo changes heard immediately during playback
- multiple VST3 insert slots existing in the UI
- opening one insert UI at a time with insert buttons

### Still Rough

- auto-split quality
- plugin swap/clear behavior across all third-party plugins
- VST3 real-time streaming integration (currently per-trigger only)
- continuous-mode polish
- built-in FX live parameter updates
- recording/export UX refinement
- broader import fidelity for every Radium semantic

## Suggested Resume Priorities

If work resumes later, these are the most useful next priorities.

### 1. VST3 Real-Time Streaming Integration

The biggest remaining gap between the app and a DAW-like workflow.

Priority work:

- integrate VST3 `process_block()` into the `StreamingMixer` per-buffer callback
- allow plugin parameter changes to be heard during playback
- ensure plugin thread safety in the real-time audio path
- improve plugin swap / clear stability

### 2. Live Built-In FX

- move built-in FX (reverb, delay, bass boost, compressor) into the streaming path
- allow FX parameter changes to take effect during playback

### 3. One-Shot Workflow Polish

- keep manual region editing fast and precise
- improve region editing UX before investing more in auto-split
- keep synced trigger behavior stable across imported presets

### 4. Continuous Workflow

- polish loop/sustain/release behavior in the streaming engine
- add dedicated loop/pacing/randomness polish for held textures

### 5. Import Fidelity

- improve reconstruction of imported semantics where it clearly matters to audible behavior
- keep uncertain semantics visible instead of silently guessing

## Files And Subsystems To Know About

If the project needs to be resumed later, these are the key code areas:

- `src/radium_parser.cpp`
  - SQLite/container parsing and fixture format inspection
- `src/import_model.cpp`
  - maps parsed data into the internal preset/layer model
- `src/playback_engine.cpp`
  - offline render engine for one-shot / continuous playback and WAV export
- `src/streaming_mixer.cpp`
  - real-time callback-driven audio mixer with atomic `LiveParams` for gain/mute/solo
- `src/fixture_audio_bridge.cpp`
  - audio decode and fixture/media bridge behavior
- `src/app_controller.cpp`
  - main application coordination layer between import, editing, streaming playback, and offline rendering
- `src/windows_app.cpp`
  - Win32 UI, focused editor, insert UI, keyboard trigger → streaming wiring
- `src/windows_audio_player.cpp`
  - WASAPI playback: static WAV playback and real-time streaming from `StreamingMixer`
- `src/vst3_host.cpp`
  - in-process VST3 host/session logic
- `src/vst3_host_worker.cpp`
  - worker used for safer plugin/module enumeration tasks
- `src/project_file.cpp`
  - `.riproj` save/load

## Testing Expectations

Normal Windows-side validation should include:

- build:
  - `cmake --build build-vs2026-vcpkg --config Release`
- tests:
  - `ctest --test-dir build-vs2026-vcpkg --build-config Release --output-on-failure`
- run:
  - `.\build-vs2026-vcpkg\Release\radium_importer_app.exe`

Most important manual smoke checks:

- import a real `.radium` fixture
- audition a few isolated layers
- create and edit authored trigger regions
- trigger from the virtual keyboard
- while playing, change gain on a layer and verify immediate audible change
- while playing, toggle mute on a layer and verify immediate silence / return
- while playing, toggle solo on a layer and verify immediate isolation
- save and reload a `.riproj`
- assign at least one known-good VST3 insert
- use Render / Export and verify offline WAV output is correct

## Scope Reminder

Even though the code now contains VST3 hosting work, the core purpose of the app is still:

- import layered Radium presets
- let the user rebuild and edit useful layered behavior
- support practical one-shot and later continuous sound-design work on Windows

The app is most valuable when it helps turn imported Radium presets into editable, triggerable layered assets that can be previewed, refined, and rendered locally.

## Reference

Windows build notes:

- [docs/windows-build.md](/mnt/h/Codex/projects/radium-importer/docs/windows-build.md)
