// FableIIBenchmark: the main executable of "Fable II Benchmark.app" (tools/fable2_benchmark_kit.py,
// docs/benchmark-kit.md). Double-clicked on any Apple Silicon Mac, with no Xcode, Python or repo there.
//
// It runs the portable game (FableIILauncher -> fable_2, same bundle) once per arm in the built-in
// benchmark mode (src/core/fable2_benchmark.h: boot the bundled save, wait for gameplay, f2diag's
// headings8 camera plan, quit), then cuts each run's frame CSV by the frame ids the game recorded and
// writes report.json + report.txt to ~/Desktop/Fable II Benchmark/<model>-<date>/.
//
// All game state lives in its own folder, ~/Library/Application Support/Fable II Recomp Benchmark
// (never the normal "Fable II Recomp" folder, whose GameData is only read: APFS-cloned when complete,
// so a Mac that already runs the portable build needs no second extraction).
//
//   FableIIBenchmark [--state DIR] [--out DIR] [--iso IMAGE]
//                    [--arms cold,1u,1c,2u,2u-ssm,2c,3u,census1x,census2x-ssm,census2x (default; also 3c, 3u-ssm, …)]
//                    [--headless] [--no-clone] [--arm-timeout SECONDS]
//                    [--app BUNDLE --game-data DIR --save-dir SAVES] [--warm-cache CACHE] [--label TEXT]
//   FableIIBenchmark --flash [--reps N] [--arms adapt,adapt-nomatch,adapt-tess3,noadapt(,adapt-snap,noadapt-snap,induce)] ...
//                    (the flash kit, docs/flash-kit.md; also on when Info.plist has Fable2FlashKit = true:
//                    one 2x / 60 FPS run per arm and rep with the flash plan and the in-game flash detector)
//   FableIIBenchmark --reanalyze REPORT_FOLDER [--out DIR]
//                    (recompute hitches and shader warm-up of an existing report from its raw/ files; writes
//                    report.json + report.txt to DIR, default REPORT_FOLDER/reanalyzed)
// --app/--game-data/--save-dir run another (non-portable) Fable II.app on extracted game files with a
// staged saves folder: tools/fable2_benchmark.py and the Dev Launcher's "Run Benchmark…" use them on the
// build Mac. --warm-cache: the warm arms start from an APFS clone of that cache folder (read only).
// Environment (tests): FABLE2_BENCHMARK_STATE_ROOT, FABLE2_BENCHMARK_OUT; everything else in the
// environment reaches the game (e.g. FABLE2_NS_ASSUME_NO_XCODE=1, FABLE2_PORTABLE_HEADLESS=1).
import AppKit
import Darwin
import IOKit
import IOKit.ps
import Metal

// MARK: - Options

struct Options {
  var state: String?
  var out: String?
  var iso: String?
  // 2026-10-16: 3c dropped from the default (a GPU-bound Mac at 3x runs the same as 3u); the switch arms
  // 2u-ssm / census2x-ssm decide FABLE2_SSM_MERGE on the tester's Mac, paired A-B / B-A against 2u / census2x.
  var arms = ["cold", "1u", "1c", "2u", "2u-ssm", "2c", "3u", "census1x", "census2x-ssm", "census2x"]
  var headless = false
  var clone = true
  var armTimeout: Double = 420
  var app: String?
  var gameData: String?
  var saveDir: String?
  var warmCache: String?
  var label: String?
  var reanalyze: String?
  // Flash kit (docs/flash-kit.md): --flash or Info.plist Fable2FlashKit.
  var flash = (Bundle.main.object(forInfoDictionaryKey: "Fable2FlashKit") as? Bool) == true
  var reps = 2
  var armsGiven = false

  static func parse(_ args: [String]) -> Options {
    var o = Options()
    var i = 1
    func value() -> String? { i += 1; return i < args.count ? args[i] : nil }
    while i < args.count {
      switch args[i] {
      case "--state": o.state = value()
      case "--out": o.out = value()
      case "--iso": o.iso = value()
      case "--arms": o.arms = (value() ?? "").split(separator: ",").map(String.init); o.armsGiven = true
      case "--flash": o.flash = true
      case "--reps": o.reps = max(1, Int(value() ?? "") ?? o.reps)
      case "--headless": o.headless = true
      case "--no-clone": o.clone = false
      case "--arm-timeout": o.armTimeout = Double(value() ?? "") ?? o.armTimeout
      case "--app": o.app = value()
      case "--game-data": o.gameData = value()
      case "--save-dir": o.saveDir = value()
      case "--warm-cache": o.warmCache = value()
      case "--label": o.label = value()
      case "--reanalyze": o.reanalyze = value()
      default: break  // -psn_… from Finder, unknown flags
      }
      i += 1
    }
    let env = ProcessInfo.processInfo.environment
    if o.state == nil, let s = env["FABLE2_BENCHMARK_STATE_ROOT"], !s.isEmpty { o.state = s }
    if o.out == nil, let s = env["FABLE2_BENCHMARK_OUT"], !s.isEmpty { o.out = s }
    if o.flash && !o.armsGiven { o.arms = FlashArms.all }
    return o
  }
}

// Flash kit arms (docs/flash-kit.md): 2x, 60 FPS cap, tessellation trace on, the flash plan, the flash detector.
// Every switch is passed explicitly so nothing else (environment, saved settings) decides the arm.
enum FlashArms {
  // docs/adaptive-terrain-flashes-20261014.md: adapt-nomatch is the build before the layer fix (the flashes);
  // the snap arms are not default any more (the uv snap made no difference on the M3 Pro).
  static let all = ["adapt", "adapt-nomatch", "adapt-tess3", "noadapt"]
  static func env(_ id: String) -> (String, [String: String])? {
    let base = ["FABLE2_NATIVE_TESS_TRACE": "1", "FABLE2_NATIVE_TESS": "1", "FABLE2_NATIVE_TESS_SNAP_UV": "0",
                "FABLE2_TERRAIN_LAYER_MATCH": "1"]
    func arm(_ title: String, _ extra: [String: String]) -> (String, [String: String]) {
      (title, base.merging(extra) { $1 })
    }
    switch id {
    case "adapt": return arm("Adaptive terrain on (layers matched)", ["FABLE2_TERRAIN_ADAPTIVE": "1"])
    case "adapt-nomatch": return arm("Adaptive on, layers not matched (before the fix)",
                                     ["FABLE2_TERRAIN_ADAPTIVE": "1", "FABLE2_TERRAIN_LAYER_MATCH": "0"])
    case "adapt-snap": return arm("Adaptive on + snap uv", ["FABLE2_TERRAIN_ADAPTIVE": "1", "FABLE2_NATIVE_TESS_SNAP_UV": "1"])
    case "adapt-tess3": return arm("Adaptive on + reference tessellator (TESS=3)",
                                   ["FABLE2_TERRAIN_ADAPTIVE": "1", "FABLE2_NATIVE_TESS": "3"])
    case "noadapt": return arm("Adaptive terrain off", ["FABLE2_TERRAIN_ADAPTIVE": "0"])
    case "noadapt-snap": return arm("Adaptive off + snap uv", ["FABLE2_TERRAIN_ADAPTIVE": "0", "FABLE2_NATIVE_TESS_SNAP_UV": "1"])
    // Detector check only (not a default arm): FABLE2_FLASH_INDUCE makes the renderer flash white on purpose.
    case "induce": return arm("Induced flashes (detector check)", ["FABLE2_TERRAIN_ADAPTIVE": "0", "FABLE2_FLASH_INDUCE": "1"])
    default: return nil
    }
  }
}

// Switch arms: "<arm>-<switch>" is <arm> (any non-flash, warm arm) with one renderer switch set, so a kit run on another
// Mac decides a default there. report.txt pairs each with its base arm ("Switch arms").
enum SwitchArms {
  static let all: [String: (title: String, env: [String: String])] = [
    // docs/ssm-merge-20261015.md: P8a's transfer inside the shadow-mask pass (opt-in; meant for bandwidth-bound Macs).
    "ssm": ("SSM merge", ["FABLE2_SSM_MERGE": "1"]),
  ]
  /// (base arm id, switch) for "<base>-<switch>", else nil.
  static func split(_ id: String) -> (String, String)? {
    guard let dash = id.lastIndex(of: "-") else { return nil }
    let base = String(id[..<dash]), name = String(id[id.index(after: dash)...])
    return all[name] != nil && !base.isEmpty ? (base, name) : nil
  }
}

struct Arm {
  let id: String, title: String, scale: Int, capped: Bool, cold: Bool
  var env: [String: String] = [:]
  var flash = false
  var flashArm = ""  // the flash arm id without the rep suffix
  // Census arms (run last): uncapped, with the per-pass GPU trace and the HLE census on, so
  // tools/f2diag census-cost can split this Mac's GPU time by pass role (docs/hle-census-20261013.md).
  // The tracing costs a little; their numbers are not comparable with the other arms.
  var census = false
  static func make(_ id: String) -> Arm? {
    // Flash arms: "<arm>" or "<arm>-r<rep>".
    var flashId = id, rep = ""
    if let r = id.range(of: #"-r\d+$"#, options: .regularExpression) { flashId = String(id[..<r.lowerBound]); rep = String(id[r]) }
    if let (title, env) = FlashArms.env(flashId) {
      return Arm(id: id, title: title + (rep.isEmpty ? "" : " (run \(rep.dropFirst(2)))"), scale: 2, capped: true,
                 cold: false, env: env, flash: true, flashArm: flashId)
    }
    if let (baseId, name) = SwitchArms.split(id), let sw = SwitchArms.all[name], let base = Arm.make(baseId),
       !base.flash, !base.cold {
      return Arm(id: id, title: "\(base.title) + \(sw.title)", scale: base.scale, capped: base.capped, cold: false,
                 env: base.env.merging(sw.env) { $1 }, census: base.census)
    }
    if id == "cold" { return Arm(id: id, title: "Cold first launch, 1x, 60 FPS cap", scale: 1, capped: true, cold: true) }
    if id.hasPrefix("census"), id.count == 8, id.hasSuffix("x"), let scale = Int(String(id.dropFirst(6).first!)),
       (1...3).contains(scale) {
      return Arm(id: id, title: "\(scale)x uncapped, pass trace", scale: scale, capped: false, cold: false,
                 census: true)
    }
    guard id.count == 2, let scale = Int(String(id.first!)), (1...3).contains(scale) else { return nil }
    switch id.last! {
    case "u": return Arm(id: id, title: "\(scale)x uncapped", scale: scale, capped: false, cold: false)
    case "c": return Arm(id: id, title: "\(scale)x 60 FPS cap", scale: scale, capped: true, cold: false)
    default: return nil
    }
  }
}

enum BenchError: LocalizedError {
  case message(String)
  case cancelled
  var errorDescription: String? {
    switch self {
    case .message(let text): return text
    case .cancelled: return "Cancelled."
    }
  }
}

let fm = FileManager.default
let home = fm.homeDirectoryForCurrentUser.path

func iso8601(_ date: Date = Date()) -> String {
  let f = ISO8601DateFormatter()
  f.formatOptions = [.withInternetDateTime]
  f.timeZone = .current
  return f.string(from: date)
}

func round(_ v: Double, _ places: Int) -> Double {
  let m = pow(10.0, Double(places))
  return (v * m).rounded() / m
}

// MARK: - Machine info

func sysctlString(_ name: String) -> String? {
  var size = 0
  guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return nil }
  var buffer = [CChar](repeating: 0, count: size)
  guard sysctlbyname(name, &buffer, &size, nil, 0) == 0 else { return nil }
  return String(cString: buffer)
}

