# Demo Recorder for macOS

Swift 6 + AppKit menu bar app. Requires macOS 14 Sonoma or later (decision D2)
and Xcode 16 or later.

## Build and run

```bash
brew install xcodegen
cd macos
xcodegen generate
open DemoRecorder.xcodeproj
```

The Xcode project is generated from `project.yml` and is not committed. Run
`xcodegen generate` again after adding or removing files.

The shared core (`../core/src`) is compiled into the app, and Swift imports
its C API as the `DRCore` module.

## What P0 does

- Menu bar item: **Start Recording** / **Stop Recording**, plus **Open Recordings Folder**.
- Records the main display at native resolution, 60 fps, with ScreenCaptureKit.
  Frames arrive as NV12 and go straight to the hardware encoder (VideoToolbox,
  through `AVAssetWriter`). The output is a fragmented MP4 with a fragment
  every second, and the cursor is left out of the video.
- Logs the cursor position and buttons at 120 Hz to `cursor.json`, and derives
  clicks from them into `events.json`.
- Projects go to `~/Movies/Demo Recorder/`.

The first run asks for the **Screen Recording** permission. After granting it,
quit and reopen the app; macOS requires a relaunch.

## Scripted recording (G0 and nightly performance runs)

```bash
"build/Debug/Demo Recorder.app/Contents/MacOS/Demo Recorder" \
  --record-seconds 600 --max-height 1080 --fps 60 --output /tmp/g0
```

This records for the given time, writes `stats.json` into the project folder,
prints it, and quits. Watch CPU with `top -pid <pid>` or Instruments while it
runs.

## Layout

| Path | Purpose |
| --- | --- |
| `DemoRecorder/App/` | App entry point, menu bar, permissions |
| `DemoRecorder/Recording/` | Capture session, screen recorder, cursor sampler, core bridge |
| `DemoRecorder/Support/` | Info.plist and entitlements |

## Next (P1)

Window and area capture, system audio, microphone and camera, `CGEventTap` for
exact click and key timing, cursor shapes, the countdown and global hotkeys,
and permission onboarding.
