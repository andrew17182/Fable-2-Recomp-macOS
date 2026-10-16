// Fable II Setup: first-run app that turns a fresh copy of this project plus the user's own
// Fable II disc image into a playable build. Built and opened by Install.command.
//
// Steps: check prerequisites -> choose the disc image (SHA-256) -> extract the game files
// (built-in Xbox 360 disc reader, GDFX.swift) -> run tools/build_macos_metal.py -> build the
// Dev Launcher (tools/macos/build_dev_launcher.sh, not installed) -> Play.
// Everything is written inside the project folder (game files in the root, builds in out/).
//
// Unattended / test runs (same code as the buttons):
//   "Fable II Setup.app/Contents/MacOS/FableIISetup" --iso <image> --auto
//       [--extract-only] [--rebuild] [--clean-env] [--stay] [--root <project>]
//   ... --list <image>     print the disc's file tree and exit
import AppKit
import SwiftUI

@main
struct SetupMain {
  static func main() {
    let options = Options.parse(CommandLine.arguments)
    if let iso = options.list {
      do {
        let disc = try DiscImage(path: iso)
        print(String(format: "GDFX header at 0x%llX", disc.headerOffset))
        for entry in try disc.rootEntries() {
          for file in entry.files { print("\(file.size)\t\(file.path)") }
        }
        exit(0)
      } catch {
        print(error.localizedDescription)
        exit(2)
      }
    }
    SetupApp.main()
  }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
  static var model: SetupModel?
  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
  func applicationWillTerminate(_ notification: Notification) { Self.model?.cancel() }
}

struct SetupApp: App {
  @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
  @StateObject private var holder = ModelHolder()

  var body: some Scene {
    WindowGroup("Fable II Setup") {
      if let model = holder.model {
        SetupView(model: model).frame(minWidth: 720, minHeight: 560)
      } else {
        VStack(spacing: 12) {
          Text("Fable II Setup").font(.title2.bold())
          Text("Could not find the project folder. Keep this app inside the project's out/setup folder, or run Install.command again.")
        }.padding(30).frame(width: 520)
      }
    }
    .windowResizability(.contentMinSize)
  }
}

final class ModelHolder: ObservableObject {
  let model: SetupModel?
  init() {
    let options = Options.parse(CommandLine.arguments)
    if let paths = Paths.find(options) {
      model = SetupModel(options: options, paths: paths)
      AppDelegate.model = model
    } else {
      model = nil
    }
  }
}

// MARK: - Views

struct StatusIcon: View {
  let status: Status
  var body: some View {
    Group {
      switch status {
      case .pending: Image(systemName: "circle").foregroundStyle(.secondary)
      case .running: ProgressView().controlSize(.small)
      case .ok: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
      case .skipped: Image(systemName: "checkmark.circle").foregroundStyle(.green)
      case .warning: Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow)
      case .failed: Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
      }
    }.frame(width: 18)
  }
}

struct SetupView: View {
  @ObservedObject var model: SetupModel

  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      Text("Fable II Setup").font(.title2.bold())
      Text("Builds the macOS version of Fable II from your own disc image. Everything stays in this project folder.")
        .foregroundStyle(.secondary)
      StepBar(model: model)
      Divider()
      Group {
        switch model.phase {
        case .checks: ChecksView(model: model)
        case .disc: DiscView(model: model)
        case .working: WorkingView(model: model)
        case .finished: FinishedView(model: model)
        case .failed: FailedView(model: model)
        }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
    .padding(20)
    .onAppear {
      if model.options.auto { model.runAutomatically() } else { model.work { model.runChecks() } }
    }
  }
}

struct StepBar: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    let checks: Status = model.phase == .checks ? (model.busy ? .running : .pending) : .ok
    let disc: Status = model.phase == .checks ? .pending : model.phase == .disc ? .running : .ok
    HStack(spacing: 18) {
      item("Check", checks)
      item("Disc image", disc)
      item("Extract", model.extractStatus)
      item("Build", model.buildStatus)
      item("Launcher", model.launcherStatus)
      item("Shaders", model.warmStatus)
      item("Play", model.phase == .finished ? .ok : .pending)
    }
  }
  func item(_ title: String, _ status: Status) -> some View {
    HStack(spacing: 5) { StatusIcon(status: status); Text(title) }
  }
}

