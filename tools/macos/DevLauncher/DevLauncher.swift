// Fable II Dev Launcher: pick a save, boot straight into it, and have the game's own
// saves copied back (live slot + datestamped library entry).
//
// All session/save logic lives in tools/fable2_dev_session.py (shared with the test
// tools); this app is a small SwiftUI front end for it. Build/install with
// tools/macos/build_dev_launcher.sh.
//
// Layout (2026-10-12): the main window only plays (build, save, Launch, status). Every setting
// is in the Settings window (⌘,; tabs General, Display, Performance, Fixes, Diagnostics), and the
// actions are in the Tools menu (benchmark, portable export, folders). UserDefaults keys are the
// same as before the split, so saved choices carry over.
//
// CLI test mode (same code path as the Launch button):
//   "Fable II Dev Launcher.app/Contents/MacOS/FableIIDevLauncher" --launch
//       [--pick title|live:Hero000|library:NAME] [--no-skip-intro] [--no-boot]
//       [--target-slot HeroNNN|none] [--test-control] [--fps settings|auto|30|60|uncapped]
//       [--turbo-notice overlay|game] [--turbo-speed 1.1-4.0] [--turbo-audio natural|high-pitch|normal] [--trace-preset standard|off|loading|gpu|everything]
//       [--resolution auto|native|2|3] [--upscaling off|metalfx-spatial] [--effects original|full]
//       [--display windowed|fullscreen] [--build playtest|latest] [--extra-env "NAME=VALUE ..."]
//       [--<switch>|--no-<switch>] for every on/off setting (LaunchSettings.flags: fur-fix,
//       memexport-readback, unlock-extras, fps-counter, shader-autolearn, rtt-1x, rt-shrink, ...)
//       [--keyboard-map KEY:INPUT,...] [--mouse-look|--no-mouse-look] [--mouse-look-scale N]
//       [--bind bind_console|bind_settings|bind_debug_overlay|bind_achievements=KEY] [--view-button-to-game]
//       [--print-args]   print the fable2_dev_session.py arguments as JSON instead of launching
// With no --pick it uses the choices last made in the window.
//   --snapshot PATH.png [--tab main|picker|general|display|controls|performance|fixes|diagnostics] [--pick ...]
//       opens the main window, the save picker sheet (picker) or a Settings tab (without taking focus), saves
//       that window to a PNG, quits.
//   --picker-rows LIST.json [--query TEXT] [--show-tests] [--meta PICKER.json]: the picker's rows as JSON.
import AppKit
import Foundation
import IOKit
import SwiftUI

// Settings model, tables and the game-environment mapping: tools/macos/Shared/LaunchSettings.swift.
// Settings tab views: tools/macos/Shared/SettingsViews.swift (both shared with the player app).

// MARK: - Backend (tools/fable2_dev_session.py)

enum Backend {
  static var repoRoot: String { DevCheckout.path ?? "" }
  static var script: String { repoRoot + "/tools/fable2_dev_session.py" }

  /// Version of the built game ("0.1.0 (c0b9d4a)"): the Fable2VersionString key that
  /// tools/package_macos_app.py writes into Fable II.app; falls back to VERSION.txt.
  static var gameVersion: String {
    let plist = repoRoot + "/out/build/mac-arm64-metal-release/Fable II.app/Contents/Info.plist"
    if let info = NSDictionary(contentsOfFile: plist),
       let version = info["Fable2VersionString"] as? String, !version.isEmpty {
      return version
    }
    let file = (try? String(contentsOfFile: repoRoot + "/VERSION.txt", encoding: .utf8))?
      .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    return file.isEmpty ? "(not built)" : file + " (not built)"
  }

  /// Version the playtest copy was frozen from (out/playtest/playtest.json), nil when there is none.
  static var playtestVersion: String? {
    let path = repoRoot + "/out/playtest/playtest.json"
    guard let data = FileManager.default.contents(atPath: path),
          let info = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
    return (info["version"] as? String) ?? "unknown version"
  }

  /// Whether the frozen playtest copy exists (a fresh checkout has none until Update Playtest Build creates it);
  /// the same check as tools/fable2_dev_session.py launch.
  static var playtestExists: Bool {
    var dir: ObjCBool = false
    return FileManager.default.fileExists(atPath: repoRoot + "/out/playtest/Fable II.app", isDirectory: &dir)
      && dir.boolValue
  }

  /// Note shown when a Playtest pick launches Latest because there is no playtest copy.
  static let playtestFallbackNote = "No playtest copy, so the latest build was used (Update Playtest Build creates one)."

  /// Whether the app a build choice launches has the shader converter (docs/no-converter.md): the playtest copy
  /// (out/playtest) or the latest build (out/build).
  static func hasConverter(_ build: BuildChoice) -> Bool {
    let app = build.effective(playtestExists: playtestExists) == .playtest ? "/out/playtest/Fable II.app" : "/out/build/mac-arm64-metal-release/Fable II.app"
    return LaunchSettings.hasConverter(macOSFolder: repoRoot + app + "/Contents/MacOS")
  }

  /// Live state folder of Fable II.app (logs, saves), as tools/fable2_dev_session.py LIVE_STATE.
  static var liveState: String {
    if let env = ProcessInfo.processInfo.environment["FABLE2_LIVE_STATE_ROOT"], !env.isEmpty { return env }
    return NSHomeDirectory() + "/Library/Application Support/Fable II Recomp"
  }

  /// Runs the Python backend; returns (exit status, stdout).
  static func run(_ arguments: [String], script: String = Backend.script) -> (Int32, String) {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
    process.arguments = [script] + arguments
    process.currentDirectoryURL = URL(fileURLWithPath: repoRoot)
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    do { try process.run() } catch { return (-1, "{\"ok\": false, \"error\": \"\(error.localizedDescription)\"}") }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    return (process.terminationStatus, String(decoding: data, as: UTF8.self))
  }

