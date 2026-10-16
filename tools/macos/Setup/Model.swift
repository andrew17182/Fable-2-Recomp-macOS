// "Fable II Setup" state and the work behind each step. Every step is a synchronous function
// that runs on `queue`; the window's buttons and the unattended --auto run call the same code.
import AppKit
import Foundation

enum Phase { case checks, disc, working, finished, failed }
enum Status { case pending, running, ok, skipped, warning, failed }

enum CheckAction { case installCLT, getXcode, installMetal, downloadCMake }

struct Check: Identifiable {
  let id: String
  let title: String
  var status: Status
  var detail: String
  var action: CheckAction?
}

/// Build stages announced by tools/build_macos_metal.py and tools/build_macos.sh
/// ("[fable2-build] stage=<key>: <text>").
struct Stage: Identifiable {
  let id: String
  let title: String
  var status: Status = .pending
  var start: Date?
  var end: Date?
  var duration: TimeInterval? { start.map { (end ?? Date()).timeIntervalSince($0) } }

  static let all: [Stage] = [
    Stage(id: "deps", title: "Downloading dependencies"),
    Stage(id: "patch", title: "Patching dependencies"),
    Stage(id: "sdksrc", title: "Building the ReXGlue SDK (first time only)"),
    Stage(id: "sdk", title: "Code generation"),
    Stage(id: "game", title: "Building the game"),
    Stage(id: "dxc", title: "Building the shader compiler (slow, first time only)"),
    Stage(id: "renderer", title: "Building the Metal renderer"),
    Stage(id: "package", title: "Packaging Fable II.app"),
  ]
}

final class SetupModel: ObservableObject {
  let options: Options
  let paths: Paths
  private var state: SavedState
  private(set) var tools: ToolEnvironment
  let queue = DispatchQueue(label: "setup.work")

  @Published var phase: Phase = .checks
  @Published var busy = false
  @Published var checks: [Check] = []
  @Published var checksDone = false
  @Published var isoPath: String?
  @Published var isoHash: String?
  @Published var hashProgress: Double?
  @Published var discSummary = ""
  @Published var discWarning: String?
  @Published var knownDisc: String?
  @Published var useExistingFiles = false
  @Published var extractStatus: Status = .pending
  @Published var buildStatus: Status = .pending
  @Published var launcherStatus: Status = .pending
  @Published var warmStatus: Status = .pending
  /// Shader warm-up after the build (docs/setup-app.md); the window's checkbox, --no-warm-shaders.
  @Published var warmShaders = true
  @Published var stages: [Stage] = Stage.all
  @Published var activity = ""            // one line under the step list
  @Published var progress: Double?        // nil = indeterminate
  @Published var logLines: [String] = []
  @Published var logURL: URL?
  @Published var errorTitle = ""
  @Published var errorLines: [String] = []
  @Published var totalTime: TimeInterval?

  /// Version of the placed game ("0.1.0 (c0b9d4a)", written by tools/package_macos_app.py).
  var gameVersion: String? {
    for app in [paths.rootGameApp, paths.gameApp] {
      if let info = NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist")),
         let version = info["Fable2VersionString"] as? String, !version.isEmpty { return version }
    }
    return nil
  }

