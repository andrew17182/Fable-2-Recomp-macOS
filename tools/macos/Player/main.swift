// Fable II player app: the portable "Fable II.app"'s main executable (tools/fable2_portable.py,
// docs/player-app.md). One app, standard macOS windows, no Terminal:
//   - first launch: a welcome window that reuses a sibling app's finished game files (APFS clone, as
//     AppLauncher.m did) or asks for the disc image and runs FableIIPrepare with a progress bar, Cancel
//     and a "Show details" log (the real prepare output, also appended to logs/prepare.log);
//   - release app (docs/release-app.md): after the game files, the game is built on this Mac (BuildRunner runs
//     the kit's tools/fable2_game_build.py; Command Line Tools install, progress, cancel, Copy Details);
//   - every launch: the Play window (plain native window; Continue or title screen; resolution, frame rate,
//     display; Play);
//   - Settings > Advanced: Start Fresh and Archives (tools/macos/Shared/StartFresh.swift);
//   - Settings (⌘,): the Dev Launcher's tabs, from the shared tools/macos/Shared/*.swift (Diagnostics is
//     "Advanced" here), stored in <state>/player-settings.plist.
// Play sets the settings' environment (LaunchSettings.gameEnvironment, the same mapping the Dev Launcher
// passes through tools/fable2_dev_session.py) and runs Contents/MacOS/FableIILauncher (AppLauncher.m). That
// launcher stages the side files, seeds the shader cache, takes the state folder's lock and execs fable_2,
// exactly as before; the benchmark kit uses the same launcher. From the Play window it runs as a child
// process: the launcher hides and leaves the Dock to the game (.accessory), and comes back when the game
// quits (with a note and Show Logs after a crash). "Skip launcher, play directly" (and --play) exec it in
// this process instead, so the app ends with the game; holding Option while opening shows the launcher.
//
// Test and tooling flags (everything else is passed to the game):
//   --play               start the game as soon as the game files are ready (exec, as "Skip launcher")
//   --press-play         press the Play window's button (child game; the launcher returns when it exits)
//   --print-env          print the environment items Play would set (JSON) and exit
//   --iso IMAGE          on the welcome window, extract IMAGE at once (as if chosen in the file panel)
//   --reuse              on the welcome window, reuse the offered sibling game files at once
//   --show-launcher      as holding Option
//   --snapshot PATH.png [--window play|welcome|progress|general|display|controls|performance|fixes|advanced]
//                        show that window without taking focus, save it to a PNG and quit (progress: once
//                        the extraction has started, with details shown)
import AppKit
import Darwin
import SwiftUI
import UniformTypeIdentifiers

// MARK: - Paths and game files

/// Flags this app consumes; the rest are passed to the game.
let playerFlags: Set<String> = ["--play", "--press-play", "--print-env", "--reuse", "--show-launcher", "--start-fresh",
                                 "--archive-saves", "--list-archives", "--relaunched"]
let playerValueFlags: Set<String> = ["--iso", "--snapshot", "--window", "--restore-archive", "--delete-archive"]
var gameArguments: [String] {
  var out: [String] = []
  var i = 0
  while i < commandLine.count {
    let arg = commandLine[i]
    if playerValueFlags.contains(arg) { i += 2; continue }
    if !playerFlags.contains(arg) && !arg.hasPrefix("-psn_") { out.append(arg) }
    i += 1
  }
  return out
}

enum Paths {
  static let environment = ProcessInfo.processInfo.environment
  static let bundle = Bundle.main
  static let macOS = bundle.executableURL!.deletingLastPathComponent().path
  static let resources = bundle.resourcePath ?? ""
  /// The release app (tools/fable2_release.py) builds the game on this Mac from its kit (docs/release-app.md).
  static let buildsGame = bundle.object(forInfoDictionaryKey: "Fable2BuildsGame") as? Bool ?? false
  static let kit = resources + "/kit"
  static var kitID: String { bundle.object(forInfoDictionaryKey: "Fable2KitID") as? String ?? "" }
  /// Release: a different default.xex cannot be built (codegen is xex-specific), so a mismatch blocks.
  static let mismatchBlocks = bundle.object(forInfoDictionaryKey: "Fable2XexMismatchBlocks") as? Bool ?? false
  /// The app's folder in Application Support (Info.plist Fable2StateFolder; the release app has its own, so it
  /// never shares files with development and playtest builds in "Fable II Recomp").
  static let stateFolderName = bundle.object(forInfoDictionaryKey: "Fable2StateFolder") as? String ?? "Fable II Recomp"
  /// Same rule as AppLauncher.m: FABLE2_APP_STATE_ROOT, else ~/Library/Application Support/<stateFolderName>.
  static let state: String = {
    if let root = environment["FABLE2_APP_STATE_ROOT"], !root.isEmpty { return root }
    let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
    return support.appendingPathComponent(stateFolderName).path
  }()
  /// The extraction folder (FABLE2_PORTABLE_GAME_DATA overrides it for tests, as in AppLauncher.m).
  static let managed: String = {
    if let path = environment["FABLE2_PORTABLE_GAME_DATA"], !path.isEmpty { return path }
    return state + "/GameData"
  }()
  /// A per-launch game folder (dev/test); never remembered.
  static let environmentGameData: String? = {
    guard let path = environment["FABLE2_GAME_DATA_ROOT"], !path.isEmpty else { return nil }
    return path
  }()
  static var launcherPlist: String { state + "/launcher.plist" }
  static var settingsPlist: String { state + "/player-settings.plist" }
  static var logs: String { state + "/logs" }
  static var prepareLog: String { logs + "/prepare.log" }
  static var expectedXex: String? { bundle.object(forInfoDictionaryKey: "Fable2XexSHA256") as? String }
  static var version: String {
    (bundle.object(forInfoDictionaryKey: "Fable2VersionString") as? String)
      ?? (bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String) ?? ""
  }
}

func appendLog(_ path: String, _ line: String) {
  try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent,
                                           withIntermediateDirectories: true)
  let text = "[\(Date())] \(line)\n"
  guard let handle = FileHandle(forWritingAtPath: path) ?? {
    FileManager.default.createFile(atPath: path, contents: nil, attributes: [.posixPermissions: 0o600])
    return FileHandle(forWritingAtPath: path)
  }() else { return }
  handle.seekToEndOfFile()
  handle.write(Data(text.utf8))
  try? handle.close()
}