func sysctlInt(_ name: String) -> Int64? {
  var value: Int64 = 0
  var size = MemoryLayout<Int64>.size
  guard sysctlbyname(name, &value, &size, nil, 0) == 0 else { return nil }
  return size == 4 ? Int64(Int32(truncatingIfNeeded: value)) : value
}

func gpuCoreCount() -> Int? {
  var iterator: io_iterator_t = 0
  guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"), &iterator) == KERN_SUCCESS
  else { return nil }
  defer { IOObjectRelease(iterator) }
  var service = IOIteratorNext(iterator)
  while service != 0 {
    defer { IOObjectRelease(service) }
    if let value = IORegistryEntryCreateCFProperty(service, "gpu-core-count" as CFString, kCFAllocatorDefault, 0)?
      .takeRetainedValue() as? NSNumber {
      return value.intValue
    }
    service = IOIteratorNext(iterator)
  }
  return nil
}

func powerSource() -> String {
  guard let info = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
        let type = IOPSGetProvidingPowerSourceType(info)?.takeUnretainedValue() as String? else { return "unknown" }
  return type  // "AC Power", "Battery Power", "UPS Power"
}

func thermalState() -> String {
  switch ProcessInfo.processInfo.thermalState {
  case .nominal: return "nominal"
  case .fair: return "fair"
  case .serious: return "serious"
  case .critical: return "critical"
  @unknown default: return "unknown"
  }
}

func machineInfo() -> [String: Any] {
  var m: [String: Any] = [:]
  m["model_identifier"] = sysctlString("hw.model") ?? "unknown"
  m["chip"] = sysctlString("machdep.cpu.brand_string") ?? "unknown"
  m["cpu_cores"] = sysctlInt("hw.physicalcpu") ?? 0
  // perflevel0 = performance cores, perflevel1 = efficiency cores (Apple Silicon).
  m["performance_cores"] = sysctlInt("hw.perflevel0.physicalcpu") ?? 0
  m["efficiency_cores"] = sysctlInt("hw.perflevel1.physicalcpu") ?? 0
  m["gpu_cores"] = gpuCoreCount() ?? 0
  m["memory_gb"] = round(Double(sysctlInt("hw.memsize") ?? 0) / 1_073_741_824.0, 1)
  let v = ProcessInfo.processInfo.operatingSystemVersion
  m["macos"] = "\(v.majorVersion).\(v.minorVersion).\(v.patchVersion)"
  m["macos_build"] = sysctlString("kern.osversion") ?? ""
  if let device = MTLCreateSystemDefaultDevice() {
    m["gpu_name"] = device.name
    m["gpu_recommended_working_set_gb"] = round(Double(device.recommendedMaxWorkingSetSize) / 1_073_741_824.0, 1)
  }
  var displays: [[String: Any]] = []
  for screen in NSScreen.screens {
    let size = screen.frame.size, scale = screen.backingScaleFactor
    displays.append([
      "name": screen.localizedName, "main": screen == NSScreen.main,
      "points": "\(Int(size.width))x\(Int(size.height))",
      "pixels": "\(Int(size.width * scale))x\(Int(size.height * scale))",
      "scale": scale, "max_fps": screen.maximumFramesPerSecond,
    ])
  }
  m["displays"] = displays
  m["power_source"] = powerSource()
  m["low_power_mode"] = ProcessInfo.processInfo.isLowPowerModeEnabled
  m["thermal_state"] = thermalState()
  return m
}

// MARK: - Statistics (same definitions as tools/f2diag pacing.window and gpu.analyze)

func percentile(_ values: [Double], _ pct: Double) -> Double? {
  let v = values.sorted()
  guard !v.isEmpty else { return nil }
  let pos = Double(v.count - 1) * pct / 100
  let lo = Int(pos)
  let hi = min(lo + 1, v.count - 1)
  return v[lo] + (v[hi] - v[lo]) * (pos - Double(lo))
}

func unionLength(_ intervals: [(Double, Double)]) -> Double {
  var total = 0.0
  var current: (Double, Double)?
  for (b, e) in intervals.sorted(by: { $0.0 < $1.0 }) {
    if let c = current, b <= c.1 { current = (c.0, max(c.1, e)) } else {
      if let c = current { total += c.1 - c.0 }
      current = (b, e)
    }
  }
  if let c = current { total += c.1 - c.0 }
  return total
}

struct FrameData {
  var swaps: [Int: Double] = [:]  // frame id -> time (µs, CSV clock)
  var gpu: [Int: [(Double, Double)]] = [:]
  var tcpu: [String: [Int: Double]] = [:]
}

func readFrameCSV(_ path: String) throws -> FrameData {
  guard let text = try? String(contentsOfFile: path, encoding: .utf8) else {
    throw BenchError.message("cannot read \(path)")
  }
  var d = FrameData()
  for line in text.split(separator: "\n").dropFirst() {
    let f = line.split(separator: ",", omittingEmptySubsequences: false)
    guard f.count >= 4, let frame = Int(f[1]), let time = Double(f[2]), let duration = Double(f[3]) else { continue }
    switch f[0] {
    case "guest_swap": d.swaps[frame] = time
    case "gpu_cb", "gpu_cb_sa": d.gpu[frame, default: []].append((time, time + duration))
    case let e where e.hasPrefix("tcpu_"): d.tcpu[String(e), default: [:]][frame, default: 0] += duration
    default: break
    }
  }
  return d
}

// [hitch] lines (backends/metal/port/rex/graphics/metal/metal_hitch.h). Since 2026-10-13 the game logs only
// relative hitches and adds the baseline: "(cpu C, base B)". Older builds logged every frame >= 40 ms ("(cpu C)"):
// those lines are reclassified here with the same rule from the run's per-frame intervals.
struct Hitch { let frame: Int; let ms: Double; let cpu: Double; let base: Double?; let parts: [String: Double] }

enum HitchRule {
  static let floorMs = 40.0, ceilMs = 100.0, ratio = 1.5, window = 32, minSamples = 8
  static let text = "frame >= 1.5 x median of the previous 32 frames, at least 40 ms; any frame >= 100 ms"
  static func threshold(baseMs: Double) -> Double { min(max(baseMs * ratio, floorMs), ceilMs) }
  /// Median of up to `window` intervals before `frame` (0 when fewer than minSamples), as the game computes it.
  static func baseline(_ intervals: [(Int, Double)], before frame: Int) -> Double {
    var prior = intervals.filter { $0.0 < frame }.suffix(window).map(\.1)
    // The report only has the sweep window: its first frames take the window's first 32 frames instead.
    if prior.count < minSamples { prior = Array(intervals.prefix(window).map(\.1)) }
    guard prior.count >= minSamples else { return 0 }
    let v = prior.sorted()
    return v.count % 2 == 1 ? v[v.count / 2] : (v[v.count / 2 - 1] + v[v.count / 2]) / 2
  }
}