  private var logHandle: FileHandle?
  private var pendingLines: [String] = []
  private let lineLock = NSLock()
  private var flushScheduled = false
  private var running: Process?
  private var currentHash: String?
  private var cancelled = false
  private var lastPrintedPercent = -1
  private let labelPattern = try! NSRegularExpression(pattern: #"^\[fable2-build\] stage=(\w+)"#)
  private let percentPattern = try! NSRegularExpression(pattern: #"^\[\s*(\d+)%\]"#)

  init(options: Options, paths: Paths) {
    self.options = options
    self.paths = paths
    state = SavedState.load(paths)
    tools = ToolEnvironment(path: [], developerDir: nil)
    if options.rebuild { state.buildSucceeded = false; state.launcherBuilt = false; state.shadersWarmed = false }
    useExistingFiles = gameFilesPresent
    warmShaders = options.warmShaders
  }

  var gameFilesPresent: Bool {
    let fm = FileManager.default
    return fm.fileExists(atPath: paths.root.appendingPathComponent("default.xex").path)
      && fm.fileExists(atPath: paths.root.appendingPathComponent("data/dir.manifest").path)
  }

  // MARK: - Output

  private func onMain(_ body: @escaping () -> Void) { DispatchQueue.main.async(execute: body) }

  /// Status line for unattended runs (stdout) and the log file.
  func say(_ text: String) {
    if options.auto { print("[setup] " + text); fflush(stdout) }
    appendLog("[setup] " + text + "\n")
    addLines(["[setup] " + text])
  }

  private func appendLog(_ text: String) {
    if let data = text.data(using: .utf8) { logHandle?.write(data) }
  }

  private func addLines(_ lines: [String]) {
    lineLock.lock()
    pendingLines += lines
    let schedule = !flushScheduled
    flushScheduled = true
    lineLock.unlock()
    guard schedule else { return }
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
      guard let self = self else { return }
      self.lineLock.lock()
      let lines = self.pendingLines
      self.pendingLines = []
      self.flushScheduled = false
      self.lineLock.unlock()
      self.logLines += lines
      if self.logLines.count > 400 { self.logLines.removeFirst(self.logLines.count - 400) }
    }
  }

  private func openLog() {
    guard logHandle == nil else { return }
    try? FileManager.default.createDirectory(at: paths.setup, withIntermediateDirectories: true)
    let formatter = DateFormatter()
    formatter.dateFormat = "yyyyMMdd-HHmmss"
    let url = paths.setup.appendingPathComponent("build-\(formatter.string(from: Date())).log")
    FileManager.default.createFile(atPath: url.path, contents: nil)
    logHandle = try? FileHandle(forWritingTo: url)
    onMain { self.logURL = url }
    say("Log: \(url.path)")
  }

  /// Run a child process, streaming its output into the log and the window.
  /// Returns the exit status (or -1 if it could not start).
  private func stream(_ executable: String, _ args: [String], cwd: URL? = nil,
                      env: [String: String]? = nil, onLine: ((String) -> Void)? = nil) -> Int32 {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: executable)
    process.arguments = args
    process.currentDirectoryURL = cwd ?? paths.root
    process.environment = env ?? tools.variables
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    process.standardInput = FileHandle.nullDevice
    do { try process.run() } catch {
      say("Could not start \(executable): \(error.localizedDescription)")
      return -1
    }
    running = process
    let reader = pipe.fileHandleForReading
    var partial = Data()
    while true {
      let chunk = autoreleasepool { reader.availableData }
      if chunk.isEmpty { break }
      logHandle?.write(chunk)
      partial.append(chunk)
      var lines: [String] = []
      while let cut = partial.firstIndex(where: { $0 == 10 || $0 == 13 }) {
        let line = String(decoding: partial[partial.startIndex..<cut], as: UTF8.self)
        partial.removeSubrange(partial.startIndex...cut)
        if !line.trimmingCharacters(in: .whitespaces).isEmpty { lines.append(line) }
      }
      if !lines.isEmpty {
        addLines(lines)
        lines.forEach { onLine?($0) }
      }
    }
    if !partial.isEmpty { let line = String(decoding: partial, as: UTF8.self); addLines([line]); onLine?(line) }
    process.waitUntilExit()
    running = nil
    return process.terminationStatus
  }

  func cancel() {
    cancelled = true
    if let process = running { Run.killTree(process.processIdentifier) }
  }

  // MARK: - Step 1: prerequisites