  static func json(_ arguments: [String], script: String = Backend.script) -> [String: Any] {
    let (_, output) = run(arguments, script: script)
    if let data = output.data(using: .utf8),
       let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
      return object
    }
    // stderr shares the pipe: progress lines ([seed], [portable]) may come before the final pretty-printed object,
    // which starts with a "{" line of its own.
    if let start = output.range(of: "\n{\n", options: .backwards),
       let data = String(output[output.index(after: start.lowerBound)...]).data(using: .utf8),
       let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
      return object
    }
    return ["ok": false, "error": output.trimmingCharacters(in: .whitespacesAndNewlines)]
  }

  /// The launch arguments for one pick. Shared by the Launch button and --launch.
  static func launchArguments(pick: String, targetSlot: String?, testControl: Bool,
                              settings s: LaunchSettings) -> [String] {
    // Playtest without a playtest copy launches Latest (BuildChoice.effective), never a failed launch.
    var args = ["launch", "--build", s.build.effective(playtestExists: Backend.playtestExists).rawValue]
    if pick.hasPrefix("live:") {
      args += ["--mode", "live", "--save", String(pick.dropFirst(5))]
    } else if pick.hasPrefix("library:") {
      args += ["--mode", "library", "--save", String(pick.dropFirst(8))]
    } else {
      args += ["--mode", "title"]
    }
    if pick != "title" {
      // Test sessions never write back to a live slot (tools/fable2_dev_session.py refuses it).
      if testControl { args += ["--target-slot", "none"] }
      else if let slot = targetSlot, !slot.isEmpty { args += ["--target-slot", slot] }
      if !s.boot { args.append("--no-boot") }
      if testControl { args.append("--test-control") }
    }
    if let fps = s.frameRate.argument { args += ["--fps", fps] }
    // Auto: the game picks 60 or 30 FPS from the GPU core count (src/core/fable2_fps_auto.h).
    // Vsync on, as the dev session does for 30/60.
    if s.frameRate == .auto { for item in s.frameRateEnvironment { args += ["--env", item] } }
    if !s.skipIntro { args.append("--no-skip-intro") }
    // Every other setting as NAME=VALUE (the shared mapping, LaunchSettings.settingsEnvironment).
    for item in s.settingsEnvironment { args += ["--env", item] }
    return args
  }
}

// MARK: - Model

// SaveItem, PickerMeta (stars, custom titles) and the picker sheet: SavePicker.swift.

final class LauncherModel: SettingsModel {
  @Published var live: [SaveItem] = []
  @Published var library: [SaveItem] = []
  @Published var meta = PickerMeta(path: Backend.repoRoot + "/out/save-library/.picker.json")
  @Published var showPicker = false
  /// The first save list has arrived (the --snapshot of the picker waits for it).
  @Published var loaded = false
  @Published var pick: String {
    didSet {
      defaults.set(pick, forKey: "pick")
      if pick != oldValue { syncTarget(pickChanged: true) }
    }
  }
  @Published var targetSlot: String { didSet { defaults.set(targetSlot, forKey: "targetSlot") } }
  @Published var playtestVersion = Backend.playtestVersion
  @Published var playtestExists = Backend.playtestExists
  @Published var others: [String] = []
  @Published var benchmarking = false
  @Published var running = false
  @Published var busy = false
  @Published var message = ""
  /// Logs folder of a game that closed unexpectedly (crash or kill): the footer shows Show Logs next to the note.
  @Published var crashLogs: String?
  /// Play games (this launcher's, Fable II.app, fable_2 or a run_macos_*.command; never agent test games) seen
  /// running at the last poll. When one ends the launcher comes back to the front with its window.
  private var watchedGames = Set<Int>()
  @Published var lastSave = ""
  @Published var gameVersion = Backend.gameVersion
  /// State folder of the launcher's current (or last) session; nil for title-screen play.
  @Published var sessionState: String?

  let defaults = UserDefaults.standard
  private var timer: Timer?
  static let slots = (0...9).map { String(format: "Hero%03d", $0) }