/// The game folder checks of AppLauncher.m (ValidContent, PortableMarker).
enum GameFiles {
  static func valid(_ path: String) -> Bool {
    var directory: ObjCBool = false
    let fm = FileManager.default
    guard path.hasPrefix("/"),
          fm.fileExists(atPath: path + "/default.xex", isDirectory: &directory), !directory.boolValue,
          fm.fileExists(atPath: path + "/data", isDirectory: &directory), directory.boolValue else { return false }
    return true
  }
  static func marker(_ path: String) -> [String: Any]? {
    guard let data = FileManager.default.contents(atPath: path + "/.fable2-portable.json") else { return nil }
    return (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
  }
  static func complete(_ path: String) -> Bool { marker(path)?["complete"] as? Bool == true }

  static var remembered: [String: Any] { (NSDictionary(contentsOfFile: Paths.launcherPlist) as? [String: Any]) ?? [:] }

  /// The folder the game will use (AppLauncher.m's choice for a portable build).
  static var root: String {
    if let env = Paths.environmentGameData { return env }
    if let override = Paths.environment["FABLE2_PORTABLE_GAME_DATA"], !override.isEmpty { return Paths.managed }
    return remembered["GameDataRoot"] as? String ?? Paths.managed
  }
  static var ready: Bool {
    let path = root
    if Paths.environmentGameData != nil { return valid(path) }
    let managed = (path as NSString).standardizingPath == (Paths.managed as NSString).standardizingPath
    return valid(path) && (!managed || complete(path))
  }

  /// Stores the game folder for FableIILauncher (launcher.plist, merged; AllowXexMismatch: a different disc
  /// image accepted with "Continue Anyway").
  static func remember(root: String, allowMismatch: Bool) {
    var values = remembered
    values["GameDataRoot"] = root
    values["AllowXexMismatch"] = allowMismatch
    (values as NSDictionary).write(toFile: Paths.launcherPlist, atomically: true)
  }

  struct Candidate: Equatable {
    let path: String
    let appName: String  // the Application Support folder it belongs to
  }

  /// Finished extractions of sibling apps this build can reuse (port of AppLauncher.m ReuseSiblingGameData:
  /// complete marker, default.xex + data/, this build's xex hash). FABLE2_PORTABLE_REUSE_FROM
  /// (colon-separated GameData folders) replaces the list for tests; set but empty disables reuse.
  static func reuseCandidates() -> [Candidate] {
    guard let expected = Paths.expectedXex, !expected.isEmpty,
          !FileManager.default.fileExists(atPath: Paths.managed) else { return [] }
    var paths: [String]
    if let override = Paths.environment["FABLE2_PORTABLE_REUSE_FROM"] {
      paths = override.isEmpty ? [] : override.components(separatedBy: ":")
    } else {
      let support = (Paths.state as NSString).deletingLastPathComponent
      paths = ["Fable II Recomp", "Fable II Recomp Benchmark"].map { support + "/" + $0 + "/GameData" }
    }
    var found: [Candidate] = []
    for source in paths where source.hasPrefix("/") {
      if (source as NSString).standardizingPath == (Paths.managed as NSString).standardizingPath { continue }
      guard FileManager.default.fileExists(atPath: source) else { continue }
      guard let marker = marker(source), marker["complete"] as? Bool == true, valid(source) else {
        appendLog(Paths.prepareLog, "[portable] reuse: \(source) is not a complete extraction; skipped")
        continue
      }
      guard let sha = marker["xexSHA256"] as? String, sha.caseInsensitiveCompare(expected) == .orderedSame else {
        appendLog(Paths.prepareLog, "[portable] reuse: \(source) has default.xex \(marker["xexSHA256"] ?? "(unknown)"), "
                  + "this build expects \(expected); skipped")
        continue
      }
      let folder = ((source as NSString).deletingLastPathComponent as NSString).lastPathComponent
      let names = ["Fable II Recomp": "Fable II app's", "Fable II Recomp Benchmark": "Fable II Benchmark's"]
      found.append(Candidate(path: source, appName: names[folder] ?? "“\(folder)”"))
    }
    return found
  }

  /// APFS-clones `candidate` into the managed folder (clonefile; the source is only read). Returns an error
  /// text, or nil when the clone is complete.
  static func reuse(_ candidate: Candidate) -> String? {
    let fm = FileManager.default
    let target = Paths.managed
    try? fm.createDirectory(atPath: (target as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
    let start = Date()
    if clonefile(candidate.path, target, 0) != 0 {
      let code = errno
      appendLog(Paths.prepareLog, "[portable] reuse: clonefile \(candidate.path) -> \(target) failed: "
                + String(cString: strerror(code)) + "; skipped")
      if code != EEXIST { try? fm.removeItem(atPath: target) }  // never remove what we did not create
      return "The game files could not be copied from the \(candidate.appName) folder: \(String(cString: strerror(code)))."
    }
    guard complete(target), valid(target) else {
      appendLog(Paths.prepareLog, "[portable] reuse: clone of \(candidate.path) is incomplete; removed")
      try? fm.removeItem(atPath: target)
      return "The copied game files were incomplete."
    }
    appendLog(Paths.prepareLog, String(format: "[portable] reuse: game data cloned from %@ (APFS clone, %.2f s, xex %@)",
                                       candidate.path, -start.timeIntervalSinceNow,
                                       (marker(target)?["xexSHA256"] as? String) ?? "?"))
    remember(root: target, allowMismatch: false)
    return nil
  }
}

// MARK: - Saves (read only)

struct SaveSummary {
  let title: String
  let hero: String  // folder name, HeroNNN: Continue loads this one (FABLE2_BOOT_SAVE_HERO)
  let detail: String
  let count: Int
}

enum Saves {
  /// Words of an internal name: "QC075_JourneyToRavenscar" -> "Journey To Ravenscar", "BowerLake" -> "Bower Lake".
  static func words(_ raw: String) -> String {
    var name = raw
    if let underscore = name.firstIndex(of: "_"), name.first == "Q" { name = String(name[name.index(after: underscore)...]) }
    var out = ""
    let chars = Array(name)
    for (i, c) in chars.enumerated() {
      if c == "_" { out.append(" "); continue }
      if i > 0, c.isUppercase, chars[i - 1].isLowercase || (i + 1 < chars.count && chars[i + 1].isLowercase && chars[i - 1].isUppercase) {
        out.append(" ")
      }
      out.append(c)
    }
    return out.trimmingCharacters(in: .whitespaces)
  }

  static func field(_ path: String, _ name: String) -> String? {
    guard let data = FileManager.default.contents(atPath: path), data.count > 4 else { return nil }
    let text = String(decoding: data.dropFirst(4), as: UTF8.self)
    guard let range = text.range(of: "<\(name) type=\"[^\"]*\">([^<]*)</\(name)>", options: .regularExpression) else { return nil }
    let match = String(text[range])
    guard let open = match.firstIndex(of: ">"), let close = match.range(of: "</") else { return nil }
    return String(match[match.index(after: open)..<close.lowerBound]).components(separatedBy: " ").first
  }

  /// The newest hero save in the state folder (saves/<profile>/4D5307F1/00000001/HeroNNN) and how many there are.
  static func latest() -> SaveSummary? {
    let fm = FileManager.default
    let root = Paths.state + "/saves"
    var newest: (date: Date, hero: String, container: String)?
    var count = 0
    for profile in (try? fm.contentsOfDirectory(atPath: root)) ?? [] {
      let container = root + "/" + profile + "/4D5307F1"
      for hero in (try? fm.contentsOfDirectory(atPath: container + "/00000001")) ?? [] where hero.hasPrefix("Hero") {
        guard let date = (try? fm.attributesOfItem(atPath: container + "/00000001/" + hero + "/mainsave.bin"))?[.modificationDate]
                as? Date else { continue }
        count += 1
        if newest == nil || date > newest!.date { newest = (date, hero, container) }
      }
    }
    guard let save = newest else { return nil }
    let folder = save.container + "/00000001/" + save.hero
    var title = save.hero
    if let header = fm.contents(atPath: save.container + "/Headers/00000001/" + save.hero + ".header"), header.count >= 0x108 {
      let name = String(data: header[8..<0x108], encoding: .utf16BigEndian)?.components(separatedBy: "\0").first ?? ""
      if !name.isEmpty { title = name }
    }
    var parts: [String] = []
    if let quest = field(folder + "/chaptersave.bin", "QuestName") { parts.append(words(quest)) }
    if let level = field(folder + "/chaptersave.bin", "LevelName") { parts.append(words(level)) }
    let formatter = DateFormatter()
    formatter.dateStyle = .medium
    formatter.timeStyle = .short
    formatter.doesRelativeDateFormatting = true
    parts.append(formatter.string(from: save.date))
    return SaveSummary(title: title, hero: save.hero, detail: parts.joined(separator: " · "), count: count)
  }
}

// MARK: - Extraction (FableIIPrepare)

final class PrepareRunner: ObservableObject {
  enum Phase: Equatable { case idle, running, mismatch(String), failed(String), done }
  @Published var phase = Phase.idle
  @Published var fraction = 0.0
  @Published var detail = "Reading the disc image…"
  @Published var log = ""
  private var process: Process?
  private var buffer = ""
  var image: String?
  var onDone: (() -> Void)?

  /// Extracts `image` into the managed folder (FableIIPrepare --headless --progress). With
  /// `stopOnMismatch` it stops after default.xex when its hash differs, so the player can choose.
  func start(image: String, stopOnMismatch: Bool = true) {
    self.image = image
    let fm = FileManager.default
    try? fm.createDirectory(atPath: Paths.logs, withIntermediateDirectories: true)
    let result = Paths.state + "/prepare-result.plist"
    try? fm.removeItem(atPath: result)
    var args = ["--target", Paths.managed, "--lua", Paths.resources + "/recomp-lua", "--result", result,
                "--iso", image, "--headless", "--progress"]
    if let expected = Paths.expectedXex, !expected.isEmpty { args += ["--expected-xex", expected] }
    if stopOnMismatch { args.append("--stop-on-mismatch") }
    let process = Process()
    process.executableURL = URL(fileURLWithPath: Paths.macOS + "/FableIIPrepare")
    process.arguments = args
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
      let data = handle.availableData
      guard !data.isEmpty else { return }
      DispatchQueue.main.async { self?.consume(String(decoding: data, as: UTF8.self)) }
    }
    process.terminationHandler = { [weak self] p in
      DispatchQueue.main.async {
        pipe.fileHandleForReading.readabilityHandler = nil
        let rest = pipe.fileHandleForReading.readDataToEndOfFile()
        self?.consume(String(decoding: rest, as: UTF8.self) + "\n")
        self?.finished(status: p.terminationReason == .exit ? p.terminationStatus : -1)
      }
    }
    phase = .running
    fraction = 0
    detail = "Reading the disc image…"
    appendLog(Paths.prepareLog, "[player] extracting the game files from \(image)")
    do { try process.run() } catch {
      phase = .failed("Could not start the extractor: \(error.localizedDescription)")
      return
    }
    self.process = process
  }

  func cancel() {
    guard let process = process, process.isRunning else { return }
    appendLog(Paths.prepareLog, "[player] extraction cancelled; the files copied so far are kept and the next try resumes")
    process.terminate()
  }

  private func consume(_ text: String) {
    buffer += text
    while let newline = buffer.firstIndex(of: "\n") {
      let line = String(buffer[..<newline])
      buffer = String(buffer[buffer.index(after: newline)...])
      if line.hasPrefix("[progress] ") {
        let rest = line.dropFirst("[progress] ".count)
        let parts = rest.split(separator: " ", maxSplits: 1)
        if let value = parts.first.flatMap({ Double($0) }) { fraction = value }
        if parts.count > 1 { detail = String(parts[1]) }
        continue  // five a second: kept out of the details and the log file
      }
      if line.isEmpty { continue }
      appendLog(Paths.prepareLog, line)
      log += line + "\n"
      if log.count > 60_000 { log = String(log.suffix(40_000)) }
    }
  }

  private func finished(status: Int32) {
    process = nil
    switch status {
    case 0:
      let result = NSDictionary(contentsOfFile: Paths.state + "/prepare-result.plist")
      let matches = result?["XexMatches"] as? Bool ?? true
      GameFiles.remember(root: Paths.managed, allowMismatch: !matches)
      fraction = 1
      phase = .done
      onDone?()
    case 4:
      let digest = GameFiles.marker(Paths.managed)?["xexSHA256"] as? String ?? "unknown"
      phase = .mismatch(digest)
    case 3, -1, 15:
      phase = .idle  // cancelled
    default:
      let error = log.split(separator: "\n").last { $0.contains("error:") }
        .map { String($0).replacingOccurrences(of: "[prepare] error: ", with: "") }
      phase = .failed(error ?? "The extraction stopped (exit \(status)). See the details below.")
    }
  }
}

// MARK: - Shader preparation (FABLE2_SHADER_PREP, docs/offline-shader-seed.md)

/// After the game files are ready: one short headless run of the game per resolution scale (FableIILauncher with
/// FABLE2_SHADER_PREP; no window, the game itself never starts) translates and compiles every remembered shader and
/// pipeline into this Mac's caches, so the first launch at any scale needs no shader work. Optional: Skip or a
/// failure just goes on to the Play window (the game then compiles during play, as before).
final class ShaderPrepRunner: ObservableObject {
  enum Phase: Equatable { case idle, running, done }
  @Published var phase = Phase.idle
  @Published var fraction = 0.0
  @Published var detail = ""
  /// Scales to prepare: every scale with a bundled seed (portable); the release app prepares the scale the Play
  /// window will use (Auto's pick) and 1× (PlayerModel.filesReady sets it).
  var scales = [1, 2, 3]
  /// Release app: the disc recipe (tools/fable2_release.py --shader-recipe) rebuilds the shader and pipeline lists
  /// from the player's own disc before the prep (docs/release-app.md "Shaders").
  static var recipe: String? {
    let path = Paths.resources + "/shader-recipe.json"
    return FileManager.default.fileExists(atPath: path) ? path : nil
  }
  private var process: Process?
  private var timer: Timer?
  private var index = 0
  private var maxPending = 1
  private var started = Date()
  private var skipped = false
  var environment: [String] = []
  var onDone: (() -> Void)?
  var releaseLock: (() -> Void)?
  var retakeLock: (() -> Void)?

  static var markerPath: String { Paths.state + "/cache/shader-prep.json" }
  /// What a finished preparation is valid for: this build and its bundled shader seed.
  static var stamp: String {
    let info = Paths.bundle.infoDictionary ?? [:]
    return [info["Fable2VersionString"] as? String ?? "", info["Fable2ShaderSeedID"] as? String ?? "",
            info["CFBundleVersion"] as? String ?? "", info["Fable2KitID"] as? String ?? ""].joined(separator: "|")
  }
  static var needed: Bool {
    if ProcessInfo.processInfo.environment["FABLE2_PLAYER_SHADER_PREP"] == "0" || Snapshot.path != nil { return false }
    // Nothing to prepare yet: no bundled seed and no pipeline lists remembered on this Mac (a first install of the
    // release app, which ships no game-derived shaders). The game compiles while playing and remembers its lists;
    // the next launch of a new version prepares them.
    if !hasLists { return false }
    guard let data = FileManager.default.contents(atPath: markerPath),
          let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return true }
    return json["stamp"] as? String != stamp
  }
  static var hasLists: Bool {
    let fm = FileManager.default
    if fm.fileExists(atPath: Paths.resources + "/native-shaders") || recipe != nil { return true }
    let shareable = Paths.state + "/cache/shaders/shareable/"
    return fm.fileExists(atPath: shareable + "4D5307F1.xsh") || fm.fileExists(atPath: shareable + "4D5307F1.metal.xpso")
  }
  func statusPath(_ scale: Int) -> String { Paths.logs + "/shader-prep-\(scale)x.json" }

  func start() {
    try? FileManager.default.createDirectory(atPath: Paths.logs, withIntermediateDirectories: true)
    if let recipe = Self.recipe { rebuildLists(recipe) }
    index = 0
    skipped = false
    started = Date()
    phase = .running
    appendLog(Paths.prepareLog, "[player] preparing shaders for \(scales.map { "\($0)x" }.joined(separator: ", "))")
    releaseLock?()
    runNext()
  }

  /// FableIIPrepare --shader-lists: the shader list (microcode) from this disc's shader bank and the pipeline list,
  /// into the state folder's cache. Existing lists are merged by the game itself; a list there already is kept.
  private func rebuildLists(_ recipe: String) {
    let shareable = Paths.state + "/cache/shaders/shareable/"
    if FileManager.default.fileExists(atPath: shareable + "4D5307F1.xsh") {
      appendLog(Paths.prepareLog, "[player] shader lists already on this Mac; the disc recipe is not needed")
      return
    }
    let started = Date()
    let process = Process()
    process.executableURL = URL(fileURLWithPath: Paths.macOS + "/FableIIPrepare")
    process.arguments = ["--target", GameFiles.root, "--shader-lists", recipe, "--cache", Paths.state + "/cache"]
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    guard (try? process.run()) != nil else { return }
    let output = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
    process.waitUntilExit()
    for line in output.split(separator: "\n") { appendLog(Paths.prepareLog, String(line)) }
    appendLog(Paths.prepareLog, String(format: "[player] shader lists from the disc recipe: exit %d in %.2f s",
                                       process.terminationStatus, Date().timeIntervalSince(started)))
  }

  private func runNext() {
    guard index < scales.count, !skipped else { return finish() }
    let scale = scales[index]
    let status = statusPath(scale)
    try? FileManager.default.removeItem(atPath: status)
    maxPending = 1
    detail = "Preparing shaders for \(scale)× resolution…"
    var env = ProcessInfo.processInfo.environment
    for item in environment {
      guard let eq = item.firstIndex(of: "=") else { continue }
      env[String(item[..<eq])] = String(item[item.index(after: eq)...])
    }
    env["FABLE2_SHADER_PREP"] = status
    env["FABLE2_RES_SCALE"] = "\(scale)"
    env["FABLE2_PLAYER_LAUNCH"] = "1"
    env["FABLE2_TEST_BACKGROUND"] = "1"  // never takes focus
    env.removeValue(forKey: "FABLE2_BOOT_SAVE")
    let process = Process()
    process.executableURL = URL(fileURLWithPath: Paths.macOS + "/FableIILauncher")
    process.arguments = ["--audio_mute=true"]
    process.environment = env
    process.standardOutput = FileHandle.nullDevice
    process.standardError = FileHandle.nullDevice
    let begun = Date()
    process.terminationHandler = { [weak self] p in
      DispatchQueue.main.async {
        guard let self = self else { return }
        let json = self.readStatus(status)
        let ok = p.terminationReason == .exit && p.terminationStatus == 0 && (json?["done"] as? Bool ?? false)
        appendLog(Paths.prepareLog, String(format: "[player] shader prep %dx: %@ in %.1f s (%@)", scale,
                                           ok ? "done" : "stopped", Date().timeIntervalSince(begun),
                                           json.map { "\($0["pipelines_compiled"] ?? "?") pipelines compiled, \($0["native_xlate"] ?? "?") native shaders" } ?? "no status"))
        if !ok && !self.skipped { appendLog(Paths.prepareLog, "[player] shader prep stopped (exit \(p.terminationStatus)); the game compiles the rest while playing") }
        self.index += 1
        if ok { self.runNext() } else { self.finish(success: false) }
      }
    }
    do { try process.run() } catch {
      appendLog(Paths.prepareLog, "[player] could not start the shader prep: \(error.localizedDescription)")
      return finish(success: false)
    }
    self.process = process
    timer?.invalidate()
    timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.poll() }
  }