  func runChecks() {
    onMain { self.checksDone = false; self.busy = true }
    var result: [Check] = []
    let fm = FileManager.default

    // Xcode / command-line tools. The Metal shader compiler ships with Xcode only, so a full
    // Xcode is needed; if xcode-select points at the command-line tools, use DEVELOPER_DIR.
    let selected = Run.capture("/usr/bin/xcode-select", ["-p"], timeout: 10)
    let selectedPath = selected.output.trimmingCharacters(in: .whitespacesAndNewlines)
    let cltInstalled = selected.status == 0 && fm.fileExists(atPath: selectedPath)
    var developerDir: String?
    var xcodePath: String?
    if cltInstalled && selectedPath.hasSuffix(".app/Contents/Developer") {
      xcodePath = selectedPath
    } else {
      let apps = ((try? fm.contentsOfDirectory(atPath: "/Applications")) ?? [])
        .filter { $0.hasPrefix("Xcode") && $0.hasSuffix(".app") }
        .sorted { a, b in a == "Xcode.app" ? true : b == "Xcode.app" ? false : a < b }
      if let app = apps.first {
        xcodePath = "/Applications/\(app)/Contents/Developer"
        developerDir = xcodePath
      }
    }
    result.append(Check(id: "clt", title: "Xcode command-line tools",
                        status: cltInstalled || xcodePath != nil ? .ok : .failed,
                        detail: cltInstalled ? selectedPath : "Not installed. Click Install, finish Apple's installer, then Check again.",
                        action: cltInstalled || xcodePath != nil ? nil : .installCLT))
    result.append(Check(id: "xcode", title: "Xcode (provides the Metal shader compiler)",
                        status: xcodePath != nil ? .ok : .failed,
                        detail: xcodePath.map { ($0 as NSString).deletingLastPathComponent.replacingOccurrences(of: "/Contents", with: "") }
                          ?? "Install Xcode from the App Store, open it once, then Check again.",
                        action: xcodePath != nil ? nil : .getXcode))
    tools = ToolEnvironment.make(paths: paths, cleanEnv: options.cleanEnv, developerDir: developerDir)

    if xcodePath != nil {
      let shown = Run.capture("/usr/bin/xcodebuild", ["-showComponent", "MetalToolchain", "-json"],
                              env: tools.variables, timeout: 60)
      let installed = shown.status == 0 && shown.output.contains("\"installed\"")
      result.append(Check(id: "metal", title: "Metal Toolchain (Xcode component)",
                          status: installed ? .ok : .failed,
                          detail: installed ? "Installed" : "Not installed (about 700 MB download from Apple).",
                          action: installed ? nil : .installMetal))
    }

    // CMake 3.25 or later (the portable copy in out/tools/cmake is first in PATH).
    if let cmake = tools.find("cmake") {
      let out = Run.capture(cmake, ["--version"], env: tools.variables, timeout: 20).output
      let numbers = out.split(separator: "\n").first?.split(separator: " ").last?
        .split(separator: ".").compactMap { Int($0) } ?? []
      let ok = numbers.count >= 2 && (numbers[0] > 3 || (numbers[0] == 3 && numbers[1] >= 25))
      let version = numbers.map(String.init).joined(separator: ".")
      result.append(Check(id: "cmake", title: "CMake 3.25 or later", status: ok ? .ok : .failed,
                          detail: ok ? "\(version) (\(cmake))" : "\(version) at \(cmake) is too old. Download a private copy for this project.",
                          action: ok ? nil : .downloadCMake))
    } else {
      result.append(Check(id: "cmake", title: "CMake 3.25 or later", status: .failed,
                          detail: "Not found. Download a private copy into out/tools (about 80 MB, no admin needed).",
                          action: .downloadCMake))
    }

    // Python 3 (Xcode's /usr/bin/python3 is fine).
    if cltInstalled || xcodePath != nil, let python = tools.find("python3") {
      let out = Run.capture(python, ["--version"], env: tools.variables, timeout: 60).output
        .trimmingCharacters(in: .whitespacesAndNewlines)
      let parts = out.replacingOccurrences(of: "Python ", with: "").split(separator: ".").compactMap { Int($0) }
      let ok = parts.count >= 2 && parts[0] == 3 && parts[1] >= 9
      result.append(Check(id: "python", title: "Python 3", status: ok ? .ok : .failed,
                          detail: ok ? "\(out) (\(python))" : "Python 3.9 or later is needed (found: \(out)).", action: nil))
    } else {
      result.append(Check(id: "python", title: "Python 3", status: .failed,
                          detail: "Comes with the Xcode command-line tools.", action: nil))
    }

    let basics = ["curl", "ditto", "shasum", "patch", "tar"]
    let missing = basics.filter { !fm.isExecutableFile(atPath: "/usr/bin/" + $0) }
    result.append(Check(id: "basics", title: "System tools (curl, ditto, shasum, patch, tar)",
                        status: missing.isEmpty ? .ok : .failed,
                        detail: missing.isEmpty ? "Present" : "Missing: " + missing.joined(separator: ", "), action: nil))

    // Disk space: game files (unless already extracted) + first build output.
    let free = (try? paths.root.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
      .volumeAvailableCapacityForImportantUsage).flatMap { $0 }.map { UInt64($0) } ?? 0
    let gameBytes: UInt64 = gameFilesPresent ? 0 : 7_000_000_000
    let buildBytes: UInt64 = fm.fileExists(atPath: paths.gameApp.path) ? 0 : Setup.buildBytes
    let needed = gameBytes + buildBytes
    let diskStatus: Status = free < needed ? .failed : free < needed + 3_000_000_000 ? .warning : .ok
    result.append(Check(id: "disk", title: "Free disk space", status: diskStatus,
                        detail: "\(formatBytes(free)) free; " + (needed == 0 ? "game files and build already present" :
                          "needs about \(formatBytes(needed)) (" + [gameBytes > 0 ? "game files \(formatBytes(gameBytes))" : nil,
                                    buildBytes > 0 ? "first build \(formatBytes(buildBytes))" : nil]
                            .compactMap { $0 }.joined(separator: ", ") + ")"),
                        action: nil))

    for check in result where check.status != .ok {
      say("Check: \(check.title): \(check.detail)")
    }
    onMain {
      self.checks = result
      self.checksDone = true
      self.busy = false
    }
  }