struct ChecksView: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      Text("1. Prerequisites").font(.headline)
      if model.checks.isEmpty { ProgressView("Checking…") }
      ForEach(model.checks) { check in
        HStack(alignment: .top, spacing: 8) {
          StatusIcon(status: check.status)
          VStack(alignment: .leading, spacing: 2) {
            Text(check.title)
            Text(check.detail).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
          }
          Spacer()
          if let action = check.action {
            Button(label(action)) { model.perform(action) }.disabled(model.busy)
          }
        }
      }
      if !model.activity.isEmpty {
        HStack { ProgressView().controlSize(.small); Text(model.activity) }
      }
      Spacer()
      HStack {
        Button("Check again") { model.work { model.runChecks() } }.disabled(model.busy)
        Spacer()
        Button("Continue") { model.phase = .disc }
          .keyboardShortcut(.defaultAction)
          .disabled(!model.checksPassed || model.busy)
      }
    }
  }
  func label(_ action: CheckAction) -> String {
    switch action {
    case .installCLT: return "Install…"
    case .getXcode: return "Get Xcode…"
    case .installMetal: return "Install"
    case .downloadCMake: return "Download CMake"
    }
  }
}

struct DiscView: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("2. Your Fable II disc image").font(.headline)
      Text("Choose an .iso of your own Fable II disc (the Game of the Year edition was used for development). default.xex, data/, $SystemUpdate/ and nxeart are copied into the project folder; nothing is sent anywhere.")
        .fixedSize(horizontal: false, vertical: true)
      if model.gameFilesPresent && model.isoPath == nil {
        HStack {
          Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
          Text("Game files are already in the project folder.")
          Toggle("Use them (skip the disc image)", isOn: $model.useExistingFiles)
        }
      }
      HStack {
        Button("Choose disc image…") { choose() }.disabled(model.busy || model.hashProgress != nil)
        if let path = model.isoPath { Text(path).lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary) }
      }
      if !model.discSummary.isEmpty { Text(model.discSummary).font(.callout) }
      if let progress = model.hashProgress {
        ProgressView("Computing SHA-256…", value: progress)
      }
      if let hash = model.isoHash {
        HStack(alignment: .top) {
          StatusIcon(status: model.discWarning == nil ? .ok : .warning)
          VStack(alignment: .leading) {
            Text(model.knownDisc.map { "Known disc image: \($0)" } ?? "Unknown disc image")
            Text("SHA-256 \(hash)").font(.caption.monospaced()).textSelection(.enabled).foregroundStyle(.secondary)
          }
        }
      }
      if let warning = model.discWarning {
        Text(warning).font(.callout).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
      }
      Spacer()
      Toggle("Prepare shaders after the build (about 2 minutes: the game runs muted in a background window and closes itself; fewer stutters the first time you play)",
             isOn: $model.warmShaders)
      Text("The first build downloads about 200 MB of sources and takes a few minutes (about 3 minutes on an 18-core Mac; longer on smaller Macs). Later builds are faster.")
        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      HStack {
        Button("Back") { model.phase = .checks }
        Spacer()
        Button("Extract and build") { model.work { model.install() } }
          .keyboardShortcut(.defaultAction)
          .disabled(!ready)
      }
    }
  }

  var ready: Bool {
    if model.busy || model.hashProgress != nil { return false }
    return model.isoHash != nil || (model.useExistingFiles && model.gameFilesPresent)
  }

  func choose() {
    let panel = NSOpenPanel()
    panel.title = "Choose your Fable II disc image"
    panel.canChooseDirectories = false
    panel.allowsMultipleSelection = false
    if panel.runModal() == .OK, let url = panel.url {
      model.work { model.prepareISO(url.path) }
    }
  }
}