func readHitches(_ log: String) -> [Hitch] {
  guard let text = try? String(contentsOfFile: log, encoding: .utf8) else { return [] }
  let line = try! NSRegularExpression(pattern: #"\[hitch\] f(\d+) ([\d.]+) ms \(cpu ([\d.]+)(?:, base ([\d.]+))?\): (.*)$"#)
  let part = try! NSRegularExpression(pattern: #"([a-z_]+) ([\d.]+)"#)
  var out: [Hitch] = []
  for raw in text.split(separator: "\n") where raw.contains("[hitch] f") {
    let s = String(raw)
    let ns = s as NSString
    guard let m = line.firstMatch(in: s, range: NSRange(location: 0, length: ns.length)) else { continue }
    let detail = ns.substring(with: m.range(at: 5))
    var parts: [String: Double] = [:]
    let dn = detail as NSString
    for p in part.matches(in: detail, range: NSRange(location: 0, length: dn.length)) {
      parts[dn.substring(with: p.range(at: 1)), default: 0] += Double(dn.substring(with: p.range(at: 2))) ?? 0
    }
    let base = m.range(at: 4).location != NSNotFound ? Double(ns.substring(with: m.range(at: 4))) : nil
    out.append(Hitch(frame: Int(ns.substring(with: m.range(at: 1))) ?? 0, ms: Double(ns.substring(with: m.range(at: 2))) ?? 0,
                     cpu: Double(ns.substring(with: m.range(at: 3))) ?? 0, base: base, parts: parts))
  }
  return out
}

/// window-frames.csv rows (frame id, interval ms ending at that frame's swap).
func readWindowIntervals(_ path: String) -> [(Int, Double)] {
  guard let text = try? String(contentsOfFile: path, encoding: .utf8) else { return [] }
  return text.split(separator: "\n").dropFirst().compactMap { line in
    let f = line.split(separator: ",")
    guard f.count >= 2, let id = Int(f[0]), let ms = Double(f[1]) else { return nil }
    return (id, ms)
  }
}

/// The report's hitch summary: lines in the sweep window [f0, f1] that are hitches under HitchRule (lines of an
/// older absolute-rule build are reclassified with the window's own intervals), plus the session totals.
func summarizeHitches(_ hitches: [Hitch], first f0: Int, last f1: Int, intervals: [(Int, Double)]) -> [String: Any] {
  let absolute = hitches.contains { $0.base == nil }
  func isHitch(_ h: Hitch) -> Bool {
    if h.base != nil { return true }  // the game already applied the rule
    return h.ms >= HitchRule.threshold(baseMs: HitchRule.baseline(intervals, before: h.frame))
  }
  let rawWindow = hitches.filter { $0.frame >= f0 && $0.frame <= f1 }
  let inWindow = rawWindow.filter(isHitch)
  var parts: [String: Double] = [:]
  for h in inWindow { for (k, v) in h.parts { parts[k, default: 0] += v } }
  var r: [String: Any] = [
    "rule": HitchRule.text,
    "session_count": hitches.count, "session_worst_ms": hitches.map(\.ms).max() ?? 0,
    "session_over_100ms": hitches.filter { $0.ms >= 100 }.count,
    "window_count": inWindow.count, "window_worst_ms": inWindow.map(\.ms).max() ?? 0,
    "window_ms_by_cause": parts.mapValues { round($0, 1) },
    "window_frames_over_40ms": intervals.filter { $0.0 >= f0 && $0.0 <= f1 && $0.1 >= 40 }.count,
  ]
  if absolute {
    // An older build: session counts are frames >= 40 ms (at most 400 lines), the window count is reclassified.
    r["log_rule"] = "absolute (frames >= 40 ms; reclassified from window-frames.csv)"
    r["window_count_absolute"] = rawWindow.count
  }
  return r
}

/// Pacing (FPS, 1% low, percentiles) over frames [first, last] plus GPU and thread CPU per frame.
func analyze(_ d: FrameData, first: Int, last: Int, perFrameCSV: String?) -> [String: Any] {
  let ids = d.swaps.keys.filter { $0 >= first && $0 <= last }.sorted()
  guard ids.count > 2, let t0 = d.swaps[ids.first!], let t1 = d.swaps[ids.last!], t1 > t0 else {
    return ["frames": ids.count, "error": "too few frames in the window"]
  }
  var gaps: [Double] = []
  var gpuMs: [Double] = []
  var rows = "frame,interval_ms,gpu_ms\n"
  for (a, b) in zip(ids, ids.dropFirst()) {
    let gap = (d.swaps[b]! - d.swaps[a]!) / 1000
    gaps.append(gap)
    let g = unionLength(d.gpu[a] ?? []) / 1000
    if b == a + 1 { gpuMs.append(g) }
    rows += "\(a),\(round(gap, 3)),\(round(g, 3))\n"
  }
  if let path = perFrameCSV { try? rows.write(toFile: path, atomically: true, encoding: .utf8) }
  let seconds = (t1 - t0) / 1e6
  let worst = gaps.sorted(by: >).prefix(max(1, gaps.count / 100))
  var r: [String: Any] = [
    "frames": ids.count, "seconds": round(seconds, 3),
    "fps_mean": round(Double(gaps.count) / seconds, 2),
    "fps_1pct_low": round(1000 / (worst.reduce(0, +) / Double(worst.count)), 1),
    "frame_ms_p50": round(percentile(gaps, 50)!, 2), "frame_ms_p95": round(percentile(gaps, 95)!, 2),
    "frame_ms_p99": round(percentile(gaps, 99)!, 2), "frame_ms_max": round(gaps.max()!, 1),
    "frames_over_33ms": gaps.filter { $0 > 33.4 }.count, "frames_over_50ms": gaps.filter { $0 > 50 }.count,
  ]
  if !gpuMs.isEmpty && gpuMs.contains(where: { $0 > 0 }) {
    r["gpu_ms_mean"] = round(gpuMs.reduce(0, +) / Double(gpuMs.count), 3)
    r["gpu_ms_p95"] = round(percentile(gpuMs, 95)!, 3)
    r["gpu_ms_p99"] = round(percentile(gpuMs, 99)!, 3)
  }
  let names = ["tcpu_gpucmd_us": "GPU Commands", "tcpu_game_us": "GameThread", "tcpu_3d_us": "3D Engine",
               "tcpu_audio_us": "Audio Worker"]
  var threads: [String: Double] = [:]
  for (event, perFrame) in d.tcpu {
    let total = ids.dropLast().reduce(0.0) { $0 + (perFrame[$1] ?? 0) }
    threads[names[event] ?? event] = round(total / 1e6 / seconds, 3)
  }
  if !threads.isEmpty { r["thread_cpu_cores"] = threads }
  return r
}

/// The game's shader warm-up summary lines (Metal shader storage, [xfer-prewarm]): how much was really compiled.
func readShaderWarmup(_ log: String) -> [String: Any] {
  guard let text = try? String(contentsOfFile: log, encoding: .utf8) else { return [:] }
  let storage = try! NSRegularExpression(pattern:
    #"background warm-up complete in ([\d.]+)s; binary archive hits (\d+) .*?compiled (\d+) pipelines \((\d+) ms summed"#)
  let xfer = try! NSRegularExpression(pattern: #"\[xfer-prewarm\] \d+ recorded transfer pipelines: built (\d+) in background \((\d+) ms summed"#)
  var r: [String: Any] = [:]
  for raw in text.split(separator: "\n") {
    let s = String(raw), ns = s as NSString, range = NSRange(location: 0, length: ns.length)
    if r["pipelines_compiled"] == nil, let m = storage.firstMatch(in: s, range: range) {
      r["warmup_s"] = Double(ns.substring(with: m.range(at: 1))) ?? 0
      r["archive_hits"] = Int(ns.substring(with: m.range(at: 2))) ?? 0
      let n = Int(ns.substring(with: m.range(at: 3))) ?? 0, ms = Double(ns.substring(with: m.range(at: 4))) ?? 0
      r["pipelines_compiled"] = n
      r["compile_ms_summed"] = ms
      if n > 0 { r["compile_ms_per_pipeline"] = round(ms / Double(n), 2) }
    } else if r["transfer_pipelines_built"] == nil, let m = xfer.firstMatch(in: s, range: range) {
      r["transfer_pipelines_built"] = Int(ns.substring(with: m.range(at: 1))) ?? 0
      r["transfer_ms_summed"] = Double(ns.substring(with: m.range(at: 2))) ?? 0
    }
  }
  return r
}

/// macOS keeps the GPU code it compiles for an app in <user cache dir>/<bundle id>/com.apple.metal (plus
/// com.apple.gpuarchiver and the compiler front end's com.apple.metalfe). Our own binary archives start empty in the
/// cold arm, but this cache survives, so a Mac that ran the benchmark before compiles nothing (2026-10-13, M3 Pro:
/// 443 pipelines 23.5 s summed, next run 398 in 0.18 s; boot 19.4 s -> 6.0 s). Metal has no setting to move or
/// skip it, so the cold arm deletes those three folders, for a dedicated benchmark identity only
/// (org.fable2recomp.metal.benchmark[.*]: the kit app, or tools/fable2_benchmark.py's clone of a build). Any other
/// bundle id (a normal Fable II.app shares its cache with normal play) is left alone and the arm is marked not cold.
/// FABLE2_BENCHMARK_KEEP_OS_CACHE=1 skips it (tests).
func clearOSShaderCache(bundleID: String?, log: (String) -> Void) -> [String: Any] {
  var r: [String: Any] = ["cleared": false]
  func refuse(_ reason: String) -> [String: Any] { r["reason"] = reason; log("OS shader cache not cleared: \(reason)"); return r }
  guard let id = bundleID, !id.isEmpty else { return refuse("the game app has no bundle identifier") }
  r["bundle_id"] = id
  if ProcessInfo.processInfo.environment["FABLE2_BENCHMARK_KEEP_OS_CACHE"] == "1" {
    return refuse("FABLE2_BENCHMARK_KEEP_OS_CACHE=1")
  }
  let dedicated = "org.fable2recomp.metal.benchmark"
  guard id == dedicated || id.hasPrefix(dedicated + "."),
        id.range(of: #"^[A-Za-z0-9.-]+$"#, options: .regularExpression) != nil else {
    return refuse("bundle id \(id) is not a benchmark-only identity (its cache is shared with normal play)")
  }
  var buffer = [CChar](repeating: 0, count: Int(PATH_MAX))
  guard confstr(_CS_DARWIN_USER_CACHE_DIR, &buffer, buffer.count) > 0 else { return refuse("no user cache folder") }
  let cacheDir = URL(fileURLWithPath: String(cString: buffer)).resolvingSymlinksInPath().standardizedFileURL.path
  guard cacheDir.hasPrefix("/private/var/folders/") || cacheDir.hasPrefix("/var/folders/"),
        (cacheDir as NSString).lastPathComponent == "C" else {
    return refuse("unexpected user cache folder \(cacheDir)")
  }
  let root = "\(cacheDir)/\(id)"
  r["path"] = root
  func isRealDir(_ path: String) -> Bool {
    var st = stat()
    return lstat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR
  }
  if !fm.fileExists(atPath: root) { r["cleared"] = true; r["removed_mb"] = 0.0; log("OS shader cache: \(root) absent"); return r }
  guard isRealDir(root) else { return refuse("\(root) is not a plain folder") }
  var removedBytes: Int64 = 0
  var removed: [String] = []
  for sub in ["com.apple.metal", "com.apple.gpuarchiver", "com.apple.metalfe"] {
    let path = "\(root)/\(sub)"
    guard fm.fileExists(atPath: path) else { continue }
    guard isRealDir(path) else { return refuse("\(path) is not a plain folder") }
    var bytes: Int64 = 0
    if let e = fm.enumerator(at: URL(fileURLWithPath: path), includingPropertiesForKeys: [.totalFileAllocatedSizeKey]) {
      for case let url as URL in e {
        bytes += Int64((try? url.resourceValues(forKeys: [.totalFileAllocatedSizeKey]).totalFileAllocatedSize) ?? 0)
      }
    }
    do { try fm.removeItem(atPath: path) } catch { return refuse("could not remove \(path): \(error.localizedDescription)") }
    removedBytes += bytes
    removed.append(sub)
    log("OS shader cache: removed \(path) (\(round(Double(bytes) / 1_048_576, 1)) MB)")
  }
  r["cleared"] = true
  r["removed"] = removed
  r["removed_mb"] = round(Double(removedBytes) / 1_048_576, 1)
  return r
}

// MARK: - Abnormal exits

/// Copies the macOS crash reports (.ips) of game process `pid` written since `since` into `dir` (as crash-*.ips).
/// ReportCrash writes them a moment after the process dies: waits for one to appear (polled), with `guardSeconds`
/// as a guard only. Returns the copied file names.
func collectCrashReports(pid: Int32, since: Date, into dir: String, guardSeconds: Double = 20) -> [String] {
  let folder = "\(home)/Library/Logs/DiagnosticReports"
  let deadline = Date().addingTimeInterval(guardSeconds)
  let pidKey = "\"pid\" : \(pid),"
  let pidKeyCompact = "\"pid\":\(pid),"
  while true {
    var copied: [String] = []
    for name in (try? fm.contentsOfDirectory(atPath: folder)) ?? []
    where name.hasSuffix(".ips") && (name.hasPrefix("fable_2") || name.hasPrefix("FableIILauncher")) {
      let path = "\(folder)/\(name)"
      guard let mod = (try? fm.attributesOfItem(atPath: path))?[.modificationDate] as? Date,
            mod >= since.addingTimeInterval(-1),
            let text = try? String(contentsOfFile: path, encoding: .utf8),
            text.contains(pidKey) || text.contains(pidKeyCompact) else { continue }
      let dest = "\(dir)/crash-\(name)"
      try? fm.removeItem(atPath: dest)
      if (try? fm.copyItem(atPath: path, toPath: dest)) != nil { copied.append("crash-\(name)") }
    }
    if !copied.isEmpty || Date() >= deadline { return copied }
    Thread.sleep(forTimeInterval: 0.5)
  }
}

/// The first lines of a crash report worth quoting in the report: exception and the crashed thread's top frames.
func crashSummary(_ path: String) -> String? {
  guard let text = try? String(contentsOfFile: path, encoding: .utf8),
        let nl = text.firstIndex(of: "\n"),
        let data = String(text[text.index(after: nl)...]).data(using: .utf8),
        let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
  let ex = json["exception"] as? [String: Any] ?? [:]
  var s = "\(ex["type"] ?? "?") \(ex["signal"] ?? "")"
  if let threads = json["threads"] as? [[String: Any]], let i = json["faultingThread"] as? Int, i < threads.count {
    let t = threads[i]
    let frames = ((t["frames"] as? [[String: Any]]) ?? []).prefix(6).compactMap { $0["symbol"] as? String }
    s += " on thread \"\(t["name"] ?? i)\": " + frames.joined(separator: " < ")
  }
  return s
}

/// "Run problems" lines for report.txt: every run whose game did not exit cleanly, was retried or failed.
func problemLines(_ results: [[String: Any]]) -> [String] {
  var lines: [String] = []
  for r in results {
    guard let outcome = r["outcome"] as? String, outcome != "ok" else { continue }
    lines.append("  \(r["arm"] ?? "?"): \(outcome)")
    for c in (r["crash_summaries"] as? [String]) ?? [] { lines.append("      crash: \(c)") }
    if let first = r["first_attempt"] as? [String: Any] {
      lines.append("      first attempt: \(first["outcome"] ?? "no data")")
      for c in (first["crash_summaries"] as? [String]) ?? [] { lines.append("      first attempt crash: \(c)") }
    }
  }
  return lines
}

// MARK: - Flash kit (docs/flash-kit.md)

/// flash.json of one run (the in-game detector, backends/metal/port/rex/ui/metal/metal_flash_detect.h).
func readFlash(_ dir: String) -> [String: Any] {
  guard let data = fm.contents(atPath: "\(dir)/flash.json"),
        let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
    return ["error": "no flash.json (detector off or the run ended early)"]
  }
  return json
}

/// What the tessellation trace says about the run: device line, snap-uv confirmation, paths per VS.
func readTessTrace(_ log: String) -> [String: Any] {
  guard let text = try? String(contentsOfFile: log, encoding: .utf8) else { return [:] }
  var r: [String: Any] = [:]
  var combos: [String] = []
  var snapCompiled = 0
  var windows: [String] = []
  for raw in text.split(separator: "\n") where raw.contains("[tess-trace]") {
    guard let at = raw.range(of: "[tess-trace] ") else { continue }
    let msg = String(raw[at.upperBound...])
    if msg.hasPrefix("device ") { r["device"] = msg }
    else if msg.hasPrefix("snap-uv: native domain shader") { snapCompiled += 1 }
    else if msg.hasPrefix("snap-uv: ") { r["snap_uv"] = String(msg.dropFirst("snap-uv: ".count)) }
    else if msg.hasPrefix("VS ") { combos.append(msg) }
    else if msg.hasPrefix("last ") { windows.append(msg) }
  }
  r["snap_uv_domain_shaders_logged"] = snapCompiled
  r["combinations"] = combos
  r["path_counts"] = windows
  return r
}

// MARK: - Runner

final class Runner {
  let options: Options
  let status: (String, Double) -> Void
  var cancelled = false
  var child: Process?
  lazy var bundle: Bundle = options.app.flatMap { Bundle(path: $0) } ?? Bundle.main
  lazy var resources = bundle.resourcePath!
  lazy var macos = "\(bundle.bundlePath)/Contents/MacOS"
  lazy var saveSource = options.saveDir ?? "\(Bundle.main.resourcePath!)/benchmark-save/saves"
  lazy var state = options.state ??
    "\(home)/Library/Application Support/Fable II Recomp \(options.flash ? "Flash Kit" : "Benchmark")"
  var extraEnv: [String: String] = [:]
  var warmCacheApplied = false

  init(options: Options, status: @escaping (String, Double) -> Void) {
    self.options = options
    self.status = status
  }

  func log(_ text: String) {
    let line = "[\(iso8601())] \(text)\n"
    FileHandle.standardError.write(line.data(using: .utf8)!)
    let path = "\(state)/benchmark.log"
    if !fm.fileExists(atPath: path) { fm.createFile(atPath: path, contents: nil) }
    if let h = FileHandle(forWritingAtPath: path) {
      h.seekToEndOfFile(); h.write(line.data(using: .utf8)!); h.closeFile()
    }
  }

  func complete(_ gameData: String) -> Bool {
    guard let data = fm.contents(atPath: "\(gameData)/.fable2-portable.json"),
          let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return false }
    return (json["complete"] as? Bool) == true
  }

  func run(_ executable: String, _ args: [String], logTo: String) -> Int32 {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: executable)
    p.arguments = args
    fm.createFile(atPath: logTo, contents: nil)
    if let h = FileHandle(forWritingAtPath: logTo) { h.seekToEndOfFile(); p.standardOutput = h; p.standardError = h }
    do { try p.run() } catch { return 1 }
    p.waitUntilExit()
    return p.terminationStatus
  }

  /// Game files: already there, cloned from the normal portable install (read only), or extracted from the disc image.
  func prepareGameData() throws {
    if let data = options.gameData { log("game data: \(data) (read only)"); return }
    let target = "\(state)/GameData"
    if complete(target) && options.iso == nil { log("game data present"); return }
    // The normal portable install's game files, or (flash kit) the benchmark kit's: only read, APFS-cloned.
    let candidates = ["\(home)/Library/Application Support/Fable II Recomp/GameData",
                      "\(home)/Library/Application Support/Fable II Recomp Benchmark/GameData"]
    if options.clone && options.iso == nil && !fm.fileExists(atPath: target),
       let normal = candidates.first(where: { $0 != target && complete($0) }) {
      status("Copying the game files of your Fable II install (APFS clone)…", 0.02)
      if clonefile(normal, target, 0) == 0 && complete(target) {
        log("game data cloned from \(normal)"); return
      }
      log("clone failed (errno \(errno)); extracting from the disc image instead")
      try? fm.removeItem(atPath: target)
    }
    status("Choose your Fable II disc image (.iso) — the game files are copied from it once.", 0.02)
    let result = "\(state)/prepare-result.plist"
    try? fm.removeItem(atPath: result)
    var args = ["--target", target, "--lua", "\(resources)/recomp-lua", "--result", result]
    if let sha = bundle.object(forInfoDictionaryKey: "Fable2XexSHA256") as? String, !sha.isEmpty {
      args += ["--expected-xex", sha]
    }
    if let iso = options.iso { args += ["--iso", iso] }
    if options.headless || ProcessInfo.processInfo.environment["FABLE2_PORTABLE_HEADLESS"] == "1" { args.append("--headless") }
    let code = run("\(macos)/FableIIPrepare", args, logTo: "\(state)/logs/prepare.log")
    if code == 3 { throw BenchError.cancelled }
    if code != 0 { throw BenchError.message("The game files could not be prepared (see \(state)/logs/prepare.log).") }
    if let r = NSDictionary(contentsOfFile: result), (r["XexMatches"] as? Bool) == false {
      log("disc image differs from the build's default.xex; continuing (FABLE2_XEX_MISMATCH_OK=1)")
      extraEnv["FABLE2_XEX_MISMATCH_OK"] = "1"
    }
  }

  /// The bundled save (and its profile options) becomes the only save in the benchmark state.
  func resetSaves() throws {
    let saves = "\(state)/saves"
    try? fm.removeItem(atPath: saves)
    try fm.copyItem(atPath: saveSource, toPath: saves)
  }

  func cleanLogs() {
    let logs = "\(state)/logs"
    for name in (try? fm.contentsOfDirectory(atPath: logs)) ?? [] where name != "prepare.log" {
      try? fm.removeItem(atPath: "\(logs)/\(name)")
    }
  }

  func runArm(_ arm: Arm, index: Int, count: Int, runs: String) throws -> [String: Any] {
    let base = 0.05 + 0.9 * Double(index) / Double(count)
    status("Run \(index + 1) of \(count): \(arm.title). Please don't use the Mac.", base)
    log("arm \(arm.id): \(arm.title)")
    let dir = "\(runs)/\(arm.id)"
    try? fm.removeItem(atPath: dir)
    try fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
    var osCache: [String: Any]?
    if arm.cold {
      try? fm.removeItem(atPath: "\(state)/cache")  // fresh cache: only the app's bundled shader seed
      try fm.createDirectory(atPath: "\(state)/cache", withIntermediateDirectories: true)
      osCache = clearOSShaderCache(bundleID: bundle.bundleIdentifier) { self.log($0) }
    } else if let warm = options.warmCache, !warmCacheApplied {
      try? fm.removeItem(atPath: "\(state)/cache")
      if clonefile(warm, "\(state)/cache", 0) != 0 { try fm.copyItem(atPath: warm, toPath: "\(state)/cache") }
      warmCacheApplied = true
      log("warm arms start from a clone of \(warm)")
    }
    try resetSaves()
    cleanLogs()
    let resultPath = "\(dir)/result.json"
    var env = ProcessInfo.processInfo.environment
    for (k, v) in extraEnv { env[k] = v }
    env["FABLE2_APP_STATE_ROOT"] = state
    env["FABLE2_BENCHMARK"] = resultPath
    env["FABLE2_BOOT_SAVE"] = "1"
    env["FABLE2_PHYSICAL_INPUT"] = "0"  // a stray mouse or key cannot move the camera
    env["FABLE2_RES_SCALE"] = String(arm.scale)
    env["FABLE2_FPS_60"] = arm.capped ? "1" : "0"  // as tools/f2diag --fps 60 / uncapped
    env["REX_VSYNC"] = arm.capped ? "1" : "0"
    env["FABLE2_GPU_TIME_TRACE"] = "1"  // gpu_cb + tcpu_* rows in the frame CSV
    env["FABLE2_PASS_TRACE"] = arm.census ? "1" : "0"
    if arm.census { env["FABLE2_HLE_CENSUS"] = "1" }
    if arm.flash {
      env["FABLE2_BENCHMARK_PLAN"] = "flash"
      env["FABLE2_FLASH_DETECT"] = dir
    }
    for (k, v) in arm.env { env[k] = v }  // flash and switch arms
    if env["FABLE2_TRACE"] == nil { env["FABLE2_TRACE"] = "gpu:1,cpu:1,sampler:1,frame:1" }  // alias back to default levels
    if env["FABLE2_INSTANCE_ROLE"] == nil { env["FABLE2_INSTANCE_ROLE"] = "benchmark" }
    // Window-title tag (src/core/fable2_window_title.h), e.g. "Fable II … · 3× · uncapped · benchmark 3u".
    env["FABLE2_WINDOW_TAG"] = "benchmark \(arm.id)"
    // The portable launcher defaults this from Info.plist; a normal Fable II.app (--app) does not.
    if env["FABLE2_SKIP_INTRO_MOVIES"] == nil { env["FABLE2_SKIP_INTRO_MOVIES"] = "1" }
    // A test-control folder per state: the game then holds an App Nap exemption while it runs
    // (metal_test_control.mm), so a covered or background window is not throttled.
    if env["FABLE2_TEST_CONTROL_DIR"] == nil {
      let control = "\(state)/control"
      try? fm.createDirectory(atPath: control, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
      env["FABLE2_TEST_CONTROL_DIR"] = control
    }
    if let data = options.gameData { env["FABLE2_GAME_DATA_ROOT"] = data } else { env.removeValue(forKey: "FABLE2_GAME_DATA_ROOT") }
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "\(macos)/FableIILauncher")
    // No "reopen windows?" alert after an earlier crash (tools/fable2_common.py TEST_LAUNCH_APPKIT_ARGS).
    p.arguments = ["--audio_mute=true", "-ApplePersistenceIgnoreState", "YES"]
    p.environment = env
    let started = Date()
    try p.run()
    child = p
    // tools/fable2_benchmark.py registers the game (FableIILauncher execs fable_2 in the same process).
    FileHandle.standardError.write("[game-pid] \(p.processIdentifier) \(arm.id)\n".data(using: .utf8)!)
    log("arm \(arm.id): game pid \(p.processIdentifier)")
    while p.isRunning {
      if cancelled { p.terminate() }
      if Date().timeIntervalSince(started) > options.armTimeout {
        log("arm \(arm.id): timeout after \(Int(options.armTimeout)) s; stopping the game")
        p.terminate()
        Thread.sleep(forTimeInterval: 5)
        if p.isRunning { kill(p.processIdentifier, SIGKILL) }
        break
      }
      let t = Date().timeIntervalSince(started)
      status("Run \(index + 1) of \(count): \(arm.title) — \(Int(t)) s. Please don't use the Mac.",
             base + 0.9 / Double(count) * min(0.95, t / 60))
      Thread.sleep(forTimeInterval: 0.5)
    }
    let timedOut = p.isRunning || Date().timeIntervalSince(started) > options.armTimeout
    p.waitUntilExit()
    child = nil
    let wall = Date().timeIntervalSince(started)
    if cancelled { throw BenchError.cancelled }
    // Keep this run's logs next to its result.
    let logs = "\(state)/logs"
    for name in (try? fm.contentsOfDirectory(atPath: logs)) ?? [] where name != "prepare.log" {
      try? fm.moveItem(atPath: "\(logs)/\(name)", toPath: "\(dir)/\(name)")
    }
    var out: [String: Any] = ["arm": arm.id, "title": arm.title, "resolution_scale": arm.scale,
                              "fps_cap": arm.capped ? "60" : "uncapped", "cold_cache": arm.cold,
                              "exit_status": Int(p.terminationStatus), "launch_to_exit_s": round(wall, 1),
                              "thermal_state_after": thermalState()]
    if !arm.env.isEmpty { out["env"] = arm.env }
    // The game ends with exit status 0 (hard exit after the benchmark's quit). Anything else, a signal
    // (crash) or the arm timeout is abnormal: the caller keeps the data if it was written, else retries once.
    let signalled = p.terminationReason == .uncaughtSignal
    let abnormal = signalled || p.terminationStatus != 0 || timedOut
    out["abnormal_exit"] = abnormal
    if signalled { out["exit_signal"] = Int(p.terminationStatus) }
    if timedOut { out["timed_out"] = true }
    if signalled {
      // Guard for ReportCrash (tests set FABLE2_CRASH_REPORT_WAIT_S=0: a fake game leaves no report).
      let guardS = Double(ProcessInfo.processInfo.environment["FABLE2_CRASH_REPORT_WAIT_S"] ?? "") ?? 20
      let reports = collectCrashReports(pid: p.processIdentifier, since: started, into: dir, guardSeconds: guardS)
      out["crash_reports"] = reports
      out["crash_summaries"] = reports.compactMap { crashSummary("\(dir)/\($0)") }
      log("arm \(arm.id): game ended by signal \(p.terminationStatus); crash reports: \(reports.isEmpty ? "none found" : reports.joined(separator: ", "))")
    } else if abnormal {
      log("arm \(arm.id): game exited abnormally (status \(p.terminationStatus)\(timedOut ? ", timeout" : ""))")
    }
    out["has_data"] = false
    if let c = osCache { out["os_shader_cache"] = c }
    let warmup = readShaderWarmup("\(dir)/fable_2.log")
    if !warmup.isEmpty { out["shader_warmup"] = warmup }
    if arm.flash {
      out["flash_arm"] = arm.flashArm
      out["env"] = arm.env
      out["flash"] = readFlash(dir)
      out["tess_trace"] = readTessTrace("\(dir)/fable_2.log")
    }
    if arm.cold { out["truly_cold"] = (osCache?["cleared"] as? Bool) == true }
    guard let data = fm.contents(atPath: resultPath),
          let game = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
      out["error"] = "the game wrote no benchmark result (see \(dir)/fable_2.log)"
      return out
    }
    if let e = game["error"] { out["error"] = e }
    out["gameplay_s"] = game["gameplay_s"]
    out["level_load_seen_s"] = game["level_load_seen_s"]
    // Readiness (docs/readiness.md): the sweep starts when the game says it is ready, not after a fixed wait.
    for key in ["plan", "ready_s", "ready_after_gameplay_s", "ready_timeout", "csv_flushed"] where game[key] != nil {
      out[key] = game[key]
    }
    guard let sweep = game["sweep"] as? [String: Any], let f0 = sweep["frame_start"] as? Int,
          let f1 = sweep["frame_end"] as? Int else { return out }
    let csv = ((try? fm.contentsOfDirectory(atPath: dir)) ?? []).filter { $0.hasPrefix("frames-") && $0.hasSuffix(".csv") }
      .sorted().last
    guard let csvName = csv, let frames = try? readFrameCSV("\(dir)/\(csvName)") else {
      out["error"] = "no frame CSV"
      return out
    }
    // The sweep's data is on disk (the flash arms also need the detector's flash.json).
    out["has_data"] = !arm.flash || fm.fileExists(atPath: "\(dir)/flash.json")
    var stats = analyze(frames, first: f0, last: f1, perFrameCSV: "\(dir)/window-frames.csv")
    stats["process_cpu_cores"] = sweep["cpu_cores"].map { round(($0 as? Double) ?? 0, 2) }
    stats["memory_footprint_mb_mean"] = (sweep["footprint_mb_mean"] as? Double).map { round($0, 0) }
    stats["memory_footprint_mb_peak"] = (sweep["footprint_mb_peak"] as? Double).map { round($0, 0) }
    stats["top_threads"] = sweep["threads"]
    out["sweep"] = stats
    // Phase-level FPS (turns vs holds).
    if let phases = game["phases"] as? [[String: Any]] {
      var turnFrames = 0, holdFrames = 0
      var turnUs = 0.0, holdUs = 0.0
      for ph in phases {
        guard let a = ph["frame_start"] as? Int, let b = ph["frame_end"] as? Int, b > a,
              let ta = frames.swaps[a], let tb = frames.swaps[b], let name = ph["name"] as? String else { continue }
        if name.hasPrefix("turn") { turnFrames += b - a; turnUs += tb - ta } else { holdFrames += b - a; holdUs += tb - ta }
      }
      if turnUs > 0 { out["fps_turns"] = round(Double(turnFrames) / turnUs * 1e6, 1) }
      if holdUs > 0 { out["fps_holds"] = round(Double(holdFrames) / holdUs * 1e6, 1) }
    }
    out["hitches"] = summarizeHitches(readHitches("\(dir)/fable_2.log"), first: f0, last: f1,
                                      intervals: readWindowIntervals("\(dir)/window-frames.csv"))
    if !arm.census {  // census arms keep the full frame CSV for tools/f2diag census-cost
      try? fm.removeItem(atPath: "\(dir)/\(csvName)")  // the per-frame window CSV is kept instead
    }
    return out
  }

  func execute() throws -> String {
    try fm.createDirectory(atPath: "\(state)/logs", withIntermediateDirectories: true)
    log("benchmark start; state \(state)")
    // Flash kit: every arm once per rep, reps interleaved (A B C … A B C …) so drift hits every arm alike.
    let ids = options.flash ? (1...options.reps).flatMap { r in options.arms.map { "\($0)-r\(r)" } } : options.arms
    let arms = ids.compactMap(Arm.make)
    if arms.isEmpty { throw BenchError.message("No valid arms in \(options.arms)") }
    let machine = machineInfo()
    try prepareGameData()
    let runs = "\(state)/runs"
    try? fm.removeItem(atPath: runs)
    var results: [[String: Any]] = []
    for (i, arm) in arms.enumerated() {
      if cancelled { throw BenchError.cancelled }
      results.append(try runArmChecked(arm, index: i, count: arms.count, runs: runs))
    }
    status("Writing the report…", 0.97)
    return try writeReport(machine: machine, results: results, runs: runs)
  }

  /// One arm with the kit's failure policy: a game that crashes or exits abnormally AFTER the arm's data was
  /// written keeps the data (noted in the report); one that fails BEFORE is retried once (the first attempt's
  /// folder is kept as <arm>.attempt1); a second failure is recorded as failed and the kit goes on with the
  /// other arms. Every outcome is written to the result ("outcome") and to report.txt ("Run problems").
  func runArmChecked(_ arm: Arm, index: Int, count: Int, runs: String) throws -> [String: Any] {
    var first: [String: Any]?
    for attempt in 1...2 {
      var r: [String: Any]
      do {
        r = try runArm(arm, index: index, count: count, runs: runs)
      } catch BenchError.cancelled {
        throw BenchError.cancelled
      } catch {
        log("arm \(arm.id): could not run: \(error.localizedDescription)")
        r = ["arm": arm.id, "title": arm.title, "resolution_scale": arm.scale, "fps_cap": arm.capped ? "60" : "uncapped",
             "cold_cache": arm.cold, "error": "the run could not start: \(error.localizedDescription)",
             "abnormal_exit": true, "has_data": false]
        if arm.flash { r["flash_arm"] = arm.flashArm; r["env"] = arm.env }
      }
      let abnormal = (r["abnormal_exit"] as? Bool) == true
      let how: String = {
        if let sig = r["exit_signal"] { return "the game crashed (signal \(sig))" }
        if (r["timed_out"] as? Bool) == true { return "the game timed out after \(Int(options.armTimeout)) s" }
        if let e = r["error"] as? String, (r["exit_status"] as? Int) == nil { return e }
        return "the game exited with status \(r["exit_status"] ?? "?")"
      }()
      if (r["has_data"] as? Bool) == true {
        r["outcome"] = abnormal ? "data kept: \(how) after the arm's data was written" : "ok"
        if let f = first {
          r["first_attempt"] = f
          r["outcome"] = "ok on the retry" + (abnormal ? " (\(how) after the data was written)" : "")
        }
        if abnormal { log("arm \(arm.id): \(r["outcome"]!)") }
        return r
      }
      let reason = "\(how) before the arm's data was written" + ((r["error"] as? String).map { " (\($0))" } ?? "")
      if attempt == 1 {
        log("arm \(arm.id): no data: \(reason); retrying once")
        let dir = "\(runs)/\(arm.id)", kept = "\(runs)/\(arm.id).attempt1"
        try? fm.removeItem(atPath: kept)
        try? fm.moveItem(atPath: dir, toPath: kept)
        var f: [String: Any] = ["outcome": reason, "folder": "raw/\(arm.id)/attempt1"]
        for key in ["exit_status", "exit_signal", "timed_out", "crash_reports", "crash_summaries", "error", "launch_to_exit_s"] where r[key] != nil {
          f[key] = r[key]
        }
        first = f
        if cancelled { throw BenchError.cancelled }
        continue
      }
      log("arm \(arm.id): FAILED twice: \(reason); continuing with the other arms")
      r["failed"] = true
      r["outcome"] = "FAILED (no data after 2 attempts): \(reason)"
      r["first_attempt"] = first
      return r
    }
    fatalError("unreachable")
  }

  /// Raw files of one run into the report folder. A run that did not exit cleanly, or was retried, keeps its
  /// whole run folder (game and launcher logs, flight-recorder dumps, crash reports) and the first attempt's.
  func copyRaw(_ r: [String: Any], runs: String, folder: String, files: [String]) {
    guard let id = r["arm"] as? String else { return }
    let raw = "\(folder)/raw/\(id)"
    try? fm.createDirectory(atPath: raw, withIntermediateDirectories: true)
    let full = (r["outcome"] as? String).map { $0 != "ok" } ?? false
    let names = full ? ((try? fm.contentsOfDirectory(atPath: "\(runs)/\(id)")) ?? []) : files
    for file in names {
      try? fm.copyItem(atPath: "\(runs)/\(id)/\(file)", toPath: "\(raw)/\(file)")
    }
    if r["first_attempt"] != nil {
      try? fm.copyItem(atPath: "\(runs)/\(id).attempt1", toPath: "\(raw)/attempt1")
    }
  }

  func appInfo() -> [String: Any] {
    var a: [String: Any] = [:]
    for key in ["Fable2VersionString", "Fable2GitSHA", "Fable2GitDirty", "Fable2PortableBuilt", "Fable2ShaderSeedID",
                "Fable2BenchmarkSave", "Fable2MSCMinTarget", "CFBundleShortVersionString"] {
      if let v = bundle.object(forInfoDictionaryKey: key) { a[key] = v }
    }
    let present = ["/var/db/xcode_select_link", "/Applications/Xcode.app", "/Library/Developer/CommandLineTools"]
      .contains { fm.fileExists(atPath: $0) }
    let assumeNone = ProcessInfo.processInfo.environment["FABLE2_NS_ASSUME_NO_XCODE"] == "1"
    a["metal_toolchain"] = assumeNone ? "treated as absent (FABLE2_NS_ASSUME_NO_XCODE=1; bundled shaders only)"
      : present ? "present (optimized shaders may be compiled locally)" : "absent (bundled shaders only)"
    return a
  }

  func writeReport(machine: [String: Any], results: [[String: Any]], runs: String) throws -> String {
    if options.flash { return try writeFlashReport(machine: machine, results: results, runs: runs) }
    let stamp: String = {
      let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd_HH-mm"; return f.string(from: Date())
    }()
    let model = ((machine["chip"] as? String) ?? "Mac").replacingOccurrences(of: "Apple ", with: "")
      .replacingOccurrences(of: " ", with: "-")
    let name = "\(model)-\(machine["model_identifier"] ?? "")-\(stamp)".replacingOccurrences(of: ",", with: "_")
    let parent = options.out ?? "\(home)/Desktop/Fable II Benchmark"
    var folder = "\(parent)/\(name)"
    do {
      try fm.createDirectory(atPath: folder, withIntermediateDirectories: true)
    } catch {  // e.g. no Desktop access granted: keep the report in the benchmark folder
      folder = "\(state)/reports/\(name)"
      try fm.createDirectory(atPath: folder, withIntermediateDirectories: true)
    }
    var app = appInfo()
    if let label = options.label { app["label"] = label }
    let report: [String: Any] = ["schema": 1, "created": iso8601(), "machine": machine, "app": app,
                                 "plan": "\((results.first?["plan"] as? String) ?? "headings8") (tools/f2diag), Bower Lake save, GPU time trace on",
                                 "arms": results]
    let json = try JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys])
    try json.write(to: URL(fileURLWithPath: "\(folder)/report.json"))
    try text(report: report).write(toFile: "\(folder)/report.txt", atomically: true, encoding: .utf8)
    // Raw per-run files (small): result.json, logs, per-frame window CSV.
    for r in results {
      guard let id = r["arm"] as? String else { continue }
      var files = ["result.json", "window-frames.csv", "fable_2.log", "launcher.log"]
      if id.hasPrefix("census") {  // pass and frame traces for tools/f2diag census-cost
        files += ((try? fm.contentsOfDirectory(atPath: "\(runs)/\(id)")) ?? [])
          .filter { ($0.hasPrefix("passes-") || $0.hasPrefix("frames-")) && $0.hasSuffix(".csv") }
      }
      copyRaw(r, runs: runs, folder: folder, files: files)
    }
    try? fm.copyItem(atPath: "\(state)/benchmark.log", toPath: "\(folder)/benchmark.log")
    log("report: \(folder)")
    return folder
  }

  /// Flash kit report: report.json, report.txt (flash score per arm), examples/ (the worst moments), raw/.
  func writeFlashReport(machine: [String: Any], results: [[String: Any]], runs: String) throws -> String {
    let stamp: String = {
      let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd_HH-mm"; return f.string(from: Date())
    }()
    let model = ((machine["chip"] as? String) ?? "Mac").replacingOccurrences(of: "Apple ", with: "")
      .replacingOccurrences(of: " ", with: "-")
    let name = "\(model)-\(machine["model_identifier"] ?? "")-\(stamp)-flash".replacingOccurrences(of: ",", with: "_")
    let parent = options.out ?? "\(home)/Desktop/Fable II Flash Kit"
    var folder = "\(parent)/\(name)"
    do {
      try fm.createDirectory(atPath: folder, withIntermediateDirectories: true)
    } catch {
      folder = "\(state)/reports/\(name)"
      try fm.createDirectory(atPath: folder, withIntermediateDirectories: true)
    }
    try? fm.createDirectory(atPath: "\(folder)/examples", withIntermediateDirectories: true)
    // Per run: raw files + example PNGs.
    for r in results {
      guard let id = r["arm"] as? String else { continue }
      copyRaw(r, runs: runs, folder: folder,
              files: ["result.json", "flash.json", "flash-frames.csv", "window-frames.csv", "fable_2.log", "launcher.log"])
      for png in ((try? fm.contentsOfDirectory(atPath: "\(runs)/\(id)")) ?? []) where png.hasPrefix("flash-") && png.hasSuffix(".png") {
        try? fm.copyItem(atPath: "\(runs)/\(id)/\(png)", toPath: "\(folder)/examples/\(id)-\(png)")
      }
    }
    // Per arm: sums over its runs.
    var order: [String] = []
    var byArm: [String: [[String: Any]]] = [:]
    for r in results {
      let a = (r["flash_arm"] as? String) ?? (r["arm"] as? String ?? "?")
      if byArm[a] == nil { order.append(a) }
      byArm[a, default: []].append(r)
    }
    func num(_ v: Any?) -> Double { (v as? NSNumber)?.doubleValue ?? 0 }
    var summary: [[String: Any]] = []
    for a in order {
      let all = byArm[a]!
      // Runs without data (failed twice) do not count as zero flashes: they are left out of the averages.
      let runs = all.filter { ($0["has_data"] as? Bool) != false }
      var flash = 0.0, crack = 0.0, frames = 0.0, worstFlash = 0.0, worstCrack = 0.0
      var perRun: [String] = []
      var errors: [String] = []
      for r in all where (r["has_data"] as? Bool) == false {
        errors.append("\(r["arm"] ?? "?"): \(r["outcome"] ?? r["error"] ?? "no data"); left out of the averages")
        perRun.append("failed")
      }
      for r in runs {
        let f = r["flash"] as? [String: Any] ?? [:]
        if let e = f["error"] { errors.append("\(r["arm"] ?? "?"): \(e)") }
        if let e = r["error"] { errors.append("\(r["arm"] ?? "?"): \(e)") }
        flash += num(f["flash_frames"]); crack += num(f["crack_frames"]); frames += num(f["frames_scored"])
        worstFlash = max(worstFlash, num(f["worst_flash_cluster"])); worstCrack = max(worstCrack, num(f["worst_crack_cluster"]))
        perRun.append("\(Int(num(f["flash_frames"])))+\(Int(num(f["crack_frames"])))")
      }
      let first = runs.first ?? all.first ?? [:]
      let trace = first["tess_trace"] as? [String: Any] ?? [:]
      let n = Double(max(1, runs.count))
      summary.append(["arm": a, "title": ((first["title"] as? String) ?? a).replacingOccurrences(of: #" \(run \d+\)$"#, with: "", options: .regularExpression),
                      "runs": runs.count, "failed_runs": all.count - runs.count, "frames_scored": Int(frames), "flash_frames": Int(flash), "crack_frames": Int(crack),
                      "flash_per_run": round(flash / n, 1), "crack_per_run": round(crack / n, 1),
                      "worst_flash_cells": Int(worstFlash), "worst_crack_cells": Int(worstCrack),
                      "per_run": perRun, "env": first["env"] ?? [:],
                      "snap_uv": trace["snap_uv"] ?? "not logged", "snap_uv_domain_shaders": trace["snap_uv_domain_shaders_logged"] ?? 0,
                      "tess_device": trace["device"] ?? "not logged", "errors": errors])
    }
    // Compared with adaptive off (the floor: scene content that flickers by itself), per run.
    if let base = summary.first(where: { $0["arm"] as? String == "noadapt" }) {
      for i in summary.indices {
        summary[i]["flash_vs_adaptive_off"] = round(num(summary[i]["flash_per_run"]) - num(base["flash_per_run"]), 1)
        summary[i]["crack_vs_adaptive_off"] = round(num(summary[i]["crack_per_run"]) - num(base["crack_per_run"]), 1)
      }
    }
    var app = appInfo()
    if let label = options.label { app["label"] = label }
    let report: [String: Any] = ["schema": 2, "kind": "flash", "created": iso8601(), "machine": machine, "app": app,
                                 "plan": "flash (turns + walking, ~12 s), 2x, 60 FPS cap, FABLE2_NATIVE_TESS_TRACE=1",
                                 "summary": summary, "arms": results]
    let json = try JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys])
    try json.write(to: URL(fileURLWithPath: "\(folder)/report.json"))
    try flashText(report: report).write(toFile: "\(folder)/report.txt", atomically: true, encoding: .utf8)
    try? fm.copyItem(atPath: "\(state)/benchmark.log", toPath: "\(folder)/flash-kit.log")
    log("report: \(folder)")
    return folder
  }

  func flashText(report: [String: Any]) -> String {
    let m = report["machine"] as? [String: Any] ?? [:]
    let a = report["app"] as? [String: Any] ?? [:]
    var s = "Fable II Recomp flash kit — \(report["created"] ?? "")\n\n"
    s += "Mac:      \(m["chip"] ?? "?") (\(m["model_identifier"] ?? "?")), \(m["gpu_cores"] ?? "?") GPU cores, macOS \(m["macos"] ?? "?")\n"
    s += "GPU:      \(m["gpu_name"] ?? "?")\n"
    s += "App:      \(a["Fable2VersionString"] ?? "?") sha \(a["Fable2GitSHA"] ?? "?")\((a["Fable2GitDirty"] as? Bool) == true ? " (dirty)" : "")\n"
    if let label = a["label"] { s += "Run:      \(label)\n" }
    s += "Plan:     \(report["plan"] ?? "")\n\n"
    let header = ["Arm", "flash", "crack", "vs off", "per run", "largest", "frames"]
    let widths = [46, 8, 8, 13, 16, 10, 7]
    s += zip(header, widths).map { h, w in h.padding(toLength: w, withPad: " ", startingAt: 0) }.joined() + "\n"
    let summary = report["summary"] as? [[String: Any]] ?? []
    func signed(_ v: Any?) -> String { let d = num(v); return d > 0 ? "+\(d)" : "\(d)" }
    for r in summary {
      var row = ((r["title"] as? String) ?? "?").padding(toLength: 46, withPad: " ", startingAt: 0)
      let vs = r["flash_vs_adaptive_off"] == nil ? "-" : "\(signed(r["flash_vs_adaptive_off"]))/\(signed(r["crack_vs_adaptive_off"]))"
      let cols: [String] = ["\(r["flash_per_run"] ?? 0)", "\(r["crack_per_run"] ?? 0)", vs,
                            ((r["per_run"] as? [String]) ?? []).joined(separator: " "),
                            "\(r["worst_flash_cells"] ?? 0)/\(r["worst_crack_cells"] ?? 0)", "\(r["frames_scored"] ?? 0)"]
      for (c, w) in zip(cols, widths.dropFirst()) { row += c.padding(toLength: w, withPad: " ", startingAt: 0) }
      s += row + "\n"
      for e in (r["errors"] as? [String]) ?? [] { s += "    ERROR: \(e)\n" }
    }
    let problems = problemLines((report["arms"] as? [[String: Any]]) ?? [])
    if !problems.isEmpty {
      s += "\nRun problems (crash reports and logs in raw/<run>/):\n" + problems.joined(separator: "\n") + "\n"
    }
    s += """

      flash / crack = frames per run (~12 s, ~700 frames) with something white for exactly one frame: white
      (every colour channel >= 0.55), bright (luma >= 0.65) and >= 0.25 brighter than everything around it in
      BOTH the previous and the next frame, in a cluster of at least 6 cells of a 480-cell-wide grid. Only the
      ground is scored: the top 30% of the screen (sky through leaves, lamps, quest text), the d-pad HUD and a
      4% border are skipped.
        flash = by the cell's average colour (patches a few pixels wide and up);
        crack = by the cell's whitest pixel (hairlines and single pixels, e.g. the sky seen through a crack).
      Scene content flickers by itself too (sky through leaves, sparkles), so read an arm against "Adaptive
      terrain off": vs off = flash/crack per run minus that arm's. per run = flash+crack of each run.
      largest = the biggest flash/crack in cells (a cell is ~5x5 pixels at 2x). Lower is better.

      """
    s += "Switches per arm (checked in the game log):\n"
    for r in summary {
      let env = (r["env"] as? [String: String] ?? [:]).filter { $0.key != "FABLE2_NATIVE_TESS_TRACE" }
        .sorted { $0.key < $1.key }.map { "\($0.key.replacingOccurrences(of: "FABLE2_", with: ""))=\($0.value)" }.joined(separator: " ")
      s += "  \((r["title"] as? String) ?? "?"): \(env)\n"
      s += "    \(r["tess_device"] ?? "")\n"
      s += "    snap-uv: \(r["snap_uv"] ?? "")\(num(r["snap_uv_domain_shaders"]) > 0 ? ", \(r["snap_uv_domain_shaders"]!) domain shaders compiled snapped" : "")\n"
    }
    func num(_ v: Any?) -> Double { (v as? NSNumber)?.doubleValue ?? 0 }
    s += "\nWorst moments (examples/: previous | frame | next | detected cells: red = flash, orange = crack):\n"
    for r in (report["arms"] as? [[String: Any]]) ?? [] {
      let f = r["flash"] as? [String: Any] ?? [:]
      for e in ((f["examples"] as? [[String: Any]]) ?? []).prefix(3)
      where max(num(e["flash_cluster"]), num(e["crack_cluster"])) >= 6 {
        s += "  \(r["arm"] ?? "?")-\(e["png"] ?? ""): frame \(e["frame"] ?? "?"), flash \(e["flash_cluster"] ?? 0) / crack \(e["crack_cluster"] ?? 0) cells\n"
      }
    }
    for r in (report["arms"] as? [[String: Any]]) ?? [] {
      let f = r["flash"] as? [String: Any] ?? [:]
      if num(f["induced"]) > 0 {
        s += "\(r["arm"] ?? "?"): detector check, induced white shapes found: \(f["induced_flash_detected"] ?? 0) as flash, \(f["induced_crack_detected"] ?? 0) as crack, of \(f["induced_scored"] ?? 0)\n"
        for d in (f["induced_frames"] as? [[String: Any]]) ?? [] {
          s += "    frame \(d["frame"] ?? "?") \(d["shape"] ?? "?"): flash \(d["flash"] ?? 0), crack \(d["crack"] ?? 0)\n"
        }
      }
    }
    for r in (report["arms"] as? [[String: Any]]) ?? [] {
      if let st = r["sweep"] as? [String: Any], let fps = st["fps_mean"] {
        s += "\(r["arm"] ?? "?"): FPS \(fps), p99 \(st["frame_ms_p99"] ?? "-") ms\n"
      }
    }
    return s
  }

  /// --reanalyze: the current hitch rule and warm-up summary applied to an existing report's raw files.
  func reanalyze(_ folder: String) throws -> String {
    guard let data = fm.contents(atPath: "\(folder)/report.json"),
          var report = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
      throw BenchError.message("no report.json in \(folder)")
    }
    var arms: [[String: Any]] = []
    for var arm in (report["arms"] as? [[String: Any]]) ?? [] {
      let raw = "\(folder)/raw/\(arm["arm"] as? String ?? "?")"
      if let rd = fm.contents(atPath: "\(raw)/result.json"),
         let game = try? JSONSerialization.jsonObject(with: rd) as? [String: Any],
         let sweep = game["sweep"] as? [String: Any], let f0 = sweep["frame_start"] as? Int, let f1 = sweep["frame_end"] as? Int {
        arm["hitches"] = summarizeHitches(readHitches("\(raw)/fable_2.log"), first: f0, last: f1,
                                          intervals: readWindowIntervals("\(raw)/window-frames.csv"))
      }
      let warmup = readShaderWarmup("\(raw)/fable_2.log")
      if !warmup.isEmpty { arm["shader_warmup"] = warmup }
      arms.append(arm)
    }
    report["arms"] = arms
    report["reanalyzed"] = iso8601()
    let out = options.out ?? "\(folder)/reanalyzed"
    try fm.createDirectory(atPath: out, withIntermediateDirectories: true)
    let json = try JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys])
    try json.write(to: URL(fileURLWithPath: "\(out)/report.json"))
    try text(report: report).write(toFile: "\(out)/report.txt", atomically: true, encoding: .utf8)
    return out
  }

  func text(report: [String: Any]) -> String {
    let m = report["machine"] as? [String: Any] ?? [:]
    let a = report["app"] as? [String: Any] ?? [:]
    var s = "Fable II Recomp benchmark — \(report["created"] ?? "")\n\n"
    s += "Mac:      \(m["chip"] ?? "?") (\(m["model_identifier"] ?? "?")), \(m["performance_cores"] ?? "?")P + \(m["efficiency_cores"] ?? "?")E CPU cores, \(m["gpu_cores"] ?? "?") GPU cores, \(m["memory_gb"] ?? "?") GB\n"
    s += "macOS:    \(m["macos"] ?? "?") (\(m["macos_build"] ?? ""))\n"
    for d in (m["displays"] as? [[String: Any]]) ?? [] {
      s += "Display:  \(d["name"] ?? "?") \(d["pixels"] ?? "?") px (\(d["points"] ?? "?") pt), up to \(d["max_fps"] ?? "?") Hz\((d["main"] as? Bool) == true ? ", main" : "")\n"
    }
    s += "Power:    \(m["power_source"] ?? "?"), low power mode \((m["low_power_mode"] as? Bool) == true ? "ON" : "off"), thermal \(m["thermal_state"] ?? "?") at start\n"
    s += "App:      \(a["Fable2VersionString"] ?? "?") sha \(a["Fable2GitSHA"] ?? "?")\((a["Fable2GitDirty"] as? Bool) == true ? " (dirty)" : ""), built \(a["Fable2PortableBuilt"] ?? "?")\n"
    if let label = a["label"] { s += "Run:      \(label)\n" }
    s += "Shaders:  Metal toolchain \(a["metal_toolchain"] ?? "?")\n"
    s += "Plan:     \(report["plan"] ?? "")\n\n"
    func cell(_ v: Any?, _ width: Int, count: Bool = false) -> String {
      var t: String
      switch v {
      case let n as NSNumber where count: t = String(n.intValue)  // a JSON number reads back as Double (--reanalyze)
      case let d as Double: t = String(format: d >= 100 ? "%.0f" : "%.1f", d)
      case let i as Int: t = String(i)
      case let x?: t = "\(x)"
      default: t = "-"
      }
      return String(repeating: " ", count: max(0, width - t.count)) + t
    }
    let header = ["Run", "FPS", "1%low", "p99ms", "maxms", ">33ms", "GPUms", "GPUp95", "CPUcores", "MemMB", "hitches", "boot s"]
    let widths = [36, 7, 7, 7, 7, 6, 7, 7, 9, 7, 8, 7]
    s += zip(header, widths).map { h, w in h.padding(toLength: w, withPad: " ", startingAt: 0) }.joined() + "\n"
    for r in (report["arms"] as? [[String: Any]]) ?? [] {
      let st = r["sweep"] as? [String: Any] ?? [:]
      let h = r["hitches"] as? [String: Any] ?? [:]
      var row = ((r["title"] as? String) ?? "?").padding(toLength: 36, withPad: " ", startingAt: 0)
      row += cell(st["fps_mean"], 6) + " " + cell(st["fps_1pct_low"], 6) + " " + cell(st["frame_ms_p99"], 6) + " "
      row += cell(st["frame_ms_max"], 6) + " " + cell(st["frames_over_33ms"], 5, count: true) + " " + cell(st["gpu_ms_mean"], 6) + " "
      row += cell(st["gpu_ms_p95"], 6) + " " + cell(st["process_cpu_cores"], 8) + " " + cell(st["memory_footprint_mb_peak"], 6) + " "
      row += cell(h["window_count"], 7, count: true) + " " + cell(r["gameplay_s"], 6)
      s += row + "\n"
      if let e = r["error"] { s += "    ERROR: \(e)\n" }
    }
    s += """

      FPS = mean over the 8-heading camera sweep (~11 s; headings8-long ~24 s); 1%low = 1000 / mean of the worst 1% frame times;
      p99ms / maxms = frame-time percentile / maximum; >33ms = frames longer than 33.4 ms; GPUms = GPU time
      per frame (command buffers, mean) and its 95th percentile; CPUcores = process CPU time / wall time;
      MemMB = peak memory footprint; hitches = stutters in the sweep: frames >= 1.5 x the median of the previous
      32 frames and >= 40 ms, or any frame >= 100 ms (steady slow frames of a GPU-bound Mac are not hitches);
      boot s = launch to gameplay. The cold run starts from an empty cache (only the app's bundled shaders) and,
      when it says so below, an empty macOS shader cache for the game.

      """
    for r in (report["arms"] as? [[String: Any]]) ?? [] {
      let w = r["shader_warmup"] as? [String: Any] ?? [:]
      guard (r["cold_cache"] as? Bool) == true || r["os_shader_cache"] != nil else { continue }
      var line = "\((r["title"] as? String) ?? "?"): "
      if let c = r["os_shader_cache"] as? [String: Any] {
        if (c["cleared"] as? Bool) == true {
          line += "truly cold (macOS shader cache for \(c["bundle_id"] ?? "?") cleared, \(c["removed_mb"] ?? 0) MB)"
        } else {
          line += "NOT truly cold (macOS shader cache kept: \(c["reason"] ?? "?"))"
        }
      } else {
        line += "macOS shader cache state unknown (older benchmark)"
      }
      if let n = w["pipelines_compiled"], let ms = w["compile_ms_summed"] {
        line += "; warm-up compiled \(n) pipelines, \(ms) ms summed"
        if let each = w["compile_ms_per_pipeline"] { line += " (\(each) ms each)" }
      }
      if let n = w["transfer_pipelines_built"], let ms = w["transfer_ms_summed"] {
        line += ", \(n) transfer pipelines \(ms) ms"
      }
      if r["os_shader_cache"] == nil || (r["os_shader_cache"] as? [String: Any])?["cleared"] as? Bool != true,
         let each = w["compile_ms_per_pipeline"] as? Double, each < 2 {
        line += " -- under 2 ms each: served from macOS's shader cache, not compiled"
      }
      s += line + "\n"
    }
    for r in (report["arms"] as? [[String: Any]]) ?? [] {
      if let h = r["hitches"] as? [String: Any], let abs = h["window_count_absolute"] {
        s += "\((r["title"] as? String) ?? "?"): hitches reclassified from an older build's log (\(abs) frames >= 40 ms)\n"
      }
    }
    for r in (report["arms"] as? [[String: Any]]) ?? [] {
      let st = r["sweep"] as? [String: Any] ?? [:]
      if let t = st["thread_cpu_cores"] as? [String: Double] {
        s += "\((r["title"] as? String) ?? "?"): thread CPU (cores) " +
          t.sorted { $0.key < $1.key }.map { "\($0.key) \($0.value)" }.joined(separator: ", ")
        if let ft = r["fps_turns"], let fh = r["fps_holds"] { s += "; FPS turns \(ft) / holds \(fh)" }
        if let rs = r["ready_after_gameplay_s"] as? Double {
          s += String(format: "; ready %.1f s after gameplay", rs) + ((r["ready_timeout"] as? Bool) == true ? " (TIMEOUT)" : "")
        }
        if let h = r["hitches"] as? [String: Any], let c = h["window_ms_by_cause"] as? [String: Double], !c.isEmpty {
          s += "; hitch ms by cause " + c.sorted { $0.value > $1.value }.map { "\($0.key) \($0.value)" }.joined(separator: " ")
        }
        s += "\n"
      }
    }
    s += switchLines((report["arms"] as? [[String: Any]]) ?? [])
    let problems = problemLines((report["arms"] as? [[String: Any]]) ?? [])
    if !problems.isEmpty {
      s += "\nRun problems (crash reports and logs in raw/<run>/):\n" + problems.joined(separator: "\n") + "\n"
    }
    return s
  }
}