  var checksPassed: Bool { checksDone && !checks.contains { $0.status == .failed } }

  func perform(_ action: CheckAction) {
    switch action {
    case .getXcode:
      NSWorkspace.shared.open(URL(string: "macappstore://apps.apple.com/app/id497799835")!)
    case .installCLT:
      _ = Run.capture("/usr/bin/xcode-select", ["--install"], timeout: 30)
    case .installMetal:
      work {
        self.openLog()
        self.onMain { self.activity = "Downloading the Metal Toolchain from Apple…"; self.progress = nil }
        let status = self.stream("/usr/bin/xcodebuild", ["-downloadComponent", "MetalToolchain"])
        self.say(status == 0 ? "Metal Toolchain installed." : "Metal Toolchain download failed (\(status)).")
        self.onMain { self.activity = "" }
        self.runChecks()
      }
    case .downloadCMake:
      work {
        _ = self.downloadCMake()
        self.runChecks()
      }
    }
  }

  /// Download the official portable CMake into out/tools/cmake (checksum verified).
  @discardableResult
  func downloadCMake() -> Bool {
    openLog()
    let fm = FileManager.default
    let downloads = paths.out.appendingPathComponent("downloads")
    let archive = downloads.appendingPathComponent("cmake-\(Setup.cmakeVersion)-macos-universal.tar.gz")
    try? fm.createDirectory(at: downloads, withIntermediateDirectories: true)
    onMain { self.activity = "Downloading CMake \(Setup.cmakeVersion)…"; self.progress = nil }
    say("Downloading CMake \(Setup.cmakeVersion) from \(Setup.cmakeURL)")
    if !fm.fileExists(atPath: archive.path) {
      let partial = archive.appendingPathExtension("part")
      let status = stream("/usr/bin/curl", ["--fail", "--location", "--silent", "--show-error",
                                            Setup.cmakeURL, "--output", partial.path])
      guard status == 0 else { say("CMake download failed (curl \(status))."); return false }
      try? fm.removeItem(at: archive)
      try? fm.moveItem(at: partial, to: archive)
    }
    let digest = Hashing.sha256(of: archive) { _ in }
    guard digest == Setup.cmakeSHA256 else {
      say("CMake download has the wrong checksum (\(digest ?? "unreadable")); deleted it.")
      try? fm.removeItem(at: archive)
      return false
    }
    let staging = paths.out.appendingPathComponent("tools/.cmake-unpack")
    try? fm.removeItem(at: staging)
    try? fm.createDirectory(at: staging, withIntermediateDirectories: true)
    guard stream("/usr/bin/tar", ["-xzf", archive.path, "-C", staging.path]) == 0,
          let top = try? fm.contentsOfDirectory(atPath: staging.path).first(where: { $0.hasPrefix("cmake-") }) else {
      say("Could not unpack \(archive.path)")
      return false
    }
    try? fm.removeItem(at: paths.cmakeHome)
    do {
      try fm.moveItem(at: staging.appendingPathComponent(top), to: paths.cmakeHome)
    } catch {
      say("Could not install CMake: \(error.localizedDescription)")
      return false
    }
    try? fm.removeItem(at: staging)
    say("CMake \(Setup.cmakeVersion) installed in out/tools/cmake (checksum verified).")
    onMain { self.activity = "" }
    return true
  }

  // MARK: - Step 2: disc image