struct LogView: View {
  let lines: [String]
  var body: some View {
    ScrollViewReader { proxy in
      ScrollView {
        LazyVStack(alignment: .leading, spacing: 0) {
          ForEach(Array(lines.enumerated()), id: \.offset) { index, line in
            Text(line).font(.system(size: 11, design: .monospaced)).lineLimit(1).id(index)
          }
        }.padding(6).frame(maxWidth: .infinity, alignment: .leading)
      }
      .background(Color(nsColor: .textBackgroundColor))
      .border(Color.secondary.opacity(0.3))
      .onChange(of: lines.count) { proxy.scrollTo(lines.count - 1, anchor: .bottom) }
    }
  }
}

struct WorkingView: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      if model.buildStatus == .running || model.buildStatus == .ok || model.buildStatus == .failed {
        ForEach(model.stages) { stage in
          HStack(spacing: 6) {
            StatusIcon(status: stage.status)
            Text(stage.title).foregroundStyle(stage.status == .pending ? .secondary : .primary)
            Spacer()
            if let duration = stage.duration { Text(formatDuration(duration)).font(.caption).monospacedDigit() }
          }
        }
      }
      if !model.activity.isEmpty {
        if let progress = model.progress {
          ProgressView(model.activity, value: progress)
        } else {
          HStack { ProgressView().controlSize(.small); Text(model.activity).lineLimit(1) }
        }
      }
      LogView(lines: model.logLines)
      HStack {
        Button("Reveal log") { model.revealLog() }.disabled(model.logURL == nil)
        Spacer()
        Button("Cancel") { model.cancel() }
      }
    }
  }
}

struct FailedView: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack { StatusIcon(status: .failed); Text(model.errorTitle).font(.headline) }
      Text("Last error lines:").font(.callout)
      ScrollView {
        Text(model.errorLines.joined(separator: "\n"))
          .font(.system(size: 11, design: .monospaced))
          .textSelection(.enabled)
          .frame(maxWidth: .infinity, alignment: .leading)
          .padding(6)
      }
      .background(Color(nsColor: .textBackgroundColor))
      .border(Color.secondary.opacity(0.3))
      if let url = model.logURL {
        Text("Full log: \(url.path)").font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
      }
      HStack {
        Button("Copy log") { model.copyLog() }
        Button("Reveal log") { model.revealLog() }
        Spacer()
        Button("Back") { model.phase = .disc }
        Button("Try again") { model.work { model.install() } }.keyboardShortcut(.defaultAction)
      }
    }
  }
}

struct FinishedView: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack { StatusIcon(status: .ok); Text("Fable II is ready.").font(.headline) }
      if let version = model.gameVersion { Text("Fable II Recomp \(version)").foregroundStyle(.secondary).textSelection(.enabled) }
      if let total = model.totalTime { Text("Finished in \(formatDuration(total)).").foregroundStyle(.secondary) }
      appRow("Game (direct start, advanced)", model.paths.rootGameApp)
      appRow("Fable II Launcher (the way to play: pick a save and boot straight into it)", model.paths.rootLauncherApp)
      Text("Both apps are in the project folder, next to Install.command; you can drag them to the Dock. Saves and settings go to ~/Library/Application Support/Fable II Recomp.")
        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      Spacer()
      HStack {
        Button("Rebuild") { model.rebuild() }
        Button("Reveal log") { model.revealLog() }.disabled(model.logURL == nil)
        Spacer()
        // The launcher is the way to play (settings, saves, Continue, the save library); the game app directly is
        // the advanced path (2026-10-15).
        Button("Play Directly") { NSWorkspace.shared.open(model.paths.rootGameApp) }
          .help("Opens Fable II.app without the launcher (the launcher's skip-intro choice still applies).")
        Button("Open Launcher") { NSWorkspace.shared.open(model.paths.rootLauncherApp) }.keyboardShortcut(.defaultAction)
      }
    }
  }

  func appRow(_ title: String, _ url: URL) -> some View {
    VStack(alignment: .leading, spacing: 2) {
      Text(title)
      HStack {
        Text(url.path.replacingOccurrences(of: model.paths.root.path + "/", with: ""))
          .font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
        Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
          .buttonStyle(.link).font(.caption)
      }
    }
  }
}