/// report.txt: each switch arm against its base arm (same scale, cap and tracing), e.g. 2u-ssm vs 2u.
func switchLines(_ arms: [[String: Any]]) -> String {
  var byId: [String: [String: Any]] = [:]
  for r in arms { if let id = r["arm"] as? String { byId[id] = r } }
  var lines: [String] = []
  for r in arms {
    guard let id = r["arm"] as? String, let (baseId, name) = SwitchArms.split(id), let sw = SwitchArms.all[name],
          let b = byId[baseId] else { continue }
    let st = r["sweep"] as? [String: Any] ?? [:], bt = b["sweep"] as? [String: Any] ?? [:]
    let env = sw.env.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }.joined(separator: " ")
    var line = "\(sw.title) (\(env)) vs default, \((b["title"] as? String) ?? baseId):"
    if let f0 = bt["fps_mean"] as? Double, let f1 = st["fps_mean"] as? Double, f0 > 0 {
      line += String(format: " FPS %.1f -> %.1f (%+.1f%%)", f0, f1, (f1 / f0 - 1) * 100)
    } else {
      line += " FPS missing"
    }
    if let g0 = bt["gpu_ms_mean"] as? Double, let g1 = st["gpu_ms_mean"] as? Double {
      line += String(format: ", GPU ms %.2f -> %.2f (%+.2f)", g0, g1, g1 - g0)
    }
    if let p0 = bt["frame_ms_p99"] as? Double, let p1 = st["frame_ms_p99"] as? Double {
      line += String(format: ", p99 ms %.1f -> %.1f", p0, p1)
    }
    lines.append(line)
  }
  return lines.isEmpty ? "" : "\nSwitch arms (same Mac, same run; a few % is within run-to-run noise):\n"
    + lines.joined(separator: "\n") + "\n"
}