  /// Check the image, then hash it (cached in out/setup/state.json by path, size and date).
  @discardableResult
  func prepareISO(_ path: String) -> Bool {
    onMain {
      self.isoPath = path; self.isoHash = nil; self.discWarning = nil
      self.discSummary = "Reading…"; self.hashProgress = 0; self.useExistingFiles = false
    }
    let url = URL(fileURLWithPath: path)
    do {
      let disc = try DiscImage(path: path)
      let root = try disc.rootEntries()
      let names = Set(root.map(\.name))
      let missing = Setup.requiredDiscEntries.subtracting(names)
      if !missing.isEmpty {
        throw NSError(domain: "setup", code: 1, userInfo: [NSLocalizedDescriptionKey:
          "This disc image has no \(missing.sorted().joined(separator: " or ")); it does not look like Fable II."])
      }
      let selected = root.filter { Setup.discEntries.contains($0.name) }
      let summary = selected.map { e in
        e.isDirectory ? "\(e.name)/ (\(e.files.count) files)" : e.name
      }.joined(separator: ", ") + " — \(formatBytes(selected.reduce(0) { $0 + $1.totalSize }))"
      onMain { self.discSummary = summary }
      say("Disc image: \(path)\n  contents: \(summary)")
    } catch {
      say("Disc image error: \(error.localizedDescription)")
      onMain { self.discSummary = ""; self.discWarning = error.localizedDescription; self.hashProgress = nil }
      return false
    }

    let attributes = try? FileManager.default.attributesOfItem(atPath: path)
    let size = (attributes?[.size] as? NSNumber)?.uint64Value
    let modified = (attributes?[.modificationDate] as? Date)?.timeIntervalSince1970
    var digest: String?
    if state.isoPath == path, state.isoSize == size, state.isoModified == modified, let cached = state.isoSHA256 {
      digest = cached
      say("SHA-256 (cached): \(cached)")
    } else {
      say("Computing SHA-256 of the disc image…")
      let started = Date()
      var lastPrinted = -1
      digest = Hashing.sha256(of: url) { fraction in
        self.onMain { self.hashProgress = fraction }
        let tenth = Int(fraction * 10)
        if self.options.auto && tenth != lastPrinted { lastPrinted = tenth; print("[setup] hashing \(tenth * 10)%"); fflush(stdout) }
      }
      guard let digest = digest else {
        onMain { self.discWarning = "Could not read the disc image." }
        return false
      }
      say("SHA-256: \(digest) (\(formatDuration(Date().timeIntervalSince(started))))")
      state.isoPath = path; state.isoSize = size; state.isoModified = modified; state.isoSHA256 = digest
      state.save(paths)
    }
    let knownName = digest.flatMap { Setup.knownISOs[$0] }
    say(knownName.map { "Known disc image: \($0)" } ?? "Warning: unknown disc image (not one of the tested ones); it may still work.")
    onMain {
      self.isoHash = digest
      self.hashProgress = nil
      self.knownDisc = knownName
      self.discWarning = knownName != nil ? nil : "This image's SHA-256 does not match a tested Fable II disc image. It may still work; continue if you like."
    }
    return true
  }

  // MARK: - Step 3-5: extract, build, Dev Launcher

  /// Everything after the disc step. Returns true on success.
  @discardableResult
  func install() -> Bool {
    cancelled = false
    openLog()
    let started = Date()
    onMain {
      self.phase = .working; self.busy = true; self.errorLines = []
      self.extractStatus = .pending; self.buildStatus = .pending; self.launcherStatus = .pending
      self.warmStatus = .pending
      self.stages = Stage.all
    }
    defer { onMain { self.busy = false } }

    // Values set from the window (main queue) before this background step started.
    let (chosenISO, useExisting, hash, warm) = DispatchQueue.main.sync { (isoPath, useExistingFiles, isoHash, warmShaders) }
    currentHash = hash

    // Extract
    if let iso = chosenISO, !useExisting {
      onMain { self.extractStatus = .running }
      if !extract(iso) { return fail("Extracting the game files failed.") }
    } else if gameFilesPresent {
      say("Using the game files already in the project folder.")
      onMain { self.extractStatus = .skipped }
    } else {
      return fail("No game files: choose your Fable II disc image first.")
    }
    if options.extractOnly { return finish(started) }

    // Build
    if state.buildSucceeded == true && FileManager.default.fileExists(atPath: paths.gameApp.path) {
      say("Fable II.app is already built; skipping the build (use Rebuild to build again).")
      onMain { self.buildStatus = .skipped }
    } else {
      onMain { self.buildStatus = .running }
      if !build() { return fail("The build failed.") }
    }

    // Launcher (built into out/dev-launcher; copied to the project folder below)
    if state.launcherBuilt == true && FileManager.default.fileExists(atPath: paths.launcherApp.path) {
      onMain { self.launcherStatus = .skipped }
    } else {
      onMain { self.launcherStatus = .running; self.activity = "Building the launcher…"; self.progress = nil }
      say("Building the launcher")
      var launcherEnv = tools.variables
      launcherEnv["FABLE2_BUILD_ROLE"] = "play"  // never waits for agent perf runs
      let status = stream("/bin/zsh", [paths.launcherScript.path], env: launcherEnv)
      guard status == 0 else {
        onMain { self.launcherStatus = .failed }
        return fail("Building the launcher failed.")
      }
      state.launcherBuilt = true
      state.save(paths)
      onMain { self.launcherStatus = .ok }
    }

    // Copies of both apps in the project folder, next to Install.command (gitignored).
    // The builds in out/ stay where they are for the dev and test tools.
    for (source, target) in [(paths.gameApp, paths.rootGameApp), (paths.launcherApp, paths.rootLauncherApp)] {
      if !placeCopy(source, at: target) { return fail("Could not copy \(target.lastPathComponent) to the project folder.") }
    }
    try? FileManager.default.removeItem(at: paths.oldRootLauncherApp)  // renamed "Fable II Launcher.app" 2026-10-15

    // Shader warm-up (optional, never fatal): one background session of the new build.
    if !warm {
      onMain { self.warmStatus = .skipped }
    } else if state.shadersWarmed == true && options.warmStateRoot == nil {
      say("Shaders were already prepared by an earlier setup run; skipping.")
      onMain { self.warmStatus = .skipped }
    } else {
      warmShaderCache()
    }
    return finish(started)
  }