  /// `store`: the settings store (the Dev Launcher role: UserDefaults; the player role in developer mode: the
  /// player's own settings file, so both windows launch with the same settings).
  init(poll: Bool = true, store: SettingsStore = UserDefaults.standard) {
    pick = UserDefaults.standard.string(forKey: "pick") ?? "title"
    targetSlot = UserDefaults.standard.string(forKey: "targetSlot") ?? "Hero000"
    super.init(store: store)
    converterProbe = { Backend.hasConverter($0) }
    dropMissingPlaytest()
    if DevSnapshot.request != nil, let snapshotPick = argument(after: "--pick") { pick = snapshotPick }
    if poll {
      refresh()
      timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.pollStatus() }
    }
  }

  var selected: SaveItem? { (live + library).first { $0.id == pick } }
  /// The main window's label for the pick: the custom title if any, else the readable automatic one.
  var selectedTitle: String {
    if pick == "title" { return "Title screen (live saves, like Fable II.app)" }
    guard let item = selected else { return pick }
    return meta.title(item)
  }
  var isLive: Bool { pick.hasPrefix("live:") }

  /// Continue from last save (2026-10-15): the newest save by save time among the live slots and the library's
  /// play saves (agent test saves skipped); a tie goes to the live slot, the playthrough itself.
  var continueItem: SaveItem? {
    let candidates = live.filter { !$0.savedAt.isEmpty } + library.filter { !$0.test && !$0.savedAt.isEmpty }
    return candidates.max { a, b in
      a.savedAt != b.savedAt ? a.savedAt < b.savedAt : (a.id.hasPrefix("library:") && b.id.hasPrefix("live:"))
    }
  }

  /// One click: boots straight into continueItem through the save-pick path (a session copy, FABLE2_BOOT_SAVE).
  /// A live slot writes back to itself; a library save to the slot it was written back to, else library only.
  /// The pick and write-back choice of the main window are not changed.
  func continueLatest() {
    guard let item = continueItem else { return }
    let target = item.id.hasPrefix("live:") ? item.folder : (item.writtenBack.isEmpty ? "none" : item.writtenBack)
    launch(pick: item.id, target: target, boot: true)
  }
  var canLaunch: Bool { !running && !busy }
  var canBenchmark: Bool { !busy && !running && others.isEmpty }

  /// A live pick always writes back to its own slot. Picking a library entry resets the
  /// target to that entry's own HeroNNN folder; the user can then change it.
  private func syncTarget(pickChanged: Bool = false) {
    guard let item = selected else { return }
    if isLive || pickChanged { targetSlot = item.folder }
  }

  func refresh() {
    DispatchQueue.global().async {
      let result = Backend.json(["list"])
      let version = Backend.gameVersion
      let playtest = Backend.playtestVersion
      let exists = Backend.playtestExists
      DispatchQueue.main.async {
        self.apply(list: result); self.gameVersion = version; self.playtestVersion = playtest
        self.playtestExists = exists
        self.dropMissingPlaytest()
        self.refreshConverter()
      }
    }
  }

  func apply(list result: [String: Any]) {
    guard result["ok"] as? Bool == true, let data = result["result"] as? [String: Any] else {
      message = "Could not list saves: \(result["error"] ?? "unknown error")"
      return
    }
    live = (data["live"] as? [[String: Any]] ?? []).map(SaveItem.live)
    library = (data["library"] as? [[String: Any]] ?? []).map(SaveItem.library)
    if let path = data["picker_meta"] as? String, !path.isEmpty { meta.path = path }
    meta = PickerMeta.load(meta.path)
    loaded = true
    running = data["running"] as? Bool ?? false
    if pick != "title" && selected == nil { pick = "title" }
    syncTarget()
  }

  /// Stars and renames only change .picker.json (never a save folder).
  func toggleStar(_ item: SaveItem) {
    guard !item.name.isEmpty else { return }
    var next = meta
    if next.starred.contains(item.name) { next.starred.remove(item.name) } else { next.starred.insert(item.name) }
    store(next)
  }

  func setTitle(_ item: SaveItem, _ title: String) {
    guard !item.name.isEmpty else { return }
    var next = meta
    let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
    next.titles[item.name] = trimmed.isEmpty || trimmed == item.title ? nil : trimmed
    store(next)
  }

  private func store(_ next: PickerMeta) {
    if next.save() { meta = next } else { message = "Could not write \(next.path)" }
  }

  func reveal(_ item: SaveItem) {
    let folder = (meta.path as NSString).deletingLastPathComponent + "/" + item.name
    let path = FileManager.default.fileExists(atPath: folder) ? folder
      : Backend.repoRoot + "/out/save-library/" + item.name
    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
  }

  func pollStatus() {
    DispatchQueue.global().async {
      let result = Backend.json(["status"])
      guard let data = result["result"] as? [String: Any] else { return }
      DispatchQueue.main.async {
        let wasRunning = self.running
        self.refreshConverter()  // a rebuild may have switched the app between converter / no-converter
        self.running = data["running"] as? Bool ?? false
        self.others = data["others"] as? [String] ?? []
        let games = Set((data["instances"] as? [[String: Any]] ?? []).compactMap { entry -> Int? in
          guard ["play", "playtest", "untracked"].contains(entry["role"] as? String ?? ""),
                entry["test_control"] as? Bool != true else { return nil }
          return entry["pid"] as? Int
        })
        let ended = !self.watchedGames.subtracting(games).isEmpty
        self.watchedGames = games
        if let session = data["session"] as? [String: Any] {
          self.sessionState = session["state"] as? String
        }
        if let last = data["last_save"] as? [String: Any], let name = last["library"] as? String {
          let slot = (last["written_back_to"] as? String).map { " → live \($0)" } ?? ""
          if self.lastSave != "Last save: \(name)\(slot)" {
            self.lastSave = "Last save: \(name)\(slot)"
            self.refresh()
          }
        }
        if wasRunning && !self.running {
          let exit = data["last_exit"] as? [String: Any]
          if exit?["kind"] as? String == "unclean" {
            self.message = "The game closed unexpectedly (\(exit?["detail"] as? String ?? "no clean exit"))."
            self.crashLogs = exit?["logs"] as? String
          } else {
            self.message = "Game closed."
          }
          self.refresh()
        }
        if (wasRunning && !self.running) || ended { LauncherReturn.bringBack() }
      }
    }
  }

  /// Playtest picked but no playtest copy (fresh checkout, or the copy was removed): the pick becomes Latest, so
  /// the picker never shows a choice that cannot launch. Playtest is never picked automatically.
  func dropMissingPlaytest() {
    playtestExists = Backend.playtestExists
    if settings.build == .playtest && !playtestExists { settings.build = .latest }
  }

  func launch() { launch(pick: pick, target: pick == "title" ? nil : targetSlot, boot: nil) }

  /// `boot`: overrides "Boot straight into save" (Continue always boots).
  func launch(pick: String, target: String?, boot: Bool?) {
    reloadSettings()
    let fellBack = settings.build == .playtest && !Backend.playtestExists
    dropMissingPlaytest()
    var launchSettings = settings
    if let boot = boot { launchSettings.boot = boot }
    let args = Backend.launchArguments(pick: pick, targetSlot: target, testControl: false, settings: launchSettings)
    busy = true
    crashLogs = nil
    message = (fellBack ? Backend.playtestFallbackNote + " " : "")
      + (pick == "title" ? "Launching…" : "Preparing session and launching…")
    lastSave = ""
    DispatchQueue.global().async {
      let result = Backend.json(args)
      DispatchQueue.main.async {
        self.busy = false
        if result["ok"] as? Bool == true {
          let info = result["result"] as? [String: Any]
          let session = info?["session"] as? String
          self.sessionState = session.map { $0 + "/test-state" }
          self.message = (session.map { "Running session \(URL(fileURLWithPath: $0).lastPathComponent)" }
            ?? "Running on the live state folder.")
            + ((fellBack ? Backend.playtestFallbackNote : info?["note"] as? String).map { " " + $0 } ?? "")
          self.running = true
        } else {
          self.message = "Launch failed: \(result["error"] ?? "unknown error")"
        }
      }
    }
  }

  func closeGame() {
    busy = true
    message = "Closing the game…"
    DispatchQueue.global().async {
      let result = Backend.json(["close", "--current"])
      DispatchQueue.main.async {
        self.busy = false
        self.message = result["ok"] as? Bool == true ? "Game closed." : "Close failed: \(result["error"] ?? "")"
        self.pollStatus()
      }
    }
  }

  /// Packages the no-converter release build (out/build-release, refreshed first; docs/no-converter.md) as a
  /// portable app + zip for another Mac (tools/fable2_portable.py build, docs/portable-build.md) and shows the zip
  /// in Finder.
  func exportPortable() {
    busy = true
    message = "Exporting the portable build (out/portable): refreshing the no-converter release build first (a few minutes the first time), then packaging…"
    DispatchQueue.global().async {
      let result = Backend.json(["build"], script: Backend.repoRoot + "/tools/fable2_portable.py")
      DispatchQueue.main.async {
        self.busy = false
        if result["ok"] as? Bool == true, let info = result["result"] as? [String: Any] {
          let zip = info["zip"] as? String ?? ""
          let mb = ((info["zip_bytes"] as? NSNumber)?.doubleValue ?? 0) / 1_000_000
          self.message = String(format: "Portable build exported (no-converter, %.0f MB zip). Copy it to the other Mac; see README.txt.", mb)
          if !zip.isEmpty { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: zip)]) }
        } else {
          self.message = "Export failed: \(result["error"] ?? "unknown error")"
        }
      }
    }
  }

  /// Built-in benchmark on this Mac (tools/fable2_benchmark.py run, docs/benchmark-kit.md): the chosen build,
  /// the chosen save (Bower Lake when the pick is the title screen), its own state folder (out/benchmark/state;
  /// the live saves are only read). Shows progress and opens the report folder at the end.
  func runBenchmark() {
    reloadSettings()
    if running || !others.isEmpty {
      message = "Close the running game first: the benchmark needs the Mac to itself."
      return
    }
    let save: String
    if pick.hasPrefix("library:") { save = String(pick.dropFirst("library:".count)) }
    else if pick.hasPrefix("live:") { save = pick }
    else { save = "" }
    busy = true
    benchmarking = true
    message = "Benchmark starting (7 game launches, about 6 minutes; don't use the Mac)…"
    let script = Backend.repoRoot + "/tools/fable2_benchmark.py"
    let progress = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
      DispatchQueue.global().async {
        let p = Backend.json(["progress"], script: script)["result"] as? [String: Any] ?? [:]
        guard let text = p["text"] as? String, p["done"] == nil else { return }
        DispatchQueue.main.async {
          if self?.benchmarking == true { self?.message = "Benchmark \(p["pct"] ?? 0)%: \(text)" }
        }
      }
    }
    let build = settings.build.effective(playtestExists: Backend.playtestExists)
    DispatchQueue.global().async {
      var args = ["run", "--build", build.rawValue]
      if !save.isEmpty { args += ["--save", save] }
      let result = Backend.json(args, script: script)
      DispatchQueue.main.async {
        progress.invalidate()
        self.busy = false
        self.benchmarking = false
        if result["ok"] as? Bool == true, let info = result["result"] as? [String: Any],
           let folder = info["folder"] as? String {
          self.message = "Benchmark done: \(folder)"
          NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: folder + "/report.txt")])
        } else {
          self.message = "Benchmark failed: \(result["error"] ?? "unknown error")"
        }
        self.pollStatus()
      }
    }
  }

  /// Flash kit (docs/flash-kit.md): opens the packaged "Fable II Flash Kit.app" (tools/fable2_flash_kit.py build ->
  /// out/flash-kit), which runs its arms by itself and puts the report on the Desktop.
  func openFlashKit() {
    if running || !others.isEmpty {
      message = "Close the running game first: the flash kit launches the game itself."
      return
    }
    let app = Backend.repoRoot + "/out/flash-kit/Fable II Flash Kit/Fable II Flash Kit.app"
    if FileManager.default.fileExists(atPath: app) {
      NSWorkspace.shared.open(URL(fileURLWithPath: app))
      message = "Flash kit opened (about 5-10 minutes; report in Desktop/Fable II Flash Kit)."
    } else {
      message = "No flash kit yet: build it with python3 tools/fable2_flash_kit.py build (out/flash-kit)."
    }
  }

  /// Freezes the latest build as the playtest copy (tools/fable2_playtest.py update).
  func updatePlaytest() {
    busy = true
    message = "Updating the playtest build from the latest build…"
    DispatchQueue.global().async {
      let result = Backend.json(["update"], script: Backend.repoRoot + "/tools/fable2_playtest.py")
      let version = Backend.playtestVersion
      let exists = Backend.playtestExists
      DispatchQueue.main.async {
        self.busy = false
        self.playtestVersion = version
        self.playtestExists = exists
        if result["ok"] as? Bool == true {
          self.message = "Playtest build updated to \(version ?? "?")."
        } else {
          self.message = "Update failed: \(result["error"] ?? "unknown error")"
        }
      }
    }
  }

  /// Opens a folder in Finder (created first only inside the repo's out/ folder).
  func open(folder path: String) {
    if path.hasPrefix(Backend.repoRoot + "/out/") {
      try? FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: true)
    }
    guard FileManager.default.fileExists(atPath: path) else {
      message = "Folder not found: \(path)"
      return
    }
    NSWorkspace.shared.open(URL(fileURLWithPath: path, isDirectory: true))
  }

  /// The logs of the launcher's session when it has one, else the live state's logs (title-screen play).
  var logsFolder: String {
    if let state = sessionState, FileManager.default.fileExists(atPath: state + "/logs") { return state + "/logs" }
    return Backend.liveState + "/logs"
  }
}

