# Demo Recorder for Windows

The native C++20 engine (C++/WinRT, Direct3D 11, Media Foundation) and
`drrec.exe`, the P0 recording spike. Per decision D3, the UI will be a C# WinUI 3
app that calls this engine through a C ABI. That shell starts in P1 with the
tray app.

Requires Windows 10 21H2 or later. Windows 11 removes the yellow capture border.

## Build

You need Visual Studio 2022 with the "Desktop development with C++" workload
and a Windows 11 SDK. From the repo root:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

To work in the IDE, open `build\DemoRecorder.sln`; `drrec` is the startup project.

## Run

```powershell
build\windows\Release\drrec.exe --list-monitors
build\windows\Release\drrec.exe --seconds 600 --max-height 1080 --fps 60
```

Without `--seconds`, `drrec` records until Enter or Ctrl+C. Projects go to
`%USERPROFILE%\Videos\Demo Recorder\` (change it with `--output DIR`).
`drrec` prints `stats.json`, which includes the encoder it picked and whether
that encoder is hardware. It exits with code 3 if any frame was dropped.

## Pipeline

1. **Windows.Graphics.Capture** delivers each frame as an `ID3D11Texture2D`,
   without the cursor, stamped with `SystemRelativeTime` (the QPC master clock).
2. **`ID3D11VideoProcessor`** converts BGRA to NV12 (BT.709) on the GPU, into
   a fixed pool of textures. Slots are claimed through an atomic bitmask.
3. A lock-free queue hands each slot to the encoder thread. The **Media
   Foundation hardware H.264 MFT** reads the GPU texture directly
   (`MFCreateDXGISurfaceBuffer`). When MF releases a sample
   (`IMFTrackedSample`), its slot returns to the pool.
4. The sink writer writes a **fragmented MP4** (`MFTranscodeContainerType_FMPEG4`).
5. A 120 Hz thread logs the cursor to `cursor.json` and `events.json` through
   the core.

No frame is ever read back to the CPU. If the encoder falls behind, the frame
is dropped and counted (`frames_dropped_encoder_busy`); capture never blocks.

## Layout

| Path | Purpose |
| --- | --- |
| `engine/include/drwin/recorder.h` | Public engine API |
| `engine/src/capture.*` | Windows.Graphics.Capture source |
| `engine/src/gpu.*` | D3D11 device, NV12 pool, video-processor converter |
| `engine/src/encoder.*` | Media Foundation encoder and fragmented MP4 writer |
| `engine/src/cursor.*` | Cursor sampler |
| `engine/src/recorder.cpp` | Session orchestration and project.json |
| `recorder_cli/` | `drrec.exe` |

## Next (P1)

Direct NVENC / AMF / oneVPL encoders, with Media Foundation as the fallback;
window and area capture; WASAPI microphone and loopback; process loopback;
low-level mouse and keyboard hooks; cursor shapes; the WinUI 3 tray app.
