# SysRecord

A compact Windows tray utility that captures whole-system output through WASAPI loopback and streams it directly to MP3.

## Included

- Tray controls for start/stop, pause/resume, output-folder access, startup registration, settings, and exit.
- WASAPI loopback capture at the default output device's sample rate, with a bounded four-second PCM ring buffer.
- Dedicated LAME encoder thread that incrementally writes the MP3 and flushes it on stop.
- Compact dark settings window for the output folder, filename pattern, CBR bitrate, notifications, and launch-at-startup.
- UTF-8 JSON configuration under `%APPDATA%\SysRecord\config.json`.
- Rotating application log under `%APPDATA%\SysRecord\logs` using spdlog.
- Catch2 tests for configuration validation and filename formatting.
- Static x64 MSVC build through a vcpkg manifest.

FLTK 1.4.5 is fetched directly from its GitHub release tag. nlohmann-json, spdlog, miniaudio, LAME, and Catch2 are resolved through vcpkg.

## Build

Requirements:

- Windows 10 or 11
- Visual Studio 2022 with Desktop development with C++
- CMake 3.25 or newer
- vcpkg, with `VCPKG_ROOT` set

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

The executable is written to `build\Release\SysRecord.exe`.

## Run

The application starts in the notification area. Left-click the icon to start or stop recording. Right-click for the full menu, including Settings.

Recordings default to `%USERPROFILE%\Documents\SysRecord Recordings` and use names such as `SysRecord_2026-08-19_14-30-00.mp3`.