  /// Run tools/fable2_cache_warm.py --new-game: the game boots in a background window (muted, no
  /// focus, no keyboard or controller input, pipelines compiled before drawing), starts a new game for about 45 s and closes; the shaders
  /// it compiled go into the player's shader cache (~/Library/Application Support/Fable II Recomp/cache).
  /// About 2 minutes. A failure only means the first play compiles them instead.
  private func warmShaderCache() {
    guard let python = tools.find("python3") else { say("python3 not found; skipping the shader warm-up"); return }
    onMain { self.warmStatus = .running; self.activity = "Preparing shaders: the game compiles the shaders from your disc, then runs muted in a background window for about 2 minutes and closes itself…"; self.progress = nil }
    say("Preparing shaders: \(python) tools/fable2_cache_warm.py --new-game")
    var args = [paths.warmScript.path, "--new-game"]
    if let root = options.warmStateRoot { args += ["--state-root", root] }
    var env = tools.variables
    env["PYTHONUNBUFFERED"] = "1"
    let status = stream(python, args, env: env)
    if status == 0 {
      if options.warmStateRoot == nil { state.shadersWarmed = true; state.save(paths) }
      onMain { self.warmStatus = .ok }
    } else {
      say("The shader warm-up did not finish (exit \(status)); the game prepares them while you play instead.")
      onMain { self.warmStatus = .warning }
    }
  }

  /// Copy an app bundle from out/ to the project folder when the copy is missing or older than
  /// the build. The new copy is made under a temporary name and swapped in, so an old copy stays
  /// intact until the new one is complete.
  private func placeCopy(_ source: URL, at target: URL) -> Bool {
    let fm = FileManager.default
    let name = target.lastPathComponent
    func stamp(_ app: URL) -> Date? {
      (try? fm.attributesOfItem(atPath: app.appendingPathComponent("Contents/Info.plist").path))?[.modificationDate] as? Date
    }
    guard let built = stamp(source) else { say("\(source.path) is missing."); return false }
    if (try? target.resourceValues(forKeys: [.isSymbolicLinkKey]))?.isSymbolicLink == true {
      say("\(name) in the project folder is a symbolic link; leaving it alone.")
      return false
    }
    if let placed = stamp(target), placed >= built {
      say("\(name) in the project folder is up to date.")
      return true
    }
    if fm.fileExists(atPath: target.path) {
      // Only replace a bundle this setup placed (same bundle identifier as the build).
      let ours = Bundle(url: source)?.bundleIdentifier
      guard let theirs = Bundle(url: target)?.bundleIdentifier, theirs == ours else {
        say("\(name) in the project folder is not a copy of this build; leaving it alone.")
        return false
      }
    }
    say("Copying \(name) to the project folder")
    let tag = UUID().uuidString.prefix(8)
    let staging = target.deletingLastPathComponent().appendingPathComponent(".\(name).new-\(tag)")
    let previous = target.deletingLastPathComponent().appendingPathComponent(".\(name).old-\(tag)")
    guard stream("/usr/bin/ditto", [source.path, staging.path]) == 0 else {
      try? fm.removeItem(at: staging)
      return false
    }
    do {
      if fm.fileExists(atPath: target.path) {
        try fm.moveItem(at: target, to: previous)
        do { try fm.moveItem(at: staging, to: target) } catch {
          try? fm.moveItem(at: previous, to: target)
          throw error
        }
        try? fm.removeItem(at: previous)
      } else {
        try fm.moveItem(at: staging, to: target)
      }
    } catch {
      say("Could not place \(name): \(error.localizedDescription)")
      try? fm.removeItem(at: staging)
      return false
    }
    return true
  }

  private func finish(_ started: Date) -> Bool {
    let total = Date().timeIntervalSince(started)
    say("Done in \(formatDuration(total)).")
    if let version = gameVersion { say("Version: \(version)") }
    say("Game: \(paths.rootGameApp.path)")
    say("Launcher: \(paths.rootLauncherApp.path)")
    onMain { self.totalTime = total; self.phase = .finished; self.activity = ""; self.progress = nil }
    return true
  }

