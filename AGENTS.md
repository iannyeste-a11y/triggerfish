# AGENTS.md

## Project goal

Build a Windows-first desktop app that imports Soundminer `.radium` preset files and provides a practical subset of Radium focused on layered sound design for two use cases:

1. One-shot synced events
   - Example: creature footsteps
   - Each trigger should fire the active layers once, in sync
   - Each layer may choose a random source or region per trigger
   - Goal is transient-aligned composite playback

2. Continuous evolving beds
   - Example: fire, machinery, texture beds
   - Playback sustains while a key is held
   - Layers may loop and fluctuate based on randomness settings
   - Note-off ends playback via release behavior

The app must support:
- importing `.radium` files
- best-effort reconstruction of useful preset behavior
- loading and replacing audio on layers
- per-layer gain, mute, solo, randomness, and basic FX
- octave-ranged virtual keyboard triggering
- 24-bit / 48 kHz WAV recording and export

The app does NOT need to export native `.radium` files in v1.

## Target platform

This project is Windows-first.

Requirements:
- the shipping app must run natively on Windows 10
- development may happen in WSL on Windows 10
- any build tooling chosen must be compatible with a Windows 10 runtime target
- avoid dependencies that are known to require Windows 11 at runtime

## Development environment guidance

Preferred development environment:
- Windows 10 host machine
- WSL workspace for Codex-driven development
- native Windows build and runtime testing

Reason:
- Codex works best in WSL on Windows
- the final app must still compile and run natively on Windows 10

Do not assume Linux is the target runtime.
Linux may be used only as a development environment.

## Hard boundaries

Do NOT:
- decompile, disassemble, patch, or inspect Soundminer binaries
- attempt dynamic reverse engineering of the Soundminer application
- attempt plugin binary compatibility with Soundminer
- attempt AAX or VST hosting in v1
- attempt a full Soundminer clone
- attempt full native `.radium` authoring or export in v1

Work only from:
- public documentation
- user-supplied `.radium` files
- normal file inspection and deterministic parser work
- original app code written in this repo

## Known facts from sample files

Treat these as working assumptions, and validate them with tests:

- `.radium` files in our fixture set are SQLite databases
- Core tables observed:
  - `radiumpresets`
  - `radiumdata`
  - `schema`
- Sample files were not encrypted
- `radiumdata` contains embedded FLAC payloads
- The preset blob contains repeated compressed blocks
- Repeated per-slot chunk labels observed:
  - `params`
  - `misc`
  - `modsettings`
  - `pluginParams`
  - `sample`
  - `sound`
- Internal format appears to represent 8 slot groups
- Some files contain fewer active sound slots than total slot groups
- Some files contain more embedded audio blobs than currently active slots
- Public docs describe 5 visible user-facing slots, so UI and file format may not map 1:1

## Product definition for v1

### Import
- Open `.radium` files
- Parse SQLite tables safely
- Extract embedded FLAC assets
- Identify active vs inactive slots
- Extract and display:
  - preset name
  - source file names if available
  - source file paths if available
  - active slot count
  - embedded media inventory
  - basic per-layer parameters where decodable

### Layer model
Each layer should support:
- one or more source audio items
- optional source regions
- gain
- mute
- solo
- pan
- coarse and fine pitch
- reverse
- start offset
- stop offset
- fade in / fade out
- delay
- loop enable
- loop start / loop end
- ADSR or ADSHR envelope
- random gain range
- random pan range
- random pitch range
- random region or sample selection
- no immediate repeat mode

### FX for v1
Built-in effects only:
- EQ or simple tone shaping
- low shelf / bass boost
- filter
- reverb
- saturation or distortion
- optional master limiter

Randomization for FX should be simple:
- per-trigger randomized value inside a min/max range
- optional slow drift only if easy to implement cleanly

### Triggering
- Octave-ranged virtual keyboard in the UI
- Mouse-click trigger
- Optional computer keyboard mapping
- Note-on / note-off behavior
- One-shot mode:
  - one trigger starts all active layers once in sync
- Continuous mode:
  - trigger sustains while held
  - looping layers continue until note-off
  - release stage applies on note-off

### Recording / rendering
- Render or record to 24-bit / 48 kHz WAV
- Two output modes:
  1. One-shot render from a single trigger
  2. Live capture while the user plays the virtual keyboard

## Architecture guidance

