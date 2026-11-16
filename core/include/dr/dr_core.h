/*
 * Demo Recorder core — C ABI.
 *
 * This is the one boundary both native apps use:
 *   macOS   Swift imports it through the module map in core/include/module.modulemap
 *   Windows the C++ engine links it directly; the C# WinUI 3 shell (decision
 *           D3) will P/Invoke the same functions from dr_core.dll
 *
 * Conventions
 *   - Strings are UTF-8, NUL-terminated. Returned strings are owned by the core.
 *   - Functions returning dr_status set a thread-local message readable with
 *     dr_last_error() on failure.
 *   - Host times are nanoseconds on the master clock (dr_clock_now_ns).
 */
#ifndef DR_CORE_H
#define DR_CORE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(DR_CORE_SHARED)
#if defined(DR_CORE_BUILDING)
#define DR_API __declspec(dllexport)
#else
#define DR_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define DR_API __attribute__((visibility("default")))
#else
#define DR_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dr_status {
  DR_OK = 0,
  DR_ERR_INVALID_ARGUMENT = 1,
  DR_ERR_IO = 2,
  DR_ERR_FORMAT = 3,
  DR_ERR_STATE = 4,
} dr_status;

DR_API const char* dr_core_version(void);
DR_API int32_t dr_project_format_version(void);
/* Message for the last failed call on this thread; "" if none. */
DR_API const char* dr_last_error(void);

/* ---- Master clock ------------------------------------------------------ */

/* Raw ticks: mach_absolute_time / QueryPerformanceCounter. */
DR_API uint64_t dr_clock_now_ticks(void);
DR_API int64_t dr_clock_now_ns(void);
/* Converts platform ticks (e.g. a CMTime host time or a WGC SystemRelativeTime
 * already in QPC ticks) to master-clock nanoseconds. */
DR_API int64_t dr_clock_ticks_to_ns(uint64_t ticks);
/* "mach_absolute_time", "qpc" or "clock_monotonic". */
DR_API const char* dr_clock_domain(void);

/* ---- Project manifest (project.json) ----------------------------------- */

typedef struct dr_project dr_project;

typedef enum dr_source_kind {
  DR_SOURCE_SCREEN = 0,
  DR_SOURCE_CAMERA = 1,
  DR_SOURCE_MICROPHONE = 2,
  DR_SOURCE_SYSTEM_AUDIO = 3,
  DR_SOURCE_CURSOR = 4,
  DR_SOURCE_EVENTS = 5,
} dr_source_kind;

/* Creates a project in the "recording" state with a new id. */
DR_API dr_project* dr_project_create(const char* platform, const char* app_version,
                                     const char* os_version, int64_t epoch_host_ns);
DR_API void dr_project_free(dr_project* project);
DR_API const char* dr_project_id(const dr_project* project);

DR_API dr_status dr_project_add_display(dr_project* project, const char* display_id,
                                        int32_t width_px, int32_t height_px, double scale,
                                        double refresh_hz);

DR_API dr_status dr_project_add_video_source(dr_project* project, const char* source_id,
                                             dr_source_kind kind, const char* file,
                                             const char* display_id, const char* codec,
                                             int32_t width, int32_t height, double fps,
                                             int64_t start_offset_ns);

DR_API dr_status dr_project_add_audio_source(dr_project* project, const char* source_id,
                                             dr_source_kind kind, const char* file,
                                             int32_t sample_rate, int32_t channels,
                                             int64_t start_offset_ns);

/* Cursor and events logs. */
DR_API dr_status dr_project_add_data_source(dr_project* project, const char* source_id,
                                            dr_source_kind kind, const char* file);

DR_API dr_status dr_project_set_source_start(dr_project* project, const char* source_id,
                                             int64_t start_offset_ns);
DR_API void dr_project_set_duration_ns(dr_project* project, int64_t duration_ns);
DR_API void dr_project_mark_complete(dr_project* project);

/* Writes <dir>/project.json atomically. Call once when recording starts (so a
 * crash leaves a manifest behind) and again when it stops. */
DR_API dr_status dr_project_save(const dr_project* project, const char* dir);

/* Loads and validates <dir>/project.json; returns NULL on failure. */
DR_API dr_project* dr_project_load(const char* dir);
DR_API int64_t dr_project_duration_ns(const dr_project* project);
DR_API size_t dr_project_source_count(const dr_project* project);

/* ---- Input recorder (cursor.json, events.json) ------------------------- */

typedef struct dr_input_recorder dr_input_recorder;

typedef enum dr_input_event_type {
  DR_EVENT_MOUSE_DOWN = 1,
  DR_EVENT_MOUSE_UP = 2,
  DR_EVENT_KEY_DOWN = 3,
  DR_EVENT_KEY_UP = 4,
} dr_input_event_type;

typedef struct dr_input_stats {
  uint64_t cursor_rows;
  uint64_t event_rows;
  uint64_t cursor_dropped;
  uint64_t event_dropped;
} dr_input_stats;

/* Opens cursor.json and events.json in `dir` and starts the writer thread. */
DR_API dr_input_recorder* dr_input_recorder_start(const char* dir, int64_t epoch_host_ns);

/* Real-time safe: no allocation, no locks. Call each function from one thread
 * at a time. Coordinates are capture-space pixels, origin top-left. */
DR_API void dr_input_recorder_push_cursor(dr_input_recorder* recorder, int64_t host_ns, double x,
                                          double y, int32_t shape_id, uint32_t buttons);
DR_API void dr_input_recorder_push_event(dr_input_recorder* recorder, int64_t host_ns,
                                         dr_input_event_type type, int32_t code);

DR_API dr_input_stats dr_input_recorder_stats(const dr_input_recorder* recorder);
/* Flushes, closes both files and frees the recorder. */
DR_API dr_status dr_input_recorder_stop(dr_input_recorder* recorder);

/* ---- Crash recovery ---------------------------------------------------- */

/* Closes a cursor.json / events.json left open by a crash. */
DR_API dr_status dr_recover_row_log(const char* path, uint64_t* rows_kept);

#ifdef __cplusplus
}
#endif

#endif /* DR_CORE_H */