// MARK: - Main window

struct LauncherView: View {
  @ObservedObject var model: LauncherModel

  var body: some View {
    VStack(spacing: 0) {
      Form {
        Section {
          Picker("Build", selection: $model.settings.build) {
            Text("Latest build (\(model.gameVersion))").tag(BuildChoice.latest)
            if model.playtestExists {
              Text("Playtest (frozen, \(model.playtestVersion ?? "unknown version"))").tag(BuildChoice.playtest)
            } else {
              Text("Playtest (no copy)").tag(BuildChoice.playtest).disabled(true)
            }
          }
          .help("Latest: out/build, replaced by every build (the default). "
                + "Playtest: the frozen copy in out/playtest that agents never rebuild.")
          if !model.playtestExists {
            Text("No playtest copy; Update Playtest Build creates one.")
              .font(.caption).foregroundStyle(.secondary)
          }
          LabeledContent("Save") {
            Button("Choose Save…") { model.showPicker = true }
              .keyboardShortcut("o")
              .help("A save launch runs in its own session copy; saves made in game go to the library "
                    + "(and to the live slot chosen below). ⌘O")
          }
          VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
              if let item = model.selected, model.meta.starred.contains(item.name) {
                Image(systemName: "star.fill").foregroundStyle(Color.yellow)
              }
              Text(model.selectedTitle).fontWeight(.medium).lineLimit(2)
            }
            if let item = model.selected, !item.detail.isEmpty {
              let custom = model.meta.titles[item.name] != nil
              Text(custom ? item.title + " · " + item.detail : item.detail)
                .font(.caption).foregroundStyle(.secondary).lineLimit(2)
            }
          }
        } footer: {
          footerText
        }
        if model.pick != "title" {
          Section {
            Toggle("Boot straight into save", isOn: model.binding(\.boot))
              .help("Load the save from the main menu automatically (off: stop at the main menu).")
            if !model.isLive {
              Picker("Write saves back to", selection: $model.targetSlot) {
                Text("Library only (no live slot)").tag("none")
                ForEach(LauncherModel.slots, id: \.self) { Text("Live slot \($0)").tag($0) }
              }
              .help("Saves made in game are always added to the save library; this also copies them "
                    + "to a live slot (the old copy is backed up first).")
            }
          }
        }
      }
      .formStyle(.grouped)
      .scrollDisabled(true)
      .fixedSize(horizontal: false, vertical: true)

