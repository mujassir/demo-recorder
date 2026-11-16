// Project format v0 (see docs/project-format.md).
//
// A project is a folder. This header models project.json, the manifest
// written by the native app when a recording starts and finalized when it
// stops. The reader is defensive: it validates every field it uses, ignores
// unknown fields, and refuses versions newer than it understands.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dr/json.h"

namespace dr {

inline constexpr int kProjectFormatVersion = 0;
inline constexpr const char* kProjectFormatName = "demo-recorder-project";
inline constexpr const char* kProjectFileName = "project.json";

enum class SourceKind { Screen, Camera, Microphone, SystemAudio, Cursor, Events };

const char* to_string(SourceKind kind);
std::optional<SourceKind> source_kind_from_string(std::string_view s);

struct AppInfo {
  std::string name = "Demo Recorder";
  std::string version;
  std::string platform;  // "macos" | "windows" | "linux"
  std::string os_version;
};

struct ClockInfo {
  std::string domain;         // MasterClock::domain() of the recording machine
  int64_t epoch_host_ns = 0;  // host time (ns) that recording time 0 maps to
};

struct Display {
  std::string id;
  int32_t width_px = 0;
  int32_t height_px = 0;
  double scale = 1.0;       // backing scale factor (macOS) / DPI scale (Windows)
  double refresh_hz = 0.0;  // 0 when unknown
};

struct VideoInfo {
  std::string codec;  // "h264" | "hevc" | "prores"
  int32_t width = 0;
  int32_t height = 0;
  double fps = 0.0;
};

struct AudioInfo {
  int32_t sample_rate = 48000;
  int32_t channels = 1;
};

struct Source {
  std::string id;
  SourceKind kind = SourceKind::Screen;
  std::string file;           // path relative to the project folder
  int64_t start_offset_ns = 0;  // first sample's time relative to the epoch
  std::string display_id;     // screen sources only
  std::optional<VideoInfo> video;
  std::optional<AudioInfo> audio;
};

enum class ProjectState { Recording, Complete };

struct Project {
  int version = kProjectFormatVersion;
  std::string id;          // random UUID v4
  std::string created_at;  // RFC 3339 UTC, e.g. 2026-11-16T09:30:00Z
  ProjectState state = ProjectState::Recording;
  AppInfo app;
  ClockInfo clock;
  int64_t duration_ns = 0;
  std::vector<Display> displays;
  std::vector<Source> sources;

  const Source* find_source(std::string_view id) const;
};

// Creates a new project with a fresh id and creation time.
Project make_project(AppInfo app, ClockInfo clock);

json::Value to_json(const Project& project);

struct LoadError {
  std::string message;
};

// Validates and converts a parsed document. Fails with a field path in the
// message, e.g. "sources[1].file: missing or not a string".
std::optional<Project> project_from_json(const json::Value& doc, LoadError* error = nullptr);

// Reads <dir>/project.json.
std::optional<Project> load_project(const std::string& dir, LoadError* error = nullptr);

// Writes <dir>/project.json atomically (temporary file + rename), so a crash
// mid-write never leaves a truncated manifest.
bool save_project(const Project& project, const std::string& dir, std::string* error = nullptr);

// Rejects paths that could escape the project folder (absolute paths, "..",
// drive letters, backslashes, NUL). Applied to every file name read from disk.
bool is_safe_relative_path(std::string_view path);

std::string generate_uuid_v4();
std::string utc_now_rfc3339();

}  // namespace dr
