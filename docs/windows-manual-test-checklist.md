# Windows 10 Manual Test Checklist

1. Build the Win32 app in `Release`.
2. Launch `radium_importer_app.exe`.
3. Click `Import .radium` and open `fixtures\\radium\\NJR LARGE FACTORY POWER DOWN.radium`.
4. Confirm the summary panel shows the imported preset, active-layer count, and mapped-audio count.
5. Confirm diagnostics are visible and mention any current assumptions.
6. Verify the first 5 layer strips are visible with mute, solo, and gain controls.
7. Toggle mute or solo on a visible layer and trigger a key from the virtual keyboard.
8. Change a visible layer gain value, trigger again, and confirm audible level changes.
9. In `One-shot` mode, click a virtual keyboard key and confirm playback starts immediately.
10. In `Continuous` mode, trigger a note, then release it through the UI path you are testing and confirm playback stops with a release tail.
11. Click `Render / Export`, save a WAV, and confirm the output file is 24-bit / 48 kHz.
12. Trigger a note, then click `Record Last`, save a WAV, and confirm the file is written.
13. Import at least one other fixture and confirm the summary/diagnostics update without crashing.

Current dependency note:
- Real embedded FLAC decode still uses `ffmpeg` on Windows. The current bridge expects `ffmpeg.exe` to be available on the Windows host path.
