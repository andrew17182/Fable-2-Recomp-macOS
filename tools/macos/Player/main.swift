// Fable II player app: the portable "Fable II.app"'s main executable (tools/fable2_portable.py,
// docs/player-app.md). One app, standard macOS windows, no Terminal:
//   - first launch: a welcome window that reuses a sibling app's finished game files (APFS clone, as
//     AppLauncher.m did) or asks for the disc image and runs FableIIPrepare with a progress bar, Cancel
//     and a "Show details" log (the real prepare output, also appended to logs/prepare.log);
//   - release app (docs/release-app.md): after the game files, the game is built on this Mac (BuildRunner runs
//     the kit's tools/fable2_game_build.py; Command Line Tools install, progress, cancel, Copy Details);
//   - every launch: the Play window (2026-10-19: Start at Title Screen and Load Saved Game, the save menu newest first;
//     resolution and frame rate; Skip intro; display);
//   - Settings: the second page of the same window (Settings / Launcher button, ⌘,; Shared/LauncherPages.swift),
//     the shared tabs from tools/macos/Shared/*.swift, stored in <state>/player-settings.plist. General also has
//     the launcher, game files, logs and data (Start Fresh, Archives: Shared/StartFresh.swift); Performance, Fixes
//     and Diagnostics only with Dev Options on;
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
//   --press-play         select the newest save and press Load Saved Game (child game; the launcher returns when it exits)
//   --print-env          print the environment items Play would set (JSON) and exit
//   --preset original|enhanced|custom   with --print-env, --play or --snapshot: that preset for this run (not stored)
//   --iso IMAGE          on the welcome window, extract IMAGE at once (as if chosen in the file panel)
//   --reuse              on the welcome window, reuse the offered sibling game files at once
//   --show-launcher      as holding Option
//   --snapshot PATH.png [--window play|ready|welcome|progress|denied|general|display|controls|mods|performance|
//                        fixes|diagnostics|advanced|devlauncher] [--dev-options on|off]
//                        show that window without taking focus, save it to a PNG and quit (progress: once
//                        the extraction has started, with details shown; denied: once a refused access is shown)
//   --access-check PATH, --bundle-location [APP]: the access helpers as JSON (tests)
import AppKit
import Darwin
import SwiftUI
import UniformTypeIdentifiers

// MARK: - Paths and game files

/// Flags this app consumes; the rest are passed to the game.
let playerFlags: Set<String> = ["--play", "--press-play", "--print-env", "--reuse", "--show-launcher", "--start-fresh",
                                 "--archive-saves", "--list-archives", "--relaunched"]
let playerValueFlags: Set<String> = ["--iso", "--snapshot", "--window", "--restore-archive", "--delete-archive", "--preset"]
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
  let hero: String  // folder name, HeroNNN: Load Saved Game loads this one (FABLE2_BOOT_SAVE_HERO)
  let detail: String
  let count: Int
  var label = ""    // the save menu: "Name · Place · date"
}

/// Import saves (release app, 2026-10-15): the release app has its own state folder, so it starts without the saves
/// of a portable or dev install ("~/Library/Application Support/Fable II Recomp/saves"). Copies (never moves) hero
/// saves into this app's saves/; an existing hero save is only replaced when asked (the old one is backed up first).
enum SaveImport {
  /// Saves folders of other installs on this Mac (FABLE2_IMPORT_SAVES_FROM overrides, for tests).
  static func otherInstalls() -> [String] {
    if let path = Paths.environment["FABLE2_IMPORT_SAVES_FROM"], !path.isEmpty {
      return heroes(in: savesFolder(path)).isEmpty ? [] : [savesFolder(path)]
    }
    let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!.path
    let own = (Paths.state as NSString).standardizingPath
    return ["Fable II Recomp"].map { support + "/" + $0 }
      .filter { ($0 as NSString).standardizingPath != own }
      .map { $0 + "/saves" }
      .filter { !heroes(in: $0).isEmpty }
  }

  /// A state folder or a saves folder -> the saves folder.
  static func savesFolder(_ path: String) -> String {
    FileManager.default.fileExists(atPath: path + "/saves") ? path + "/saves" : path
  }

  /// Hero saves under a saves folder: "<profile>/4D5307F1/00000001/HeroNNN" (relative paths), with a header.
  static func heroes(in saves: String) -> [String] {
    let fm = FileManager.default
    var out: [String] = []
    for profile in ((try? fm.contentsOfDirectory(atPath: saves)) ?? []).sorted() {
      let container = profile + "/4D5307F1"
      for hero in ((try? fm.contentsOfDirectory(atPath: saves + "/" + container + "/00000001")) ?? []).sorted()
      where hero.hasPrefix("Hero") && fm.fileExists(atPath: saves + "/" + container + "/Headers/00000001/" + hero + ".header") {
        out.append(container + "/00000001/" + hero)
      }
    }
    return out
  }

  static func header(of hero: String) -> String {
    let parts = hero.components(separatedBy: "/")
    return parts[0] + "/4D5307F1/Headers/00000001/" + parts[3] + ".header"
  }

  /// Hero saves of `source` that this app already has.
  static func conflicts(from source: String, to dest: String = Paths.state + "/saves") -> [String] {
    heroes(in: savesFolder(source)).filter { FileManager.default.fileExists(atPath: dest + "/" + $0) }
  }

  struct Result { var imported: [String] = []; var replaced: [String] = []; var skipped: [String] = []; var backup: String? }