  private func readStatus(_ path: String) -> [String: Any]? {
    guard let data = FileManager.default.contents(atPath: path) else { return nil }
    return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
  }

  private func poll() {
    guard index < scales.count, let json = readStatus(statusPath(scales[index])) else { return }
    let pending = (json["pending"] as? [String: Any] ?? [:]).values.compactMap { $0 as? Int }.reduce(0, +)
    maxPending = max(maxPending, pending)
    let inner = json["done"] as? Bool == true ? 1.0 : 1.0 - Double(pending) / Double(maxPending)
    fraction = (Double(index) + max(0, min(inner, 1))) / Double(scales.count)
    let compiled = json["pipelines_compiled"] as? Int ?? 0
    detail = "Preparing shaders for \(scales[index])× resolution… \(compiled) pipelines"
  }

  func skip() {
    skipped = true
    appendLog(Paths.prepareLog, "[player] shader prep skipped; the game compiles shaders while playing")
    if let process = process, process.isRunning { process.terminate() } else { finish(success: false) }
  }

  private func finish(success: Bool = true) {
    timer?.invalidate()
    timer = nil
    process = nil
    guard phase == .running else { return }
    if success && !skipped {
      let json: [String: Any] = ["stamp": Self.stamp, "scales": scales, "seconds": Date().timeIntervalSince(started),
                                 "when": ISO8601DateFormatter().string(from: Date())]
      if let data = try? JSONSerialization.data(withJSONObject: json) {
        FileManager.default.createFile(atPath: Self.markerPath, contents: data)
      }
      appendLog(Paths.prepareLog, String(format: "[player] shaders prepared in %.1f s", Date().timeIntervalSince(started)))
    }
    fraction = 1
    phase = .done
    retakeLock?()
    onDone?()
  }
}

// MARK: - Building the game on this Mac (release app, tools/fable2_game_build.py)

enum GameBuild {
  static var build: String { Paths.state + "/Build" }
  static var logPath: String { Paths.logs + "/build.log" }
  static var engine: String { Paths.state + "/Game/Fable II.bundle/Contents/MacOS/fable_2" }
  static var record: [String: Any]? {
    guard let data = FileManager.default.contents(atPath: build + "/build-state.json") else { return nil }
    return (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
  }
  /// The game is built for this app's kit (always true for apps that ship the game, e.g. the portable build).
  static var ready: Bool {
    guard Paths.buildsGame else { return true }
    guard let r = record, r["ok"] as? Bool == true, r["kit_id"] as? String == Paths.kitID else { return false }
    return FileManager.default.isExecutableFile(atPath: engine)
  }
  /// An earlier build exists (an update rebuilds incrementally).
  static var isUpdate: Bool { record?["kit_id"] != nil }
  static var machine: String {
    var size = 0
    sysctlbyname("hw.model", nil, &size, nil, 0)
    var model = [CChar](repeating: 0, count: max(size, 1))
    sysctlbyname("hw.model", &model, &size, nil, 0)
    let ram = Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824
    return String(format: "%@, %d cores, %.0f GB, macOS %@", String(cString: model), ProcessInfo.processInfo.processorCount,
                  ram, ProcessInfo.processInfo.operatingSystemVersionString)
  }
  /// First-build estimate before any progress is known (docs/release-app.md, "Measurements"): 745 s of compile CPU
  /// and ~30 s of code generation on an M5 core, scaled by the chip generation's speed, over the parallelism of
  /// the job count (performance cores count fully, efficiency cores 0.4).
  static var estimateMinutes: Int {
    func sysctlInt(_ name: String) -> Double {
      var value: Int32 = 0
      var size = MemoryLayout<Int32>.size
      return sysctlbyname(name, &value, &size, nil, 0) == 0 ? Double(value) : 0
    }
    let ram = Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824
    let cores = Double(ProcessInfo.processInfo.processorCount)
    let perf = max(1, sysctlInt("hw.perflevel0.physicalcpu"))
    let efficiency = sysctlInt("hw.perflevel1.physicalcpu")
    let jobs = max(1, min(cores, floor((ram - 3.5) / 0.75)))
    let parallel = max(1, min(jobs, perf + 0.4 * efficiency))
    let slowness: [Int: Double] = [1: 1.8, 2: 1.6, 3: 1.4, 4: 1.15]
    let factor = slowness[AutoResolution.generation(of: GpuInfo.read.model ?? "")] ?? 1.0
    let seconds = factor * (30 + 745 / parallel + 25)  // codegen + compile + the longest single file
    return max(1, Int((seconds / 60).rounded(.up)))
  }
}

/// Apple's Command Line Tools (or Xcode): clang and /usr/bin/python3 work only with a developer directory.
enum DeveloperTools {
  /// Checked without running anything that could open Apple's install dialog: xcode-select -p and the clang there.
  static var ready: Bool {
    if let marker = Paths.environment["FABLE2_SIMULATE_NO_CLT"], !marker.isEmpty {
      return FileManager.default.fileExists(atPath: marker)  // test: "installed" once the marker file exists
    }
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
    process.arguments = ["-p"]
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = FileHandle.nullDevice
    guard (try? process.run()) != nil else { return false }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    guard process.terminationStatus == 0 else { return false }
    let path = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
    let fm = FileManager.default
    return fm.isExecutableFile(atPath: path + "/usr/bin/clang")
      || fm.isExecutableFile(atPath: path + "/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang")
  }

  /// Apple's standard "install the command line developer tools" dialog.
  static func requestInstall() {
    if Paths.environment["FABLE2_SIMULATE_NO_CLT"] != nil {
      appendLog(GameBuild.logPath, "[player] (simulated) xcode-select --install; waiting for the marker file")
      return
    }
    appendLog(GameBuild.logPath, "[player] $ /usr/bin/xcode-select --install")
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
    process.arguments = ["--install"]
    try? process.run()
  }
}

final class BuildRunner: ObservableObject {
  enum Phase: Equatable { case idle, needsTools, waitingForTools, running, cancelled, failed(String), done }
  struct Step: Identifiable, Equatable {
    let key: String
    let title: String
    var id: String { key }
  }
  static let steps = [Step(key: "tools", title: "Apple's Command Line Tools"), Step(key: "cmake", title: "CMake"),
                      Step(key: "sync", title: "Copy the build kit"), Step(key: "codegen", title: "Translate the game code"),
                      Step(key: "compile", title: "Compile"), Step(key: "link", title: "Link"),
                      Step(key: "assemble", title: "Sign on this Mac")]
  @Published var phase = Phase.idle
  @Published var fraction = 0.0
  @Published var detail = ""
  @Published var current = "tools"
  @Published var finished: Set<String> = []
  @Published var eta: Int?
  @Published var log = ""
  @Published var seconds: [String: Double] = [:]
  private var process: Process?
  private var buffer = ""
  private var poll: Timer?
  private var stepStarted = Date()
  private var started = Date()
  var onDone: (() -> Void)?