      VStack(alignment: .leading, spacing: 10) {
        Toggle("Skip intro movies", isOn: model.binding(\.skipIntro))
          .help("Off (default): the original logo movies and title intro play. On: straight to the title screen, "
                + "with a shorter title fade (fast title). Same setting as Settings > General.")
        if let item = model.continueItem {
          // Continue from last save (2026-10-15): one click, the default button when a save exists.
          VStack(alignment: .leading, spacing: 3) {
            Button { model.continueLatest() } label: {
              Text(model.busy ? "Working…" : "Continue from Last Save").font(.title3.weight(.semibold))
                .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .keyboardShortcut(.defaultAction)
            .disabled(!model.canLaunch)
            .help("Boots straight into the newest save (live slots and library). ↩")
            Text(model.meta.title(item)).font(.caption).fontWeight(.medium).lineLimit(1)
            if !item.detail.isEmpty {
              Text(item.detail).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
          }
        }
        HStack(spacing: 10) {
          if model.continueItem != nil {
            Button { model.launch() } label: {
              Text(model.busy ? "Working…" : "Launch").font(.title3.weight(.semibold))
                .frame(maxWidth: .infinity)
            }
            .controlSize(.large)
            .disabled(!model.canLaunch)
            .help("Launch the pick above (the title screen or the chosen save).")
          } else {
            Button { model.launch() } label: {
              Text(model.busy ? "Working…" : "Launch").font(.title3.weight(.semibold))
                .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .keyboardShortcut(.defaultAction)
            .disabled(!model.canLaunch)
          }
          if model.running {
            Button("Close Game") { model.closeGame() }
              .controlSize(.large)
              .disabled(model.busy)
              .help("Quit the game this launcher started (never another instance).")
          }
        }
        HStack {
          Button("Update Playtest Build") { model.updatePlaytest() }
            .disabled(model.busy || (model.running && model.settings.build == .playtest))
            .help("Copy the latest build to the frozen playtest copy (out/playtest).")
          Spacer()
          SettingsLink { Label("Settings…", systemImage: "gearshape") }
            .help("Display, performance, fixes and diagnostics (⌘,)")
        }
        Divider()
        statusLines
      }
      .padding(.horizontal, 20)
      .padding(.bottom, 16)
    }
    .frame(width: 520)
    .sheet(isPresented: $model.showPicker) { SavePickerView(model: model) }
  }

  private var footerText: some View {
    Group {
      if model.pick == "title" {
        Text("Uses the live save folder exactly like Fable II.app; nothing is copied.")
      } else if model.isLive {
        Text("Saves made in game are copied back to live slot \(model.targetSlot) (old copy backed up) "
             + "and added to the library.")
      } else {
        Text("Saves made in game are added to the save library.")
      }
    }
    .font(.caption).foregroundStyle(.secondary)
  }

  private var statusLines: some View {
    VStack(alignment: .leading, spacing: 4) {
      HStack(spacing: 6) {
        Circle().fill(model.running ? Color.green : Color.secondary.opacity(0.5)).frame(width: 8, height: 8)
        Text(model.running ? "Game running" : "Game not running").font(.callout)
        Spacer()
        Text("Fable II Recomp \(model.gameVersion)").font(.caption).foregroundStyle(.secondary)
          .textSelection(.enabled)
      }
      if !model.others.isEmpty {
        Text("Also running (not this launcher's game): \(model.others.joined(separator: "; "))")
          .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      }
      if !model.lastSave.isEmpty { Text(model.lastSave).font(.caption) }
      if !model.message.isEmpty {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
          Text(model.message).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
            .fixedSize(horizontal: false, vertical: true)
          if let logs = model.crashLogs {
            Button("Show Logs") { NSWorkspace.shared.open(URL(fileURLWithPath: logs, isDirectory: true)) }
              .controlSize(.small)
          }
        }
      }
    }
  }
}