// MARK: - UI

final class AppDelegate: NSObject, NSApplicationDelegate {
  let options: Options
  var window: NSWindow!
  var label: NSTextField!
  var bar: NSProgressIndicator!
  var runner: Runner!

  init(options: Options) { self.options = options }

  func applicationDidFinishLaunching(_ notification: Notification) {
    NSApp.setActivationPolicy(.regular)
    NSApp.activate(ignoringOtherApps: true)
    let intro = NSAlert()
    let runs = options.flash ? options.arms.count * options.reps : options.arms.count
    intro.messageText = options.flash ? "Fable II Flash Kit" : "Fable II Benchmark"
    intro.informativeText = options.flash ? """
      This looks for white flashes on the terrain. The game opens and closes by itself \(runs) times \
      (about 5–10 minutes): each run walks and turns the camera at Bower Lake with different tessellation \
      settings, and the game itself detects flashes — you don't need to watch.

      Don't use the Mac until it finishes. The report goes to the folder "Fable II Flash Kit" on your Desktop.
      """ : """
      This measures how Fable II runs on this Mac. The game opens and closes by itself 7 times \
      (about 5–15 minutes; the first time it also asks for your Fable II disc image).

      Plug in the power adapter, close other apps, and don't use the Mac until it finishes. \
      The report goes to the folder "Fable II Benchmark" on your Desktop.
      """
    intro.addButton(withTitle: "Start")
    intro.addButton(withTitle: "Quit")
    if intro.runModal() != .alertFirstButtonReturn { NSApp.terminate(nil); return }

    window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 120), styleMask: [.titled, .miniaturizable],
                      backing: .buffered, defer: false)
    window.title = options.flash ? "Fable II Flash Kit" : "Fable II Benchmark"
    let content = window.contentView!
    label = NSTextField(wrappingLabelWithString: "Starting…")
    label.frame = NSRect(x: 20, y: 56, width: 480, height: 48)
    bar = NSProgressIndicator(frame: NSRect(x: 20, y: 24, width: 380, height: 20))
    bar.isIndeterminate = false
    bar.minValue = 0
    bar.maxValue = 1
    let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel))
    cancel.frame = NSRect(x: 410, y: 18, width: 90, height: 30)
    [label, bar, cancel].forEach { content.addSubview($0!) }
    window.center()
    window.makeKeyAndOrderFront(nil)
    runner = Runner(options: options) { [weak self] text, progress in
      DispatchQueue.main.async { self?.label.stringValue = text; self?.bar.doubleValue = progress }
    }
    Thread.detachNewThread { [self] in
      do {
        let folder = try runner.execute()
        DispatchQueue.main.async { self.finish(folder: folder, error: nil) }
      } catch {
        DispatchQueue.main.async { self.finish(folder: nil, error: error) }
      }
    }
  }

  @objc func cancel() {
    runner.cancelled = true
    label.stringValue = "Cancelling…"
  }

  func finish(folder: String?, error: Error?) {
    NSApp.activate(ignoringOtherApps: true)
    let alert = NSAlert()
    if let folder = folder {
      alert.messageText = options.flash ? "Flash check finished" : "Benchmark finished"
      alert.informativeText = "The report is in:\n\(folder)\n\nreport.txt is the summary; please send the whole folder."
      alert.addButton(withTitle: "Show Report")
      alert.addButton(withTitle: "Done")
      if alert.runModal() == .alertFirstButtonReturn {
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: "\(folder)/report.txt")])
      }
    } else if case BenchError.cancelled? = error {
      // nothing to report
    } else {
      alert.messageText = options.flash ? "The flash check stopped" : "The benchmark stopped"
      alert.informativeText = (error?.localizedDescription ?? "Unknown error") +
        "\n\nLogs: ~/Library/Application Support/Fable II Recomp \(options.flash ? "Flash Kit" : "Benchmark")/"
      alert.runModal()
    }
    NSApp.terminate(nil)
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

@main
struct Main {
  static func main() {
    let options = Options.parse(CommandLine.arguments)
    if let folder = options.reanalyze {
      do {
        print(try Runner(options: options) { _, _ in }.reanalyze(folder))
        exit(0)
      } catch {
        FileHandle.standardError.write("reanalyze failed: \(error.localizedDescription)\n".data(using: .utf8)!)
        exit(1)
      }
    }
    if options.headless {
      let runner = Runner(options: options) { text, progress in
        FileHandle.standardError.write("[\(Int(progress * 100))%] \(text)\n".data(using: .utf8)!)
      }
      signal(SIGTERM) { _ in exit(130) }
      do {
        let folder = try runner.execute()
        print(folder)
        exit(0)
      } catch {
        FileHandle.standardError.write("benchmark failed: \(error.localizedDescription)\n".data(using: .utf8)!)
        exit(1)
      }
    }
    let app = NSApplication.shared
    let delegate = AppDelegate(options: options)
    app.delegate = delegate
    app.run()
  }
}