  /// Checks the developer tools, then runs the build (or waits for the tools' installer).
  func start() {
    try? FileManager.default.createDirectory(atPath: Paths.logs, withIntermediateDirectories: true)
    finished = []
    current = "tools"
    started = Date()
    stepStarted = Date()
    eta = nil
    if !DeveloperTools.ready {
      appendLog(GameBuild.logPath, "[player] Command Line Tools not found (xcode-select -p)")
      phase = .needsTools
      // Tests: press "Install Command Line Tools…" at once (with FABLE2_SIMULATE_NO_CLT nothing is installed).
      if Paths.environment["FABLE2_TEST_AUTO_INSTALL_TOOLS"] == "1" { installTools() }
      return
    }
    runBuild()
  }

  func installTools() {
    DeveloperTools.requestInstall()
    phase = .waitingForTools
    detail = "Waiting for the Command Line Tools installer…"
    poll?.invalidate()
    // Readiness signal: xcode-select -p names a developer folder with clang. Checked every 2 s while the installer
    // runs; the window continues on its own.
    poll = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] timer in
      guard let self = self else { timer.invalidate(); return }
      if DeveloperTools.ready {
        timer.invalidate()
        appendLog(GameBuild.logPath, "[player] Command Line Tools are installed")
        self.runBuild()
      }
    }
  }

  func checkToolsAgain() {
    if DeveloperTools.ready { poll?.invalidate(); runBuild() }
  }

  private func runBuild() {
    if let running = process, running.isRunning { return }  // one build at a time
    seconds["tools"] = Date().timeIntervalSince(stepStarted)
    finished.insert("tools")
    stepStarted = Date()
    var args = ["-I", "-B", "-u", Paths.kit + "/game-src/tools/fable2_game_build.py", "build",
                "--kit", Paths.kit, "--state", Paths.state, "--game-data", GameFiles.root]
    if let extra = Paths.environment["FABLE2_GAME_BUILD_ARGS"], !extra.isEmpty {
      args += extra.split(separator: " ").map(String.init)  // tests: --measure, --jobs N, --ignore-system-cmake, …
    }
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
    process.arguments = args
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
      let data = handle.availableData
      guard !data.isEmpty else { return }
      DispatchQueue.main.async { self?.consume(String(decoding: data, as: UTF8.self)) }
    }
    process.terminationHandler = { [weak self] p in
      DispatchQueue.main.async {
        pipe.fileHandleForReading.readabilityHandler = nil
        let rest = pipe.fileHandleForReading.readDataToEndOfFile()
        self?.consume(String(decoding: rest, as: UTF8.self) + "\n")
        self?.ended(status: p.terminationReason == .exit ? p.terminationStatus : -1)
      }
    }
    log = ""
    phase = .running
    fraction = 0
    detail = "Starting…"
    appendLog(GameBuild.logPath, "[player] building the game (\(GameBuild.machine)): /usr/bin/python3 "
              + args.joined(separator: " "))
    do { try process.run() } catch {
      phase = .failed("Could not start the build (/usr/bin/python3): \(error.localizedDescription)")
      return
    }
    self.process = process
  }

  func cancel() {
    poll?.invalidate()
    guard let process = process, process.isRunning else { phase = .cancelled; return }
    appendLog(GameBuild.logPath, "[player] build cancelled; the build folder is kept and the next try continues")
    process.terminate()
  }

  private func consume(_ text: String) {
    buffer += text
    while let newline = buffer.firstIndex(of: "\n") {
      let line = String(buffer[..<newline])
      buffer = String(buffer[buffer.index(after: newline)...])
      if line.hasPrefix("[progress] ") {
        let parts = line.dropFirst("[progress] ".count).split(separator: " ", maxSplits: 1)
        if let value = parts.first.flatMap({ Double($0) }) { fraction = value }
        if parts.count > 1 { detail = String(parts[1]) }
        continue  // frequent: kept out of the details and the log file
      }
      if line.hasPrefix("[eta] ") { eta = Int(line.dropFirst(6)); continue }
      if line.hasPrefix("[stage] ") {
        let key = String(line.dropFirst(8).split(separator: " ").first ?? "")
        if key != current {
          seconds[current] = Date().timeIntervalSince(stepStarted)
          finished.insert(current)
          stepStarted = Date()
          current = key
          if key != "compile" { eta = nil }
        }
      }
      if line.isEmpty { continue }
      appendLog(GameBuild.logPath, line)
      log += line + "\n"
      if log.count > 120_000 { log = String(log.suffix(80_000)) }
    }
  }

  private func ended(status: Int32) {
    process = nil
    switch status {
    case 0:
      finished.formUnion(Self.steps.map(\.key))
      fraction = 1
      phase = .done
      appendLog(GameBuild.logPath, String(format: "[player] game built in %.0f s", Date().timeIntervalSince(started)))
      onDone?()
    case 3, 15, -1:
      phase = .cancelled
    default:
      let error = log.split(separator: "\n").last { $0.hasPrefix("[error] ") }
        .map { String($0.dropFirst("[error] ".count)) }
      phase = .failed(error ?? "The build stopped (exit \(status)). See the details below.")
    }
  }

  /// Text for "Copy Details": the error, this Mac, the app and the end of the log.
  var report: String {
    var lines = ["Fable II build report", "App: \(Paths.version) (kit \(Paths.kitID))", "Mac: \(GameBuild.machine)"]
    if case .failed(let error) = phase { lines.append("Error: \(error)") }
    lines.append("Log (\(GameBuild.logPath)), last lines:")
    lines += log.split(separator: "\n", omittingEmptySubsequences: false).suffix(200).map(String.init)
    return lines.joined(separator: "\n")
  }
}

// MARK: - Model

enum StartMode: String, CaseIterable, Identifiable {
  case continueSave = "continue", title
  var id: String { rawValue }
}

enum Screen { case welcome, preparing, building, play }

final class PlayerModel: SettingsModel {
  @Published var screen: Screen
  @Published var latestSave: SaveSummary?
  @Published var candidates: [GameFiles.Candidate] = []
  @Published var message = ""
  @Published var skipLauncher: Bool { didSet { store.set(skipLauncher, forKey: "skipLauncher") } }
  @Published var startMode: StartMode { didSet { store.set(startMode.rawValue, forKey: "startMode") } }
  /// The game started from the Play window is running (a child process); the launcher is hidden until it exits.
  @Published var gameRunning = false
  /// Set when the game from the Play window ended abnormally (crash, kill, non-zero exit).
  @Published var gameNote = ""
  @Published var showStartFresh = false
  @Published var showStartFreshMain = false
  /// Developer mode (Settings > Advanced): the Dev Launcher's window and tools in this app (docs/player-app.md).
  @Published var developerMode: Bool {
    didSet {
      store.set(developerMode, forKey: "developerMode")
      if developerMode { DevHost.shared.ensure() }
    }
  }
  @Published var showArchives = false
  private var hiddenWindows: [NSWindow] = []
  let prepare = PrepareRunner()
  let shaderPrep = ShaderPrepRunner()
  let builder = BuildRunner()
  var lock: Int32 = -1

