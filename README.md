# demo-recorder
A simple screen recorder for capturing, creating, and sharing professional demos, tutorials, and walkthroughs.

Two fully native apps, one for macOS and one for Windows, share one C++ core.
The core holds the project format and, later, the effects math, so a project
renders the same on both platforms.

| Folder | What | Stack |
| --- | --- | --- |
| [`core/`](core) | Shared core: project format, master clock, cursor and event logs, C API | C++20, CMake |
| [`macos/`](macos) | macOS app | Swift 6, AppKit, ScreenCaptureKit, VideoToolbox |
| [`windows/`](windows) | Windows engine and recorder | C++20, C++/WinRT, Windows.Graphics.Capture, Direct3D 11, Media Foundation |
| [`shaders/`](shaders) | Effects shaders (HLSL source; Metal generated) | Starts in P2 |
| [`testdata/`](testdata) | Fixture projects for tests | |
| [`docs/`](docs) | Spec, plan and file format | |

## Status

Phase **P0 Foundations** (see the [development plan](docs/feature-spec-and-dev-plan.md#development-plan)):

- [x] Monorepo, CMake, CI on macOS, Windows and Linux
- [x] Project format v0 ([docs/project-format.md](docs/project-format.md)) with a defensive, fuzzed parser
- [x] Master clock design (`core/include/dr/clock.h`)
- [x] macOS spike: ScreenCaptureKit → VideoToolbox → fragmented MP4, cursor data at 120 Hz
- [x] Windows spike: Windows.Graphics.Capture → GPU NV12 → Media Foundation hardware H.264 → fragmented MP4, cursor data at 120 Hz
- [ ] Gate G0: a 10-minute 1080p60 recording at ≤10% CPU on the low-end reference machines (needs the reference hardware)

## Build

Core and tests, on any OS:

```bash
cmake -S . -B build -DDR_BUILD_WINDOWS_APP=OFF
cmake --build build
ctest --test-dir build
```

- macOS app: see [macos/README.md](macos/README.md)
- Windows engine: see [windows/README.md](windows/README.md)

## Docs

- [Feature spec & development plan](docs/feature-spec-and-dev-plan.md)
- [Project format](docs/project-format.md)
