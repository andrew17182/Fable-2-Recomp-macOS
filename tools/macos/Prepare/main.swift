// FableIIPrepare: first-run game-data helper inside the portable "Fable II.app"
// (tools/fable2_portable.py, docs/portable-build.md). AppLauncher.m runs it when the app has no
// game data yet, or when the player asks to choose the disc image again.
//
// It reuses the setup app's disc reader (tools/macos/Setup/GDFX.swift) and hashing
// (Support.swift); it is compiled together with those two files.
//
//   FableIIPrepare --target <GameData> [--iso <image>] [--expected-xex <sha256>] [--lua <dir>]
//                  [--result <plist>] [--headless] [--progress] [--stop-on-mismatch]
//       Ask for the disc image (unless --iso), extract default.xex, data/, $SystemUpdate/ and
//       nxeart into <GameData>, then stage our side files (below). Exit 0 = ready, 3 = cancelled,
//       1 = failed (the error was shown or printed), 4 = default.xex differs (--stop-on-mismatch).
//       --progress (with --headless; the player app, tools/macos/Player/main.swift, shows its own
//       window): also print "[progress] <fraction 0-1> <text>" about five times a second.
//       --stop-on-mismatch (with --headless): stop with exit 4 right after default.xex is extracted
//       and hashed when it differs from --expected-xex, so the caller can ask the player; running
//       again without it continues anyway (the files already copied are kept).
//   FableIIPrepare --stage --target <GameData> --lua <dir>
//       Fast, no UI; run by the launcher before every portable launch:
//         - our Lua scripts -> data/scripts/recomp/ (+ their data/dir.manifest lines, as
//           tools/ensure_recomp_manifest.cmake does for a normal build),
//         - the skip-intro stand-in movies out/skipintro/{ms,lh,mw}.bik: 2-frame trims of the
//           disc's logo movies (game content, so they are made here from the extracted files,
//           never shipped; same bytes as out/validation/skip-intro-build/trim_bik.py).
//   FableIIPrepare --trim-bik <source.bik> <dest.bik> <frames>     (testing)
//   FableIIPrepare --target <GameData> --shader-lists <recipe.json> --cache <cache folder>
//       Rebuild the shader and pipeline lists from a shader recipe and the disc's shader bank (ShaderLists.swift,
//       docs/offline-shader-seed.md); the player app then runs the headless shader preparation on them.
//
// The game data folder gets a marker (.fable2-portable.json) once everything is in place; the
// launcher re-runs the full prepare (which resumes) while it is missing.
import AppKit
import UniformTypeIdentifiers

let markerName = ".fable2-portable.json"
/// Logo movies redirected by src/core/fable2_skip_intro.h (game:\out\skipintro\<short>.bik).
let skipIntroMovies = [("microsoft_logo.bik", "ms.bik"), ("lionhead_logo.bik", "lh.bik"),
                       ("middlewarelogos.bik", "mw.bik")]
let skipIntroFrames = 2
/// Written by the game at runtime (src/core/fable2_game_message.h): created if missing, never replaced.
let runtimeWrittenLua: Set<String> = ["recomp_notify_text.lua"]

struct PrepareOptions {
  var target: String?
  var iso: String?
  var expectedXex: String?
  var lua: String?
  var result: String?
  var headless = false
  var progressLines = false
  var stopOnMismatch = false
  var stage = false
  var trim: [String] = []
  var shaderRecipe: String?
  var cache: String?

  static func parse(_ args: [String]) -> PrepareOptions {
    var o = PrepareOptions()
    var i = 1
    func value() -> String? { i += 1; return i < args.count ? args[i] : nil }
    while i < args.count {
      switch args[i] {
      case "--target": o.target = value()
      case "--iso": o.iso = value()
      case "--expected-xex": o.expectedXex = value()?.lowercased()
      case "--lua": o.lua = value()
      case "--result": o.result = value()
      case "--headless": o.headless = true
      case "--progress": o.progressLines = true
      case "--stop-on-mismatch": o.stopOnMismatch = true
      case "--stage": o.stage = true
      case "--shader-lists": o.shaderRecipe = value()
      case "--cache": o.cache = value()
      case "--trim-bik":
        o.trim = [value(), value(), value()].compactMap { $0 }
      default: break
      }
      i += 1
    }
    return o
  }
}

func log(_ text: String) {
  print("[prepare] \(text)")
  fflush(stdout)
}