  init() {
    try? FileManager.default.createDirectory(atPath: Paths.state, withIntermediateDirectories: true)
    let store = PlistSettingsStore(path: Paths.settingsPlist)
    skipLauncher = store.object(forKey: "skipLauncher") as? Bool ?? false
    startMode = StartMode(rawValue: store.string(forKey: "startMode") ?? "") ?? .continueSave
    developerMode = store.object(forKey: "developerMode") as? Bool ?? AppRole.developerModeDefault
    // One settings file, one store object: the checkout and the Dev Launcher window share it.
    DevCheckout.store = store
    DevHost.shared.store = store
    screen = !GameFiles.ready ? .welcome : GameBuild.ready ? .play : .building
    super.init(store: store)
    // The game next to this executable: no-converter (release/portable) builds force the native switches on.
    let hasConverter = LaunchSettings.hasConverter(macOSFolder: Paths.macOS)
    converterProbe = { _ in hasConverter }
    prepare.onDone = { [weak self] in self?.filesReady() }
    builder.onDone = { [weak self] in self?.filesReady() }
    shaderPrep.onDone = { [weak self] in self?.showPlay() }
    shaderPrep.releaseLock = { [weak self] in
      guard let self = self else { return }
      if self.lock >= 0 { close(self.lock); self.lock = -1 }
      if PlayerApp.heldLock >= 0 { close(PlayerApp.heldLock); PlayerApp.heldLock = -1 }
    }
    shaderPrep.retakeLock = { [weak self] in
      let fd = open(Paths.state + "/app.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
      if fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0 { PlayerApp.heldLock = fd; self?.lock = fd } else if fd >= 0 { close(fd) }
    }
    refresh()
  }

  func refresh() {
    if screen == .play {
      latestSave = Saves.latest()
    } else {
      candidates = GameFiles.reuseCandidates()
    }
  }

  /// The game files are ready: build the game on this Mac when this app does that and it is not built for this
  /// version yet (BuildRunner), prepare the shaders once per build (ShaderPrepRunner), then the Play window.
  func filesReady() {
    message = ""
    if GameFiles.ready && !GameBuild.ready {
      screen = .building
      if builder.phase != .running && builder.phase != .waitingForTools { builder.start() }
      return
    }
    if ShaderPrepRunner.needed && GameFiles.ready {
      screen = .preparing
      shaderPrep.environment = settings.gameEnvironment
      if Paths.buildsGame {
        let pick = settings.resolution.effectiveScale(frameRate: settings.frameRate, rule2x: settings.auto2x)
        shaderPrep.scales = pick == 1 ? [1] : [pick, 1]
      }
      shaderPrep.start()
      return
    }
    showPlay()
  }

  func showPlay() {
    screen = .play
    refresh()
  }

  func reuse(_ candidate: GameFiles.Candidate) {
    if let error = GameFiles.reuse(candidate) {
      message = error
      candidates = GameFiles.reuseCandidates()
    } else {
      filesReady()
    }
  }

  /// Asks for the disc image (or uses `path`) and starts the extraction.
  func chooseImage(_ path: String? = nil) {
    var image = path
    if image == nil {
      let panel = NSOpenPanel()
      panel.title = "Choose your Fable II disc image"
      panel.message = "Its game files are copied into this Mac's Application Support folder (about 7 GB). "
        + "The image itself is only read."
      panel.prompt = "Use This Image"
      panel.canChooseFiles = true
      panel.canChooseDirectories = false
      panel.allowsMultipleSelection = false
      if let iso = UTType(filenameExtension: "iso") { panel.allowedContentTypes = [iso] }
      guard panel.runModal() == .OK, let url = panel.url else { return }
      image = url.path
    }
    message = ""
    screen = .preparing
    prepare.start(image: image!)
  }

  /// The environment Play sets: every setting (the shared mapping), the start choice, and the marker that
  /// tells FableIILauncher the files are prepared and Option is ours.
  var playEnvironment: [String] {
    var env = settings.gameEnvironment
    if startMode == .continueSave, let save = latestSave, save.count > 0 {
      env.append("FABLE2_BOOT_SAVE=1")
      // With several saves the game would show its slot cards; load the newest one directly instead.
      if save.count > 1 { env.append("FABLE2_BOOT_SAVE_HERO=" + save.hero) }
    }
    env.append("FABLE2_PLAYER_LAUNCH=1")
    return env
  }

  /// Play button: starts FableIILauncher (which stages, takes the lock and execs fable_2) as a child process,
  /// hides the launcher and leaves the Dock to the game (.accessory). When the game exits the Play window comes
  /// back (latest save refreshed); an abnormal exit shows a note with Show Logs. ⌘Q in the game therefore
  /// returns here, and ⌘Q in the launcher quits the app.
  func playFromLauncher() {
    guard !gameRunning else { return }
    latestSave = Saves.latest()
    var env = Paths.environment  // the app's own launch environment: nothing from an earlier Play leaks in
    for item in playEnvironment {
      guard let eq = item.firstIndex(of: "=") else { continue }
      env[String(item[..<eq])] = String(item[item.index(after: eq)...])
    }
    appendLog(Paths.logs + "/player.log", "play (returns to the launcher): " + playEnvironment.joined(separator: " "))
    if lock >= 0 { close(lock); lock = -1 }  // the launcher takes the state folder's lock itself
    let process = Process()
    process.executableURL = URL(fileURLWithPath: Paths.macOS + "/FableIILauncher")
    process.arguments = gameArguments
    process.environment = env
    process.terminationHandler = { [weak self] ended in
      DispatchQueue.main.async { self?.gameEnded(ended) }
    }
    do {
      try process.run()
    } catch {
      lock = relock()
      message = "Could not start the game: \(error.localizedDescription)"
      return
    }
    gameRunning = true
    gameNote = ""
    message = ""
    // No windows and no Dock icon: keep macOS from ending the app (automatic/sudden termination) while it waits.
    ProcessInfo.processInfo.disableAutomaticTermination("Fable II is running")
    ProcessInfo.processInfo.disableSuddenTermination()
    let background = Paths.environment["FABLE2_TEST_BACKGROUND"] == "1"
    if !background, let id = Bundle.main.bundleIdentifier {
      NSApp.yieldActivation(toApplicationWithBundleIdentifier: id)  // the game has the same bundle id
    }
    hiddenWindows = NSApp.windows.filter { $0.isVisible }
    hiddenWindows.forEach { $0.orderOut(nil) }
    NSApp.setActivationPolicy(.accessory)  // one Dock icon while the game runs: the game's
  }

  private func gameEnded(_ process: Process) {
    let status = process.terminationStatus
    let clean = process.terminationReason == .exit && status == 0
    appendLog(Paths.logs + "/player.log", "game ended: " + (process.terminationReason == .exit
      ? "exit \(status)" : "signal \(status)"))
    gameRunning = false
    ProcessInfo.processInfo.enableAutomaticTermination("Fable II is running")
    ProcessInfo.processInfo.enableSuddenTermination()
    lock = relock()
    if !clean {
      gameNote = process.terminationReason == .exit
        ? "The game closed unexpectedly (exit code \(status))."
        : "The game closed unexpectedly (signal \(status))."
    }
    refresh()
    let background = Paths.environment["FABLE2_TEST_BACKGROUND"] == "1"
    if !background { NSApp.setActivationPolicy(.regular) }
    let windows = hiddenWindows.isEmpty ? NSApp.windows.filter { $0.title == "Fable II" } : hiddenWindows
    hiddenWindows = []
    windows.forEach { $0.makeKeyAndOrderFront(nil) }
    if !background { NSApp.activate(ignoringOtherApps: true) }
  }

  /// Takes the state folder's lock again after the game released it (best effort: never fatal here).
  private func relock() -> Int32 {
    let fd = open(Paths.state + "/app.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
    guard fd >= 0 else { return -1 }
    if flock(fd, LOCK_EX | LOCK_NB) != 0 { close(fd); return -1 }
    return fd
  }

  /// Direct start (skip launcher, --play): execs FableIILauncher (AppLauncher.m) in this process, which execs
  /// fable_2, so the app ends with the game. Returns only on failure.
  func play() {
    latestSave = Saves.latest()
    for item in playEnvironment {
      guard let eq = item.firstIndex(of: "=") else { continue }
      setenv(String(item[..<eq]), String(item[item.index(after: eq)...]), 1)
    }
    let launcher = Paths.macOS + "/FableIILauncher"
    appendLog(Paths.logs + "/player.log", "play: " + playEnvironment.joined(separator: " "))
    if lock >= 0 { close(lock); lock = -1 }  // the launcher takes the state folder's lock itself
    let argv = ([launcher] + gameArguments).map { strdup($0) } + [nil]
    execv(launcher, argv)
    message = "Could not start the game: \(String(cString: strerror(errno)))"
    NSApp.activate(ignoringOtherApps: true)
  }
}

// MARK: - Views

/// The window's top: the title on the standard window background (no artwork; native materials, light and dark).
struct Header: View {
  var subtitle = "for Mac"
  var body: some View {
    HStack(alignment: .firstTextBaseline, spacing: 8) {
      Text("Fable II").font(.system(size: 28, weight: .semibold, design: .serif))
      Text(subtitle).font(.callout).foregroundStyle(.secondary)
      Spacer()
    }
    .padding(.horizontal, 22).padding(.top, 14).padding(.bottom, 4)
    .frame(maxWidth: .infinity)
  }
}

struct PlayView: View {
  @ObservedObject var model: PlayerModel

  var body: some View {
    VStack(spacing: 0) {
      Header()
      Form {
        Section {
          if let save = model.latestSave {
            Picker("Start", selection: $model.startMode) {
              Text("Continue").tag(StartMode.continueSave)
              Text("Title screen").tag(StartMode.title)
            }
            .pickerStyle(.segmented)
            VStack(alignment: .leading, spacing: 2) {
              Text("Latest save: \(save.title)")
              Text(save.detail).font(.caption).foregroundStyle(.secondary)
              if model.startMode == .continueSave && save.count > 1 {
                Text("Continue loads this save. To pick another of your \(save.count) saves, start at the Title screen.")
                  .font(.caption).foregroundStyle(.secondary)
              }
            }
          } else {
            LabeledContent("Start", value: "New game (no saves yet)")
          }
        }
        Section {
          Picker("Resolution", selection: $model.settings.resolution) {
            ForEach(InternalResolution.allCases) { scale in
              Text(scale.title(frameRate: model.settings.frameRate, rule2x: model.settings.auto2x)).tag(scale)
            }
          }
          .help("The size the game renders at. Auto picks 1×, 2× or 3× from this Mac's GPU and memory.")
          Picker("Frame rate", selection: $model.settings.frameRate) {
            ForEach(FrameRate.allCases) { fps in Text(fps.title(resolution: model.settings.resolution, rule2x: model.settings.auto2x)).tag(fps) }
          }
          .help("Auto: 60 FPS when this Mac's GPU is estimated to hold it at the chosen resolution, uncapped at 2× when it "
                + "holds 45 FPS or more there, else 30 FPS. The original game runs at 30.")
          Picker("Display", selection: $model.settings.display) {
            ForEach(DisplayMode.allCases) { mode in Text(mode.title).tag(mode) }
          }
          .pickerStyle(.segmented)
        }
      }
      .formStyle(.grouped)
      .contentMargins(.top, 6, for: .scrollContent)
      .scrollDisabled(true)
      .fixedSize(horizontal: false, vertical: true)

      HStack(spacing: 12) {
        SettingsLink { Text("Settings…") }
          .controlSize(.large)
        if !model.message.isEmpty {
          Text(model.message).font(.caption).foregroundStyle(.red).lineLimit(2)
        } else if !model.gameNote.isEmpty {
          Text(model.gameNote).font(.caption).foregroundStyle(.red).lineLimit(2)
          Button("Show Logs") { NSWorkspace.shared.open(URL(fileURLWithPath: Paths.logs, isDirectory: true)) }
        }
        Spacer()
        Button { model.playFromLauncher() } label: {
          Text("Play").font(.title3.weight(.semibold)).frame(minWidth: 120)
        }
        .buttonStyle(.borderedProminent)
        .controlSize(.large)
        .keyboardShortcut(.defaultAction)
        .disabled(model.gameRunning)
      }
      .padding(.horizontal, 20)
      .padding(.top, 4)
      .padding(.bottom, 18)
      Text("Version \(Paths.version)").font(.caption2).foregroundStyle(.tertiary).padding(.bottom, 8)
    }
    .frame(width: 560)
    .onAppear { model.refresh() }
  }
}

struct WelcomeView: View {
  @ObservedObject var model: PlayerModel

  var body: some View {
    VStack(spacing: 0) {
      Header()
      VStack(alignment: .leading, spacing: 12) {
        Text("Welcome").font(.title2.weight(.semibold))
        if let candidate = model.candidates.first {
          Text("Fable II's game files are already on this Mac, from the \(candidate.appName) data "
               + "folder. They can be used here without copying them again, and take no extra space.")
            .fixedSize(horizontal: false, vertical: true)
          Text("Or choose your Fable II disc image (.iso) to copy the game files from it.")
            .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        } else {
          Text("To play, choose your own Fable II disc image (.iso). Its game files are copied once into this Mac's "
               + "Application Support folder (" + (Paths.buildsGame ? "about 8 GB of free space needed, with the game "
               + "built on this Mac" : "about 7 GB of free space needed") + "). The image itself is only read.")
            .fixedSize(horizontal: false, vertical: true)
        }
        if !model.message.isEmpty {
          Text(model.message).font(.callout).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
        }
        HStack {
          Button("Quit") { NSApp.terminate(nil) }
          Spacer()
          if let candidate = model.candidates.first {
            Button("Choose Disc Image…") { model.chooseImage() }
            Button("Use These Files") { model.reuse(candidate) }
              .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
          } else {
            Button("Choose Disc Image…") { model.chooseImage() }
              .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
          }
        }
        .controlSize(.large)
        .padding(.top, 6)
      }
      .padding(20)
    }
    .frame(width: 560)
  }
}

/// A plain progress bar drawn by SwiftUI (the AppKit one lagged far behind the copy, which runs at several GB/s
/// when the image is in the page cache).
struct CopyBar: View {
  let fraction: Double
  var body: some View {
    GeometryReader { proxy in
      ZStack(alignment: .leading) {
        Capsule().fill(Color.secondary.opacity(0.2))
        Capsule().fill(Color.accentColor).frame(width: max(6, proxy.size.width * min(max(fraction, 0), 1)))
      }
    }
    .frame(height: 6)
    .accessibilityElement()
    .accessibilityLabel("Copy progress")
    .accessibilityValue("\(Int(fraction * 100)) percent")
  }
}

struct PreparingView: View {
  @ObservedObject var model: PlayerModel
  @ObservedObject var prepare: PrepareRunner
  @ObservedObject var shaders: ShaderPrepRunner
  @State var showDetails = Snapshot.window == "progress"

  var body: some View {
    if shaders.phase == .running { shaderBody } else { filesBody }
  }

  /// Second step: the shader preparation (no game window; the game itself does not start).
  var shaderBody: some View {
    VStack(spacing: 0) {
      Header()
      VStack(alignment: .leading, spacing: 10) {
        Text("Preparing shaders…").font(.title3.weight(.semibold))
        CopyBar(fraction: shaders.fraction)
        Text(shaders.detail).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
        Text("Done once per version, so the game starts without stutter at every resolution.")
          .font(.caption).foregroundStyle(.secondary)
        HStack {
          Button("Show Log File") {
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: Paths.prepareLog)])
          }
          Spacer()
          Button("Skip") { shaders.skip() }.keyboardShortcut(.cancelAction)
        }
        .controlSize(.large)
      }
      .padding(20)
    }
    .frame(width: 560)
  }

  var filesBody: some View {
    VStack(spacing: 0) {
      Header()
      VStack(alignment: .leading, spacing: 10) {
        switch prepare.phase {
        case .failed(let error):
          Text("The game files could not be prepared").font(.title3.weight(.semibold))
          Text(error).foregroundStyle(.secondary).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
        default:
          Text("Preparing game files…").font(.title3.weight(.semibold))
          CopyBar(fraction: prepare.fraction)
          Text(prepare.detail).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
        }
        DisclosureGroup("Show details", isExpanded: $showDetails) {
          ScrollViewReader { proxy in
            ScrollView {
              Text(prepare.log.isEmpty ? "Waiting for the extractor…" : prepare.log)
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(6)
              Color.clear.frame(height: 1).id("end")
            }
            .frame(height: 150)
            .background(Color(nsColor: .textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .onChange(of: prepare.log) { proxy.scrollTo("end") }
          }
        }
        HStack {
          Button("Show Log File") {
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: Paths.prepareLog)])
          }
          Spacer()
          switch prepare.phase {
          case .failed:
            Button("Quit") { NSApp.terminate(nil) }
            Button("Choose Another Image…") { model.chooseImage() }.buttonStyle(.borderedProminent)
          default:
            Button("Cancel") { prepare.cancel() }.keyboardShortcut(.cancelAction)
          }
        }
        .controlSize(.large)
      }
      .padding(20)
    }
    .frame(width: 560)
    .onChange(of: prepare.phase) {
      if prepare.phase == .idle { model.screen = .welcome; model.refresh() }
    }
    .alert(Paths.mismatchBlocks ? "This disc image is a different version of Fable II"
           : "This disc image differs from the one this build was made from", isPresented: Binding(
      get: { if case .mismatch = prepare.phase { return true } else { return false } }, set: { _ in })) {
      if !Paths.mismatchBlocks {
        Button("Continue Anyway") { prepare.start(image: prepare.image ?? "", stopOnMismatch: false) }
      }
      Button("Choose Another Image…") { model.chooseImage() }
      Button("Quit", role: .cancel) { NSApp.terminate(nil) }
    } message: {
      if case .mismatch(let digest) = prepare.phase {
        if Paths.mismatchBlocks {
          Text("Its default.xex has SHA-256 \(digest), but this release translates the game code from the version with "
               + "SHA-256 \(Paths.expectedXex ?? "?"). The translation is specific to that exact file, so this image "
               + "cannot be used. See the README for the supported disc.")
        } else {
          Text("default.xex on this image has SHA-256 \(digest); this build was compiled from \(Paths.expectedXex ?? "?"). "
               + "The game may misbehave.")
        }
      }
    }
  }
}