  private func fail(_ title: String) -> Bool {
    say(cancelled ? "Cancelled." : title)
    // Last error-looking lines of the log (falls back to the last lines).
    var tail: [String] = []
    if let url = DispatchQueue.main.sync(execute: { logURL }), let text = try? String(contentsOf: url, encoding: .utf8) {
      tail = text.split(whereSeparator: { $0 == "\n" || $0 == "\r" }).suffix(400).map(String.init)
    }
    let pattern = try! NSRegularExpression(pattern: #"(?i)\berror\b|fatal|failed|traceback|not found|no such file|missing|mismatch|denied"#)
    var errors = tail.filter { pattern.firstMatch(in: $0, range: NSRange($0.startIndex..., in: $0)) != nil }
    if errors.isEmpty { errors = Array(tail.suffix(25)) }
    let shown = Array(errors.suffix(25))
    if options.auto { shown.forEach { print("[setup]   " + $0) } }
    onMain {
      self.errorTitle = self.cancelled ? "Cancelled." : title
      self.errorLines = shown
      self.phase = .failed
      self.activity = ""
      self.progress = nil
    }
    return false
  }

  /// Copy default.xex, data/, $SystemUpdate/ and nxeart from the disc into the project root.
  /// Files that already exist with the right size are kept, so this resumes after an interruption.
  private func extract(_ iso: String) -> Bool {
    let fm = FileManager.default
    do {
      let disc = try DiscImage(path: iso)
      let root = try disc.rootEntries().filter { Setup.discEntries.contains($0.name) }
      var todo: [DiscEntry] = []
      var kept = 0
      for file in root.flatMap({ $0.files }) {
        // Only the four gitignored root entries are ever written (checked again here).
        guard let top = file.path.split(separator: "/").first, Setup.discEntries.contains(String(top)),
              !file.path.split(separator: "/").contains("..") else { throw DiscError.badName(file.path) }
        let dest = paths.root.appendingPathComponent(file.path)
        let existing = (try? fm.attributesOfItem(atPath: dest.path)[.size] as? NSNumber)?.uint64Value
        if existing == file.size || (existing != nil && Setup.buildModified.contains(file.path)) { kept += 1; continue }
        todo.append(file)
      }
      let total = todo.reduce(0) { $0 + $1.size }
      if todo.isEmpty {
        say("Game files are already in place (\(kept) files verified by size); nothing to extract.")
        onMain { self.extractStatus = .skipped }
        state.extractedFromSHA256 = currentHash
        state.save(paths)
        return true
      }
      say("Extracting \(todo.count) files (\(formatBytes(total))) into \(paths.root.path)" + (kept > 0 ? "; \(kept) already present" : ""))
      let started = Date()
      var done: UInt64 = 0
      var lastUpdate = Date.distantPast
      var lastPrinted = -1
      for file in todo {
        if cancelled { return false }
        try disc.extract(file, to: paths.root.appendingPathComponent(file.path)) { bytes in
          done += bytes
          if Date().timeIntervalSince(lastUpdate) > 0.2 {
            lastUpdate = Date()
            let fraction = Double(done) / Double(max(total, 1))
            let rate = Double(done) / max(Date().timeIntervalSince(started), 0.1)
            let text = "Extracting \(file.path) — \(formatBytes(done)) of \(formatBytes(total)) (\(formatBytes(UInt64(rate)))/s)"
            self.onMain { self.activity = text; self.progress = fraction }
            let tenth = Int(fraction * 10)
            if self.options.auto && tenth != lastPrinted { lastPrinted = tenth; print("[setup] extract \(tenth * 10)%"); fflush(stdout) }
          }
        }
      }
      say("Extracted \(todo.count) files in \(formatDuration(Date().timeIntervalSince(started))).")
      state.extractedFromSHA256 = currentHash
      state.save(paths)
      onMain { self.extractStatus = .ok; self.progress = nil }
      return true
    } catch {
      say("Extraction error: \(error.localizedDescription)")
      onMain { self.extractStatus = .failed }
      return false
    }
  }

  private func build() -> Bool {
    guard let python = tools.find("python3") else { say("python3 not found"); return false }
    var env = tools.variables
    let jobs = ProcessInfo.processInfo.activeProcessorCount
    env["FABLE2_BUILD_JOBS"] = String(jobs)
    env["FABLE2_BUILD_ROLE"] = "play"  // the player's own build: never waits for agent perf runs (local agent docs)
    env["PYTHONUNBUFFERED"] = "1"
    say("Building with \(jobs) jobs: \(python) tools/build_macos_metal.py")
    onMain { self.activity = "Starting the build…"; self.progress = nil }
    let started = Date()
    let status = stream(python, [paths.buildScript.path], env: env) { line in self.parseBuildLine(line) }
    let now = Date()
    onMain {
      for i in self.stages.indices where self.stages[i].status == .running {
        self.stages[i].end = now
        self.stages[i].status = status == 0 ? .ok : .failed
      }
    }
    // Stage timings for the log (read after the main-queue update above).
    DispatchQueue.main.sync {
      let timings = self.stages.compactMap { s in s.duration.map { "\(s.id) \(formatDuration($0))" } }
      self.say("Stage times: " + timings.joined(separator: ", "))
    }
    say("Build \(status == 0 ? "finished" : "failed (exit \(status))") after \(formatDuration(now.timeIntervalSince(started))).")
    guard status == 0, FileManager.default.fileExists(atPath: paths.gameApp.path) else {
      onMain { self.buildStatus = .failed }
      return false
    }
    state.buildSucceeded = true
    state.save(paths)
    onMain { self.buildStatus = .ok }
    return true
  }

  private func parseBuildLine(_ line: String) {
    let range = NSRange(line.startIndex..., in: line)
    if let match = labelPattern.firstMatch(in: line, range: range),
       let keyRange = Range(match.range(at: 1), in: line) {
      let key = String(line[keyRange])
      let now = Date()
      lastPrintedPercent = -1
      onMain {
        guard let index = self.stages.firstIndex(where: { $0.id == key }) else { return }
        for i in self.stages.indices where self.stages[i].status == .running {
          self.stages[i].status = .ok
          self.stages[i].end = now
        }
        if self.stages[index].start == nil { self.stages[index].start = now }
        self.stages[index].end = nil
        self.stages[index].status = .running
        self.activity = self.stages[index].title
        self.progress = nil
      }
      if options.auto, let stage = Stage.all.first(where: { $0.id == key }) {
        print("[setup] stage: \(stage.title)"); fflush(stdout)
      }
      return
    }
    if let match = percentPattern.firstMatch(in: line, range: range),
       let numberRange = Range(match.range(at: 1), in: line), let percent = Int(line[numberRange]) {
      onMain { self.progress = Double(percent) / 100 }
      if options.auto && percent / 25 != lastPrintedPercent / 25 {
        lastPrintedPercent = percent
        print("[setup]   \(percent)%"); fflush(stdout)
      }
    }
  }

  // MARK: - Helpers for the window

  func work(_ body: @escaping () -> Void) { queue.async(execute: body) }

  func copyLog() {
    guard let url = logURL, let text = try? String(contentsOf: url, encoding: .utf8) else { return }
    let clipped = text.count > 2_000_000 ? "…\n" + String(text.suffix(2_000_000)) : text
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(clipped, forType: .string)
  }

  func revealLog() {
    if let url = logURL { NSWorkspace.shared.activateFileViewerSelecting([url]) }
  }

  func rebuild() {
    state.buildSucceeded = false
    state.launcherBuilt = false
    state.shadersWarmed = false
    state.save(paths)
    useExistingFiles = true
    work { self.install() }
  }

  // MARK: - Unattended run (--auto)

  func runAutomatically() {
    work {
      self.runChecks()
      var checks = DispatchQueue.main.sync { self.checks }
      if checks.contains(where: { $0.id == "cmake" && $0.status == .failed }) {
        self.downloadCMake()
        self.runChecks()
        checks = DispatchQueue.main.sync { self.checks }
      }
      let failed = checks.filter { $0.status == .failed }
      for check in checks { print("[setup] check \(check.status == .ok ? "ok  " : check.status == .warning ? "warn" : "FAIL") \(check.title): \(check.detail)") }
      if !failed.isEmpty { self.exitAuto(1) ; return }
      self.onMain { self.phase = .disc }
      if let iso = self.options.iso {
        if !self.prepareISO(iso) { self.exitAuto(1); return }
      } else if self.gameFilesPresent {
        self.onMain { self.useExistingFiles = true }
      } else {
        print("[setup] No --iso given and no game files in the project folder."); self.exitAuto(1); return
      }
      self.exitAuto(self.install() ? 0 : 1)
    }
  }

  private func exitAuto(_ code: Int32) {
    print("[setup] exit \(code)"); fflush(stdout)
    logHandle?.synchronizeFile()
    if options.stay { return }
    DispatchQueue.main.asyncAfter(deadline: .now() + 1) { exit(code) }
  }
}