// MARK: - Settings window

struct DevSettingsView: View {
  @ObservedObject var model: LauncherModel
  @AppStorage("settingsTab") private var tab = SettingsTab.display.rawValue

  var body: some View {
    TabView(selection: $tab) {
      ForEach(SettingsTab.allCases) { item in
        SettingsTabView(model: model, tab: item, extra: { tab in
          tab == .diagnostics ? AnyView(DeveloperSection(developerMode: .constant(true), locked: true)) : AnyView(EmptyView())
        })
          .tabItem { Label(item.title, systemImage: item.symbol) }
          .tag(item.rawValue)
      }
    }
    .frame(width: 580)
    .onAppear { if let snapshotTab = DevSnapshot.settingsTab { tab = snapshotTab.rawValue } }
  }
}

// MARK: - App

/// The launcher is assumed in use (2026-10-15): when a play game ends (this launcher's, or one started another way
/// while the launcher runs) the app comes to the front with its launcher window, reopening it if it was closed.
/// Never during a --snapshot or in background test mode (FABLE2_TEST_BACKGROUND=1).
enum LauncherReturn {
  /// Set by a scene that can open the launcher window (the player role's Window menu commands).
  static var reopen: (() -> Void)?
  static var titles: Set<String> { ["Fable II Launcher", "Fable II"] }
  static func bringBack() {
    guard DevSnapshot.request == nil, ProcessInfo.processInfo.environment["FABLE2_TEST_BACKGROUND"] != "1" else { return }
    if NSApp.activationPolicy() != .regular { return }  // the player role's own Play hides it; gameEnded restores
    if NSApp.isActive, let key = NSApp.keyWindow, titles.contains(key.title) { return }  // already back
    let windows = NSApp.windows.filter { titles.contains($0.title) && ($0.isVisible || $0.isMiniaturized) }
    let launcher = windows.first { $0.title == "Fable II Launcher" } ?? windows.first
    if let window = launcher {
      if window.isMiniaturized { window.deminiaturize(nil) }
      window.makeKeyAndOrderFront(nil)
    } else {
      reopen?()
    }
    NSApp.activate(ignoringOtherApps: true)
  }
}

final class DevAppDelegate: NSObject, NSApplicationDelegate {
  func applicationWillFinishLaunching(_ notification: Notification) {
    if DevSnapshot.request != nil { NSApp.setActivationPolicy(.accessory) }  // never take focus for a snapshot
  }
  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

/// One Dev Launcher tool (Tools menu). The Dev Launcher role lists them in its Tools menu; the player role in developer
/// mode lists the same items in Window > Developer (tests/unified_launcher_tests.py checks both list every item).
struct DevTool: Identifiable {
  let id: String
  let title: String
  let group: Int
  var shortcut: KeyboardShortcut? = nil
  /// Needs the Dev Launcher window (the player role opens it first).
  var needsWindow = false
  let enabled: (LauncherModel) -> Bool
  let action: (LauncherModel) -> Void
}

enum DevTools {
  static let items: [DevTool] = [
    DevTool(id: "benchmark", title: "Run Benchmark…", group: 0, enabled: { $0.canBenchmark }, action: { $0.runBenchmark() }),
    DevTool(id: "flash-kit", title: "Run Flash Kit…", group: 0, enabled: { !$0.busy }, action: { $0.openFlashKit() }),
    DevTool(id: "export-portable", title: "Export Portable Build…", group: 0, enabled: { !$0.busy },
            action: { $0.exportPortable() }),
    DevTool(id: "update-playtest", title: "Update Playtest Build", group: 0,
            enabled: { !($0.busy || ($0.running && $0.settings.build == .playtest)) }, action: { $0.updatePlaytest() }),
    DevTool(id: "close-game", title: "Close Game", group: 1, shortcut: KeyboardShortcut("w", modifiers: [.command, .shift]),
            enabled: { $0.running && !$0.busy }, action: { $0.closeGame() }),
    DevTool(id: "choose-save", title: "Choose Save…", group: 1, needsWindow: true, enabled: { !$0.showPicker },
            action: { $0.showPicker = true }),
    DevTool(id: "refresh", title: "Refresh Saves and Status", group: 1, shortcut: KeyboardShortcut("r"),
            enabled: { _ in true }, action: { $0.refresh(); $0.pollStatus() }),
    DevTool(id: "open-logs", title: "Open Logs Folder", group: 2, enabled: { _ in true },
            action: { $0.open(folder: $0.logsFolder) }),
    DevTool(id: "open-save-library", title: "Open Save Library", group: 2, enabled: { _ in true },
            action: { $0.open(folder: Backend.repoRoot + "/out/save-library") }),
    DevTool(id: "open-sessions", title: "Open Sessions Folder", group: 2, enabled: { _ in true },
            action: { $0.open(folder: Backend.repoRoot + "/out/dev-sessions") }),
    DevTool(id: "open-benchmark-reports", title: "Open Benchmark Reports", group: 2, enabled: { _ in true },
            action: { $0.open(folder: Backend.repoRoot + "/out/benchmark/reports") }),
  ]
}

/// The tool items as menu content (both roles). `openWindow`: the player role opens the Dev Launcher window first for
/// tools that need it.
struct DevToolItems: View {
  @ObservedObject var model: LauncherModel
  var openWindow: (() -> Void)? = nil
  var body: some View {
    ForEach(0..<3, id: \.self) { group in
      if group > 0 { Divider() }
      ForEach(DevTools.items.filter { $0.group == group }) { item in
        Button(item.title) {
          if item.needsWindow { openWindow?() }
          item.action(model)
        }
        .keyboardShortcut(item.shortcut)
        .disabled(!item.enabled(model))
      }
    }
  }
}

/// The Dev Launcher model in the player role (developer mode): created when first needed and a checkout is known.
final class DevHost: ObservableObject {
  static let shared = DevHost()
  @Published private(set) var model: LauncherModel?
  var store: SettingsStore = UserDefaults.standard

