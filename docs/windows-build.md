# Windows 10 build notes

Milestone 1 is a native C++17 parser CLI and test executable intended to be built on Windows 10 with standard SQLite and zlib libraries.
Milestone 4 adds a Win32 desktop executable on top of the same libraries.

Recommended Windows 10 setup:

1. Install Visual Studio 2022 Build Tools or full Visual Studio with the Desktop development with C++ workload.
2. Install CMake 3.20+.
3. Provide SQLite and zlib development libraries in a way your toolchain can find.
4. Configure and build:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
ctest --test-dir build --build-config Release
```

CLI usage on Windows 10:

```powershell
.\build\Release\radium_parser_cli.exe fixtures\radium --output-dir artifacts\extracted --summary-dir artifacts\summaries
```

Desktop app launch on Windows 10:

```powershell
.\build\Release\radium_importer_app.exe
```

Current embedded-audio note:
- The real fixture-audio path still depends on `ffmpeg.exe` being available on the Windows machine.
- In the current dev environment this was validated with a Chocolatey-installed `ffmpeg` at `C:\ProgramData\chocolatey\bin\ffmpeg.exe`.

The parser only reads `.radium` files, writes extracted embedded media, and writes deterministic JSON summaries. It does not require any third-party authoring application or plugin host on the target machine.