  /// Copies every hero save (folder + header) of `source`, plus the profile data this app does not have yet.
  /// Existing hero saves: kept unless `replace`, then moved to saves-backup-<stamp> first.
  static func run(from source: String, replace: Bool, to dest: String = Paths.state + "/saves") throws -> Result {
    let fm = FileManager.default
    let saves = savesFolder(source)
    var result = Result()
    let stamp: String = {
      let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd_HH-mm-ss"; return f.string(from: Date())
    }()
    let backup = (dest as NSString).deletingLastPathComponent + "/saves-backup-" + stamp
    func copy(_ rel: String) throws {
      let target = dest + "/" + rel
      try fm.createDirectory(atPath: (target as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
      try fm.copyItem(atPath: saves + "/" + rel, toPath: target)
    }
    for hero in heroes(in: saves) {
      let exists = fm.fileExists(atPath: dest + "/" + hero)
      if exists && !replace { result.skipped.append(hero); continue }
      if exists {
        for rel in [hero, header(of: hero)] where fm.fileExists(atPath: dest + "/" + rel) {
          let moved = backup + "/" + rel
          try fm.createDirectory(atPath: (moved as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
          try fm.moveItem(atPath: dest + "/" + rel, toPath: moved)
        }
        result.backup = backup
      }
      try copy(hero)
      if fm.fileExists(atPath: dest + "/" + header(of: hero)) { try fm.removeItem(atPath: dest + "/" + header(of: hero)) }
      try copy(header(of: hero))
      if exists { result.replaced.append(hero) } else { result.imported.append(hero) }
    }
    // Profile data (options, achievements) only when this app has none for that profile yet.
    for profile in (try? fm.contentsOfDirectory(atPath: saves)) ?? [] {
      let rel = profile + "/4D5307F1/profile"
      if fm.fileExists(atPath: saves + "/" + rel) && !fm.fileExists(atPath: dest + "/" + rel) { try copy(rel) }
    }
    appendLog(Paths.logs + "/player.log", "import saves from \(saves): imported \(result.imported), replaced "
              + "\(result.replaced), kept \(result.skipped)")
    return result
  }

  static func name(_ hero: String) -> String { hero.components(separatedBy: "/").last ?? hero }
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

  /// Every hero save in the state folder (saves/<profile>/4D5307F1/00000001/HeroNNN), newest first; `count` is the
  /// number of saves.
  static func all() -> [SaveSummary] {
    let fm = FileManager.default
    let root = Paths.state + "/saves"
    var found: [(date: Date, hero: String, container: String)] = []
    for profile in (try? fm.contentsOfDirectory(atPath: root)) ?? [] {
      let container = root + "/" + profile + "/4D5307F1"
      for hero in (try? fm.contentsOfDirectory(atPath: container + "/00000001")) ?? [] where hero.hasPrefix("Hero") {
        guard let date = (try? fm.attributesOfItem(atPath: container + "/00000001/" + hero + "/mainsave.bin"))?[.modificationDate]
                as? Date else { continue }
        found.append((date, hero, container))
      }
    }
    found.sort { $0.date > $1.date }
    let formatter = DateFormatter()
    formatter.dateStyle = .medium
    formatter.timeStyle = .short
    formatter.doesRelativeDateFormatting = true
    return found.map { save in
      let folder = save.container + "/00000001/" + save.hero
      var title = save.hero
      if let header = fm.contents(atPath: save.container + "/Headers/00000001/" + save.hero + ".header"), header.count >= 0x108 {
        let name = String(data: header[8..<0x108], encoding: .utf16BigEndian)?.components(separatedBy: "\0").first ?? ""
        if !name.isEmpty { title = name }
      }
      let quest = field(folder + "/chaptersave.bin", "QuestName").map(words)
      // "Caves\\Bower Lake\\Thags Cave" -> "Thags Cave"
      let level = field(folder + "/chaptersave.bin", "LevelName").map { words($0.components(separatedBy: "\\").last ?? $0) }
      let when = formatter.string(from: save.date)
      let detail = [quest, level, when].compactMap { $0 }.joined(separator: " · ")
      let label = [title, level, when].compactMap { $0 }.joined(separator: " · ")
      return SaveSummary(title: title, hero: save.hero, detail: detail, count: found.count, label: label)
    }
  }

  /// The newest hero save and how many there are.
  static func latest() -> SaveSummary? { all().first }
}

// MARK: - Helper processes and access tests

/// A helper process with its output streamed to the main queue. Unlike Process, it can hand one open file to the
/// child as descriptor 3 (the disc image, opened by this app: docs/release-app.md "Protected folders").
final class ChildProcess {
  let pid: pid_t
  private(set) var running = true
  private init(pid: pid_t) { self.pid = pid }

  struct SpawnError: Error, CustomStringConvertible {
    let code: Int32
    var description: String { String(cString: strerror(code)) }
  }

  /// `output` gets text as it arrives; `ended` the exit status (-1 after a signal), after the last output.
  static func spawn(_ path: String, _ arguments: [String], inheriting fd: Int32? = nil,
                    output: @escaping (String) -> Void, ended: @escaping (Int32) -> Void) throws -> ChildProcess {
    var fds: [Int32] = [0, 0]
    guard pipe(&fds) == 0 else { throw SpawnError(code: errno) }
    var actions: posix_spawn_file_actions_t?
    posix_spawn_file_actions_init(&actions)
    defer { posix_spawn_file_actions_destroy(&actions) }
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0)
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1)
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2)
    if let fd = fd {
      if fd == 3 { posix_spawn_file_actions_addinherit_np(&actions, 3) } else { posix_spawn_file_actions_adddup2(&actions, fd, 3) }
    }
    var attributes: posix_spawnattr_t?
    posix_spawnattr_init(&attributes)
    defer { posix_spawnattr_destroy(&attributes) }
    posix_spawnattr_setflags(&attributes, Int16(POSIX_SPAWN_CLOEXEC_DEFAULT))  // nothing else leaks into the child
    let argv = ([path] + arguments).map { strdup($0) } + [nil]
    defer { argv.forEach { free($0) } }
    var pid: pid_t = 0
    let code = posix_spawn(&pid, path, &actions, &attributes, argv, environ)
    close(fds[1])
    guard code == 0 else { close(fds[0]); throw SpawnError(code: code) }
    let child = ChildProcess(pid: pid)
    let reader = FileHandle(fileDescriptor: fds[0], closeOnDealloc: true)
    reader.readabilityHandler = { handle in
      let data = handle.availableData
      guard !data.isEmpty else { return }
      DispatchQueue.main.async { output(String(decoding: data, as: UTF8.self)) }
    }
    Thread.detachNewThread {
      var status: Int32 = 0
      while waitpid(pid, &status, 0) < 0 && errno == EINTR {}
      let exited = status & 0x7f == 0
      DispatchQueue.main.async {
        reader.readabilityHandler = nil
        let rest = reader.readDataToEndOfFile()
        output(String(decoding: rest, as: UTF8.self) + "\n")
        child.running = false
        ended(exited ? (status >> 8) & 0xff : -1)
      }
    }
    return child
  }

  func terminate() { if running { kill(pid, SIGTERM) } }
}

/// FABLE2_TEST_RETRY_WHEN=<file>: while access is refused, press Try Again once that file appears (it is removed
/// first, so a second refusal waits for a new one). The readiness signal is the file; nothing waits a fixed time.
enum AccessTest {
  static func watchRetry(_ retry: @escaping () -> Void) -> Timer? {
    guard let marker = Paths.environment["FABLE2_TEST_RETRY_WHEN"], marker.hasPrefix("/") else { return nil }
    return Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { timer in
      guard FileManager.default.fileExists(atPath: marker) else { return }
      timer.invalidate()
      try? FileManager.default.removeItem(atPath: marker)
      retry()
    }
  }
}

// MARK: - Extraction (FableIIPrepare)

final class PrepareRunner: ObservableObject {
  enum Phase: Equatable { case idle, running, mismatch(String), failed(String), denied(AppAccess.Problem), done }
  @Published var phase = Phase.idle
  @Published var fraction = 0.0
  @Published var detail = "Reading the disc image…"
  @Published var log = ""
  /// After Try Again: when it was checked and that access is still refused (shown under the explanation).
  @Published var retryNote = ""
  private var process: ChildProcess?
  private var buffer = ""
  private var retryWatch: Timer?
  var image: String?
  var lastStopOnMismatch = true
  var onDone: (() -> Void)?

