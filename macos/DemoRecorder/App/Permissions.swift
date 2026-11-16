import AppKit
import CoreGraphics

/// Screen Recording permission check. P1 turns this into the full onboarding
/// flow with a live check for every permission (spec P-2).
@MainActor
enum Permissions {
  static var hasScreenRecording: Bool { CGPreflightScreenCaptureAccess() }

  /// Shows the system prompt the first time; afterwards macOS only allows
  /// granting it in System Settings, and the app must be relaunched.
  @discardableResult
  static func requestScreenRecording() -> Bool { CGRequestScreenCaptureAccess() }

  static func openScreenRecordingSettings() {
    if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") {
      NSWorkspace.shared.open(url)
    }
  }
}