  /// The model, created on first use; nil without a source checkout.
  @discardableResult
  func ensure() -> LauncherModel? {
    if model == nil, DevCheckout.path != nil { model = LauncherModel(store: store) }
    return model
  }
}

/// Window > Fable II Dev Launcher in the player role: the Dev Launcher's main window, or the checkout note.
struct DevLauncherWindowHost: View {
  @ObservedObject var host = DevHost.shared
  @State private var error = ""
  var body: some View {
    if let model = host.model {
      LauncherView(model: model)
    } else {
      VStack(alignment: .leading) {
        NeedsCheckoutView { error = DevCheckout.choose() ?? ""; host.ensure() }
        if !error.isEmpty { Text(error).font(.caption).foregroundStyle(.red).padding(.horizontal, 20) }
      }
      .onAppear { host.ensure() }
    }
  }
}

struct ToolsCommands: Commands {
  @ObservedObject var model: LauncherModel
  @Environment(\.openWindow) private var openWindow
  var body: some Commands {
    let _ = LauncherReturn.reopen = { [openWindow] in openWindow(id: "main") }
    CommandMenu("Tools") { DevToolItems(model: model) }
  }
}

/// The main window plus the --snapshot hook (it needs the openSettings action of a window's environment).
struct DevMainWindow: View {
  @ObservedObject var model: LauncherModel
  @Environment(\.openSettings) private var openSettings
  var body: some View {
    LauncherView(model: model).onAppear {
      if DevSnapshot.request?.tab == "picker" { model.showPicker = true }
      DevSnapshot.start(ready: { model.loaded }) { openSettings() }
      // Testing: --press-continue / --press-launch press that front-page button once the save list is in (as the
      // player's --press-play), so the whole button path, game end and return to the launcher run unattended.
      for (flag, press) in [("--press-continue", { model.continueLatest() }), ("--press-launch", { model.launch() })]
      where commandLine.contains(flag) {
        func attempt() { if model.loaded { press() } else { DispatchQueue.main.asyncAfter(deadline: .now() + 0.2, execute: attempt) } }
        attempt()
      }
    }
  }
}

struct DevLauncherApp: App {
  @NSApplicationDelegateAdaptor(DevAppDelegate.self) var delegate
  @StateObject private var model = LauncherModel()
  var body: some Scene {
    Window("Fable II Launcher", id: "main") { DevMainWindow(model: model) }
      .windowResizability(.contentSize)
      .commands { ToolsCommands(model: model) }
    Settings { DevSettingsView(model: model) }
  }
}

// MARK: - Entry point

func cliLaunch(_ argv: [String]) -> Int32 {
  let defaults = UserDefaults.standard
  var pick = defaults.string(forKey: "pick") ?? "title"
  var target: String? = defaults.string(forKey: "targetSlot")
  var s = LaunchSettings.load(from: defaults)
  s.converterBuiltIn = Backend.hasConverter(s.build)
  var testControl = false
  var explicitTarget = false
  var printOnly = false
  func fail(_ text: String) -> Int32 { print("{\"ok\": false, \"error\": \"\(text)\"}"); return 2 }
  var i = 0
  while i < argv.count {
    let arg = argv[i]
    let value: String? = i + 1 < argv.count ? argv[i + 1] : nil
    switch arg {
    case "--pick" where value != nil: pick = value!; i += 1
    case "--target-slot" where value != nil: target = value; explicitTarget = true; i += 1
    case "--test-control": testControl = true
    case "--print-args": printOnly = true
    case "--build":
      guard let v = value.flatMap(BuildChoice.init(rawValue:)) else { return fail("--build expects playtest or latest") }
      s.build = v; s.converterBuiltIn = Backend.hasConverter(v); i += 1
    case "--turbo-notice":
      guard let v = value.flatMap(TurboNotice.init(rawValue:)) else { return fail("--turbo-notice expects overlay or game") }
      s.turboNotice = v; i += 1
    case "--turbo-speed":
      guard let v = value.flatMap(Double.init), TurboSpeed.range.contains(v) else {
        return fail("--turbo-speed expects 1.1 to 4.0")
      }
      s.turboSpeed = TurboSpeed.clean(v); i += 1
    case "--turbo-audio":
      guard let v = value.flatMap(TurboAudio.init(rawValue:)) else {
        return fail("--turbo-audio expects natural, high-pitch or normal")
      }
      s.turboAudio = v; i += 1
    case "--trace-preset":
      guard let v = value.flatMap(TracePreset.init(rawValue:)) else {
        return fail("--trace-preset expects standard, off, loading, gpu or everything")
      }
      s.tracePreset = v; i += 1
    case "--extra-env" where value != nil: s.extraEnv = value!; i += 1
    case "--resolution":
      guard let v = value.flatMap(InternalResolution.init(argument:)) else { return fail("--resolution expects auto, native, 2 or 3") }
      s.resolution = v; i += 1
    case "--upscaling", "--upscaler":
      guard let v = value.flatMap(Upscaling.init(argument:)) else { return fail("--upscaling expects off or metalfx-spatial") }
      s.upscaling = v; i += 1
    case "--effects":
      guard let v = value.flatMap(EffectsSize.init(rawValue:)) else { return fail("--effects expects original or full") }
      s.effects = v; i += 1
    case "--display":
      guard let v = value.flatMap(DisplayMode.init(rawValue:)) else { return fail("--display expects windowed or fullscreen") }
      s.display = v; i += 1
    case "--fps":
      guard let v = value.flatMap(FrameRate.init(rawValue:)) else {
        return fail("--fps expects settings, auto, 30, 60 or uncapped")
      }
      s.frameRate = v; i += 1
    default:
      if s.applyFlag(arg) { break }
      // Controls tab options (ControlsView.swift: --keyboard-map, --mouse-look-scale, --bind, ...).
      switch s.controls.applyCLI(arg, value) {
      case .notMine: break
      case .ok(let consumed): i += consumed
      case .error(let text): return fail(text)
      }
    }
    i += 1
  }
  // A live pick writes back to its own slot unless told otherwise (the backend default).
  if pick.hasPrefix("live:") && !explicitTarget { target = nil }
  if pick == "title" { target = nil }
  let args = Backend.launchArguments(pick: pick, targetSlot: target, testControl: testControl, settings: s)
  if s.build == .playtest && !Backend.playtestExists {  // launched as Latest (BuildChoice.effective)
    FileHandle.standardError.write(Data((Backend.playtestFallbackNote + "\n").utf8))
  }
  if printOnly {
    let data = (try? JSONSerialization.data(withJSONObject: args, options: [])) ?? Data()
    print(String(decoding: data, as: UTF8.self))
    return 0
  }
  let (status, output) = Backend.run(args)
  print(output, terminator: "")
  return status
}

/// --snapshot PATH [--tab main|general|...]: the app starts as usual (without taking focus), shows the
/// main window or opens Settings on that tab, then saves that one window of its own (with its shadow-free
/// frame) to a PNG and quits. Settings are only read (a --pick becomes the saved pick, as before).
enum DevSnapshot {
  static let request: (path: String, tab: String)? = argument(after: "--snapshot").map {
    ($0, argument(after: "--tab") ?? "main")
  }
  static var settingsTab: SettingsTab? { request.flatMap { SettingsTab(rawValue: $0.tab) } }

