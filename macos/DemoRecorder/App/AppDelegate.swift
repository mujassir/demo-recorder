import AppKit
import OSLog

private let log = Logger(subsystem: "com.demorecorder.mac", category: "app")

@main
enum DemoRecorderMain {
  @MainActor
  static func main() {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    app.setActivationPolicy(.accessory)  // menu bar only (spec P-1)
    withExtendedLifetime(delegate) { app.run() }
  }
}

/// Menu bar shell. P0 scope: start/stop a screen recording and open the
/// library. The recording flow (countdown, hotkeys, region picker) is P1.
///
/// Scripted mode for nightly performance runs (spec: Engineering practices):
///   "Demo Recorder.app/Contents/MacOS/Demo Recorder" --record-seconds 600 \
///       [--output DIR] [--max-height 1080] [--fps 60]
/// records, writes stats.json into the project folder, and quits.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
  private enum State {
    case idle
    case starting
    case recording(RecordingSession, startedAt: Date)
    case stopping
  }

  private var state = State.idle {
    didSet { updateMenu() }
  }

  private var statusItem: NSStatusItem!
  private let toggleItem = NSMenuItem(title: "Start Recording", action: #selector(toggleRecording), keyEquivalent: "")
  private var elapsedTimer: Timer?
  private var recordingStartedAt: Date?
  private var scripted: ScriptedRun?

  private var libraryURL: URL {
    if let output = scripted?.output { return output }
    let movies = FileManager.default.urls(for: .moviesDirectory, in: .userDomainMask).first
      ?? FileManager.default.homeDirectoryForCurrentUser
    return movies.appendingPathComponent("Demo Recorder", isDirectory: true)
  }

  func applicationDidFinishLaunching(_ notification: Notification) {
    statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    statusItem.button?.imagePosition = .imageLeading
    statusItem.button?.setAccessibilityLabel("Demo Recorder")

    let menu = NSMenu()
    toggleItem.target = self
    menu.addItem(toggleItem)
    menu.addItem(.separator())
    let open = NSMenuItem(title: "Open Recordings Folder", action: #selector(openLibrary), keyEquivalent: "")
    open.target = self
    menu.addItem(open)
    menu.addItem(.separator())
    let about = NSMenuItem(title: "Demo Recorder \(Bundle.main.shortVersion) (core \(Core.version))",
                           action: nil, keyEquivalent: "")
    about.isEnabled = false
    menu.addItem(about)
    menu.addItem(NSMenuItem(title: "Quit Demo Recorder", action: #selector(NSApplication.terminate(_:)),
                            keyEquivalent: "q"))
    statusItem.menu = menu
    updateMenu()

    scripted = ScriptedRun(arguments: ProcessInfo.processInfo.arguments)
    if let scripted {
      log.info("scripted run: \(scripted.seconds) s")
      startRecording()
    }
  }

  // MARK: Actions

  @objc private func toggleRecording() {
    switch state {
    case .idle: startRecording()
    case .recording: stopRecording()
    case .starting, .stopping: break
    }
  }

  @objc private func openLibrary() {
    try? FileManager.default.createDirectory(at: libraryURL, withIntermediateDirectories: true)
    NSWorkspace.shared.open(libraryURL)
  }

  private func startRecording() {
    guard case .idle = state else { return }
    guard Permissions.hasScreenRecording else {
      Permissions.requestScreenRecording()
      showPermissionAlert()
      if scripted != nil { NSApp.terminate(nil) }
      return
    }
    state = .starting
    var options = RecordingOptions()
    if let scripted {
      options.fps = scripted.fps
      options.maxPixelHeight = scripted.maxHeight
    }
    Task {
      do {
        let session = try await RecordingSession.start(options: options, in: libraryURL) { [weak self] error in
          Task { @MainActor in self?.handleUnexpectedStop(error) }
        }
        state = .recording(session, startedAt: Date())
        if let seconds = scripted?.seconds {
          try? await Task.sleep(for: .seconds(seconds))
          stopRecording()
        }
      } catch {
        state = .idle
        report(error, title: "Recording could not start")
      }
    }
  }

  private func stopRecording() {
    guard case let .recording(session, _) = state else { return }
    state = .stopping
    Task {
      do {
        let summary = try await session.stop()
        state = .idle
        finished(summary)
      } catch {
        state = .idle
        report(error, title: "Recording could not be saved")
      }
    }
  }

  private func handleUnexpectedStop(_ error: Error) {
    log.error("capture stopped unexpectedly: \(error.localizedDescription)")
    stopRecording()
  }

  private func finished(_ summary: RecordingSummary) {
    let stats = StatsReport(summary: summary)
    stats.write(to: summary.folder)
    if scripted != nil {
      print(stats.json)
      NSApp.terminate(nil)
      return
    }
    NSWorkspace.shared.activateFileViewerSelecting([summary.folder])
    if summary.screen.framesDroppedEncoderBusy > 0 || summary.inputDropped > 0 {
      let alert = NSAlert()
      alert.messageText = "Recording saved with dropped frames"
      alert.informativeText = stats.humanReadable
      alert.runModal()
    }
  }

  // MARK: UI

  private func updateMenu() {
    guard let button = statusItem?.button else { return }
    elapsedTimer?.invalidate()
    elapsedTimer = nil
    switch state {
    case .idle:
      toggleItem.title = "Start Recording"
      toggleItem.isEnabled = true
      button.image = NSImage(systemSymbolName: "record.circle", accessibilityDescription: "Demo Recorder")
      button.title = ""
    case .starting:
      toggleItem.title = "Starting…"
      toggleItem.isEnabled = false
    case let .recording(_, startedAt):
      toggleItem.title = "Stop Recording"
      toggleItem.isEnabled = true
      button.image = NSImage(systemSymbolName: "stop.circle.fill", accessibilityDescription: "Stop recording")
      recordingStartedAt = startedAt
      tickElapsed()
      elapsedTimer = Timer.scheduledTimer(timeInterval: 1, target: self, selector: #selector(tickElapsed),
                                          userInfo: nil, repeats: true)
    case .stopping:
      toggleItem.title = "Saving…"
      toggleItem.isEnabled = false
      button.title = ""
    }
  }

  @objc private func tickElapsed() {
    guard let recordingStartedAt else { return }
    let s = Int(Date().timeIntervalSince(recordingStartedAt))
    statusItem.button?.title = String(format: " %d:%02d", s / 60, s % 60)
  }

  private func showPermissionAlert() {
    let alert = NSAlert()
    alert.messageText = "Allow Screen Recording"
    alert.informativeText = """
      Demo Recorder needs the Screen Recording permission. Turn it on in \
      System Settings → Privacy & Security → Screen & System Audio Recording, \
      then quit and reopen Demo Recorder.
      """
    alert.addButton(withTitle: "Open System Settings")
    alert.addButton(withTitle: "Later")
    NSApp.activate(ignoringOtherApps: true)
    if alert.runModal() == .alertFirstButtonReturn {
      Permissions.openScreenRecordingSettings()
    }
  }

  private func report(_ error: Error, title: String) {
    log.error("\(title): \(error.localizedDescription)")
    if scripted != nil {
      FileHandle.standardError.write(Data("\(title): \(error.localizedDescription)\n".utf8))
      exit(1)
    }
    let alert = NSAlert()
    alert.messageText = title
    alert.informativeText = error.localizedDescription
    NSApp.activate(ignoringOtherApps: true)
    alert.runModal()
  }
}

/// Command-line options for unattended recordings.
struct ScriptedRun {
  var seconds: TimeInterval
  var output: URL?
  var fps = 60
  var maxHeight: Int?

  init?(arguments: [String]) {
    func value(_ flag: String) -> String? {
      guard let i = arguments.firstIndex(of: flag), i + 1 < arguments.count else { return nil }
      return arguments[i + 1]
    }
    guard let s = value("--record-seconds").flatMap(TimeInterval.init), s > 0 else { return nil }
    seconds = s
    output = value("--output").map { URL(fileURLWithPath: $0, isDirectory: true) }
    if let f = value("--fps").flatMap(Int.init), f > 0 { fps = f }
    maxHeight = value("--max-height").flatMap(Int.init)
  }
}

/// stats.json written next to project.json; consumed by the G0 perf script.
struct StatsReport {
  let summary: RecordingSummary

  var json: String {
    let s = summary.screen
    let fields: [(String, String)] = [
      ("duration_ns", "\(summary.durationNs)"),
      ("frames_written", "\(s.framesWritten)"),
      ("frames_idle", "\(s.framesIdle)"),
      ("frames_dropped_encoder_busy", "\(s.framesDroppedEncoderBusy)"),
      ("frames_append_failed", "\(s.framesAppendFailed)"),
      ("frames_other_status", "\(s.framesOtherStatus)"),
      ("cursor_rows", "\(summary.cursorRows)"),
      ("event_rows", "\(summary.eventRows)"),
      ("input_dropped", "\(summary.inputDropped)"),
    ]
    return "{\n" + fields.map { "  \"\($0.0)\": \($0.1)" }.joined(separator: ",\n") + "\n}\n"
  }

  var humanReadable: String {
    let s = summary.screen
    return """
      \(s.framesWritten) frames written, \(s.framesDroppedEncoderBusy) dropped (encoder busy), \
      \(s.framesAppendFailed) failed. \(summary.inputDropped) cursor samples dropped.
      """
  }

  func write(to folder: URL) {
    try? Data(json.utf8).write(to: folder.appendingPathComponent("stats.json"), options: .atomic)
  }
}