Prefer a native desktop audio stack.

Recommended:
- C++ with JUCE for audio engine and desktop UI
- SQLite library for `.radium` container access
- zlib for compressed chunk inflation
- libsndfile or JUCE readers/writers for WAV and FLAC
- tests for parser and audio logic

Avoid a web-stack-first architecture for v1.
Avoid Electron for the first implementation unless there is a compelling reason.
Audio correctness is more important than fancy UI.

## Windows-specific guidance

- The final executable must run as a native Windows 10 desktop app
- File paths must be handled safely for Windows path conventions
- Audio file handling must work with common Windows user workflows
- Keep build instructions clear for Windows users
- If any helper scripts are needed, prefer cross-platform Python or simple shell commands that can be translated easily to PowerShell
- Do not require Pro Tools, AAX, ASIO-specific work, or Soundminer to be installed for v1
- If low-level driver selection is introduced later, keep a safe Windows default path first

## UX guidance

Main UI should be simple and production-oriented:
- preset header with import status
- 5 visible layer strips by default
- support internal model up to 8 imported layers even if only 5 are shown initially
- layer controls grouped consistently
- visible trigger mode toggle:
  - One-shot
  - Continuous
- visible record button
- visible render/export button
- octave virtual keyboard at bottom
- waveform or region view only where it adds real utility

## Parsing strategy

Build parser in stages:

1. SQLite container reader
2. Table inventory and schema checks
3. Embedded media extractor
4. Preset blob chunk locator
5. zlib decompression helpers
6. Chunk classifier
7. Slot reconstruction
8. Parameter decoder for the subset needed by v1

Do not overfit to one file.
Validate assumptions against the fixture corpus.

## Success criteria for v1

The project is successful when it can:
- import the supplied `.radium` fixtures without crashing
- extract embedded audio assets
- show active slots for each preset
- reconstruct enough layer behavior to audition imported presets meaningfully
- allow the user to replace or add source audio on layers
- trigger one-shot synced presets from the virtual keyboard
- trigger continuous held presets from the virtual keyboard
- render 24-bit / 48 kHz WAV files
- build and run natively on Windows 10

## Non-goals for v1

- native `.radium` export
- Soundminer database integration
- third-party plugin hosting
- exact match for all Radium DSP or modulation behavior
- AAX integration
- Pro Tools integration
- cloud sync or collaboration features
- sample library management at Soundminer scale

## Development order

### Phase 1: headless parser
- CLI tool that reads `.radium`
- list preset metadata
- extract embedded audio
- identify slot groups
- produce JSON debug output
- add tests against fixture files

### Phase 2: import domain model
- map parsed data into internal `Preset`, `Layer`, `Region`, `Randomization`, `EffectSettings`
- implement best-effort decoder for the subset of parameters needed for v1

### Phase 3: playback engine
- one-shot synced playback mode
- continuous held playback mode
- random region selection
- per-layer gain, pan, pitch, and randomness
- render to WAV

### Phase 4: desktop UI
- import flow
- layer editor
- virtual keyboard
- record/render controls

### Phase 5: polish
- fixture comparison tools
- import diagnostics
- better parameter decoding
- better region editing
- Windows packaging and smoke testing

## Testing requirements

Always add validation along the way.

Parser tests:
- can open every fixture file
- can read expected tables
- can extract non-zero embedded media
- can identify slot groups
- can produce deterministic JSON summaries

Playback tests:
- one-shot mode starts layers on the same trigger boundary
- continuous mode sustains while held
- no-immediate-repeat logic works
- 24/48 WAV export succeeds

Windows validation:
- app builds on Windows 10
- app launches on Windows 10
- fixture import works on Windows 10
- WAV render works on Windows 10

Regression tests:
- every parser improvement must be run against all fixture files

## Code quality rules

- Keep parsing logic isolated from UI
- Keep audio engine isolated from import code
- Add comments only where they explain non-obvious decisions
- Prefer small, testable modules
- Avoid large rewrites unless requested
- When unsure, choose the simpler architecture
- Make progress in small PR-sized chunks

## How to behave during implementation

Before editing many files:
- summarize the plan
- name the files you will touch
- state how you will validate the change

After each milestone:
- run tests
- report what works
- report known limitations honestly

When a format assumption is uncertain:
- mark it clearly
- preserve raw data for later analysis
- do not silently invent semantics
