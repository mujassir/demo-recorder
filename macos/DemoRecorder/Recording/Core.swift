import CoreMedia
import DRCore
import Foundation

/// Swift face of the shared C++ core (core/include/dr/dr_core.h).
enum Core {
  static var version: String { String(cString: dr_core_version()) }

  /// Master-clock nanoseconds (mach_absolute_time based).
  static func nowNs() -> Int64 { dr_clock_now_ns() }

  /// Converts a host-clock CMTime (ScreenCaptureKit, AVCaptureSession and
  /// Core Audio all stamp samples on the host clock) to master-clock ns.
  static func hostTimeToNs(_ time: CMTime) -> Int64 {
    dr_clock_ticks_to_ns(CMClockConvertHostTimeToSystemUnits(time))
  }

  fileprivate static var lastError: String { String(cString: dr_last_error()) }
}

struct CoreError: LocalizedError {
  let message: String
  var errorDescription: String? { message }
}

private func check(_ status: dr_status) throws {
  if status != DR_OK { throw CoreError(message: Core.lastError) }
}

/// project.json for one recording. Not thread-safe: the owning
/// RecordingSession keeps it on the main actor.
final class CoreProject {
  private let handle: OpaquePointer

  init(appVersion: String, osVersion: String, epochHostNs: Int64) throws {
    guard let handle = dr_project_create("macos", appVersion, osVersion, epochHostNs) else {
      throw CoreError(message: "could not create project")
    }
    self.handle = handle
  }

  deinit { dr_project_free(handle) }

  var id: String { String(cString: dr_project_id(handle)) }

  func addDisplay(id: String, widthPx: Int, heightPx: Int, scale: Double, refreshHz: Double) throws {
    try check(dr_project_add_display(handle, id, Int32(widthPx), Int32(heightPx), scale, refreshHz))
  }

  func addScreenSource(id: String, file: String, displayID: String, codec: String,
                       width: Int, height: Int, fps: Double) throws {
    try check(dr_project_add_video_source(handle, id, DR_SOURCE_SCREEN, file, displayID, codec,
                                          Int32(width), Int32(height), fps, 0))
  }

  func addDataSource(id: String, kind: dr_source_kind, file: String) throws {
    try check(dr_project_add_data_source(handle, id, kind, file))
  }

  func setSourceStart(id: String, offsetNs: Int64) throws {
    try check(dr_project_set_source_start(handle, id, offsetNs))
  }

  func setDuration(ns: Int64) { dr_project_set_duration_ns(handle, ns) }
  func markComplete() { dr_project_mark_complete(handle) }

  func save(to folder: URL) throws {
    try check(dr_project_save(handle, folder.path))
  }
}

/// Writes cursor.json / events.json on a core-owned thread. The push methods
/// are real-time safe (lock-free ring, no allocation), so this wrapper is
/// Sendable as long as each push method is used from one thread at a time.
final class CoreInputRecorder: @unchecked Sendable {
  private var handle: OpaquePointer?

  init(folder: URL, epochHostNs: Int64) throws {
    guard let handle = dr_input_recorder_start(folder.path, epochHostNs) else {
      throw CoreError(message: Core.lastError)
    }
    self.handle = handle
  }

  deinit {
    if let handle { _ = dr_input_recorder_stop(handle) }
  }

  func pushCursor(hostNs: Int64, x: Double, y: Double, shapeID: Int32, buttons: UInt32) {
    guard let handle else { return }
    dr_input_recorder_push_cursor(handle, hostNs, x, y, shapeID, buttons)
  }

  func pushMouse(hostNs: Int64, down: Bool, button: Int32) {
    guard let handle else { return }
    dr_input_recorder_push_event(handle, hostNs, down ? DR_EVENT_MOUSE_DOWN : DR_EVENT_MOUSE_UP, button)
  }

  func stats() -> dr_input_stats {
    guard let handle else { return dr_input_stats() }
    return dr_input_recorder_stats(handle)
  }

  /// Flushes and closes both files. Call after every producer has stopped.
  func stop() throws {
    guard let handle else { return }
    self.handle = nil
    try check(dr_input_recorder_stop(handle))
  }
}