/// First launch (release app): Apple's Command Line Tools, then the game is built on this Mac with progress, an
/// estimate, Cancel (the build folder is kept: Try Again continues) and the live log of the real commands.
struct BuildView: View {
  @ObservedObject var model: PlayerModel
  @ObservedObject var builder: BuildRunner
  @State var showDetails = Snapshot.window == "build"
  @State var copied = false

  var title: String {
    switch builder.phase {
    case .needsTools, .waitingForTools: return "Apple's Command Line Tools are needed"
    case .failed: return "The game could not be built"
    case .cancelled: return "Building stopped"
    default: return GameBuild.isUpdate ? "Updating Fable II for this version…" : "Building Fable II for this Mac…"
    }
  }

  var timeText: String {
    if let eta = builder.eta { return eta >= 90 ? "About \(Int((Double(eta) / 60).rounded())) min left" : "Less than 2 min left" }
    return GameBuild.isUpdate ? "Usually under a minute for an update" : "About \(GameBuild.estimateMinutes) min on this Mac (first time only)"
  }

  var body: some View {
    VStack(spacing: 0) {
      Header()
      VStack(alignment: .leading, spacing: 10) {
        Text(title).font(.title3.weight(.semibold))
        switch builder.phase {
        case .needsTools, .waitingForTools:
          Text("Fable II is translated from your disc and compiled on this Mac once. That needs Apple's free Command "
               + "Line Tools (not Xcode). macOS shows its own installer; this window continues by itself when they are "
               + "installed.")
            .fixedSize(horizontal: false, vertical: true)
          if builder.phase == .waitingForTools {
            HStack(spacing: 8) { ProgressView().controlSize(.small); Text(builder.detail).font(.callout).foregroundStyle(.secondary) }
          }
        case .failed(let error):
          Text(error).foregroundStyle(.secondary).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
        case .cancelled:
          Text("What was built so far is kept. Try Again continues from there.").foregroundStyle(.secondary)
        default:
          CopyBar(fraction: builder.fraction)
          HStack {
            Text(builder.detail).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            Spacer()
            Text(timeText).font(.caption).foregroundStyle(.secondary)
          }
        }
        VStack(alignment: .leading, spacing: 3) {
          ForEach(BuildRunner.steps) { step in
            HStack(spacing: 6) {
              Group {
                if builder.finished.contains(step.key) { Image(systemName: "checkmark.circle.fill").foregroundStyle(.green) }
                else if builder.current == step.key && builder.phase == .running { ProgressView().controlSize(.mini) }
                else { Image(systemName: "circle").foregroundStyle(.tertiary) }
              }
              .frame(width: 16)
              Text(step.title).font(.callout)
              Spacer()
              if let s = builder.seconds[step.key], builder.finished.contains(step.key), step.key != "tools" {
                Text(s < 60 ? String(format: "%.0f s", s) : String(format: "%d:%02d", Int(s) / 60, Int(s) % 60))
                  .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
              }
            }
          }
        }
        .padding(.vertical, 2)
        DisclosureGroup("Show details", isExpanded: $showDetails) {
          ScrollViewReader { proxy in
            ScrollView {
              Text(builder.log.isEmpty ? "The commands and their output appear here." : builder.log)
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(6)
              Color.clear.frame(height: 1).id("end")
            }
            .frame(height: 170)
            .background(Color(nsColor: .textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .onChange(of: builder.log) { proxy.scrollTo("end") }
          }
        }
        HStack {
          Button("Show Log File") {
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: GameBuild.logPath)])
          }
          if case .failed = builder.phase {
            Button(copied ? "Copied" : "Copy Details") {
              NSPasteboard.general.clearContents()
              NSPasteboard.general.setString(builder.report, forType: .string)
              copied = true
            }
          }
          Spacer()
          switch builder.phase {
          case .needsTools:
            Button("Quit") { NSApp.terminate(nil) }
            Button("Install Command Line Tools…") { builder.installTools() }
              .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
          case .waitingForTools:
            Button("Check Again") { builder.checkToolsAgain() }
            Button("Cancel") { builder.cancel() }.keyboardShortcut(.cancelAction)
          case .failed, .cancelled:
            Button("Quit") { NSApp.terminate(nil) }
            Button("Try Again") { builder.start() }.buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
          default:
            Button("Cancel") { builder.cancel() }.keyboardShortcut(.cancelAction)
          }
        }
        .controlSize(.large)
      }
      .padding(20)
    }
    .frame(width: 560)
    .onAppear { if builder.phase == .idle { model.filesReady() } }
  }
}

struct MainView: View {
  @ObservedObject var model: PlayerModel
  @Environment(\.openSettings) private var openSettings
  @Environment(\.openWindow) private var openWindow
  var body: some View {
    Group {
      switch model.screen {
      case .welcome: WelcomeView(model: model)
      case .preparing: PreparingView(model: model, prepare: model.prepare, shaders: model.shaderPrep)
      case .building: BuildView(model: model, builder: model.builder)
      case .play: PlayView(model: model)
      }
    }
    .sheet(isPresented: $model.showStartFreshMain) { StartFreshSheet(model: model) }
    .onAppear {
      Startup.run(model: model)
      Snapshot.start(model: model, openWindow: { openWindow(id: $0) }) { openSettings() }
    }
  }
}

/// Command-line test aids, run once when the main window first appears (same code paths as the buttons).
enum Startup {
  static var done = false
  static func run(model: PlayerModel) {
    guard !done else { return }
    done = true
    let playWhenReady = commandLine.contains("--play")
    if playWhenReady {
      let previousShaders = model.shaderPrep.onDone
      model.shaderPrep.onDone = { previousShaders?(); model.play() }
      // After the files (and, in the release app, the build): play unless the build or the shader prep still runs
      // (their onDone continues the chain).
      let playIfReady = {
        if model.screen != .building && model.shaderPrep.phase != .running && GameBuild.ready { model.play() }
      }
      let previous = model.prepare.onDone
      model.prepare.onDone = { previous?(); playIfReady() }
      let previousBuild = model.builder.onDone
      model.builder.onDone = { previousBuild?(); playIfReady() }
    }
    // A new build or shader seed with game files already in place: prepare its shaders before Play.
    if model.screen == .play && ShaderPrepRunner.needed { model.filesReady() }
    if commandLine.contains("--press-play"), model.screen == .play, model.shaderPrep.phase != .running {
      DispatchQueue.main.async { model.playFromLauncher() }
      return
    }
    if commandLine.contains("--reuse"), model.screen == .welcome, let candidate = model.candidates.first {
      model.reuse(candidate)
      if playWhenReady && model.screen == .play { model.play() }
    } else if let iso = argument(after: "--iso"), model.screen == .welcome {
      model.chooseImage(iso)
    }
  }
}

