# FLTK Sys Record

A compact Windows starter for a tray-first, frameless FLTK application.

## Included

- Frameless, draggable FLTK window with a custom header.
- Tray icon with left-click Open and right-click Open/Exit actions.
- Tray-only startup and hide-on-close behavior.
- Low-CPU message loop that continues to dispatch tray events while the FLTK window is hidden.
- JSON settings state under `%LOCALAPPDATA%\SysRecord`.
- JSON state serialization with nlohmann-json.
- Rotating application log under `%LOCALAPPDATA%\SysRecord\logs` using spdlog.
- Catch2 unit tests.
- Static x64 MSVC build through a vcpkg manifest.

FLTK 1.4.5 is fetched directly from its GitHub release tag. nlohmann-json, spdlog, and Catch2 are resolved through vcpkg.

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

The application starts in the notification area. Left-click the tray icon to open the window. Right-click it for the Open and Exit menu.

## Customize

To rename this template for a new application, run the PowerShell script from the project root:

```powershell
.\Rename-Template.ps1 -AppName "My App"
```

The script updates the CMake targets, vcpkg package name, README, C++ application strings, and test target names. It skips `build`, `.git`, and the script itself. Use `-WhatIf` to preview the files that would change.