enum PrepareError: LocalizedError {
  case message(String)
  var errorDescription: String? { if case .message(let text) = self { return text }; return nil }
}

// MARK: - Bink trim (port of out/validation/skip-intro-build/trim_bik.py)

/// Keep the first `n` frames of a Bink 1 file: header, frame index and sizes rewritten.
func trimBink(_ d: [UInt8], frames n: Int) throws -> [UInt8] {
  func u32(_ at: Int) -> Int { Int(DiscImage.u32(d, at)) }
  func bad() -> PrepareError { .message("Not a Bink movie this tool can trim") }
  guard d.count >= 0x2C, d[0] == 0x42, d[1] == 0x49, d[2] == 0x4B, n >= 1 else { throw bad() }
  let frames = u32(8), tracks = u32(0x28)
  let headerEnd = 0x2C + tracks * 12
  guard n <= frames, headerEnd + (frames + 1) * 4 <= d.count else { throw bad() }
  let index = (0...frames).map { u32(headerEnd + 4 * $0) }
  let offsets = index.map { $0 & ~1 }
  let newTableEnd = headerEnd + (n + 1) * 4
  let delta = (headerEnd + (frames + 1) * 4) - newTableEnd
  var newIndex = (0..<n).map { ((index[$0] & ~1) - delta) | (index[$0] & 1) }
  newIndex.append(offsets[n] - delta)
  guard offsets[0] <= offsets[n], offsets[n] <= d.count else { throw bad() }
  let body = d[offsets[0]..<offsets[n]]
  let largest = (0..<n).map { offsets[$0 + 1] - offsets[$0] }.max() ?? 0
  var out = Array(d[0..<headerEnd])
  let total = newTableEnd + body.count
  func put(_ value: Int, _ at: Int) {
    for k in 0..<4 { out[at + k] = UInt8((value >> (8 * k)) & 0xFF) }
  }
  put(total - 8, 4); put(n, 8); put(largest, 12); put(n, 16)
  for value in newIndex { for k in 0..<4 { out.append(UInt8((value >> (8 * k)) & 0xFF)) } }
  out.append(contentsOf: body)
  guard out.count == total else { throw bad() }
  return out
}

func writeAtomically(_ bytes: [UInt8], to url: URL) throws {
  try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
  try Data(bytes).write(to: url, options: .atomic)
}

// MARK: - Side files staged into the game data folder

/// Lua scripts, their dir.manifest lines and the skip-intro movies. Idempotent and fast.
func stage(target: URL, lua: URL?) throws {
  let fm = FileManager.default
  if let lua = lua {
    let scripts = target.appendingPathComponent("data/scripts/recomp")
    try fm.createDirectory(at: scripts, withIntermediateDirectories: true)
    let names = (try fm.contentsOfDirectory(atPath: lua.path)).filter { $0.hasSuffix(".lua") }.sorted()
    var copied = 0
    for name in names {
      let source = lua.appendingPathComponent(name), dest = scripts.appendingPathComponent(name)
      let exists = fm.fileExists(atPath: dest.path)
      if exists && (runtimeWrittenLua.contains(name) || fm.contentsEqual(atPath: source.path, andPath: dest.path)) { continue }
      try writeAtomically(Array(try Data(contentsOf: source)), to: dest)
      copied += 1
    }
    // data/dir.manifest: the game's VFS only sees listed files. Same keys and CRLF lines as
    // tools/ensure_recomp_manifest.cmake (the engine's startup self-heal is a second net).
    let manifest = target.appendingPathComponent("data/dir.manifest")
    var content = (try? Data(contentsOf: manifest)).map { Array($0) } ?? []
    let text = String(decoding: content, as: UTF8.self)
    // (A Swift Character "\r\n" is one grapheme: split on newline characters, not on "\n".)
    let have = Set(text.components(separatedBy: .newlines).filter { !$0.isEmpty })
    let missing = names.map { "scripts\\recomp\\" + $0 }.filter { !have.contains($0) }
    if !missing.isEmpty {
      if let last = content.last, last != 0x0A { content += Array("\r\n".utf8) }
      for key in missing { content += Array((key + "\r\n").utf8) }
      try writeAtomically(content, to: manifest)
    }
    if copied > 0 || !missing.isEmpty { log("staged \(copied) Lua script(s), \(missing.count) manifest line(s)") }
  }
  let videos = target.appendingPathComponent("data/art/videos")
  let out = target.appendingPathComponent("out/skipintro")
  var made = 0
  for (movie, short) in skipIntroMovies {
    let dest = out.appendingPathComponent(short)
    if fm.fileExists(atPath: dest.path) { continue }
    // The disc may list the name in another case.
    let listing = (try? fm.contentsOfDirectory(atPath: videos.path)) ?? []
    guard let found = listing.first(where: { $0.lowercased() == movie }) else {
      log("skip intro: \(movie) not found, the full movie will play")
      continue
    }
    let source = Array(try Data(contentsOf: videos.appendingPathComponent(found)))
    try writeAtomically(try trimBink(source, frames: skipIntroFrames), to: dest)
    made += 1
  }
  if made > 0 { log("skip intro: made \(made) stand-in movie(s) in out/skipintro") }
}

