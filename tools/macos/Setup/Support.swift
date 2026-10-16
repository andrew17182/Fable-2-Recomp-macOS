// Shared helpers for "Fable II Setup": project paths, tool lookup, child processes, saved state.
import CryptoKit
import Foundation

enum Setup {
  /// SHA-256 of disc images known to work: the Fable II GOTY image upstream started from
  /// (README "Origin") and the Platinum Hits image this macOS port was developed with.
  static let knownISOs = [
    "685a0d3bea9718812f17bcd155907a5359a548b6d3d8342dd2a6c944f45e35ff": "Fable II Game of the Year (upstream's image)",
    "2cdaafead95680e2c6fe8886a89f1ae3d5e41549857c7fc125a12aab1cb99ad9": "Fable II Platinum Hits (this port's test image)",
  ]
  /// Root entries copied from the disc. All are gitignored in the project root.
  static let discEntries = ["default.xex", "data", "$SystemUpdate", "nxeart"]
  static let requiredDiscEntries: Set<String> = ["default.xex", "data"]
  /// Files the build changes after extraction (the build appends its Lua helpers to
  /// dir.manifest); an existing copy counts as present.
  static let buildModified: Set<String> = ["data/dir.manifest"]
  /// Rough size of everything the first build writes under out/ and generated/.
  static let buildBytes: UInt64 = 6_000_000_000  // includes ~1 GB for the SDK built from source (out/sdk-src)

  // Portable CMake (official Kitware release; SHA-256 from cmake-4.1.2-SHA-256.txt).
  static let cmakeVersion = "4.1.2"
  static let cmakeURL = "https://github.com/Kitware/CMake/releases/download/v4.1.2/cmake-4.1.2-macos-universal.tar.gz"
  static let cmakeSHA256 = "3be85f5b999e327b1ac7d804cbc9acd767059e9f603c42ec2765f6ab68fbd367"
}

/// Command-line options (also used for unattended test runs).
struct Options {
  var iso: String?
  var auto = false
  var extractOnly = false
  var cleanEnv = false      // ignore the login shell PATH and Homebrew (simulates a fresh Mac)
  var rebuild = false
  var stay = false          // --auto: keep the window open at the end
  var list: String?
  var root: String?
  var warmShaders = true    // --no-warm-shaders: skip the shader warm-up step
  var warmStateRoot: String?  // --warm-state-root DIR (tests): warm this state folder, not Application Support

  static func parse(_ args: [String]) -> Options {
    var o = Options()
    var i = 1
    func value() -> String? { i += 1; return i < args.count ? args[i] : nil }
    while i < args.count {
      switch args[i] {
      case "--iso": o.iso = value()
      case "--auto": o.auto = true
      case "--extract-only": o.extractOnly = true
      case "--clean-env": o.cleanEnv = true
      case "--rebuild": o.rebuild = true
      case "--stay": o.stay = true
      case "--list": o.list = value()
      case "--root": o.root = value()
      case "--no-warm-shaders": o.warmShaders = false
      case "--warm-state-root": o.warmStateRoot = value()
      default: break  // ignore -psn_ and other system arguments
      }
      i += 1
    }
    return o
  }
}

struct Paths {
  let root: URL
  var out: URL { root.appendingPathComponent("out") }
  var setup: URL { out.appendingPathComponent("setup") }
  var state: URL { setup.appendingPathComponent("state.json") }
  var cmakeHome: URL { out.appendingPathComponent("tools/cmake") }
  var cmakeBin: URL { cmakeHome.appendingPathComponent("CMake.app/Contents/bin") }
  var gameApp: URL { out.appendingPathComponent("build/mac-arm64-metal-release/Fable II.app") }
  var launcherApp: URL { out.appendingPathComponent("dev-launcher/Fable II Launcher.app") }
  /// Copies placed in the project folder itself (next to Install.command) at the end of setup.
  var rootGameApp: URL { root.appendingPathComponent("Fable II.app") }
  var rootLauncherApp: URL { root.appendingPathComponent("Fable II Launcher.app") }
  /// The launcher's name before 2026-10-15 ("Fable II Dev Launcher.app"); setup removes that copy.
  var oldRootLauncherApp: URL { root.appendingPathComponent("Fable II Dev Launcher.app") }
  var buildScript: URL { root.appendingPathComponent("tools/build_macos_metal.py") }
  var launcherScript: URL { root.appendingPathComponent("tools/macos/build_dev_launcher.sh") }
  var warmScript: URL { root.appendingPathComponent("tools/fable2_cache_warm.py") }

  /// FABLE2_REPO_ROOT, --root, the folder holding this app (out/setup/<app> -> project), or
  /// the Fable2RepoRoot Info.plist key written by Install.command.
  static func find(_ options: Options) -> Paths? {
    let fm = FileManager.default
    func isRoot(_ url: URL) -> Bool {
      fm.fileExists(atPath: url.appendingPathComponent("tools/build_macos_metal.py").path)
    }
    var candidates: [URL] = []
    if let env = ProcessInfo.processInfo.environment["FABLE2_REPO_ROOT"] { candidates.append(URL(fileURLWithPath: env)) }
    if let root = options.root { candidates.append(URL(fileURLWithPath: root)) }
    var url = Bundle.main.bundleURL
    for _ in 0..<6 { url.deleteLastPathComponent(); candidates.append(url) }
    if let plist = Bundle.main.object(forInfoDictionaryKey: "Fable2RepoRoot") as? String {
      candidates.append(URL(fileURLWithPath: plist))
    }
    return candidates.first(where: isRoot).map { Paths(root: $0.standardizedFileURL) }
  }
}

