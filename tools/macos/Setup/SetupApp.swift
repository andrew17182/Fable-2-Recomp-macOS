// Fable II Setup: first-run app that turns a fresh copy of this project plus the user's own
// Fable II disc image into a playable build. Built and opened by Install.command.
//
// Steps: check prerequisites -> choose the disc image (SHA-256) -> extract the game files
// (built-in Xbox 360 disc reader, GDFX.swift) -> run tools/build_macos_metal.py -> build the
// launcher (tools/macos/build_dev_launcher.sh, not installed) -> headless shader prep -> Open Launcher.
// Passing checks and a known disc image go on by themselves; only a decision needs a click.
// Everything is written inside the project folder (game files in the root, builds in out/).
//
// Unattended / test runs (same code as the buttons):
//   "Fable II Setup.app/Contents/MacOS/FableIISetup" --iso <image> --auto
//       [--extract-only] [--rebuild] [--clean-env] [--stay] [--root <project>]
//   ... --list <image>     print the disc's file tree and exit
//   ... --snapshot <png> --screen checks|disc|disc-unknown|disc-files|working|shaders|finished|failed
//       render one screen with sample state (nothing runs, nothing is written), save the window, quit
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
  /// --auto starts here, not from the window: SwiftUI may not create the window while the app is in the background
  /// (seen 2026-10-19 with a full-screen game in front), and the unattended run must not depend on it.
  func applicationDidFinishLaunching(_ notification: Notification) {
    if let model = Self.model, model.options.auto, SetupSnapshot.request == nil { model.runAutomatically() }
  }
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
          Text("Project folder not found. Run Install.command again.")
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
      Text("Builds Fable II from your own disc image.")
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
      if let snapshot = SetupSnapshot.request { SetupSnapshot.start(snapshot, model: model); return }
      if !model.options.auto { model.work { model.checkAndContinue() } }
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
      item("Done", model.phase == .finished ? .ok : .pending)
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
      Text("Requirements").font(.headline)
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
        Button("Check Again") { model.work { model.checkAndContinue() } }.disabled(model.busy)
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
      Text("Disc Image").font(.headline)
      Text("Choose an .iso of your Fable II disc.")
      if model.gameFilesPresent && model.isoPath == nil {
        HStack {
          Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
          Text("Game files found.")
        }
      }
      HStack {
        Button("Choose Disc Image…") { choose() }.disabled(model.busy || model.hashProgress != nil)
        if let path = model.isoPath { Text(path).lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary) }
      }
      if !model.discSummary.isEmpty { Text(model.discSummary).font(.callout) }
      if let progress = model.hashProgress {
        ProgressView("Checking disc image…", value: progress)
      }
      if let hash = model.isoHash {
        HStack(alignment: .top) {
          StatusIcon(status: model.discWarning == nil ? .ok : .warning)
          VStack(alignment: .leading) {
            Text(model.knownDisc ?? "Unknown disc image")
            Text("SHA-256 \(hash)").font(.caption.monospaced()).textSelection(.enabled).foregroundStyle(.secondary)
          }
        }
      }
      if let warning = model.discWarning {
        Text(warning).font(.callout).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
      }
      Spacer()
      Toggle("Prepare shaders after the build", isOn: $model.warmShaders)
      Text("The first build takes a few minutes.").font(.caption).foregroundStyle(.secondary)
      HStack {
        Button("Back") { model.phase = .checks }
        Spacer()
        // A known disc starts on its own once checked (SetupModel.chooseISO); a button is left only for a choice:
        // an unknown disc, game files already in the folder, or a return here after a cancel or failure.
        if let title = primaryTitle {
          Button(title) {
            if model.isoHash == nil { model.useExistingFiles = true }
            model.work { model.install() }
          }
          .keyboardShortcut(.defaultAction)
          .disabled(!ready)
        }
      }
    }
  }

  var primaryTitle: String? {
    if model.isoHash != nil { return model.knownDisc == nil ? "Continue Anyway" : "Continue" }
    if model.isoPath == nil && model.gameFilesPresent { return "Use These Files" }
    return nil
  }

  var ready: Bool {
    if model.busy || model.hashProgress != nil { return false }
    return model.isoHash != nil || (model.isoPath == nil && model.gameFilesPresent)
  }

  func choose() {
    let panel = NSOpenPanel()
    panel.title = "Choose Disc Image"
    panel.canChooseDirectories = false
    panel.allowsMultipleSelection = false
    if panel.runModal() == .OK, let url = panel.url {
      model.chooseISO(url.path)
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
        Button("Show Log") { model.revealLog() }.disabled(model.logURL == nil)
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
      Text("Log").font(.callout)
      ScrollView {
        Text(model.errorLines.joined(separator: "\n"))
          .font(.system(size: 11, design: .monospaced))
          .textSelection(.enabled)
          .frame(maxWidth: .infinity, alignment: .leading)
          .padding(6)
      }
      .background(Color(nsColor: .textBackgroundColor))
      .border(Color.secondary.opacity(0.3))
      HStack {
        Button("Copy Log") { model.copyLog() }
        Button("Show Log") { model.revealLog() }
        Spacer()
        Button("Back") { model.phase = .disc }
        Button("Try Again") { model.work { model.install() } }.keyboardShortcut(.defaultAction)
      }
    }
  }
}

