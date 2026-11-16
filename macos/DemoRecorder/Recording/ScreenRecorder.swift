import AVFoundation
import OSLog
import ScreenCaptureKit

private let log = Logger(subsystem: "com.demorecorder.mac", category: "screen")

struct ScreenRecorderStats: Sendable {
  var framesWritten = 0
  /// Screen unchanged; ScreenCaptureKit sends no new pixels. Not a drop.
  var framesIdle = 0
  /// The encoder was not ready (would show as a stutter). G0 target: 0.
  var framesDroppedEncoderBusy = 0
  var framesAppendFailed = 0
  var framesOtherStatus = 0
  var firstFrameHostNs: Int64?
  var lastFrameHostNs: Int64?
  /// Where the video ends: when stop was requested, or the last frame if later.
  var endHostNs: Int64?
}

struct ScreenRecorderConfiguration: Sendable {
  var displayID: CGDirectDisplayID
  var width: Int
  var height: Int
  var fps: Int
  var codec: AVVideoCodecType
  var bitsPerSecond: Int
  var outputURL: URL
  /// Hides Demo Recorder's own windows from the capture (spec C-8).
  var excludedBundleID: String?
}

/// Spike for P0: ScreenCaptureKit → VideoToolbox (through AVAssetWriter) →
/// fragmented MP4.
///
/// Frames arrive as IOSurface-backed NV12 pixel buffers and go straight to the
/// hardware encoder: no CPU readback, no color conversion, no effects. The
/// cursor is not captured; it is logged as data by CursorSampler (spec I-1).
final class ScreenRecorder: NSObject, SCStreamOutput, SCStreamDelegate, @unchecked Sendable {
  // Everything below is touched only on `queue`, except during start/stop,
  // which serialize with the queue explicitly.
  private let queue = DispatchQueue(label: "com.demorecorder.screen", qos: .userInteractive)
  private let config: ScreenRecorderConfiguration
  private let writer: AVAssetWriter
  private let input: AVAssetWriterInput
  private var stream: SCStream?
  private var sessionStarted = false
  private var lastPTS = CMTime.invalid
  private var stopped = false
  private var stats = ScreenRecorderStats()

  /// Called on an arbitrary queue if capture stops on its own (display
  /// unplugged, permission revoked).
  var onUnexpectedStop: (@Sendable (Error) -> Void)?

  init(configuration: ScreenRecorderConfiguration) throws {
    config = configuration
    writer = try AVAssetWriter(outputURL: configuration.outputURL, fileType: .mp4)
    // Write a movie fragment every second: after a crash, everything up to
    // the last fragment is still playable (spec C-9).
    writer.movieFragmentInterval = CMTime(seconds: 1, preferredTimescale: 600)
    writer.shouldOptimizeForNetworkUse = false

    var compression: [String: Any] = [
      AVVideoAverageBitRateKey: configuration.bitsPerSecond,
      AVVideoExpectedSourceFrameRateKey: configuration.fps,
      AVVideoMaxKeyFrameIntervalDurationKey: 1,
      AVVideoAllowFrameReorderingKey: false,
    ]
    if configuration.codec == .h264 {
      compression[AVVideoProfileLevelKey] = AVVideoProfileLevelH264HighAutoLevel
    }
    let settings: [String: Any] = [
      AVVideoCodecKey: configuration.codec,
      AVVideoWidthKey: configuration.width,
      AVVideoHeightKey: configuration.height,
      AVVideoCompressionPropertiesKey: compression,
    ]
    input = AVAssetWriterInput(mediaType: .video, outputSettings: settings)
    input.expectsMediaDataInRealTime = true
    guard writer.canAdd(input) else {
      throw CoreError(message: "The video encoder rejected \(configuration.width)×\(configuration.height) \(configuration.codec.rawValue)")
    }
    writer.add(input)
    super.init()
  }