// MARK: - Full prepare (disc image -> game data folder)

final class Progress {
  let headless: Bool
  var lines = false
  var window: NSWindow?
  var label: NSTextField?
  var detail: NSTextField?
  var bar: NSProgressIndicator?
  var cancelled = false
  private var lastTenth = -1

  init(headless: Bool) { self.headless = headless }

  func show() {
    guard !headless else { return }
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 460, height: 150),
                          styleMask: [.titled], backing: .buffered, defer: false)
    window.title = "Fable II"
    let label = NSTextField(labelWithString: "Preparing game files…")
    label.font = .boldSystemFont(ofSize: 13)
    label.frame = NSRect(x: 20, y: 105, width: 420, height: 20)
    let detail = NSTextField(labelWithString: "Reading the disc image…")
    detail.font = .systemFont(ofSize: 11)
    detail.textColor = .secondaryLabelColor
    detail.lineBreakMode = .byTruncatingMiddle
    detail.frame = NSRect(x: 20, y: 52, width: 420, height: 18)
    let bar = NSProgressIndicator(frame: NSRect(x: 20, y: 78, width: 420, height: 20))
    bar.isIndeterminate = false
    bar.minValue = 0; bar.maxValue = 1
    let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancelPressed))
    cancel.frame = NSRect(x: 360, y: 12, width: 80, height: 30)
    for view in [label, detail, bar, cancel] as [NSView] { window.contentView?.addSubview(view) }
    window.center()
    window.makeKeyAndOrderFront(nil)
    (self.window, self.label, self.detail, self.bar) = (window, label, detail, bar)
  }

  @objc func cancelPressed() { cancelled = true }

  func update(_ fraction: Double, _ text: String) {
    let tenth = Int(fraction * 10)
    if tenth != lastTenth { lastTenth = tenth; log("extract \(tenth * 10)% \(text)") }
    if lines { print("[progress] \(String(format: "%.4f", fraction)) \(text)"); fflush(stdout) }
    guard !headless else { return }
    DispatchQueue.main.async {
      self.bar?.doubleValue = fraction
      self.detail?.stringValue = text
    }
  }

  func close() { DispatchQueue.main.async { self.window?.orderOut(nil) } }
}

/// Modal alert on the main thread (prepare runs on a worker thread). Returns the button index.
func ask(_ title: String, _ text: String, _ buttons: [String], style: NSAlert.Style = .warning) -> Int {
  var answer = 0
  let body = {
    let alert = NSAlert()
    alert.alertStyle = style
    alert.messageText = title
    alert.informativeText = text
    for button in buttons { alert.addButton(withTitle: button) }
    answer = alert.runModal().rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
  }
  if Thread.isMainThread { body() } else { DispatchQueue.main.sync(execute: body) }
  return answer
}

func chooseImage() -> String? {
  var path: String?
  let body = {
    let panel = NSOpenPanel()
    panel.title = "Choose your Fable II disc image (.iso)"
    panel.message = "Choose your Fable II disc image (.iso). Its game files are copied into this Mac's "
      + "Application Support folder (about 7 GB); the image itself is only read."
    panel.prompt = "Use This Image"
    panel.canChooseFiles = true
    panel.canChooseDirectories = false
    panel.allowsMultipleSelection = false
    if let iso = UTType(filenameExtension: "iso") { panel.allowedContentTypes = [iso] }
    NSApp.activate(ignoringOtherApps: true)
    if panel.runModal() == .OK { path = panel.url?.path }
  }
  if Thread.isMainThread { body() } else { DispatchQueue.main.sync(execute: body) }
  return path
}

struct Marker: Codable {
  var isoPath: String
  var isoSize: UInt64
  var isoModified: Double
  var xexSHA256: String?
  var xexMatches: Bool?
  var complete: Bool
}

