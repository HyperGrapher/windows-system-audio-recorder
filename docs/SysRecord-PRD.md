# SysRecord — System Audio Recorder (Tray App)
**PRD v1.0 — Windows 10+, C++**
---

## 1. Overview

SysRecord is a Windows system-tray utility that records **whole-system audio output** (everything playing through the default speakers/headphones) and exports it as an MP3. No virtual audio cables, no drivers — pure WASAPI loopback capture. Single tray icon, minimal UI, dark theme.

## 2. Goals & Non-Goals

**Goals**
- One-click start/stop recording of system audio from the tray.
- Direct MP3 export (streamed encode, not WAV-then-convert).
- Dark UI.
- Low idle footprint — sits in the tray doing nothing until recording starts.
- Config persisted across restarts (output folder, bitrate, device, hotkey).

**Non-Goals (v1)**
- Per-application audio capture (isolating one app's audio) — this requires `AUDIOCLIENT_ACTIVATION_PARAMS` process-loopback APIs (Win10 2004+) and is a materially different, harder feature. Explicitly out of scope for v1; note as a possible v2.
- Simultaneous microphone + system audio mixing — out of scope for v1.
- Audio editing/trimming inside the app.
- Cross-platform support. Windows 10 (2004+) and Windows 11 only.

## 3. Tech Stack

| Layer | Choice |
|---|---|
| Language | C++20 |
| UI toolkit | FLTK (consistent with LocalDev Tray / search popup / TimerTool) |
| Audio capture | miniaudio (WASAPI loopback backend) |
| MP3 encoding | LAME |
| Package manager | vcpkg (manifest mode) |
| Build | CMake |
| Config format | Flat JSON (same pattern as BackupTray) |

vcpkg.json dependencies: `miniaudio`, `lame`. Both are header/lib-only, no runtime services required — fits a portable single-exe distribution.

## 4. Architecture Overview

Single-process application (no privilege split needed — unlike LocalDev Tray, nothing here requires admin rights or a Windows Service). Three internal components running in one process:

```
┌─────────────────────────────────────────────┐
│                  SysRecord.exe                 │
│                                               │
│  ┌───────────┐   ring buffer   ┌───────────┐ │
│  │  Capture   │ ───────────────▶│  Encoder  │ │
│  │  Thread    │  (lock-free /   │  Thread   │ │
│  │ (miniaudio │   mutex+cond)   │  (LAME)   │ │
│  │  loopback) │                 └─────┬─────┘ │
│  └───────────┘                        │       │
│                                        ▼       │
│                                   .mp3 file    │
│                                        ▲       │
│  ┌────────────────────────────────────┴─────┐ │
│  │        UI Thread (FLTK, tray icon,        │ │
│  │        context menu, settings window)      │ │
│  └────────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

- **Capture thread**: owns the `ma_device` in loopback mode, pushes raw PCM float frames into a bounded ring buffer as they arrive via miniaudio's data callback.
- **Encoder thread**: pulls PCM from the ring buffer, feeds it to `lame_encode_buffer_ieee_float`, writes encoded bytes to the output `.mp3` file incrementally. Flushes and finalizes (`lame_encode_flush`) on stop.
- **UI thread**: FLTK event loop, tray icon (via `Fl_Sys_Menu`-equivalent / native Win32 `Shell_NotifyIcon`, context menu, optional settings window. UI never touches audio buffers directly — only sends start/stop/pause commands via a thread-safe command queue and receives state updates (recording/idle/error, elapsed time, current file size) via a similar queue polled on a UI timer tick.

This mirrors the two-thread producer/consumer split you'd expect from any real-time audio app — keeps the capture callback (which has hard timing constraints) free of file I/O and encoding work.

## 5. Core Features

### 5.1 Tray icon & menu
- Icon reflects state: idle (outline/gray), recording (filled/red dot or animated subtle pulse — keep it simple, avoid CPU-hungry animation), paused (amber).
- Right-click context menu:
  - **Start Recording** / **Stop Recording** (toggles based on state)
  - **Pause** / **Resume** (only enabled while recording)
  - **Open Output Folder**
  - **Settings…**
  - **Launch at Startup** (checkbox)
  - **Exit**
- Left-click (or double-click) on tray icon: toggle start/stop as a fast path — configurable in settings if double-click should instead open the settings window.
- Tooltip on hover shows current state + elapsed time when recording.

### 5.2 Recording controls
- **Start**: creates a new output file (see naming below), begins capture + encode.
- **Stop**: finalizes MP3 (flush encoder, close file), returns to idle.
- **Pause/Resume**: stops feeding the encoder without finalizing the file; capture thread can either keep running and discard frames, or be stopped/restarted — stopping the `ma_device` during pause is cleaner and avoids wasted CPU.
- Balloon/toast notification (Windows native) on start and stop, showing filename on stop — small polish touch, should be toggleable in settings.

### 5.3 Settings window (dark themed)
A single compact FLTK window, not resizable, roughly 380×420px. Sections:
- **Output folder** — path picker (defaults to `%USERPROFILE%\Documents\SysRecord Recordings`, auto-created if missing).
- **Filename pattern** — templated string, default `SysRecord_{yyyy-MM-dd}_{HH-mm-ss}.mp3`.
- **MP3 quality** — dropdown: bitrate presets (128 / 192 / 256 / 320 kbps CBR) or VBR quality 0–9. Default 192 kbps CBR (good default balance, avoids VBR mode confusion for most users).
- **Output device** — dropdown of active render (playback) devices, default = "System Default" (auto-tracks the OS default device, recommended default behavior). Listing explicit devices lets a user record from a specific output (e.g., a virtual cable) rather than whatever Windows currently calls default.
- **Hotkey** — optional global hotkey to start/stop recording (e.g., `Ctrl+Alt+R`),  Off by default.
- **Launch at Windows startup** — checkbox (registers in `HKCU\...\Run`, no admin needed).
- **Show notifications** — checkbox.

### 5.4 Config persistence
Flat JSON config, single file, human-readable, no nested schema versioning complexity for v1.

```json
{
  "outputFolder": "C:\\Users\\burak\\Documents\\SysRecord-Audio",
  "filenamePattern": "SysRecord_{yyyy-MM-dd}_{HH-mm-ss}.mp3",
  "bitrateKbps": 192,
  "vbrMode": false,
  "outputDeviceId": "default",
  "hotkeyEnabled": false,
  "hotkey": "Ctrl+Alt+R",
  "launchAtStartup": false,
  "showNotifications": true
}
```
Stored at `%APPDATA%\SysRecord\config.json`. Written on any settings change; read once at startup.

## 6. Audio Capture & Encoding Detail

### 6.1 Loopback capture via miniaudio
- Device config: `ma_device_type_loopback`, sample format `ma_format_f32`, channels/sample rate taken from the endpoint's mix format (`ma_device_get_info` or just let miniaudio negotiate native format — do **not** force a fixed sample rate, since forcing resampling on the default WASAPI shared-mode stream can introduce glitches; resample only if the encoder needs a specific rate LAME doesn't support directly).
- LAME supports arbitrary input sample rates via `lame_set_in_samplerate` / `lame_set_out_samplerate` (LAME will resample internally if needed) — so simplest correct approach: **capture at native device format, hand raw rate straight to LAME, let LAME handle any output rate normalization.** Avoids a manual resampling stage entirely.
- Ring buffer sized generously (e.g., 2–4 seconds of audio at capture format) to absorb scheduling jitter between capture callback and encoder thread without dropping frames.

### 6.2 Encoding
- `lame_init()` configured with channels, in/out sample rate, and bitrate/VBR mode from settings at recording start (not changeable mid-recording — changing bitrate requires stop/start).
- Encoder thread loop: wait on ring buffer (condition variable), pull available frames, `lame_encode_buffer_ieee_float(...)`, `fwrite` returned bytes to the `.mp3` file handle.
- On stop: `lame_encode_flush(...)` to drain any remaining internal LAME buffer, write final bytes, close file, `lame_close()`.

### 6.3 Silence / no-audio-playing behavior
**Auto-pause on silence**: Stop recording after 30 seconds of silence. Adjustable via settings.

### 6.4 Max Recording time:
**Max recording length**: Max recording is 5 Hour adjustable via settings.

## 7. Output & File Naming

- Files land directly in the configured output folder, named per the filename pattern (timestamp-based by default, avoids collisions).
- No temp-file/rename dance needed since encoding is streamed directly to the final filename — if the app crashes mid-recording, the MP3 file up to that point should still be a **playable, if not fully finalized, file** (LAME writes valid frames incrementally; only the very last flush is "at risk"). Worth testing this crash-resilience scenario explicitly.

## 8. Error Handling & Edge Cases

| Scenario | Expected behavior |
|---|---|
| No active render device (e.g., all outputs disabled) | Start button disabled / greyed with tooltip explaining why; tray icon shows a distinct "no device" state |
| Default output device changes mid-recording (e.g., user unplugs headphones) | If recording from "System Default", detect device-changed notification and either (a) seamlessly reinitialize capture on new default, or (b) stop the recording safely and notify the user. **(a) is the better UX but more work — flag as a decision point.** |
| Explicit (non-default) device selected in settings gets unplugged/disabled | Stop recording safely, finalize file, show error notification |
| Disk full during recording | Catch write failure, stop recording, finalize what's written if possible, show error notification |
| Output folder deleted while app running | Recreate on next recording start if missing; if deleted mid-recording, handle write failure per disk-full case |
| App already running (second instance launched) | Single-instance enforcement via named mutex — bring existing tray icon into focus / show a brief notification instead of launching a duplicate |
| Recording started with no output folder write permission | Validate folder is writable at settings-save time and at recording-start time; show clear error if not |

## 9. UI/UX — Dark Theme

Dark palette applied to FLTK widgets. Widgets can be custumized to get a modern look and feel.

Suggested palette:
- Background: `#1E1E1E`
- Panel/card surface: `#252526`
- Border/divider: `#3C3C3C`
- Primary text: `#E0E0E0`
- Secondary/muted text: `#9A9A9A`
- Accent (recording/active state): `#E5484D` (red)
- Accent (idle/ready): `#4A9EFF` or neutral gray
- Success/confirmation: `#3FB950`

Typography: system default (Segoe UI) at standard weight — no custom font bundling needed, keeps the binary lean and the look native.


## 10. Non-Functional Requirements

- Idle CPU usage: effectively 0% when not recording (tray icon + message loop only).
- Recording CPU usage: capture + LAME encode at 192kbps should be comfortably under a few % on any modern CPU — no GPU/hardware acceleration needed.
- Memory: ring buffer + LAME internal buffers, well under 50MB total footprint.
- Startup time: near-instant (<1s to tray icon appearing).


## 11. Build & Packaging

- CMake + vcpkg manifest mode (`vcpkg.json` declaring `miniaudio`, `lame`).
- Static-link the CRT and vcpkg libs where practical to keep it a clean single-exe distribution, no missing-DLL support headaches for end users.
- option "launch at startup" in the UI.
- Single executable without installer

## 12. Suggested Project Structure

```
SysRecord/
├── AGENTS.md                  # agent workflow notes, same pattern as your other repos
├── CMakeLists.txt
├── vcpkg.json
├── src/
│   ├── main.cpp                # entry point, single-instance check, tray init
│   ├── tray/
│   │   ├── TrayIcon.cpp/.h      # Shell_NotifyIcon wrapper + context menu
│   │   └── SettingsWindow.cpp/.h
│   ├── audio/
│   │   ├── LoopbackCapture.cpp/.h   # miniaudio device wrapper
│   │   ├── Mp3Encoder.cpp/.h        # LAME wrapper
│   │   └── RingBuffer.h
│   ├── config/
│   │   └── Config.cpp/.h        # JSON load/save
│   └── util/
│       └── (hotkey registration, notifications, autostart registry helpers)
├── resources/
│   ├── icon_idle.ico
│   ├── icon_recording.ico
│   └── icon_paused.ico
└── docs/
    └── SysRecord-PRD.md
```

## 13. Phased Implementation Plan

1. **Phase 1 — Core capture + encode pipeline (no UI)**: console/test harness that records N seconds of system audio to a fixed MP3 path. Validates miniaudio loopback + LAME streaming end-to-end.
2. **Phase 2 — Tray shell**: tray icon, context menu, start/stop wired to the Phase 1 pipeline, state-reflecting icon.
3. **Phase 3 — Settings window + config persistence**: dark-themed UI, JSON config load/save, device selection, filename pattern, bitrate.
4. **Phase 4 — Polish & edge cases**: notifications, hotkey, launch-at-startup, single-instance enforcement, device-change handling, error states.