  func start() async throws {
    guard writer.startWriting() else {
      throw writer.error ?? CoreError(message: "Could not start writing the screen recording")
    }

    let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
    guard let display = content.displays.first(where: { $0.displayID == config.displayID }) else {
      writer.cancelWriting()
      throw CoreError(message: "The selected display is no longer connected")
    }
    let excluded = content.applications.filter { app in
      config.excludedBundleID != nil && app.bundleIdentifier == config.excludedBundleID
    }
    let filter = SCContentFilter(display: display, excludingApplications: excluded, exceptingWindows: [])

    let sc = SCStreamConfiguration()
    sc.width = config.width
    sc.height = config.height
    sc.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(config.fps))
    // NV12 straight from ScreenCaptureKit; the encoder consumes it directly.
    sc.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
    sc.colorSpaceName = CGColorSpace.sRGB
    sc.showsCursor = false
    sc.queueDepth = 6
    sc.capturesAudio = false  // system audio arrives in P1 as its own track

    let stream = SCStream(filter: filter, configuration: sc, delegate: self)
    try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
    do {
      try await stream.startCapture()
    } catch {
      writer.cancelWriting()
      throw error
    }
    queue.sync { self.stream = stream }
    log.info("capture started \(self.config.width)x\(self.config.height)@\(self.config.fps) \(self.config.codec.rawValue)")
  }

  /// Stops capture and finalizes the file. Returns frame statistics.
  func stop() async throws -> ScreenRecorderStats {
    // ScreenCaptureKit only delivers changed frames, so a static screen at
    // the end sends nothing; end the movie at the stop time, not the last frame.
    let stopTime = CMClockGetTime(CMClockGetHostTimeClock())
    let stream = queue.sync { self.stream }
    if let stream {
      do {
        try await stream.stopCapture()
      } catch {
        log.error("stopCapture failed: \(error.localizedDescription)")
      }
    }
    // After this no callback can append, so the writer is ours alone.
    let (started, lastPTS, capturedStats) = queue.sync { () -> (Bool, CMTime, ScreenRecorderStats) in
      self.stopped = true
      self.stream = nil
      return (self.sessionStarted, self.lastPTS, self.stats)
    }
    var finalStats = capturedStats
    guard started else {
      writer.cancelWriting()
      throw CoreError(message: "No frames were captured. Check the Screen Recording permission.")
    }
    let endTime = lastPTS.isValid ? CMTimeMaximum(stopTime, lastPTS) : stopTime
    // Holds the last frame on screen until the end time.
    writer.endSession(atSourceTime: endTime)
    finalStats.endHostNs = Core.hostTimeToNs(endTime)
    input.markAsFinished()
    await writer.finishWriting()
    if writer.status == .failed {
      throw writer.error ?? CoreError(message: "Could not finish the screen recording")
    }
    log.info("capture stopped: \(finalStats.framesWritten) written, \(finalStats.framesDroppedEncoderBusy) dropped")
    return finalStats
  }

  // MARK: SCStreamOutput (runs on `queue`)

  func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
              of type: SCStreamOutputType) {
    guard type == .screen, !stopped, sampleBuffer.isValid else { return }
    guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false)
      as? [[SCStreamFrameInfo: Any]],
      let rawStatus = attachments.first?[.status] as? Int,
      let status = SCFrameStatus(rawValue: rawStatus)
    else { return }

    switch status {
    case .complete:
      break
    case .idle:
      stats.framesIdle += 1
      return
    default:
      stats.framesOtherStatus += 1
      return
    }

    let pts = sampleBuffer.presentationTimeStamp
    let hostNs = Core.hostTimeToNs(pts)
    if !sessionStarted {
      writer.startSession(atSourceTime: pts)
      sessionStarted = true
      stats.firstFrameHostNs = hostNs
    }
    guard input.isReadyForMoreMediaData else {
      stats.framesDroppedEncoderBusy += 1
      return
    }
    if input.append(sampleBuffer) {
      stats.framesWritten += 1
      stats.lastFrameHostNs = hostNs
      lastPTS = pts
    } else {
      stats.framesAppendFailed += 1
    }
  }

  // MARK: SCStreamDelegate

  func stream(_ stream: SCStream, didStopWithError error: Error) {
    log.error("stream stopped: \(error.localizedDescription)")
    onUnexpectedStop?(error)
  }
}