// MARK: - Settings window

struct PlayerSettingsView: View {
  @ObservedObject var model: PlayerModel
  @State private var tab = Snapshot.settingsTab?.rawValue ?? SettingsTab.display.rawValue
  @State private var confirmReset = false

  var body: some View {
    TabView(selection: $tab) {
      ForEach(SettingsTab.allCases) { item in
        SettingsTabView(model: model, tab: item, variant: model.developerMode ? .dev : .player, extra: extra)
          .tabItem { Label(item.playerTitle, systemImage: item.playerSymbol) }
          .tag(item.rawValue)
      }
    }
    .frame(width: 580)
    .sheet(isPresented: $model.showStartFresh) { StartFreshSheet(model: model) }
    .sheet(isPresented: $model.showArchives) { ArchivesSheet(model: model) }
    .confirmationDialog("Reset every setting to its default?", isPresented: $confirmReset) {
      Button("Reset All Settings", role: .destructive) { model.resetAll() }
    } message: {
      Text("Your saves and game files are not touched.")
    }
  }

  private func extra(_ tab: SettingsTab) -> AnyView {
    switch tab {
    case .general:
      return AnyView(Group {
        Section {
          Toggle(isOn: $model.skipLauncher) {
            VStack(alignment: .leading, spacing: 2) {
              Text("Skip launcher, play directly")
              Text("Opening Fable II starts the game with these settings. Hold Option while opening it to see "
                   + "the launcher again.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
          }
        } header: { Text("Launcher") }
        Section {
          LabeledContent("Folder") {
            Text(GameFiles.root).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
              .textSelection(.enabled)
          }
          HStack {
            Button("Show in Finder") {
              NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: GameFiles.root)])
            }
            Spacer()
            Button("Choose Disc Image…") { model.chooseImage() }
              .help("Copy the game files again from another disc image.")
          }
        } header: { Text("Game files") }
      })
    case .diagnostics:
      return AnyView(Section {
        HStack {
          Button("Open Logs Folder") { NSWorkspace.shared.open(URL(fileURLWithPath: Paths.logs, isDirectory: true)) }
          Button("Open Data Folder") { NSWorkspace.shared.open(URL(fileURLWithPath: Paths.state, isDirectory: true)) }
            .help("Saves, settings, shader cache and logs.")
          Spacer()
          Button("Reset All Settings…") { confirmReset = true }
            .disabled(model.isDefault)
        }
        DeveloperSection(developerMode: $model.developerMode)
        HStack {
          VStack(alignment: .leading, spacing: 2) {
            Text("Start fresh")
            Text("Moves this app's game files, build, caches, logs and settings into an archive, so the next launch is a "
                 + "first launch again. Nothing is deleted.")
              .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
          }
          Spacer()
          Button("Start Fresh…") { model.showStartFresh = true }
          Button("Archives…") { model.showArchives = true }
        }
      } header: { Text("Logs and data") })
    default:
      return AnyView(EmptyView())
    }
  }
}

// MARK: - App

/// --snapshot PATH [--window ...]: saves one window of this app to a PNG and quits (screenshots for review).
enum Snapshot {
  static let path = argument(after: "--snapshot")
  static let window = argument(after: "--window") ?? "play"
  static var settingsTab: SettingsTab? {
    guard path != nil else { return nil }
    return window == "advanced" ? .diagnostics : SettingsTab(rawValue: window)
  }

  @MainActor static func start(model: PlayerModel, openWindow: @escaping (String) -> Void = { _ in },
                               openSettings: @escaping () -> Void) {
    guard let path = path else { return }
    if let tab = settingsTab {
      model.screen = .play
      openSettings()
      capture(after: 2.0, path: path) { NSApp.windows.first { $0.isVisible && $0.title == tab.playerTitle } }
      return
    }
    if window == "devlauncher" {  // developer mode: the Dev Launcher's window in this app
      DevHost.shared.ensure()
      openWindow("devlauncher")
      capture(after: 3.0, path: path) { NSApp.windows.first { $0.isVisible && $0.title == "Fable II Dev Launcher" } }
      return
    }
    switch window {
    case "welcome": model.screen = .welcome; model.refresh()
    case "progress":
      // Started by --iso; captured once the bar has moved (or after 20 s).
      waitForProgress(path: path, model: model, deadline: Date().addingTimeInterval(20))
      return
    default: model.screen = .play; model.refresh()
    }
    capture(after: 1.5, path: path) { NSApp.windows.first { $0.isVisible && $0.title == "Fable II" } }
  }

  @MainActor static func waitForProgress(path: String, model: PlayerModel, deadline: Date) {
    if model.prepare.fraction > 0.15 || Date() > deadline {
      capture(after: 0.25, path: path) { NSApp.windows.first { $0.isVisible && $0.title == "Fable II" } }
      return
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { waitForProgress(path: path, model: model, deadline: deadline) }
  }

  static func capture(after delay: Double, path: String, find: @escaping () -> NSWindow?) {
    DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
      guard let window = find() else { print("snapshot: window not found"); exit(1) }
      guard WindowSnapshot.write(window, to: path) else { print("snapshot: could not capture the window"); exit(1) }
      print(path)
      exit(0)
    }
  }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
  func applicationWillFinishLaunching(_ notification: Notification) {
    // Snapshots and background test sessions never take focus.
    if Snapshot.path != nil || Paths.environment["FABLE2_TEST_BACKGROUND"] == "1" { NSApp.setActivationPolicy(.accessory) }
  }
  func applicationDidFinishLaunching(_ notification: Notification) {
    if Snapshot.path == nil && Paths.environment["FABLE2_TEST_BACKGROUND"] != "1" { NSApp.activate(ignoringOtherApps: true) }
  }
  // Hiding the windows for the game counts as closing the last one: stay while the game runs.
  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    PlayerApp.model?.gameRunning != true
  }
  func applicationWillTerminate(_ notification: Notification) {
    if PlayerApp.model?.gameRunning == true { appendLog(Paths.logs + "/player.log", "launcher quit while the game runs") }
    PlayerApp.model?.prepare.cancel()
  }
}

struct PlayerCommands: Commands {
  @ObservedObject var model: PlayerModel
  @ObservedObject var dev = DevHost.shared
  @Environment(\.openWindow) private var openWindow
  var body: some Commands {
    CommandGroup(after: .windowArrangement) {
      if model.developerMode {
        Divider()
        Button("Fable II Dev Launcher") { DevHost.shared.ensure(); openWindow(id: "devlauncher") }
          .keyboardShortcut("d", modifiers: [.command, .option])
        if let devModel = dev.model {
          Menu("Developer") { DevToolItems(model: devModel, openWindow: { openWindow(id: "devlauncher") }) }
        } else {
          Button("Developer tools need a source checkout…") { _ = DevCheckout.choose(); DevHost.shared.ensure() }
        }
      }
    }
    CommandGroup(replacing: .newItem) {
      Button("Choose Disc Image…") { model.chooseImage() }
        .disabled(model.screen == .preparing)
      Divider()
      Button("Open Logs Folder") { NSWorkspace.shared.open(URL(fileURLWithPath: Paths.logs, isDirectory: true)) }
      Button("Open Data Folder") { NSWorkspace.shared.open(URL(fileURLWithPath: Paths.state, isDirectory: true)) }
      Divider()
      Button("Start Fresh…") { model.showStartFreshMain = true }
        .disabled(model.gameRunning || model.screen == .preparing || model.screen == .building)
    }
  }
}

struct PlayerApp: App {
  static var model: PlayerModel?
  @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
  @StateObject private var model: PlayerModel = {
    let model = PlayerModel()
    model.lock = PlayerApp.heldLock
    PlayerApp.model = model
    return model
  }()
  static var heldLock: Int32 = -1

  var body: some Scene {
    Window("Fable II", id: "main") { MainView(model: model) }
      .windowResizability(.contentSize)
      .commands { PlayerCommands(model: model) }
    Settings { PlayerSettingsView(model: model) }
    // Developer mode: the Dev Launcher's main window (opened from the Window menu).
    Window("Fable II Dev Launcher", id: "devlauncher") { DevLauncherWindowHost() }
      .windowResizability(.contentSize)
  }
}

// MARK: - Start Fresh (tools/macos/Shared/StartFresh.swift)

enum Fresh {
  static var bundleID: String { Bundle.main.bundleIdentifier ?? "" }

