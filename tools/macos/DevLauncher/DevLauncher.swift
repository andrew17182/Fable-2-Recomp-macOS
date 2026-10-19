// Fable II Dev Launcher: pick a save, boot straight into it, and have the game's own
// saves copied back (live slot + datestamped library entry).
//
// All session/save logic lives in tools/fable2_dev_session.py (shared with the test
// tools); this app is a small SwiftUI front end for it. Build/install with
// tools/macos/build_dev_launcher.sh.
//
// Layout (2026-10-19): one window with two pages. The front page: save menu (newest first, then Title Screen) with
// Load Save, Resolution and Frame rate, Skip intro movies, then the rest (build choice with Dev Options, status).
// Settings is the second page of the same window (the Settings / Launcher button, ⌘,; LauncherPages.swift). Dev
// Options (off by default, remembered) shows the Performance, Fixes and Diagnostics tabs, the build choice, the
// Tools menu's dev tools (Save Library…, benchmark, kits, exports) and the option for Update Playtest Build.
// UserDefaults keys are unchanged, so saved choices carry over.
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
//   --snapshot PATH.png [--tab main|picker|general|display|controls|mods|performance|fixes|diagnostics]
//       [--pick ...] [--dev-options on|off]
//       shows the front page, the save library sheet (picker) or a Settings tab (without taking focus), saves the
//       window to a PNG, quits. Nothing is stored (--dev-options applies to that picture only).
//   --press-load / --press-continue (testing): select the newest save and press Load Save once the list is in;
//   --press-launch: press Load Save with the remembered pick. FABLE2_LAUNCHER_TEST_CONTROL=1 makes the button launch
//   a test session (--test-control: input-isolated, no write-back), so agents can drive the button path.
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
  static let playtestFallbackNote = "No playtest copy: used the latest build."

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

  /// One line in out/dev-sessions/launcher.log (launches and game ends of this launcher).
  static func log(_ line: String) {
    let path = repoRoot + "/out/dev-sessions/launcher.log"
    let stamp = ISO8601DateFormatter().string(from: Date())
    try? FileManager.default.createDirectory(atPath: repoRoot + "/out/dev-sessions", withIntermediateDirectories: true)
    let data = Data("\(stamp) pid \(getpid()) \(line)\n".utf8)
    if let handle = FileHandle(forWritingAtPath: path) {
      handle.seekToEndOfFile(); handle.write(data); try? handle.close()
    } else {
      FileManager.default.createFile(atPath: path, contents: data)
    }
  }

  /// FABLE2_LAUNCHER_TEST_CONTROL=1: the window's launches are test sessions (agents drive the button path).
  static var testControlLaunches: Bool { ProcessInfo.processInfo.environment["FABLE2_LAUNCHER_TEST_CONTROL"] == "1" }

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
      if persist { defaults.set(pick, forKey: "pick") }
      if pick != oldValue { syncTarget(pickChanged: true) }
    }
  }
  @Published var targetSlot: String { didSet { if persist { defaults.set(targetSlot, forKey: "targetSlot") } } }
  /// The game this window launched as a test session (FABLE2_LAUNCHER_TEST_CONTROL): watched by pid, since test
  /// sessions are not the backend's "current" game.
  private var ownTestPid: Int?
  /// The save menu starts on the newest save once per app start.
  private var autoPicked = false
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
    if DevSnapshot.request != nil, let snapshotPick = argument(after: "--pick") { pick = snapshotPick; autoPicked = true }
    if poll {
      refresh()
      timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.pollStatus() }
    }
  }

  var selected: SaveItem? { (live + library).first { $0.id == pick } }
  var isLive: Bool { pick.hasPrefix("live:") }

  /// The save menu (2026-10-19): live slots and starred library saves, plus the newest library play save when it is
  /// newer than every live slot (Continue's pick) and the current pick (e.g. from the save library), newest first.
  var saveItems: [SaveItem] {
    var items = live + library.filter { meta.starred.contains($0.name) }
    for extra in [continueItem, selected].compactMap({ $0 }) where !items.contains(where: { $0.id == extra.id }) {
      items.append(extra)
    }
    return items.sorted { a, b in
      a.savedAt != b.savedAt ? a.savedAt > b.savedAt : (a.id.hasPrefix("live:") && !b.id.hasPrefix("live:"))
    }
  }
  var saveChoices: [SaveChoice] {
    saveItems.map { item in
      let custom = meta.titles[item.name]
      let label = custom ?? item.label
      return SaveChoice(id: item.id, label: (meta.starred.contains(item.name) ? "★ " : "") + label)
    } + [SaveChoice.title]
  }

  /// Continue from last save (2026-10-15): the newest save by save time among the live slots and the library's
  /// play saves (agent test saves skipped); a tie goes to the live slot, the playthrough itself.
  var continueItem: SaveItem? {
    let candidates = live.filter { !$0.savedAt.isEmpty } + library.filter { !$0.test && !$0.savedAt.isEmpty }
    return candidates.max { a, b in
      a.savedAt != b.savedAt ? a.savedAt < b.savedAt : (a.id.hasPrefix("library:") && b.id.hasPrefix("live:"))
    }
  }

  /// --press-continue / --press-load: the newest save in the menu, then Load Save.
  func continueLatest() {
    pick = saveItems.first?.id ?? "title"
    loadSave()
  }

  /// Load Save (the front page's default button): boots straight into the pick through the save-pick path (a
  /// session copy, FABLE2_BOOT_SAVE), or starts at the title screen. A live slot writes back to itself; a library save
  /// to the slot it was written back to, else library only (with Dev Options: the "Write saves back to" choice).
  func loadSave() {
    guard pick != "title", let item = selected else {
      launch(pick: "title", target: nil, boot: nil)
      return
    }
    let target = item.id.hasPrefix("live:") ? item.folder
      : developerMode ? targetSlot : (item.writtenBack.isEmpty ? "none" : item.writtenBack)
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
    running = (data["running"] as? Bool ?? false) || ownTestPid != nil
    if !autoPicked {
      autoPicked = true
      pick = saveItems.first?.id ?? "title"
    }
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
        let pids = Set(data["pids"] as? [Int] ?? [])
        var ownEnded = false
        if let own = self.ownTestPid, !pids.contains(own) {
          Backend.log("game ended: pid \(own) (test session)")
          self.ownTestPid = nil
          ownEnded = true
        }
        self.running = (data["running"] as? Bool ?? false) || self.ownTestPid != nil
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
          let exit = ownEnded ? nil : data["last_exit"] as? [String: Any]
          if !ownEnded { Backend.log("game ended: \(exit?["kind"] as? String ?? "closed")") }
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
    let testControl = Backend.testControlLaunches && pick != "title"
    let args = Backend.launchArguments(pick: pick, targetSlot: testControl ? nil : target, testControl: testControl,
                                       settings: launchSettings)
    busy = true
    crashLogs = nil
    message = (fellBack ? Backend.playtestFallbackNote + " " : "") + "Launching…"
    Backend.log("launch: " + args.joined(separator: " "))
    lastSave = ""
    DispatchQueue.global().async {
      let result = Backend.json(args)
      DispatchQueue.main.async {
        self.busy = false
        if result["ok"] as? Bool == true {
          let info = result["result"] as? [String: Any]
          let session = info?["session"] as? String
          if testControl { self.ownTestPid = info?["pid"] as? Int }
          Backend.log("launched: pid \(info?["pid"] as? Int ?? 0) session \(session ?? "none")")
          self.sessionState = session.map { $0 + "/test-state" }
          self.message = (session.map { "Running session \(URL(fileURLWithPath: $0).lastPathComponent)" }
            ?? "Running.")
            + ((fellBack ? Backend.playtestFallbackNote : info?["note"] as? String).map { " " + $0 } ?? "")
          self.running = true
        } else {
          self.message = "Launch failed: \(result["error"] ?? "unknown error")"
          Backend.log("launch failed: \(result["error"] ?? "unknown error")")
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
    message = "Exporting portable build…"
    DispatchQueue.global().async {
      let result = Backend.json(["build"], script: Backend.repoRoot + "/tools/fable2_portable.py")
      DispatchQueue.main.async {
        self.busy = false
        if result["ok"] as? Bool == true, let info = result["result"] as? [String: Any] {
          let zip = info["zip"] as? String ?? ""
          let mb = ((info["zip_bytes"] as? NSNumber)?.doubleValue ?? 0) / 1_000_000
          self.message = String(format: "Portable build exported (%.0f MB zip).", mb)
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
      message = "Close the game first."
      return
    }
    let save: String
    if pick.hasPrefix("library:") { save = String(pick.dropFirst("library:".count)) }
    else if pick.hasPrefix("live:") { save = pick }
    else { save = "" }
    busy = true
    benchmarking = true
    message = "Benchmark running (about 6 min)…"
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
      message = "Close the game first."
      return
    }
    let app = Backend.repoRoot + "/out/flash-kit/Fable II Flash Kit/Fable II Flash Kit.app"
    if FileManager.default.fileExists(atPath: app) {
      NSWorkspace.shared.open(URL(fileURLWithPath: app))
      message = "Flash kit opened."
    } else {
      message = "No flash kit built (tools/fable2_flash_kit.py build)."
    }
  }

  /// Freezes the latest build as the playtest copy (tools/fable2_playtest.py update).
  func updatePlaytest() {
    busy = true
    message = "Updating playtest build…"
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

/// The front page (2026-10-19): save menu + Load Save, Resolution and Frame rate, Skip intro, then the rest.
struct LauncherView: View {
  @ObservedObject var model: LauncherModel

  var body: some View {
    VStack(spacing: 0) {
      Form {
        Section {
          SaveMenu(choices: model.saveChoices, selection: $model.pick, busy: model.busy, disabled: !model.canLaunch,
                   load: { model.loadSave() })
          if model.developerMode && model.pick.hasPrefix("library:") {
            Picker("Write saves back to", selection: $model.targetSlot) {
              Text("Library only").tag("none")
              ForEach(LauncherModel.slots, id: \.self) { Text("Live slot \($0)").tag($0) }
            }
            .help("Saves made in game always go to the save library. A live slot is backed up first.")
          }
        }
        FrontPageOptions(model: model)
        if model.developerMode {
          Section {
            Picker("Build", selection: $model.settings.build) {
              Text("Latest (\(model.gameVersion))").tag(BuildChoice.latest)
              if model.playtestExists {
                Text("Playtest (\(model.playtestVersion ?? "unknown version"))").tag(BuildChoice.playtest)
              } else {
                Text("Playtest (none)").tag(BuildChoice.playtest).disabled(true)
              }
            }
            .help("Latest: out/build. Playtest: the frozen copy in out/playtest.")
          }
        }
      }
      .formStyle(.grouped)
      .scrollDisabled(true)
      .fixedSize(horizontal: false, vertical: true)

      VStack(alignment: .leading, spacing: 10) {
        HStack(spacing: 10) {
          PageButton(model: model)
          Spacer()
          if model.developerMode && model.showUpdatePlaytest {
            Button("Update Playtest Build") { model.updatePlaytest() }
              .disabled(model.busy || (model.running && model.settings.build == .playtest))
              .help("Copy the latest build to out/playtest.")
          }
          if model.running {
            Button("Close Game") { model.closeGame() }
              .disabled(model.busy)
              .help("Quit the game this launcher started.")
          }
        }
        Divider()
        statusLines
      }
      .padding(.horizontal, 20)
      .padding(.bottom, 16)
    }
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
        Text("Also running: \(model.others.joined(separator: "; "))")
          .font(.caption).foregroundStyle(.secondary).lineLimit(2)
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

/// The launcher window's content: the front page or the Settings page (both roles' Fable II Launcher window).
struct LauncherWindowContent: View {
  @ObservedObject var model: LauncherModel
  static let width: CGFloat = 580

  var body: some View {
    Group {
      if model.page == .settings {
        SettingsPage(model: model, extra: { tab in
          tab == .diagnostics ? AnyView(DeveloperSection(model: model)) : AnyView(EmptyView())
        })
      } else {
        LauncherView(model: model)
      }
    }
    .frame(width: Self.width)
    .sheet(isPresented: $model.showPicker) { SavePickerView(model: model) }
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
    // Never take focus for a snapshot or in background test mode.
    if DevSnapshot.request != nil || ProcessInfo.processInfo.environment["FABLE2_TEST_BACKGROUND"] == "1" {
      NSApp.setActivationPolicy(.accessory)
    }
  }
  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

/// One Dev Launcher tool (Tools menu). The Dev Launcher role lists them in its Tools menu; the player role with Dev
/// Options lists the same items in Window > Developer (tests/unified_launcher_tests.py checks both list every item).
struct DevTool: Identifiable {
  let id: String
  let title: String
  let group: Int
  var shortcut: KeyboardShortcut? = nil
  /// Needs the Dev Launcher window (the player role opens it first).
  var needsWindow = false
  /// Listed only with Dev Options on (the Dev Launcher role's Tools menu keeps the others).
  var devOnly = true
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
            devOnly: false, enabled: { $0.running && !$0.busy }, action: { $0.closeGame() }),
    // The save library (stars, renames, search, test saves); starred saves join the front page's save menu.
    DevTool(id: "choose-save", title: "Save Library…", group: 1, shortcut: KeyboardShortcut("o"), needsWindow: true,
            enabled: { !$0.showPicker }, action: { $0.page = .launcher; $0.showPicker = true }),
    DevTool(id: "refresh", title: "Refresh Saves and Status", group: 1, shortcut: KeyboardShortcut("r"),
            devOnly: false, enabled: { _ in true }, action: { $0.refresh(); $0.pollStatus() }),
    DevTool(id: "open-logs", title: "Open Logs Folder", group: 2, devOnly: false, enabled: { _ in true },
            action: { $0.open(folder: $0.logsFolder) }),
    DevTool(id: "open-save-library", title: "Open Save Library Folder", group: 2, enabled: { _ in true },
            action: { $0.open(folder: Backend.repoRoot + "/out/save-library") }),
    DevTool(id: "open-sessions", title: "Open Sessions Folder", group: 2, enabled: { _ in true },
            action: { $0.open(folder: Backend.repoRoot + "/out/dev-sessions") }),
    DevTool(id: "open-benchmark-reports", title: "Open Benchmark Reports", group: 2, enabled: { _ in true },
            action: { $0.open(folder: Backend.repoRoot + "/out/benchmark/reports") }),
  ]
  /// The items shown for a Dev Options state.
  static func visible(developer: Bool) -> [DevTool] { items.filter { developer || !$0.devOnly } }
}

/// The tool items as menu content (both roles). `openWindow`: the player role opens the Dev Launcher window first for
/// tools that need it.
struct DevToolItems: View {
  @ObservedObject var model: LauncherModel
  var openWindow: (() -> Void)? = nil
  var body: some View {
    let shown = DevTools.visible(developer: model.developerMode)
    ForEach(0..<3, id: \.self) { group in
      if group > 0 { Divider() }
      ForEach(shown.filter { $0.group == group }) { item in
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

/// The Dev Launcher model in the player role (Dev Options): created when first needed and a checkout is known.
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

/// Window > Fable II Launcher in the player role: the Dev Launcher's window, or the checkout note.
struct DevLauncherWindowHost: View {
  @ObservedObject var host = DevHost.shared
  @State private var error = ""
  var body: some View {
    if let model = host.model {
      LauncherWindowContent(model: model)
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

/// The main window plus the --snapshot and --press-* hooks.
struct DevMainWindow: View {
  @ObservedObject var model: LauncherModel
  var body: some View {
    LauncherWindowContent(model: model).onAppear {
      DevSnapshot.start(model: model)
      // Testing: --press-load (--press-continue) / --press-launch press Load Save once the save list is in (as the
      // player's --press-play), so the whole button path, game end and return to the launcher run unattended.
      for (flag, press) in [("--press-load", { model.continueLatest() }), ("--press-continue", { model.continueLatest() }),
                            ("--press-launch", { model.loadSave() })]
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
      .commands {
        ToolsCommands(model: model)
        SettingsPageCommand(model: model, windowTitles: ["Fable II Launcher"])
      }
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

/// --snapshot PATH [--tab main|picker|general|...] [--dev-options on|off]: the app starts as usual (without taking
/// focus), shows the front page, the save library sheet or the Settings page on that tab, then saves its window
/// (shadow-free frame) to a PNG and quits. Nothing is stored (SettingsModel.persist).
enum DevSnapshot {
  static let request: (path: String, tab: String)? = argument(after: "--snapshot").map {
    ($0, argument(after: "--tab") ?? "main")
  }
  static var settingsTab: SettingsTab? { request.flatMap { SettingsTab(rawValue: $0.tab) } }

  /// Waits for the save list (20 s guard), then captures.
  @MainActor static func start(model: LauncherModel) {
    guard let request = request else { return }
    if let tab = settingsTab {
      model.settingsTab = tab
      model.page = .settings
    } else if request.tab == "picker" {
      model.showPicker = true
    }
    let deadline = Date().addingTimeInterval(20)
    func capture() {
      guard model.loaded || Date() > deadline else {
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
    let window = request.tab == "picker" ? main?.attachedSheet : main
    guard let window = window else { print("snapshot: window not found"); exit(1) }
    guard WindowSnapshot.write(window, to: request.path) else {
      print("snapshot: could not capture the window"); exit(1)
    }
    print(request.path)
    exit(0)
  }
}

/// What a role of the app can reach (tests/unified_launcher_tests.py: with Dev Options on, everything the Dev
/// Launcher role has must be reachable in the player role with a checkout; with it off, neither role shows the dev
/// tabs or dev tools). Derived from the same data the menus and windows are built from.
enum Capabilities {
  /// Settings tabs and the flags they show for a Dev Options state (flags without a tab are CLI-only).
  static func settings(developer: Bool) -> [String] {
    let tabs = SettingsTab.visible(developer: developer)
    return tabs.map { "settings:" + $0.rawValue }
      + LaunchSettings.flags.filter { $0.tab.map(tabs.contains) ?? true }.map { "flag:" + $0.key }
  }
  static func devLauncher(developer: Bool) -> [String] {
    ["window:dev-launcher", "cli:launch", "cli:print-args", "cli:picker-rows", "settings:dev-options"]
      + settings(developer: developer)
      + DevTools.visible(developer: developer).map { "tool:" + $0.id }
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