  /// Extracts `image` into the managed folder (FableIIPrepare --headless --progress). With
  /// `stopOnMismatch` it stops after default.xex when its hash differs, so the player can choose.
  /// This process opens the image (an open-panel or drag-and-drop pick grants it access even in a privacy-protected
  /// folder) and the extractor reads that open file (--iso-fd 3): it never opens the path itself.
  func start(image: String, stopOnMismatch: Bool = true) {
    let wasDenied: Bool = { if case .denied = phase { return true } else { return false } }()
    self.image = image
    lastStopOnMismatch = stopOnMismatch
    retryWatch?.invalidate()
    let fm = FileManager.default
    try? fm.createDirectory(atPath: Paths.logs, withIntermediateDirectories: true)
    let imageFD = open(image, O_RDONLY | O_CLOEXEC)
    if imageFD < 0 {
      let code = errno
      let problem = AppAccess.classify(code: code, path: image) ?? .other(path: image, code: code)
      appendLog(Paths.prepareLog, "[player] cannot open the disc image \(image): \(String(cString: strerror(code)))")
      denied(problem, again: wasDenied)
      return
    }
    retryNote = ""
    let result = Paths.state + "/prepare-result.plist"
    try? fm.removeItem(atPath: result)
    var args = ["--target", Paths.managed, "--lua", Paths.resources + "/recomp-lua", "--result", result,
                "--iso", image, "--iso-fd", "3", "--headless", "--progress"]
    if let expected = Paths.expectedXex, !expected.isEmpty { args += ["--expected-xex", expected] }
    if stopOnMismatch { args.append("--stop-on-mismatch") }
    phase = .running
    fraction = 0
    detail = "Reading the disc image…"
    appendLog(Paths.prepareLog, "[player] extracting the game files from \(image)")
    do {
      process = try ChildProcess.spawn(Paths.macOS + "/FableIIPrepare", args, inheriting: imageFD,
                                       output: { [weak self] text in self?.consume(text) },
                                       ended: { [weak self] status in self?.finished(status: status) })
    } catch {
      phase = .failed("Could not start the extractor: \(error)")
    }
    close(imageFD)  // the extractor has its own copy
  }

  /// Access refused: explain it (the window offers Choose Disc Image…, Privacy Settings and Try Again).
  private func denied(_ problem: AppAccess.Problem, again: Bool) {
    phase = .denied(problem)
    let time = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
    retryNote = again ? "Checked again at \(time): still no access." : ""
    appendLog(Paths.logs + "/player.log", "[access] \(again ? "still denied" : "denied") (disc image): \(problem.kind) \(problem.path)")
    retryWatch = AccessTest.watchRetry { [weak self] in self?.tryAgain() }
  }

  /// Try Again: opens the same image again (a new check of the access, not a replay of the old result).
  func tryAgain() {
    guard let image = image else { return }
    appendLog(Paths.logs + "/player.log", "[access] try again (disc image)")
    start(image: image, stopOnMismatch: lastStopOnMismatch)
  }