  /// Start Fresh needs the state folder to itself: refused while a game (or another copy) holds app.lock.
  static func lockFree(heldByUs: Bool) -> Bool {
    if heldByUs { return true }
    let fd = open(Paths.state + "/app.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
    guard fd >= 0 else { return false }
    defer { close(fd) }
    return flock(fd, LOCK_EX | LOCK_NB) == 0
  }

  /// Starts this app again (same executable, arguments and environment, minus one-shot flags) and exits, so the
  /// next window is the first-launch welcome. exit() rather than terminate: no window state is written back.
  static func relaunch() -> Never {
    if PlayerApp.heldLock >= 0 { close(PlayerApp.heldLock); PlayerApp.heldLock = -1 }
    if let model = PlayerApp.model, model.lock >= 0 { close(model.lock); model.lock = -1 }
    let oneShot: Set<String> = ["--play", "--press-play", "--reuse", "--start-fresh", "--archive-saves", "--relaunched"]
    var args: [String] = []
    var i = 0
    while i < commandLine.count {
      let a = commandLine[i]
      if ["--iso", "--restore-archive", "--delete-archive"].contains(a) { i += 2; continue }
      if !oneShot.contains(a) { args.append(a) }
      i += 1
    }
    let process = Process()
    process.executableURL = Bundle.main.executableURL
    process.arguments = args + ["--relaunched"]
    try? process.run()
    exit(0)
  }
}

/// --start-fresh [--archive-saves] | --list-archives | --restore-archive NAME | --delete-archive NAME: the same
/// operations as the sheets, printed as JSON (tests; state folder from FABLE2_APP_STATE_ROOT). nil: not a CLI call.
enum FreshCLI {
  static func run() -> Int32? {
    func emit(_ object: [String: Any]) -> Int32 {
      let data = (try? JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys])) ?? Data()
      print(String(decoding: data, as: UTF8.self))
      return object["ok"] as? Bool == true ? 0 : 1
    }
    func listing() -> [[String: Any]] {
      StartFresh.archives(stateRoot: Paths.state).map {
        ["name": $0.name, "path": $0.path, "keptSaves": $0.keptSaves, "reason": $0.reason, "bytes": $0.bytes]
      }
    }
    let wantsWrite = commandLine.contains("--start-fresh") || argument(after: "--restore-archive") != nil
      || argument(after: "--delete-archive") != nil
    if commandLine.contains("--list-archives") { return emit(["ok": true, "archives": listing()]) }
    guard wantsWrite else { return nil }
    guard Fresh.lockFree(heldByUs: false) else { return emit(["ok": false, "error": StartFresh.Failure.running.description]) }
    do {
      if commandLine.contains("--start-fresh") {
        let keep = !commandLine.contains("--archive-saves")
        let items = StartFresh.plan(stateRoot: Paths.state, bundleID: Fresh.bundleID, keepSaves: keep)
        let folder = try StartFresh.archive(stateRoot: Paths.state, bundleID: Fresh.bundleID, keepSaves: keep,
                                            reason: "Start Fresh")
        return emit(["ok": true, "archive": folder, "moved": items.map { $0.original }, "keptSaves": keep])
      }
      if let name = argument(after: "--restore-archive") {
        guard let archive = StartFresh.archives(stateRoot: Paths.state).first(where: { $0.name == name }) else {
          return emit(["ok": false, "error": "no archive named \(name)"])
        }
        let current = try StartFresh.restore(archive, stateRoot: Paths.state, bundleID: Fresh.bundleID)
        return emit(["ok": true, "restored": name, "previousStateArchive": current])
      }
      if let name = argument(after: "--delete-archive") {
        guard let archive = StartFresh.archives(stateRoot: Paths.state).first(where: { $0.name == name }) else {
          return emit(["ok": false, "error": "no archive named \(name)"])
        }
        try StartFresh.delete(archive, stateRoot: Paths.state)
        return emit(["ok": true, "deleted": name])
      }
    } catch {
      return emit(["ok": false, "error": "\(error)"])
    }
    return nil
  }
}

struct StartFreshSheet: View {
  @ObservedObject var model: PlayerModel
  @Environment(\.dismiss) private var dismiss
  @State private var keepSaves = true
  @State private var error = ""

  var items: [StartFresh.Item] {
    StartFresh.plan(stateRoot: Paths.state, bundleID: Fresh.bundleID, keepSaves: keepSaves)
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Start fresh?").font(.title3.weight(.semibold))
      Text("These move into a new archive in \(Paths.state)/Archive. Nothing is deleted; Archives… can move them back. "
           + "Fable II then opens as on its first launch.")
        .fixedSize(horizontal: false, vertical: true)
      Toggle("Keep my saves", isOn: $keepSaves)
        .help("On: your saves stay where they are. Off: they go into the archive too.")
      ScrollView {
        VStack(alignment: .leading, spacing: 4) {
          ForEach(items, id: \.original) { item in
            VStack(alignment: .leading, spacing: 0) {
              Text(item.label).font(.callout)
              Text(item.original).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            }
          }
          if items.isEmpty { Text("Nothing to move: this app has no files yet.").foregroundStyle(.secondary) }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
      }
      .frame(height: 160)
      VStack(alignment: .leading, spacing: 2) {
        Text("Not reset by this:").font(.caption.weight(.semibold))
        ForEach(StartFresh.notReset, id: \.self) { Text("• " + $0).font(.caption).foregroundStyle(.secondary) }
      }
      if !error.isEmpty { Text(error).font(.callout).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true) }
      HStack {
        Spacer()
        Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
        Button("Start Fresh") { startFresh() }
          .buttonStyle(.borderedProminent)
          .disabled(model.gameRunning || items.isEmpty)
      }
    }
    .padding(20)
    .frame(width: 520)
  }

  func startFresh() {
    guard !model.gameRunning, Fresh.lockFree(heldByUs: model.lock >= 0 || PlayerApp.heldLock >= 0) else {
      error = StartFresh.Failure.running.description
      return
    }
    model.prepare.cancel()
    model.builder.cancel()
    do {
      let folder = try StartFresh.archive(stateRoot: Paths.state, bundleID: Fresh.bundleID, keepSaves: keepSaves,
                                          reason: "Start Fresh")
      appendLog(folder + "/start-fresh.log", "Start Fresh by the app (keep saves: \(keepSaves))")
      Fresh.relaunch()
    } catch {
      self.error = "\(error)"
    }
  }
}

struct ArchivesSheet: View {
  @ObservedObject var model: PlayerModel
  @Environment(\.dismiss) private var dismiss
  @State private var archives = StartFresh.archives(stateRoot: Paths.state)
  @State private var selection: String?
  @State private var confirmRestore = false
  @State private var confirmDelete = false
  @State private var error = ""

  var selected: StartFresh.Archive? { archives.first { $0.name == selection } }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Archives").font(.title3.weight(.semibold))
      Text("Restore moves an archive back. What is here now is archived first, so nothing is lost.")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      List(archives, selection: $selection) { archive in
        VStack(alignment: .leading, spacing: 1) {
          Text(archive.name.replacingOccurrences(of: "_", with: " "))
          Text("\(archive.reason) · \(archive.keptSaves ? "saves kept in place" : "saves archived") · "
               + ByteCountFormatter.string(fromByteCount: archive.bytes, countStyle: .file))
            .font(.caption).foregroundStyle(.secondary)
        }
        .tag(archive.name)
      }
      .frame(height: 180)
      if !error.isEmpty { Text(error).font(.callout).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true) }
      HStack {
        Button("Delete Archive…") { confirmDelete = true }.disabled(selected == nil)
        Button("Show in Finder") {
          NSWorkspace.shared.open(URL(fileURLWithPath: Paths.state + "/" + StartFresh.archiveFolderName, isDirectory: true))
        }
        Spacer()
        Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
        Button("Restore…") { confirmRestore = true }.buttonStyle(.borderedProminent).disabled(selected == nil || model.gameRunning)
      }
    }
    .padding(20)
    .frame(width: 520)
    .confirmationDialog("Restore \(selected?.name ?? "")?", isPresented: $confirmRestore) {
      Button("Restore and Relaunch") { restore() }
    } message: {
      Text("The current game files, build, caches and settings are archived first. Fable II then relaunches.")
    }
    .confirmationDialog("Delete \(selected?.name ?? "") permanently?", isPresented: $confirmDelete) {
      Button("Delete Archive", role: .destructive) { delete() }
    } message: {
      Text("This removes the archive's files from this Mac. It cannot be undone.")
    }
  }

  func restore() {
    guard let archive = selected else { return }
    guard !model.gameRunning, Fresh.lockFree(heldByUs: model.lock >= 0 || PlayerApp.heldLock >= 0) else {
      error = StartFresh.Failure.running.description
      return
    }
    do {
      try StartFresh.restore(archive, stateRoot: Paths.state, bundleID: Fresh.bundleID)
      Fresh.relaunch()
    } catch { self.error = "\(error)" }
  }

  func delete() {
    guard let archive = selected else { return }
    do {
      try StartFresh.delete(archive, stateRoot: Paths.state)
      archives = StartFresh.archives(stateRoot: Paths.state)
      selection = nil
    } catch { self.error = "\(error)" }
  }
}

// MARK: - Entry point

func fail(_ text: String) -> Never {
  let app = NSApplication.shared
  app.setActivationPolicy(.regular)
  app.activate(ignoringOtherApps: true)
  let alert = NSAlert()
  alert.messageText = "Fable II could not start"
  alert.informativeText = text
  alert.runModal()
  exit(1)
}

/// The state folder's lock (app.lock, as AppLauncher.m): one copy of the game or its launcher per folder.
/// Close-on-exec, so FableIILauncher takes it again for the game.
func takeLock() -> Int32 {
  try? FileManager.default.createDirectory(atPath: Paths.state, withIntermediateDirectories: true)
  let fd = open(Paths.state + "/app.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
  guard fd >= 0, flock(fd, LOCK_EX | LOCK_NB) == 0 else {
    fail("Fable II is already running (another copy is using its data folder).")
  }
  return fd
}

// One app, two roles (docs/player-app.md "Developer mode"): the Dev Launcher build runs its role; both roles take
// the Dev Launcher's command-line modes (--launch, --picker-rows).
if let code = DevLauncherMain.cli() { exit(code) }
if commandLine.contains("--list-capabilities") {
  // The player role: its own windows, plus the Dev Launcher's in developer mode (with a checkout for the tools).
  var caps: [String]
  if AppRole.devLauncher {
    caps = Capabilities.devLauncher()
  } else {
    let store = PlistSettingsStore(path: Paths.settingsPlist)
    DevCheckout.store = store
    let developer = store.object(forKey: "developerMode") as? Bool ?? AppRole.developerModeDefault
    caps = ["window:player", "cli:launch", "cli:print-args", "cli:picker-rows", "settings:developer", "start-fresh"]
      + SettingsTab.allCases.map { "settings:" + $0.rawValue } + LaunchSettings.flags.map { "flag:" + $0.key }
    if developer {
      caps.append("window:dev-launcher")
      if DevCheckout.path != nil { caps += DevTools.items.map { "tool:" + $0.id } }
    }
  }
  let data = (try? JSONSerialization.data(withJSONObject: ["role": AppRole.devLauncher ? "dev-launcher" : "player",
                                                           "checkout": (DevCheckout.path as Any?) ?? NSNull(),
                                                           "capabilities": caps.sorted()])) ?? Data()
  print(String(decoding: data, as: UTF8.self))
  exit(0)
}
if AppRole.devLauncher { DevLauncherMain.run() }
if commandLine.contains("--print-env") {
  let model = PlayerModel()
  let data = (try? JSONSerialization.data(withJSONObject: model.playEnvironment)) ?? Data()
  print(String(decoding: data, as: UTF8.self))
  exit(0)
}
if !Paths.state.hasPrefix("/") { fail("The application data folder must be an absolute path.") }
if let code = FreshCLI.run() { exit(code) }
if Snapshot.path == nil { PlayerApp.heldLock = takeLock() }
// Play at once: --play, or "Skip launcher" unless Option is held (or --show-launcher).
if Snapshot.path == nil, GameFiles.ready, GameBuild.ready {
  let optionHeld = NSEvent.modifierFlags.contains(.option) || commandLine.contains("--show-launcher")
  let store = PlistSettingsStore(path: Paths.settingsPlist)
  // Not while this build's shaders still need preparing: the window does that first (then plays with --play).
  if !ShaderPrepRunner.needed,
     commandLine.contains("--play") || (store.object(forKey: "skipLauncher") as? Bool == true && !optionHeld) {
    let model = PlayerModel()
    model.lock = PlayerApp.heldLock
    model.play()
    fail(model.message)
  }
}
PlayerApp.main()
