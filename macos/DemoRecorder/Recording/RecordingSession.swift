import AVFoundation
import CoreGraphics
import DRCore
import Foundation
import OSLog

private let log = Logger(subsystem: "com.demorecorder.mac", category: "session")

struct RecordingOptions: Sendable {
  var displayID: CGDirectDisplayID = CGMainDisplayID()
  var fps = 60
  /// nil = the display's native pixel size.
  var maxPixelHeight: Int?
}

struct RecordingSummary: Sendable {
  var folder: URL
  var durationNs: Int64
  var screen: ScreenRecorderStats
  var cursorRows: UInt64
  var eventRows: UInt64
  var inputDropped: UInt64
}

/// One recording: owns the project folder and every capture source.
///
/// P0 records the screen plus cursor/click data. Microphone, system audio and
/// camera tracks are added in P1 as separate files against the same epoch.
@MainActor
final class RecordingSession {
  let folder: URL
  private let project: CoreProject
  private let screen: ScreenRecorder
  private let input: CoreInputRecorder
  private let cursor: CursorSampler
  private let epochNs: Int64

  private init(folder: URL, project: CoreProject, screen: ScreenRecorder, input: CoreInputRecorder,
               cursor: CursorSampler, epochNs: Int64) {
    self.folder = folder
    self.project = project
    self.screen = screen
    self.input = input
    self.cursor = cursor
    self.epochNs = epochNs
  }

  static func start(options: RecordingOptions, in library: URL,
                    onUnexpectedStop: @escaping @Sendable (Error) -> Void) async throws -> RecordingSession {
    let display = DisplayInfo(displayID: options.displayID)
    let (width, height) = display.outputSize(maxPixelHeight: options.maxPixelHeight)
    // H.264 tops out at 4096 wide; larger displays (5K) record HEVC.
    let codec: AVVideoCodecType = (width > 4096 || height > 4096) ? .hevc : .h264
    let bitrate = Self.bitrate(width: width, height: height, fps: options.fps)

    let folder = try Self.makeProjectFolder(in: library)
    // Recording time 0. P1 moves this to the end of the 3-second countdown.
    let epoch = Core.nowNs()

    let project = try CoreProject(appVersion: Bundle.main.shortVersion,
                                  osVersion: Self.osVersion,
                                  epochHostNs: epoch)
    let displayKey = String(options.displayID)
    try project.addDisplay(id: displayKey, widthPx: display.pixelWidth, heightPx: display.pixelHeight,
                           scale: display.scale, refreshHz: display.refreshHz)
    try project.addScreenSource(id: "screen", file: "screen.mp4", displayID: displayKey,
                                codec: codec == .hevc ? "hevc" : "h264",
                                width: width, height: height, fps: Double(options.fps))
    try project.addDataSource(id: "cursor", kind: DR_SOURCE_CURSOR, file: "cursor.json")
    try project.addDataSource(id: "events", kind: DR_SOURCE_EVENTS, file: "events.json")
    // Saved now so that a crash still leaves a readable manifest behind.
    try project.save(to: folder)

    let input = try CoreInputRecorder(folder: folder, epochHostNs: epoch)
    let cursor = CursorSampler(recorder: input, displayBounds: display.bounds,
                               outputWidth: width, outputHeight: height)

    let screen = try ScreenRecorder(configuration: ScreenRecorderConfiguration(
      displayID: options.displayID, width: width, height: height, fps: options.fps, codec: codec,
      bitsPerSecond: bitrate, outputURL: folder.appendingPathComponent("screen.mp4"),
      excludedBundleID: Bundle.main.bundleIdentifier))
    screen.onUnexpectedStop = onUnexpectedStop

    cursor.start()
    do {
      try await screen.start()
    } catch {
      cursor.stop()
      try? input.stop()
      throw error
    }
    log.info("recording to \(folder.path, privacy: .private)")
    return RecordingSession(folder: folder, project: project, screen: screen, input: input,
                            cursor: cursor, epochNs: epoch)
  }