  func cancel() {
    guard let process = process, process.running else { return }
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
    // Nothing to prepare: no bundled seed, no pipeline lists remembered on this Mac and (setting off, or no disc
    // files yet) no shader bank to compile. The game then compiles while playing and remembers its lists.
    let bank = bankEnabled && hasBank
    if !hasLists && !bank { return false }
    guard let data = FileManager.default.contents(atPath: markerPath),
          let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return true }
    // A preparation without the shader bank does not count once the bank is on.
    if bank && json["bank"] as? Bool != true { return true }
    return json["stamp"] as? String != stamp
  }
  /// The disc's shader bank (docs/shader-precompile-20261015.md): every shader of the game, read from the player's
  /// own extracted files by the headless prep (FABLE2_SHADER_PREP_BANK). Nothing game-derived ships with the app.
  static var hasBank: Bool {
    FileManager.default.fileExists(atPath: GameFiles.root + "/data/Shaders/ShadersRelease.sbk")
  }
  /// Settings > "Prepare all of the disc's shaders at install" (shaderPrepBank, default on) or
  /// FABLE2_SHADER_PREP_BANK=0.
  static var bankEnabled: Bool {
    if ProcessInfo.processInfo.environment["FABLE2_SHADER_PREP_BANK"] == "0" { return false }
    return PlistSettingsStore(path: Paths.settingsPlist).object(forKey: "shaderPrepBank") as? Bool ?? true
  }
  private var bankRun = false
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
    bankRun = Self.bankEnabled && Self.hasBank
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
    // The disc's shader bank at the first scale only (the one the Play window uses, ~25-50 s); the other scales
    // prepare the remembered lists and the renderer's own pipelines.
    env["FABLE2_SHADER_PREP_BANK"] = bankRun && index == 0 ? "1" : "0"
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
                                           json.map { "\($0["pipelines_compiled"] ?? "?") pipelines compiled, \($0["native_xlate"] ?? "?") native shaders, disc bank \(($0["bank"] as? [String: Any])?["translations"] ?? 0)" } ?? "no status"))
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
    let bank = json["bank"] as? [String: Any] ?? [:]
    if let total = bank["translations"] as? Int, total > 0, let pending = bank["pending"] as? Int, pending > 0 {
      detail = "Preparing shaders for \(scales[index])× resolution… \(total - pending) of \(total) from the game disc"
    } else {
      detail = "Preparing shaders for \(scales[index])× resolution… \(compiled) pipelines"
    }
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
                                 "when": ISO8601DateFormatter().string(from: Date()), "bank": bankRun]
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
  /// The build kit copied out of the app by this process before the build (docs/release-app.md "Protected folders"):
  /// /usr/bin/python3 and the tools it runs then never read the app bundle, which macOS privacy would question when
  /// the app sits in Downloads, Desktop, Documents or iCloud Drive. Removed after a successful build.
  static var stagedKit: String { build + "/kit" }

  /// Copies (APFS clone when it can) the app's kit into stagedKit unless this kit is there already. nil: ready.
  static func stageKit() -> AppAccess.Problem? {
    let fm = FileManager.default
    let source = Paths.kit, target = stagedKit, stamp = target + "/.staged-kit-id"
    if !Paths.kitID.isEmpty, (try? String(contentsOfFile: stamp, encoding: .utf8)) == Paths.kitID,
       fm.fileExists(atPath: target + "/kit.json") { return nil }
    if let problem = AppAccess.check(source + "/kit.json") { return problem }
    do { try fm.createDirectory(atPath: build, withIntermediateDirectories: true) } catch {
      return AppAccess.classify(code: AppAccess.errnoOf(error), path: build) ?? .other(path: build, code: EIO)
    }
    try? fm.removeItem(atPath: target)
    let started = Date()
    var how = "clone"
    if clonefile(source, target, 0) != 0 {
      let code = errno
      try? fm.removeItem(atPath: target)
      if code == EPERM || code == EACCES { return AppAccess.classify(code: code, path: source) }
      how = "copy"
      do { try fm.copyItem(atPath: source, toPath: target) } catch {
        try? fm.removeItem(atPath: target)
        let path = (error as NSError).userInfo[NSFilePathErrorKey] as? String ?? source
        let code = AppAccess.errnoOf(error)
        return AppAccess.classify(code: code, path: path) ?? .other(path: path, code: code)
      }
    }
    try? Paths.kitID.write(toFile: stamp, atomically: true, encoding: .utf8)
    appendLog(logPath, String(format: "[player] build kit copied out of the app (%@, %.2f s): %@", how,
                              Date().timeIntervalSince(started), target))
    return nil
  }
  static var machine: String {
    var size = 0
    sysctlbyname("hw.model", nil, &size, nil, 0)
    var model = [CChar](repeating: 0, count: max(size, 1))
    sysctlbyname("hw.model", &model, &size, nil, 0)
    let ram = Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824
    return String(format: "%@, %d cores, %.0f GB, macOS %@", String(cString: model), ProcessInfo.processInfo.processorCount,
                  ram, ProcessInfo.processInfo.operatingSystemVersionString)
  }
  /// First-build estimate before any progress is known (docs/release-app.md, "Build time"): 650 s of compile CPU
  /// and ~10 s of code generation and configuration on an M5 core, scaled by the chip generation's speed, over the
  /// parallelism of the job count (performance cores count fully, efficiency cores 0.4), plus a short tail.
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
    let seconds = factor * (10 + 650 / parallel + 5)  // codegen + compile + the tail
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
  enum Phase: Equatable {
    case idle, needsTools, waitingForTools, running, cancelled, failed(String), denied(AppAccess.Problem), done
  }
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
  /// Failure lines of the whole build (with the two lines before each) for Copy Details: an error can scroll far
  /// out of the last lines while parallel jobs finish (fable2_release.py ERROR_LINE is the same rule).
  private var errorLines: [String] = []
  private var recentLines: [String] = []
  @Published var seconds: [String: Double] = [:]
  /// After Try Again: when it was checked and that access is still refused (shown under the explanation).
  @Published var retryNote = ""
  private var wasDenied = false
  private var retryWatch: Timer?
  private var process: Process?
  private var buffer = ""
  private var poll: Timer?
  private var stepStarted = Date()
  private var started = Date()
  var onDone: (() -> Void)?

  /// Checks the developer tools, then runs the build (or waits for the tools' installer).
  func start() {
    try? FileManager.default.createDirectory(atPath: Paths.logs, withIntermediateDirectories: true)
    wasDenied = { if case .denied = phase { return true } else { return false } }()
    if wasDenied { appendLog(Paths.logs + "/player.log", "[access] try again (build)") }
    retryWatch?.invalidate()
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
    // The kit leaves the app bundle through this process first (macOS privacy, docs/release-app.md).
    if let problem = GameBuild.stageKit() {
      appendLog(GameBuild.logPath, "[player] the build kit could not be read: \(problem.kind) \(problem.path)")
      return denied(problem)
    }
    let kit = GameBuild.stagedKit
    var args = ["-I", "-B", "-u", kit + "/game-src/tools/fable2_game_build.py", "build",
                "--kit", kit, "--state", Paths.state, "--game-data", GameFiles.root]
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
    errorLines = []
    recentLines = []
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
      noteError(line)
    }
  }

  private static let errorPattern = try! NSRegularExpression(
    pattern: #"error:|\*\*\* |No rule to make|fatal|Error \d|\[error\]|CMake Error|FAILED"#)

  private func noteError(_ line: String) {
    let range = NSRange(line.startIndex..., in: line)
    if errorLines.count < 80, BuildRunner.errorPattern.firstMatch(in: line, range: range) != nil {
      errorLines += recentLines.filter { !errorLines.suffix(2).contains($0) } + [line]
    }
    recentLines = Array((recentLines + [line]).suffix(2))
  }

  private func ended(status: Int32) {
    process = nil
    switch status {
    case 0:
      finished.formUnion(Self.steps.map(\.key))
      fraction = 1
      phase = .done
      appendLog(GameBuild.logPath, String(format: "[player] game built in %.0f s", Date().timeIntervalSince(started)))
      wasDenied = false
      retryNote = ""
      try? FileManager.default.removeItem(atPath: GameBuild.stagedKit)  // the build has its own copies now
      onDone?()
    case 3, 15, -1:
      phase = .cancelled
    default:
      if let problem = AppAccess.classify(output: log), problem.kind == "privacy" || problem.kind == "permissions" {
        return denied(problem)
      }
      let error = log.split(separator: "\n").last { $0.hasPrefix("[error] ") }
        .map { String($0.dropFirst("[error] ".count)) }
      phase = .failed(error ?? "The build stopped (exit \(status)). See the details below.")
    }
  }

  /// Access refused (macOS privacy or file permissions): the window explains it and offers Privacy Settings and
  /// Try Again, which checks again (a new attempt, not the old result).
  private func denied(_ problem: AppAccess.Problem) {
    phase = .denied(problem)
    let time = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
    retryNote = wasDenied ? "Checked again at \(time): still no access." : ""
    appendLog(Paths.logs + "/player.log", "[access] \(wasDenied ? "still denied" : "denied") (build): \(problem.kind) \(problem.path)")
    retryWatch = AccessTest.watchRetry { [weak self] in self?.start() }
  }

  var showsCopyDetails: Bool {
    switch phase { case .failed, .denied: return true; default: return false }
  }

  /// Text for "Copy Details": the error, this Mac, the app and the end of the log.
  var report: String {
    var lines = ["Fable II build report", "App: \(Paths.version) (kit \(Paths.kitID))", "Mac: \(GameBuild.machine)"]
    if case .failed(let error) = phase { lines.append("Error: \(error)") }
    if case .denied(let problem) = phase { lines.append("Access refused (\(problem.kind)): \(problem.path)") }
    lines.append("App location: \(AppAccess.location(of: Paths.bundle.bundleURL).json)")
    if !errorLines.isEmpty {
      lines.append("Error lines:")
      lines += errorLines
    }
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
  /// The save menu's entries, newest first (refresh()).
  @Published var saves: [SaveSummary] = []
  /// The save menu's choice: a hero folder (HeroNNN) or "title". Starts on the newest save, and again after a game.
  @Published var startPick = SaveChoice.titleID
  private var pickedNewest = false
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
  @Published var showArchives = false
  /// Shown on the Play window after the game was built (first launch or an update): the build never starts the
  /// game by itself, so the player can look at the settings first (--play, a test flag, still plays when ready).
  @Published var readyNote = ""
  private var buildWasUpdate = false
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
    // One settings file, one store object: the checkout and the Dev Launcher window share it.
    DevCheckout.store = store
    DevHost.shared.store = store
    screen = Self.normalScreen
    super.init(store: store)
    // The game next to this executable: no-converter (release/portable) builds force the native switches on.
    let hasConverter = LaunchSettings.hasConverter(macOSFolder: Paths.macOS)
    converterProbe = { _ in hasConverter }
    onDeveloperModeChange = { if $0 { DevHost.shared.ensure() } }
    prepare.onDone = { [weak self] in self?.filesReady() }
    builder.onDone = { [weak self] in
      guard let self = self else { return }
      self.readyNote = self.buildWasUpdate ? "Updated. Ready to play." : "Ready to play."
      appendLog(Paths.logs + "/player.log", "[player] game built (\(self.buildWasUpdate ? "update" : "first build")); "
                + "the Play window waits for the player")
      self.filesReady()
    }
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

  /// The first screen: the welcome window, the build or the Play window. The app never moves, copies or trashes
  /// its own bundle; it runs wherever the player put it (docs/release-app.md "Protected folders").
  static var normalScreen: Screen { !GameFiles.ready ? .welcome : GameBuild.ready ? .play : .building }

  /// `pickNewest`: put the save menu on the newest save (first time, and after a game).
  func refresh(pickNewest: Bool = false) {
    if screen == .play {
      saves = Saves.all()
      latestSave = saves.first
      if pickNewest || !pickedNewest || (startPick != SaveChoice.titleID && !saves.contains { $0.hero == startPick }) {
        pickedNewest = true
        startPick = saves.first?.hero ?? SaveChoice.titleID
      }
      offerImportIfNeeded()
    } else {
      candidates = GameFiles.reuseCandidates()
    }
  }

  /// Release app, first time on the Play window with no saves of its own: offer the saves of another install on this
  /// Mac (portable or dev, "Fable II Recomp") once. Settings > Advanced and File > Import Saves… stay available.
  func offerImportIfNeeded() {
    guard Paths.buildsGame, Paths.environment["FABLE2_TEST_BACKGROUND"] != "1",
          store.object(forKey: "importSavesOffered") as? Bool != true,
          SaveImport.heroes(in: Paths.state + "/saves").isEmpty, let source = SaveImport.otherInstalls().first else { return }
    store.set(true, forKey: "importSavesOffered")
    let count = SaveImport.heroes(in: source).count
    DispatchQueue.main.async {
      let alert = NSAlert()
      alert.messageText = "Import your saves?"
      alert.informativeText = "\(count) save\(count == 1 ? "" : "s") found from another Fable II install. They are copied."
      alert.addButton(withTitle: "Import")
      alert.addButton(withTitle: "Not Now")
      if alert.runModal() == .alertFirstButtonReturn { self.importSaves(from: source) }
    }
  }

  /// File > Import Saves… and Settings > Advanced: from another install found on this Mac, or a chosen folder.
  func importSavesInteractive() {
    if let found = SaveImport.otherInstalls().first {
      let alert = NSAlert()
      alert.messageText = "Import saves"
      alert.informativeText = "\(SaveImport.heroes(in: found).count) save(s) found from another Fable II install."
      alert.addButton(withTitle: "Import")
      alert.addButton(withTitle: "Choose Folder…")
      alert.addButton(withTitle: "Cancel")
      switch alert.runModal() {
      case .alertFirstButtonReturn: importSaves(from: found); return
      case .alertSecondButtonReturn: break
      default: return
      }
    }
    let panel = NSOpenPanel()
    panel.title = "Choose a Fable II saves folder"
    panel.message = "A saves folder, or the data folder that holds one."
    panel.canChooseFiles = false
    panel.canChooseDirectories = true
    guard panel.runModal() == .OK, let url = panel.url else { return }
    if SaveImport.heroes(in: SaveImport.savesFolder(url.path)).isEmpty {
      let alert = NSAlert()
      alert.messageText = "No Fable II saves in that folder"
      alert.informativeText = url.path
      alert.runModal()
      return
    }
    importSaves(from: url.path)
  }

  /// Copies the saves; asks before replacing any save this app already has; shows what was imported.
  func importSaves(from source: String) {
    var replace = false
    let conflicts = SaveImport.conflicts(from: source)
    if !conflicts.isEmpty {
      let alert = NSAlert()
      alert.messageText = "Replace saves you already have here?"
      alert.informativeText = conflicts.map(SaveImport.name).joined(separator: ", ")
        + ". Replaced saves are backed up first."
      alert.addButton(withTitle: "Keep Mine")
      alert.addButton(withTitle: "Replace")
      alert.addButton(withTitle: "Cancel")
      switch alert.runModal() {
      case .alertFirstButtonReturn: replace = false
      case .alertSecondButtonReturn: replace = true
      default: return
      }
    }
    let alert = NSAlert()
    do {
      let r = try SaveImport.run(from: source, replace: replace)
      let done = r.imported + r.replaced
      alert.messageText = done.isEmpty ? "No saves imported" : "Imported \(done.count) save\(done.count == 1 ? "" : "s")"
      var lines: [String] = []
      if !r.imported.isEmpty { lines.append("Imported: " + r.imported.map(SaveImport.name).joined(separator: ", ")) }
      if !r.replaced.isEmpty { lines.append("Replaced: " + r.replaced.map(SaveImport.name).joined(separator: ", ")) }
      if !r.skipped.isEmpty { lines.append("Kept yours: " + r.skipped.map(SaveImport.name).joined(separator: ", ")) }
      if let backup = r.backup { lines.append("Backup of the replaced saves: " + backup) }
      alert.informativeText = lines.joined(separator: "\n")
    } catch {
      alert.messageText = "Import failed"
      alert.informativeText = error.localizedDescription
    }
    alert.runModal()
    latestSave = Saves.latest()
  }

  /// The game files are ready: build the game on this Mac when this app does that and it is not built for this
  /// version yet (BuildRunner), prepare the shaders once per build (ShaderPrepRunner), then the Play window.
  func filesReady() {
    message = ""
    if GameFiles.ready && !GameBuild.ready {
      screen = .building
      if builder.phase != .running && builder.phase != .waitingForTools {
        buildWasUpdate = GameBuild.isUpdate
        builder.start()
      }
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
      panel.message = "Copies about 7 GB of game files."
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
    let all = Saves.all()
    if startMode == .continueSave, let save = all.first(where: { $0.hero == startPick }) ?? all.first, save.count > 0 {
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
  /// Load Saved Game (the default button): the menu's save (the title screen without saves).
  func loadSave() { playFromLauncher(startPick == SaveChoice.titleID ? .title : .continueSave) }

  /// Start at Title Screen: the title (intro movies first unless skipped), no save.
  func startTitle() { playFromLauncher(.title) }

  /// The save menu: every save, newest first (Start at Title Screen is its own button).
  var saveChoices: [SaveChoice] { saves.map { SaveChoice(id: $0.hero, label: $0.label) } }

  func playFromLauncher(_ mode: StartMode) {
    guard !gameRunning else { return }
    startMode = mode  // also what "Skip launcher" starts with next time
    readyNote = ""
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
    // Front + keyboard focus for the game (Shared/GameActivation.swift): a yield by bundle id went nowhere (the game
    // shares this app's id and does not exist yet), so the game came up in the background.
    if Paths.environment["FABLE2_TEST_BACKGROUND"] != "1" {
      GameActivation.handOff(to: process.processIdentifier) { appendLog(Paths.logs + "/player.log", $0) }
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
    refresh(pickNewest: true)
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

/// The front page (2026-10-19): Start at Title Screen + Load Saved Game, the save menu, Resolution and Frame rate, Skip intro, turbo, Display; the
/// page bar (Settings bottom-left, as on the Settings page) with the version.
struct PlayView: View {
  @ObservedObject var model: PlayerModel

  var body: some View {
    VStack(spacing: 0) {
      Header()
      Form {
        Section { PresetPicker(model: model) }
        Section {
          SaveMenu(choices: model.saveChoices, selection: $model.startPick, busy: false, disabled: model.gameRunning,
                   startTitle: { model.startTitle() }, load: { model.loadSave() },
                   recording: model.settings.recordInput, showRecording: { model.openRecordingSetting() })
        }
        FrontPageOptions(model: model)
        Section {
          Picker("Display", selection: $model.settings.display) {  // segmented: fixed width
            ForEach(DisplayMode.allCases) { mode in Text(mode.title).tag(mode) }
          }
          .pickerStyle(.segmented)
        }
      }
      .formStyle(.grouped)
      .contentMargins(.top, 6, for: .scrollContent)
      .scrollDisabled(true)
      .fixedSize(horizontal: false, vertical: true)

      Spacer(minLength: 0)
      // Fixed spots: the note fills the middle (truncated), Show Logs and the version sit at the right.
      PageBar(model: model) {
        Group {
          if !model.message.isEmpty {
            Text(model.message).font(.caption).foregroundStyle(.red)
          } else if !model.readyNote.isEmpty {
            Label(model.readyNote, systemImage: "checkmark.circle.fill").font(.caption).foregroundStyle(.secondary)
          } else if !model.gameNote.isEmpty {
            Text(model.gameNote).font(.caption).foregroundStyle(.red)
          }
        }
        .lineLimit(2)
        Spacer(minLength: 0)
        if !model.gameNote.isEmpty && model.message.isEmpty && model.readyNote.isEmpty {
          Button("Show Logs") { NSWorkspace.shared.open(URL(fileURLWithPath: Paths.logs, isDirectory: true)) }
        }
        Text("Version \(Paths.version)").font(.caption2).foregroundStyle(.tertiary)
      }
    }
    .frame(width: LauncherLayout.width, height: LauncherLayout.height)
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
          Text("Game files found in \(candidate.appName). Use them here?")
            .fixedSize(horizontal: false, vertical: true)
            .help("No copy and no extra space. Or choose your disc image (.iso) instead.")
        } else {
          Text("Choose your Fable II disc image (.iso).")
            .fixedSize(horizontal: false, vertical: true)
          Text(Paths.buildsGame ? "Needs about 8 GB free." : "Needs about 7 GB free.")
            .font(.callout).foregroundStyle(.secondary)
            .help("The game files are copied to Application Support. The image is only read.")
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
    .dropDestination(for: URL.self) { urls, _ in DiscDrop.accept(urls, model: model) }
  }
}

/// A disc image dragged onto the window: like choosing it in the open panel, this grants this app access to that
/// file even in a privacy-protected folder (Downloads, Desktop, …).
enum DiscDrop {
  static func accept(_ urls: [URL], model: PlayerModel) -> Bool {
    guard let url = urls.first(where: { $0.isFileURL && $0.pathExtension.lowercased() == "iso" }) else { return false }
    model.chooseImage(url.path)
    return true
  }
}

/// Plain-English texts for a refused access (docs/release-app.md "Protected folders").
enum AccessText {
  /// "your Downloads folder", "iCloud Drive", "an external or network volume".
  static func place(_ folder: String) -> String {
    if folder.hasPrefix("an ") || folder == "iCloud Drive" { return folder }
    if folder == "cloud storage" { return "a cloud storage folder" }
    return "your \(folder) folder"
  }
  static func name(_ path: String) -> String { (path as NSString).lastPathComponent }
  static let settingsPath = "System Settings › Privacy & Security › Files and Folders"

  static func inApp(_ problem: AppAccess.Problem) -> Bool { AppAccess.inside(problem.path, Paths.bundle.bundlePath) }

  static func build(_ problem: AppAccess.Problem) -> (title: String, text: String) {
    switch problem {
    case .privacy(let folder, _):
      if inApp(problem) {
        return ("macOS is not letting Fable II read its own files",
                "Move Fable II to Applications, or allow it for \(place(folder)) in \(settingsPath).")
      }
      return ("macOS is not letting Fable II read \(place(folder))",
              "Allow Fable II for this folder in \(settingsPath), then click Try Again.")
    case .permissions(let path):
      return ("Fable II cannot read its build files",
              "No permission to read “\(name(path))”. "
              + (inApp(problem) ? "Download Fable II again and move it to Applications."
                 : "Check its permissions in Finder, then click Try Again."))
    case .missing(let path):
      return ("A build file is missing", "“\(name(path))” is missing. Download Fable II again.")
    case .other(let path, let code):
      return ("A build file could not be read", "“\(name(path))”: \(String(cString: strerror(code))).")
    }
  }

  static func image(_ problem: AppAccess.Problem) -> (title: String, text: String) {
    switch problem {
    case .privacy:
      return ("macOS is not letting Fable II read the disc image",
              "Choose the disc image again (or drag it onto this window).")
    case .permissions(let path):
      return ("The disc image cannot be read",
              "No permission to read “\(name(path))”. Choose another copy or fix its permissions in Finder.")
    case .missing(let path):
      return ("The disc image is not there any more",
              "“\(name(path))” has moved. Choose it again.")
    case .other(let path, let code):
      return ("The disc image could not be opened",
              "“\(name(path))”: \(String(cString: strerror(code))).")
    }
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
        Text("Once per version.")
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
        case .denied(let problem):
          let text = AccessText.image(problem)
          Text(text.title).font(.title3.weight(.semibold))
          Text(text.text).fixedSize(horizontal: false, vertical: true)
          if !prepare.retryNote.isEmpty {
            Text(prepare.retryNote).font(.callout).foregroundStyle(.orange)
          }
          if problem.folder != nil {
            Button("Open Privacy Settings") { NSWorkspace.shared.open(AppAccess.privacySettingsURL) }
          }
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
          case .denied:
            Button("Quit") { NSApp.terminate(nil) }
            Button("Choose Disc Image…") { model.chooseImage() }
            Button("Try Again") { prepare.tryAgain() }.buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
          default:
            Button("Cancel") { prepare.cancel() }.keyboardShortcut(.cancelAction)
          }
        }
        .controlSize(.large)
      }
      .padding(20)
    }
    .frame(width: 560)
    .dropDestination(for: URL.self) { urls, _ in
      if case .running = prepare.phase { return false }
      return DiscDrop.accept(urls, model: model)
    }
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
          Text("This release needs a different disc version. See the README.\n\ndefault.xex SHA-256: \(digest)")
        } else {
          Text("The game may misbehave.\n\ndefault.xex SHA-256: \(digest)")
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
    case .denied(let problem): return AccessText.build(problem).title
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
          Text("Building the game needs Apple's free Command Line Tools.")
            .fixedSize(horizontal: false, vertical: true)
            .help("Not Xcode. macOS shows its own installer; this window continues when they are installed.")
          if builder.phase == .waitingForTools {
            HStack(spacing: 8) { ProgressView().controlSize(.small); Text(builder.detail).font(.callout).foregroundStyle(.secondary) }
          }
        case .failed(let error):
          Text(error).foregroundStyle(.secondary).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
        case .denied(let problem):
          Text(AccessText.build(problem).text).fixedSize(horizontal: false, vertical: true)
          if !builder.retryNote.isEmpty { Text(builder.retryNote).font(.callout).foregroundStyle(.orange) }
          HStack {
            if problem.folder != nil {
              Button("Open Privacy Settings") { NSWorkspace.shared.open(AppAccess.privacySettingsURL) }
            }
          }
        case .cancelled:
          Text("Progress is kept.").foregroundStyle(.secondary)
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
          if builder.showsCopyDetails {
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
          case .failed, .cancelled, .denied:
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
  @Environment(\.openWindow) private var openWindow
  var body: some View {
    Group {
      if model.page == .settings {
        PlayerSettingsPage(model: model)
      } else {
      switch model.screen {
      case .welcome: WelcomeView(model: model)
      case .preparing: PreparingView(model: model, prepare: model.prepare, shaders: model.shaderPrep)
      case .building: BuildView(model: model, builder: model.builder)
      case .play: PlayView(model: model)
      }
      }
    }
    .sheet(isPresented: $model.showStartFreshMain) { StartFreshSheet(model: model) }
    .onAppear {
      Startup.run(model: model)
      Snapshot.start(model: model, openWindow: { openWindow(id: $0) })
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
      // Load Saved Game on the newest save (the title screen without saves).
      DispatchQueue.main.async { model.refresh(pickNewest: true); model.loadSave() }
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

// MARK: - Settings page

/// Settings, the second page of the Play window (2026-10-19; was the Settings window).
struct PlayerSettingsPage: View {
  @ObservedObject var model: PlayerModel
  @State private var confirmReset = false

  var body: some View {
    SettingsPage(model: model, extra: extra)
      .frame(width: LauncherLayout.width, height: LauncherLayout.height)
      .sheet(isPresented: $model.showStartFresh) { StartFreshSheet(model: model) }
      .sheet(isPresented: $model.showArchives) { ArchivesSheet(model: model) }
      .confirmationDialog("Reset every setting to its default?", isPresented: $confirmReset) {
        Button("Reset All Settings", role: .destructive) { model.resetAll() }
      } message: {
        Text("Saves and game files are kept.")
      }
  }

  private func extra(_ tab: SettingsTab) -> AnyView {
    switch tab {
    case .general:
      return AnyView(Group {
        Section {
          Toggle("Skip launcher, play directly", isOn: $model.skipLauncher)
            .help("Opening Fable II starts the game. Hold Option while opening to see the launcher.")
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
        Section {
          HStack {
            Button("Open Logs Folder") { NSWorkspace.shared.open(URL(fileURLWithPath: Paths.logs, isDirectory: true)) }
            Button("Open Data Folder") { NSWorkspace.shared.open(URL(fileURLWithPath: Paths.state, isDirectory: true)) }
              .help("Saves, settings, shader cache and logs.")
            if Paths.buildsGame {
              Button("Import Saves…") { model.importSavesInteractive() }
                .disabled(model.gameRunning)
                .help("Copy saves from another Fable II install or a folder.")
            }
            Spacer()
          }
          HStack {
            Button("Reset All Settings…") { confirmReset = true }
              .disabled(model.isDefault)
            Spacer()
            Button("Start Fresh…") { model.showStartFresh = true }
              .help("Archive the game files, build, caches, logs and settings, so the next launch is a first launch. "
                    + "Nothing is deleted.")
            Button("Archives…") { model.showArchives = true }
          }
        } header: { Text("Logs and data") }
      })
    case .diagnostics:
      return AnyView(DeveloperSection(model: model))
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

  @MainActor static func start(model: PlayerModel, openWindow: @escaping (String) -> Void = { _ in }) {
    guard let path = path else { return }
    if let tab = settingsTab {
      model.screen = .play
      model.settingsTab = tab
      model.page = .settings
      capture(after: 2.0, path: path) { NSApp.windows.first { $0.isVisible && $0.title == "Fable II" } }
      return
    }
    if window == "devlauncher" {  // developer mode: the Dev Launcher's window in this app
      DevHost.shared.ensure()
      openWindow("devlauncher")
      capture(after: 3.0, path: path) { NSApp.windows.first { $0.isVisible && $0.title == "Fable II Launcher" } }
      return
    }
    switch window {
    case "ready":  // the Play window right after the build
      model.readyNote = "Ready to play."
      model.screen = .play
      model.refresh()
    case "denied":
      // A real refused access (tests: an image or kit in a chmod-ed folder); captured once the window shows it.
      waitFor(path: path, deadline: Date().addingTimeInterval(60)) {
        if case .denied = model.prepare.phase { return true }
        if case .denied = model.builder.phase { return true }
        return false
      }
      return
    case "welcome": model.screen = .welcome; model.refresh()
    case "progress":
      // Started by --iso; captured once the bar has moved (or after 20 s).
      waitForProgress(path: path, model: model, deadline: Date().addingTimeInterval(20))
      return
    default: model.screen = .play; model.refresh()
    }
    capture(after: 1.5, path: path) { NSApp.windows.first { $0.isVisible && $0.title == "Fable II" } }
  }

  @MainActor static func waitFor(path: String, deadline: Date, _ ready: @escaping () -> Bool) {
    if ready() || Date() > deadline {
      capture(after: 0.3, path: path) { NSApp.windows.first { $0.isVisible && $0.title == "Fable II" } }
      return
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { waitFor(path: path, deadline: deadline, ready) }
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
    // A game started from the launcher window ended and that window was closed: reopen it (LauncherReturn).
    let _ = LauncherReturn.reopen = { [openWindow] in openWindow(id: "devlauncher") }
    SettingsPageCommand(model: model, windowTitles: ["Fable II"])
    CommandGroup(after: .windowArrangement) {
      if model.developerMode {
        Divider()
        Button("Fable II Launcher") { DevHost.shared.ensure(); openWindow(id: "devlauncher") }
          .keyboardShortcut("d", modifiers: [.command, .option])
        if let devModel = dev.model {
          Menu("Developer") { DevToolItems(model: devModel, openWindow: { openWindow(id: "devlauncher") }) }
        } else {
          Button("Choose Source Checkout…") { _ = DevCheckout.choose(); DevHost.shared.ensure() }
        }
      }
    }
    CommandGroup(replacing: .newItem) {
      Button("Choose Disc Image…") { model.chooseImage() }
        .disabled(model.screen == .preparing)
      Divider()
      Button("Open Logs Folder") { NSWorkspace.shared.open(URL(fileURLWithPath: Paths.logs, isDirectory: true)) }
      Button("Open Data Folder") { NSWorkspace.shared.open(URL(fileURLWithPath: Paths.state, isDirectory: true)) }
      if Paths.buildsGame {
        Button("Import Saves…") { model.importSavesInteractive() }
          .disabled(model.gameRunning || model.screen != .play)
      }
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
    // Dev Options: the Dev Launcher's main window (opened from the Window menu).
    Window("Fable II Launcher", id: "devlauncher") { DevLauncherWindowHost() }
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
      Text("These move into an archive. Nothing is deleted.")
        .help("Archives… can move them back. Fable II then opens as on its first launch.")
        .fixedSize(horizontal: false, vertical: true)
      Toggle("Keep my saves", isOn: $keepSaves)
        .help("Off: saves go into the archive too.")
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
      Text("What is here now is archived first.")
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
      Text("The current files are archived first. Fable II relaunches.")
    }
    .confirmationDialog("Delete \(selected?.name ?? "") permanently?", isPresented: $confirmDelete) {
      Button("Delete Archive", role: .destructive) { delete() }
    } message: {
      Text("This cannot be undone.")
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
    caps = Capabilities.devLauncher(developer: UserDefaults.standard.object(forKey: "developerMode") as? Bool ?? false)
  } else {
    let store = PlistSettingsStore(path: Paths.settingsPlist)
    DevCheckout.store = store
    let developer = store.object(forKey: "developerMode") as? Bool ?? false
    caps = ["window:player", "cli:launch", "cli:print-args", "cli:picker-rows", "cli:save-menu", "settings:dev-options",
            "start-fresh"]
      + Capabilities.settings(developer: developer)
    if developer {
      caps.append("window:dev-launcher")
      if DevCheckout.path != nil { caps += DevTools.visible(developer: true).map { "tool:" + $0.id } }
    }
  }
  let data = (try? JSONSerialization.data(withJSONObject: ["role": AppRole.devLauncher ? "dev-launcher" : "player",
                                                           "checkout": (DevCheckout.path as Any?) ?? NSNull(),
                                                           "capabilities": caps.sorted()])) ?? Data()
  print(String(decoding: data, as: UTF8.self))
  exit(0)
}
if AppRole.devLauncher { DevLauncherMain.run() }
// Access tooling (docs/release-app.md "Protected folders"; tests/release_access_tests.py).
if let path = argument(after: "--access-check") {
  let problem = AppAccess.check(path)
  let object: [String: Any] = ["path": path, "ok": problem == nil, "problem": (problem?.json as Any?) ?? NSNull(),
                               "protectedFolder": (AppAccess.protectedFolder(for: path) as Any?) ?? NSNull()]
  print(String(decoding: (try? JSONSerialization.data(withJSONObject: object, options: .sortedKeys)) ?? Data(), as: UTF8.self))
  exit(0)
}
if commandLine.contains("--access-classify") {
  // Pure checks, no file access: stdin is a JSON list of {"path", "home", "errno"?, "output"?, "extra"?}; prints one
  // verdict per case (protected folder, errno and helper-output classification, translocation by path).
  let input = FileHandle.standardInput.readDataToEndOfFile()
  let cases = (try? JSONSerialization.jsonObject(with: input)) as? [[String: Any]] ?? []
  let results: [[String: Any]] = cases.map { c in
    let path = c["path"] as? String ?? "", home = c["home"] as? String ?? NSHomeDirectory()
    let extra = c["extra"] as? [String] ?? []
    var out: [String: Any] = ["path": path]
    out["protectedFolder"] = AppAccess.protectedFolder(for: path, home: home, extra: extra) ?? NSNull()
    if let code = c["errno"] as? Int {
      out["problem"] = AppAccess.classify(code: Int32(code), path: path, home: home, extra: extra)?.json ?? NSNull()
    }
    if let text = c["output"] as? String {
      out["outputProblem"] = AppAccess.classify(output: text, home: home, extra: extra)?.json ?? NSNull()
    }
    let translocated = path.contains("/AppTranslocation/")
    out["translocated"] = translocated
    return out
  }
  print(String(decoding: (try? JSONSerialization.data(withJSONObject: results, options: .sortedKeys)) ?? Data(), as: UTF8.self))
  exit(0)
}
if commandLine.contains("--bundle-location") {
  let given = argument(after: "--bundle-location").flatMap { $0.hasPrefix("/") ? $0 : nil }
  let location = AppAccess.location(of: given.map { URL(fileURLWithPath: $0) } ?? Bundle.main.bundleURL)
  print(String(decoding: (try? JSONSerialization.data(withJSONObject: location.json, options: .sortedKeys)) ?? Data(), as: UTF8.self))
  exit(0)
}
// --import-saves FOLDER [--replace]: the Import Saves action without the window (tests, stand-in folders only).
if let source = argument(after: "--import-saves") {
  do {
    let r = try SaveImport.run(from: source, replace: commandLine.contains("--replace"))
    let out: [String: Any] = ["ok": true, "imported": r.imported, "replaced": r.replaced, "kept": r.skipped,
                              "backup": r.backup ?? NSNull(), "other_installs": SaveImport.otherInstalls()]
    print(String(decoding: try JSONSerialization.data(withJSONObject: out, options: [.sortedKeys]), as: UTF8.self))
    exit(0)
  } catch {
    print("{\"ok\": false, \"error\": \"\(error.localizedDescription)\"}")
    exit(1)
  }
}
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