struct FinishedView: View {
  @ObservedObject var model: SetupModel
  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack { StatusIcon(status: .ok); Text("Fable II is ready.").font(.headline) }
      if let version = model.gameVersion { Text("Version \(version)").foregroundStyle(.secondary).textSelection(.enabled) }
      if let total = model.totalTime { Text("Finished in \(formatDuration(total)).").foregroundStyle(.secondary) }
      Spacer()
      HStack {
        Button("Rebuild") { model.rebuild() }
        Button("Show Log") { model.revealLog() }.disabled(model.logURL == nil)
        Spacer()
        // The launcher is the way to play (2026-10-19): settings, saves, the save library.
        Button("Open Launcher") { NSWorkspace.shared.open(model.paths.rootLauncherApp) }.keyboardShortcut(.defaultAction)
      }
    }
  }
}

// MARK: - Snapshots (tests)

/// --snapshot PATH --screen NAME: shows one screen with sample state (no checks, extraction, build or prep run; no
/// state is written), saves this app's own window to a PNG and quits. CGWindowListCreateImage needs no
/// screen-recording permission for the app's own windows; it is looked up at run time (deprecated API).
enum SetupSnapshot {
  static let request: (path: String, screen: String)? = {
    let args = CommandLine.arguments
    guard let i = args.firstIndex(of: "--snapshot"), i + 1 < args.count else { return nil }
    let screen = args.firstIndex(of: "--screen").flatMap { $0 + 1 < args.count ? args[$0 + 1] : nil } ?? "checks"
    return (args[i + 1], screen)
  }()

  static func start(_ request: (path: String, screen: String), model: SetupModel) {
    model.sample(request.screen)
    DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
      guard let window = NSApp.windows.first(where: { $0.isVisible }) else { print("snapshot: no window"); exit(1) }
      typealias Create = @convention(c) (CGRect, UInt32, UInt32, UInt32) -> Unmanaged<CGImage>?
      guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImage"),
            let image = unsafeBitCast(symbol, to: Create.self)(
              .null, CGWindowListOption.optionIncludingWindow.rawValue, UInt32(window.windowNumber),
              CGWindowImageOption([.boundsIgnoreFraming, .bestResolution]).rawValue)?.takeRetainedValue(),
            let png = NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:]),
            (try? png.write(to: URL(fileURLWithPath: request.path))) != nil else {
        print("snapshot: could not capture the window"); exit(1)
      }
      print(request.path)
      exit(0)
    }
  }
}
