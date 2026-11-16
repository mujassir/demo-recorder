# Project format

Status: **v0** (P0 Foundations). The format freezes as v1 in P4; until then
the core reads every earlier version, and any version change is listed under
[Changes](#changes).

A project is a folder with the `.demorecorder` extension. The native app
writes the raw capture files, and the shared core writes everything derived
from them. Each file below is optional except `project.json`.

| File | Contents | Written by | Since |
| --- | --- | --- | --- |
| `project.json` | Manifest: version, sources, clock, displays | Native app (through the core) | v0 |
| `screen.mp4` | Screen video, no cursor, fragmented MP4 | Native app | v0 |
| `cursor.json` | Cursor position, shape and buttons at ≥120 Hz | Core input recorder | v0 |
| `events.json` | Mouse and key down/up times, no key identity | Core input recorder | v0 |
| `stats.json` | Recording statistics for performance checks; not read by the editor | Native app | v0 |
| `camera.mp4`, `mic.wav`, `system.wav`, `cursors/` | Planned for P1 | Native app | — |
| `transcript.json`, `edits.json`, `cache/` | Planned for P2–P3 | Core | — |

## Times and the master clock

All tracks are stamped against one monotonic host clock: `mach_absolute_time`
on macOS, `QueryPerformanceCounter` on Windows. Times in files are **int64
nanoseconds relative to the recording epoch** (`clock.epoch_host_ns`), the
host time that maps to recording time 0. Integers are written exactly, never
as floating point.

A source's `start_offset_ns` is the recording time of its first sample. It can
be negative (a microphone that started a few milliseconds before the epoch).

## `project.json`

```json
{
  "format": "demo-recorder-project",
  "version": 0,
  "id": "3f2b8c1e-6a4d-4e2f-9b7a-1c5d8e9f0a12",
  "created_at": "2026-11-16T09:30:00Z",
  "state": "complete",
  "app": {"name": "Demo Recorder", "version": "0.1.0", "platform": "macos", "os_version": "14.5"},
  "clock": {"domain": "mach_absolute_time", "epoch_host_ns": 17280000000000123},
  "duration_ns": 600000000000,
  "displays": [
    {"id": "1", "width_px": 3024, "height_px": 1964, "scale": 2.0, "refresh_hz": 120.0}
  ],
  "sources": [
    {"id": "screen", "kind": "screen", "file": "screen.mp4", "start_offset_ns": 4166666,
     "display_id": "1", "video": {"codec": "h264", "width": 1920, "height": 1247, "fps": 60.0}},
    {"id": "cursor", "kind": "cursor", "file": "cursor.json", "start_offset_ns": 0},
    {"id": "events", "kind": "events", "file": "events.json", "start_offset_ns": 0}
  ]
}
```

| Field | Type | Notes |
| --- | --- | --- |
| `format` | string | Always `demo-recorder-project` |
| `version` | int | Readers reject versions newer than they know |
| `id` | string | Random UUID v4 |
| `created_at` | string | RFC 3339, UTC |
| `state` | `recording` \| `complete` | `recording` after a crash means the project needs recovery |
| `app.platform` | `macos` \| `windows` \| `linux` | |
| `clock.domain` | `mach_absolute_time` \| `qpc` \| `clock_monotonic` | |
| `clock.epoch_host_ns` | int | Host-clock ns at recording time 0 |
| `duration_ns` | int | Recording length |
| `displays[]` | object | `width_px`/`height_px` 1–16384; `scale` 0.25–8 |
| `sources[].kind` | `screen` \| `camera` \| `microphone` \| `system_audio` \| `cursor` \| `events` | |
| `sources[].file` | string | Relative path inside the project folder (see [Safety](#safety)) |
| `sources[].display_id` | string | Screen sources only; must match a display |
| `sources[].video` | object | `codec` (`h264` \| `hevc` \| `prores`), `width`, `height`, `fps` |
| `sources[].audio` | object | `sample_rate` 8000–384000, `channels` 1–32 |

Readers ignore unknown fields, so newer apps can add fields without a version
bump as long as older readers stay correct without them.

The manifest is written with state `recording` when capture starts, so a
crash leaves a readable manifest, and rewritten with state `complete` at stop.
Every write is atomic (temporary file and rename).

## Row logs: `cursor.json` and `events.json`

Both are JSON documents with a fixed line layout, so they can be written
incrementally during recording and repaired after a crash:

```
{"format":"demo-recorder-cursor","version":0,"columns":["t_ns","x","y","shape","buttons"],"rows":[
[0,512.5,384,0,0]
,[8333333,513,384.5,0,1]
]}
```

- Line 1 is the header and ends with `"rows":[`.
- Each following line is one row: a JSON array of scalars. Every row after the
  first starts with a comma.
- The last line is `]}`.

The writer flushes about once a second, so a crash loses at most about one
second of rows. `dr_recover <project>` (or `dr_recover_row_log` in the C API)
keeps every complete row and closes the file. Repair is idempotent.

### `cursor.json` columns

| Column | Type | Meaning |
| --- | --- | --- |
| `t_ns` | int | Recording time |
| `x`, `y` | number | Position in capture-space pixels, origin top-left of the captured area. May be outside the frame when the cursor is on another display |
| `shape` | int | Cursor image id in `cursors/`; `0` = unknown or system arrow (P0) |
| `buttons` | int | Bit flags: 1 left, 2 right, 4 middle |

### `events.json` columns

| Column | Type | Meaning |
| --- | --- | --- |
| `t_ns` | int | Recording time |
| `type` | int | 1 mouse down, 2 mouse up, 3 key down, 4 key up |
| `code` | int | Mouse: button index (0 left, 1 right, 2 middle). Keys: always `0` |

Key identity is never stored in `events.json` (spec I-2). Keystroke overlay
data (I-3) will go in a separate file that exists only when the overlay is on.

## Safety

Projects can come from other people, so the core parses every file
defensively:

- Bounded input size and nesting depth, strict JSON grammar, and UTF-8
  validation; integers keep exact int64 values.
- File names must be relative paths inside the project folder: no absolute
  paths, `..`, `.`, empty segments, backslashes, `:` (drive letters and NTFS
  alternate data streams), control characters, Windows device names (`CON`,
  `NUL`, `COM1`, ...), or segments ending in a dot or space.
- Every numeric field is range-checked, and errors name the field, for example
  `$.sources[1].file: unsafe path`.
- The JSON, project and row-log parsers are fuzzed in CI (`core/fuzz/`).

## Changes

| Version | Change |
| --- | --- |
| 0 | Initial format: `project.json`, `screen.mp4`, `cursor.json`, `events.json` |
