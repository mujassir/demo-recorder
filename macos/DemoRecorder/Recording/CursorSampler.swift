import CoreGraphics
import Foundation

/// Samples the cursor at 120 Hz into cursor.json (spec I-1) and derives mouse
/// down/up events from button transitions into events.json.
///
/// P0 stand-in: P1 replaces the button polling with a CGEventTap (exact click
/// times, key timing) and adds cursor shape capture (I-4).
final class CursorSampler: @unchecked Sendable {
  static let rateHz = 120.0

  private let queue = DispatchQueue(label: "com.demorecorder.cursor", qos: .userInteractive)
  private let recorder: CoreInputRecorder
  /// Captured display in global coordinates (points, top-left origin).
  private let displayBounds: CGRect
  private let scaleX: Double
  private let scaleY: Double
  // Touched only on `queue`.
  private var timer: DispatchSourceTimer?
  private var lastButtons: UInt32 = 0

  init(recorder: CoreInputRecorder, displayBounds: CGRect, outputWidth: Int, outputHeight: Int) {
    self.recorder = recorder
    self.displayBounds = displayBounds
    scaleX = Double(outputWidth) / displayBounds.width
    scaleY = Double(outputHeight) / displayBounds.height
  }

  func start() {
    queue.sync {
      let timer = DispatchSource.makeTimerSource(flags: .strict, queue: queue)
      let interval = Int(1_000_000_000 / Self.rateHz)
      timer.schedule(deadline: .now(), repeating: .nanoseconds(interval), leeway: .microseconds(500))
      timer.setEventHandler { [weak self] in self?.sample() }
      timer.resume()
      self.timer = timer
    }
  }

  func stop() {
    queue.sync {
      timer?.cancel()
      timer = nil
    }
  }

  private func sample() {
    let now = Core.nowNs()
    guard let location = CGEvent(source: nil)?.location else { return }
    // Global points → capture-space pixels (origin top-left of the display).
    let x = (location.x - displayBounds.minX) * scaleX
    let y = (location.y - displayBounds.minY) * scaleY

    var buttons: UInt32 = 0
    if CGEventSource.buttonState(.combinedSessionState, button: .left) { buttons |= 1 << 0 }
    if CGEventSource.buttonState(.combinedSessionState, button: .right) { buttons |= 1 << 1 }
    if CGEventSource.buttonState(.combinedSessionState, button: .center) { buttons |= 1 << 2 }

    recorder.pushCursor(hostNs: now, x: x, y: y, shapeID: 0, buttons: buttons)

    let changed = buttons ^ lastButtons
    if changed != 0 {
      for bit: UInt32 in 0..<3 where changed & (1 << bit) != 0 {
        recorder.pushMouse(hostNs: now, down: buttons & (1 << bit) != 0, button: Int32(bit))
      }
      lastButtons = buttons
    }
  }
}