func readMarker(_ target: URL) -> Marker? {
  guard let data = try? Data(contentsOf: target.appendingPathComponent(markerName)) else { return nil }
  return try? JSONDecoder().decode(Marker.self, from: data)
}

func writeMarker(_ marker: Marker, _ target: URL) throws {
  let encoder = JSONEncoder()
  encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
  try encoder.encode(marker).write(to: target.appendingPathComponent(markerName), options: .atomic)
}

/// --stop-on-mismatch: default.xex differs from --expected-xex (exit 4).
struct MismatchStop: Error {}

/// Returns 0 (ready), 3 (cancelled), 4 (xex mismatch with --stop-on-mismatch) or 1 (failed).
func prepare(_ o: PrepareOptions, target: URL, progress: Progress) -> Int32 {
  let fm = FileManager.default
  let interactive = !o.headless
  var isoPath = o.iso
  while true {
    if isoPath == nil {
      guard interactive, let chosen = chooseImage() else { return 3 }
      isoPath = chosen
    }
    let iso = isoPath!
    do {
      log("disc image: \(iso)")
      let attributes = try fm.attributesOfItem(atPath: iso)
      let isoSize = (attributes[.size] as? NSNumber)?.uint64Value ?? 0
      let isoModified = (attributes[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
      let disc = try DiscImage(path: iso)
      let root = try disc.rootEntries().filter { Setup.discEntries.contains($0.name) }
      let names = Set(root.map { $0.name })
      guard Setup.requiredDiscEntries.isSubset(of: names) else {
        throw PrepareError.message("This image has no default.xex and data folder; it does not look like a Fable II disc.")
      }
      // A marker from a different image means sizes alone can't be trusted: extract everything again.
      var marker = readMarker(target)
      let sameImage = marker.map { $0.isoSize == isoSize && $0.isoPath == iso && $0.isoModified == isoModified } ?? true
      if !sameImage { log("game data came from another image; extracting everything again") }
      marker = Marker(isoPath: iso, isoSize: isoSize, isoModified: isoModified, xexSHA256: nil, xexMatches: nil,
                      complete: false)
      try fm.createDirectory(at: target, withIntermediateDirectories: true)
      try writeMarker(marker!, target)

      var todo: [DiscEntry] = []
      var kept = 0
      // default.xex first: it is hashed before the 7 GB copy.
      let files = root.flatMap { $0.files }.sorted { ($0.path == "default.xex" ? 0 : 1) < ($1.path == "default.xex" ? 0 : 1) }
      for file in files {
        guard let top = file.path.split(separator: "/").first, Setup.discEntries.contains(String(top)),
              !file.path.split(separator: "/").contains("..") else { throw DiscError.badName(file.path) }
        let dest = target.appendingPathComponent(file.path)
        let existing = (try? fm.attributesOfItem(atPath: dest.path)[.size] as? NSNumber)?.uint64Value
        if sameImage && (existing == file.size || (existing != nil && Setup.buildModified.contains(file.path))) {
          kept += 1; continue
        }
        todo.append(file)
      }
      let total = todo.reduce(0) { $0 + $1.size }
      let free = (try? target.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        .volumeAvailableCapacityForImportantUsage).flatMap { $0 }.map { UInt64($0) } ?? UInt64.max
      if total + 500_000_000 > free {
        throw PrepareError.message("Not enough free disk space: \(formatBytes(total)) is needed in \(target.path), "
          + "\(formatBytes(free)) is free.")
      }
      log("extracting \(todo.count) files (\(formatBytes(total))) into \(target.path); \(kept) already present")
      let started = Date()
      var done: UInt64 = 0
      var lastUpdate = Date.distantPast
      var checkedXex = false
      func checkXex() throws -> Bool {
        checkedXex = true
        let xex = target.appendingPathComponent("default.xex")
        guard let digest = Hashing.sha256(of: xex, progress: { _ in }) else { return true }
        marker!.xexSHA256 = digest
        guard let expected = o.expectedXex, !expected.isEmpty else { return true }
        marker!.xexMatches = digest == expected
        try writeMarker(marker!, target)
        if digest == expected { log("default.xex SHA-256 matches this build (\(digest))"); return true }
        log("WARNING: default.xex SHA-256 \(digest) differs from the one this build was made from (\(expected))")
        if o.stopOnMismatch { throw MismatchStop() }
        guard interactive else { return true }
        let answer = ask("This disc image differs from the one this build was made from",
                         "default.xex on this image has SHA-256\n\(digest)\nThis build was compiled from\n\(expected)\n\n"
                         + "The game may misbehave. You can continue anyway, or choose another image.",
                         ["Continue Anyway", "Choose Another Image", "Quit"])
        if answer == 0 { return true }
        if answer == 1 { isoPath = nil; return false }
        throw CancellationError()
      }
      for file in todo {
        if progress.cancelled { return 3 }
        if !checkedXex && file.path != "default.xex" { if try !checkXex() { break } }
        try disc.extract(file, to: target.appendingPathComponent(file.path)) { bytes in
          done += bytes
          if Date().timeIntervalSince(lastUpdate) > 0.2 {
            lastUpdate = Date()
            let rate = Double(done) / max(Date().timeIntervalSince(started), 0.1)
            progress.update(Double(done) / Double(max(total, 1)),
                            "\(file.path) — \(formatBytes(done)) of \(formatBytes(total)) (\(formatBytes(UInt64(rate)))/s)")
          }
        }
      }
      if isoPath == nil { continue }  // "Choose Another Image"
      if !checkedXex { if try !checkXex() { continue } }
      log("extracted \(todo.count) files in \(formatDuration(Date().timeIntervalSince(started)))")
      progress.update(1, "Finishing…")
      try stage(target: target, lua: o.lua.map { URL(fileURLWithPath: $0) })
      marker!.complete = true
      try writeMarker(marker!, target)
      if let result = o.result {
        let record: [String: Any] = ["GameDataRoot": target.path, "IsoPath": iso,
                                     "XexSHA256": marker!.xexSHA256 ?? "", "XexMatches": marker!.xexMatches ?? true]
        (record as NSDictionary).write(toFile: result, atomically: true)
      }
      return 0
    } catch is CancellationError {
      return 3
    } catch is MismatchStop {
      return 4
    } catch {
      log("error: \(error.localizedDescription)")
      guard interactive else { return 1 }
      let answer = ask("Fable II could not prepare its game files", error.localizedDescription,
                       ["Choose Another Image", "Quit"], style: .critical)
      if answer != 0 { return 1 }
      isoPath = nil
    }
  }
}

@main
struct PrepareMain {
  static func main() {
    let o = PrepareOptions.parse(CommandLine.arguments)
    if o.trim.count == 3, let n = Int(o.trim[2]) {
      do {
        let source = Array(try Data(contentsOf: URL(fileURLWithPath: o.trim[0])))
        try writeAtomically(try trimBink(source, frames: n), to: URL(fileURLWithPath: o.trim[1]))
        exit(0)
      } catch { log("error: \(error.localizedDescription)"); exit(1) }
    }
    guard let targetPath = o.target, targetPath.hasPrefix("/") else {
      log("usage: FableIIPrepare --target <absolute game data folder> [--iso <image>] | --stage ...")
      exit(2)
    }
    let target = URL(fileURLWithPath: targetPath)
    if let recipe = o.shaderRecipe, let cache = o.cache {
      do {
        let r = try ShaderLists.rebuild(recipe: URL(fileURLWithPath: recipe), gameData: target, cache: URL(fileURLWithPath: cache))
        print("[shader-lists] rebuilt \(r.ok) shaders from the disc's shader bank (\(r.mismatched) did not match)")
        exit(r.ok > 0 ? 0 : 1)
      } catch { log("shader lists error: \(error.localizedDescription)"); exit(1) }
    }
    if o.stage {
      do { try stage(target: target, lua: o.lua.map { URL(fileURLWithPath: $0) }); exit(0) }
      catch { log("stage error: \(error.localizedDescription)"); exit(1) }
    }
    let progress = Progress(headless: o.headless)
    progress.lines = o.progressLines
    if o.headless {
      exit(prepare(o, target: target, progress: progress))
    }
    let app = NSApplication.shared
    app.setActivationPolicy(.regular)
    // Background test sessions (FABLE2_TEST_BACKGROUND=1) must not take focus.
    if ProcessInfo.processInfo.environment["FABLE2_TEST_BACKGROUND"] != "1" { app.activate(ignoringOtherApps: true) }
    DispatchQueue.main.async {
      progress.show()
      DispatchQueue.global(qos: .userInitiated).async {
        let code = prepare(o, target: target, progress: progress)
        DispatchQueue.main.async { progress.close(); exit(code) }
      }
    }
    app.run()
  }
}