/// Saved progress, so a second run skips finished steps.
struct SavedState: Codable {
  var isoPath: String?
  var isoSize: UInt64?
  var isoModified: Double?
  var isoSHA256: String?
  var extractedFromSHA256: String?
  var buildSucceeded: Bool?
  var launcherBuilt: Bool?
  var shadersWarmed: Bool?

  static func load(_ paths: Paths) -> SavedState {
    guard let data = try? Data(contentsOf: paths.state),
          let state = try? JSONDecoder().decode(SavedState.self, from: data) else { return SavedState() }
    return state
  }

  func save(_ paths: Paths) {
    try? FileManager.default.createDirectory(at: paths.setup, withIntermediateDirectories: true)
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    try? encoder.encode(self).write(to: paths.state, options: .atomic)
  }
}

/// The PATH used for every child process.
struct ToolEnvironment {
  let path: [String]
  let developerDir: String?   // set when xcode-select points at the command-line tools only

  static func make(paths: Paths, cleanEnv: Bool, developerDir: String?) -> ToolEnvironment {
    var parts = [paths.cmakeBin.path]
    if !cleanEnv {
      parts += loginShellPath()
      parts += ["/opt/homebrew/bin", "/usr/local/bin", "/Applications/CMake.app/Contents/bin"]
    }
    parts += ["/usr/bin", "/bin", "/usr/sbin", "/sbin"]
    var seen = Set<String>()
    return ToolEnvironment(path: parts.filter { !$0.isEmpty && seen.insert($0).inserted },
                           developerDir: developerDir)
  }

  /// PATH from the user's login shell (Finder-launched apps get only /usr/bin:/bin:...).
  static func loginShellPath() -> [String] {
    let shell = ProcessInfo.processInfo.environment["SHELL"] ?? "/bin/zsh"
    let result = Run.capture(shell, ["-lc", "printf %s \"$PATH\""], timeout: 5)
    guard result.status == 0 else { return [] }
    return result.output.split(separator: ":").map(String.init)
  }

  func find(_ tool: String) -> String? {
    for dir in path {
      let candidate = (dir as NSString).appendingPathComponent(tool)
      if FileManager.default.isExecutableFile(atPath: candidate) { return candidate }
    }
    return nil
  }

  var variables: [String: String] {
    var env = ProcessInfo.processInfo.environment
    env["PATH"] = path.joined(separator: ":")
    if let developerDir = developerDir { env["DEVELOPER_DIR"] = developerDir }
    return env
  }
}

enum Run {
  /// Run a short command and capture its combined output.
  static func capture(_ executable: String, _ args: [String], env: [String: String]? = nil,
                      cwd: URL? = nil, timeout: TimeInterval = 60) -> (status: Int32, output: String) {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: executable)
    process.arguments = args
    if let env = env { process.environment = env }
    if let cwd = cwd { process.currentDirectoryURL = cwd }
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    process.standardInput = FileHandle.nullDevice
    do { try process.run() } catch { return (-1, error.localizedDescription) }
    var data = Data()
    let reader = DispatchQueue(label: "capture")
    let group = DispatchGroup()
    group.enter()
    reader.async { data = pipe.fileHandleForReading.readDataToEndOfFile(); group.leave() }
    if group.wait(timeout: .now() + timeout) == .timedOut {
      process.terminate()
      return (-1, "timed out")
    }
    process.waitUntilExit()
    return (process.terminationStatus, String(decoding: data, as: UTF8.self))
  }

  /// Terminate a process and all of its descendants (make/clang children of the build).
  static func killTree(_ pid: Int32) {
    let children = capture("/usr/bin/pgrep", ["-P", String(pid)], timeout: 5).output
      .split(whereSeparator: \.isNewline).compactMap { Int32($0) }
    for child in children { killTree(child) }
    kill(pid, SIGTERM)
  }
}

enum Hashing {
  /// Streaming SHA-256 with progress (fraction 0...1). Returns nil if cancelled/unreadable.
  static func sha256(of url: URL, progress: (Double) -> Void) -> String? {
    guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
    defer { try? handle.close() }
    let total = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? UInt64) ?? 0
    var hasher = SHA256()
    var done: UInt64 = 0
    var lastReport = Date.distantPast
    while true {
      let chunk = autoreleasepool { try? handle.read(upToCount: 16 << 20) } ?? nil
      guard let data = chunk, !data.isEmpty else { break }
      hasher.update(data: data)
      done += UInt64(data.count)
      if Date().timeIntervalSince(lastReport) > 0.1, total > 0 {
        progress(Double(done) / Double(total))
        lastReport = Date()
      }
    }
    progress(1)
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
  }
}

func formatBytes(_ bytes: UInt64) -> String {
  ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
}

func formatDuration(_ seconds: TimeInterval) -> String {
  let s = Int(seconds.rounded())
  return s >= 60 ? "\(s / 60) min \(s % 60) s" : "\(s) s"
}