  /// `ready`: the save list has loaded (the capture waits for it, 20 s guard).
  @MainActor static func start(ready: @escaping () -> Bool = { true }, openSettings: @escaping () -> Void) {
    guard let request = request else { return }
    if settingsTab != nil { openSettings() }
    let deadline = Date().addingTimeInterval(20)
    func capture() {
      guard ready() || Date() > deadline else {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { capture() }
        return
      }
      // Rendering settle time after the data is in (window/sheet animation and layout).
      DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { write(request) }
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { capture() }
  }

  @MainActor private static func write(_ request: (path: String, tab: String)) {
    let main = NSApp.windows.first { $0.identifier?.rawValue.hasPrefix("main") == true || $0.title == "Fable II Launcher" }
    let window = request.tab == "picker" ? main?.attachedSheet
      : settingsTab == nil ? main
      : NSApp.windows.first { $0.isVisible && $0 !== main && $0.title == settingsTab!.title }
    guard let window = window else { print("snapshot: window not found"); exit(1) }
    guard WindowSnapshot.write(window, to: request.path) else {
      print("snapshot: could not capture the window"); exit(1)
    }
    print(request.path)
    exit(0)
  }
}

/// The Dev Launcher role of the one Fable II app (docs/player-app.md "Developer mode"): the app runs it when its
/// Info.plist has Fable2DevLauncher (tools/macos/build_dev_launcher.sh). The player role shows the same window from
/// Window > Fable II Dev Launcher in developer mode.
/// What a role of the app can reach (tests/unified_launcher_tests.py: everything the Dev Launcher role has must be
/// reachable in the player role with developer mode and a checkout). Derived from the same data the menus and
/// windows are built from.
enum Capabilities {
  static func devLauncher() -> [String] {
    ["window:dev-launcher", "cli:launch", "cli:print-args", "cli:picker-rows", "settings:developer"]
      + SettingsTab.allCases.map { "settings:" + $0.rawValue }
      + DevTools.items.map { "tool:" + $0.id }
      + LaunchSettings.flags.map { "flag:" + $0.key }
  }
}

enum DevLauncherMain {
  /// The command-line modes (both roles): --launch and --picker-rows; nil when neither is asked for.
  static func cli() -> Int32? {
    if commandLine.contains("--launch") { return cliLaunch(commandLine) }
    if commandLine.contains("--picker-rows") { return cliPickerRows(commandLine) }
    return nil
  }
  static func run() -> Never {
    DevLauncherApp.main()
    exit(0)
  }
}