  func stop() async throws -> RecordingSummary {
    let stopNs = Core.nowNs()
    // Every source is stopped and every file closed even if one step fails;
    // the first error is rethrown at the end.
    var firstError: Error?
    var screenStats = ScreenRecorderStats()
    do {
      screenStats = try await screen.stop()
    } catch {
      firstError = error
    }
    cursor.stop()
    let inputStats = input.stats()
    do {
      try input.stop()
    } catch {
      firstError = firstError ?? error
    }

    let endNs = max(stopNs, screenStats.endHostNs ?? stopNs, screenStats.lastFrameHostNs ?? stopNs)
    let duration = max(0, endNs - epochNs)
    if let first = screenStats.firstFrameHostNs {
      try? project.setSourceStart(id: "screen", offsetNs: first - epochNs)
    }
    project.setDuration(ns: duration)
    // A failed recording keeps state "recording" so recovery can find it.
    if firstError == nil { project.markComplete() }
    do {
      try project.save(to: folder)
    } catch {
      firstError = firstError ?? error
    }
    if let firstError { throw firstError }

    return RecordingSummary(folder: folder, durationNs: duration, screen: screenStats,
                            cursorRows: inputStats.cursor_rows, eventRows: inputStats.event_rows,
                            inputDropped: inputStats.cursor_dropped + inputStats.event_dropped)
  }

  // MARK: Helpers

  /// "14.5" or "14.5.1", matching the plain version Windows stores.
  private static var osVersion: String {
    let v = ProcessInfo.processInfo.operatingSystemVersion
    return v.patchVersion == 0 ? "\(v.majorVersion).\(v.minorVersion)"
      : "\(v.majorVersion).\(v.minorVersion).\(v.patchVersion)"
  }

  /// ~0.12 bits per pixel per frame: 1080p60 ≈ 15 Mbit/s ≈ 6.7 GB/hour, inside
  /// the 8 GB/hour budget while staying visually lossless for UI content.
  private static func bitrate(width: Int, height: Int, fps: Int) -> Int {
    Int(Double(width * height * fps) * 0.12)
  }

  private static func makeProjectFolder(in library: URL) throws -> URL {
    let formatter = DateFormatter()
    formatter.locale = Locale(identifier: "en_US_POSIX")
    formatter.dateFormat = "yyyy-MM-dd 'at' HH.mm.ss"
    let name = "Recording \(formatter.string(from: Date())).demorecorder"
    let folder = library.appendingPathComponent(name, isDirectory: true)
    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    return folder
  }
}

/// Geometry of one display in pixels and points.
struct DisplayInfo {
  let bounds: CGRect  // global points, top-left origin
  let pixelWidth: Int
  let pixelHeight: Int
  let refreshHz: Double

  var scale: Double { Double(pixelWidth) / bounds.width }

  init(displayID: CGDirectDisplayID) {
    bounds = CGDisplayBounds(displayID)
    if let mode = CGDisplayCopyDisplayMode(displayID) {
      pixelWidth = mode.pixelWidth
      pixelHeight = mode.pixelHeight
      refreshHz = mode.refreshRate
    } else {
      pixelWidth = Int(bounds.width)
      pixelHeight = Int(bounds.height)
      refreshHz = 0
    }
  }

  /// Even dimensions (required by 4:2:0 encoders), optionally downscaled.
  func outputSize(maxPixelHeight: Int?) -> (Int, Int) {
    var w = Double(pixelWidth)
    var h = Double(pixelHeight)
    if let maxH = maxPixelHeight, Double(maxH) < h {
      w = w * Double(maxH) / h
      h = Double(maxH)
    }
    return (Int(w) & ~1, Int(h) & ~1)
  }
}

extension Bundle {
  var shortVersion: String {
    object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.0.0"
  }
}
