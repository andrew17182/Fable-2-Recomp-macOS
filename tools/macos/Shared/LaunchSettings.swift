// Launch settings shared by the Dev Launcher (tools/macos/DevLauncher/main.swift) and the player app
// (tools/macos/Player/main.swift, the portable "Fable II.app"): every setting, its storage keys, its
// defaults and the game environment it maps to. Compiled into both apps, so a new setting added here
// appears in both (CLAUDE.md: every new setting gets a launcher toggle).
//
// The game environment (gameEnvironment) is the single mapping: the Dev Launcher passes the same
// NAME=VALUE items to tools/fable2_dev_session.py (which adds the --fps / skip-intro items the same
// way), and the player app hands them to the game directly. tests/launcher_env_sync_tests.py checks
// that both apps give the game the same environment for the same settings.
import AppKit
import Foundation
import IOKit
import Metal
import MetalFX

/// Where settings are stored: UserDefaults (Dev Launcher) or a plist in the game's state folder (player app).
protocol SettingsStore: AnyObject {
  func object(forKey defaultName: String) -> Any?
  func string(forKey defaultName: String) -> String?
  func set(_ value: Any?, forKey defaultName: String)
}

extension UserDefaults: SettingsStore {}

/// A plist file as a settings store (written on every change).
final class PlistSettingsStore: SettingsStore {
  let path: String
  private(set) var values: [String: Any]
  init(path: String) {
    self.path = path
    values = (NSDictionary(contentsOfFile: path) as? [String: Any]) ?? [:]
  }
  func object(forKey defaultName: String) -> Any? { values[defaultName] }
  func string(forKey defaultName: String) -> String? { values[defaultName] as? String }
  func set(_ value: Any?, forKey defaultName: String) {
    values[defaultName] = value
    (values as NSDictionary).write(toFile: path, atomically: true)
  }
  func remove(keys: [String]) {
    for key in keys { values.removeValue(forKey: key) }
    (values as NSDictionary).write(toFile: path, atomically: true)
  }
}

/// An in-memory settings store (the saved Custom preset is kept as one dictionary of setting keys).
final class MemorySettingsStore: SettingsStore {
  private(set) var values: [String: Any]
  init(_ values: [String: Any] = [:]) { self.values = values }
  func object(forKey defaultName: String) -> Any? { values[defaultName] }
  func string(forKey defaultName: String) -> String? { values[defaultName] as? String }
  func set(_ value: Any?, forKey defaultName: String) { values[defaultName] = value }
}

/// GPU identity from the IORegistry, as the game reads it (src/core/fable2_fps_auto.cpp QueryGpu).
enum GpuInfo {
  static let read: (cores: Int?, model: String?) = {
    var iterator: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"),
                                       &iterator) == KERN_SUCCESS else { return (nil, nil) }
    defer { IOObjectRelease(iterator) }
    var cores: Int?
    var model: String?
    while case let entry = IOIteratorNext(iterator), entry != 0 {
      defer { IOObjectRelease(entry) }
      if cores == nil {
        cores = IORegistryEntryCreateCFProperty(entry, "gpu-core-count" as CFString, kCFAllocatorDefault, 0)?
          .takeRetainedValue() as? Int
      }
      if model == nil {
        model = IORegistryEntryCreateCFProperty(entry, "model" as CFString, kCFAllocatorDefault, 0)?
          .takeRetainedValue() as? String
      }
    }
    if model == nil {
      var size = 0
      if sysctlbyname("machdep.cpu.brand_string", nil, &size, nil, 0) == 0, size > 0 {
        var buf = [CChar](repeating: 0, count: size)
        if sysctlbyname("machdep.cpu.brand_string", &buf, &size, nil, 0) == 0 { model = String(cString: buf) }
      }
    }
    return (cores.flatMap { $0 > 0 ? $0 : nil }, model)
  }()
}

/// What the game's fps_60 = "auto" picks on this Mac. A copy of ChooseFrameTarget / ChooseBoth in
/// src/core/fable2_res_auto.h (tests/res_auto_sync_tests.py checks the tables match): 60 FPS when
/// the estimated uncapped mean GPU frame time at the scale that will run is <= maxMeanMs; at 2x,
/// uncapped when it is <= uncapped2xMaxMeanMs (the 2x rule, FABLE2_AUTO_2X); else 30. With
/// resolution Auto too, both are chosen together (both(...)). docs/resolution-scale.md §25.
enum AutoFrameRate {
  // BEGIN fps-auto tables (keep in sync with src/core/fable2_res_auto.h)
  static let frameModel: [(scale: Int, fixedMs: Double, work: Double)] = [
    (1, 3.0, 82.6),
    (2, 0.75, 261.0),
    (3, -1.1, 534.5),
  ]
  static let maxMeanMs = 18.5
  static let uncapped2xMaxMeanMs = 22.0
  static let baseTierMaxAutoFps = 30  // base chips: Auto never above this until one is measured
  // END fps-auto tables

  /// 60, 30, or 0 = uncapped.
  static func choose(name: String, cores: Int?, scale: Int, rule2x: Bool = true) -> Int {
    let effective = max(AutoResolution.effectiveCores(name: name, cores: cores), 0.1)
    let s = min(max(scale, 1), 3)
    let row = frameModel.first { $0.scale == s } ?? frameModel[0]
    let estimate = row.fixedMs + row.work / effective
    let base = AutoResolution.tier(of: name) == 0
    if estimate <= maxMeanMs { return base ? min(60, baseTierMaxAutoFps) : 60 }
    return !base && rule2x && s == 2 && estimate <= uncapped2xMaxMeanMs ? 0 : 30
  }
  /// Resolution and frame rate both Auto (ChooseBoth): frame target at 1x, the p95 rows at that
  /// target, then the 2x rule (below 2x, RAM >= 16 GB or unknown, 2x not 30 -> 2x at 60 or uncapped).
  static func both(name: String, cores: Int?, ramGb: Double, rule2x: Bool = true) -> (scale: Int, fps: Int) {
    let fps = choose(name: name, cores: cores, scale: 1, rule2x: rule2x)
    let scale = AutoResolution.choose(name: name, cores: cores, ramGb: ramGb, targetFps: fps)
    if rule2x && scale < 2 && (ramGb <= 0 || ramGb >= AutoResolution.minRamGb2x) {
      let at2 = choose(name: name, cores: cores, scale: 2, rule2x: true)
      if at2 != 30 { return (2, at2) }
    }
    return (scale, fps)
  }
  static var ramGbOfThisMac: Double { Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824 }
  /// Both Auto on this Mac.
  static func bothOnThisMac(rule2x: Bool) -> (scale: Int, fps: Int) {
    both(name: GpuInfo.read.model ?? "", cores: GpuInfo.read.cores, ramGb: ramGbOfThisMac, rule2x: rule2x)
  }
  /// This Mac at the given internal-resolution choice: 60, 30 or 0 (uncapped).
  static func onThisMac(resolution: InternalResolution, rule2x: Bool = true) -> Int {
    if resolution == .auto { return bothOnThisMac(rule2x: rule2x).fps }
    return choose(name: GpuInfo.read.model ?? "", cores: GpuInfo.read.cores,
                  scale: Int(resolution.rawValue) ?? 1, rule2x: rule2x)
  }
}

/// What the game's resolution_scale = "auto" picks. A copy of src/core/fable2_res_auto.h (the
/// documented source; tests/res_auto_sync_tests.py checks the tables match):
/// effective cores = gpu-core-count x generation factor x (0.75 on Ultra); the highest scale whose
/// row is met at the frame target and whose RAM floor is met, else 1x.
enum AutoResolution {
  // BEGIN res-auto tables (keep in sync with src/core/fable2_res_auto.h)
  static let generationFactors: [(generation: Int, factor: Double)] = [
    (1, 0.80), (2, 0.90), (3, 1.00), (4, 1.05), (5, 1.10),
  ]
  static let ultraEfficiency = 0.75
  static let scaleRows: [(targetFps: Int, need2x: Double, need3x: Double)] = [
    (60, 34.0, 47.0),
    (30, 17.0, 24.0),
  ]
  static let minRamGb2x = 16.0
  static let minRamGb3x = 32.0
  static let fallbackCores = [7, 14, 24, 48]  // base, Pro, Max, Ultra
  // END res-auto tables

  static func generation(of name: String) -> Int {
    let chars = Array(name)
    for i in 0..<max(0, chars.count - 1) where chars[i] == "M" && (i == 0 || chars[i - 1] == " ")
      && chars[i + 1].isASCII && chars[i + 1].isNumber {
      var g = 0
      var j = i + 1
      while j < chars.count, chars[j].isASCII, let d = chars[j].wholeNumberValue { g = g * 10 + d; j += 1 }
      return g
    }
    return 0
  }
  /// 0 base, 1 Pro, 2 Max, 3 Ultra.
  static func tier(of name: String) -> Int {
    name.contains("Ultra") ? 3 : name.contains("Max") ? 2 : name.contains("Pro") ? 1 : 0
  }
  /// gpu-core-count (or the tier's fallback) x generation factor x (0.75 on Ultra).
  static func effectiveCores(name: String, cores: Int?) -> Double {
    let t = tier(of: name)
    let gen = generation(of: name)
    let factor: Double
    if gen <= generationFactors[0].generation { factor = generationFactors[0].factor }
    else { factor = generationFactors.first { $0.generation == gen }?.factor ?? generationFactors.last!.factor }
    let n = Double(cores.flatMap { $0 > 0 ? $0 : nil } ?? fallbackCores[t])
    return n * factor * (t == 3 ? ultraEfficiency : 1.0)
  }
  static func choose(name: String, cores: Int?, ramGb: Double, targetFps: Int) -> Int {
    let effective = effectiveCores(name: name, cores: cores)
    let row = targetFps > 30 ? scaleRows[0] : scaleRows[1]
    let ramKnown = ramGb > 0
    if effective >= row.need3x && (!ramKnown || ramGb >= minRamGb3x) { return 3 }
    if effective >= row.need2x && (!ramKnown || ramGb >= minRamGb2x) { return 2 }
    return 1
  }
  /// This Mac at the given launcher frame-rate choice (Use game settings: the game's default,
  /// fps_60 = "auto").
  static func onThisMac(frameRate: FrameRate, rule2x: Bool = true) -> Int {
    let target: Int
    switch frameRate {
    case .thirty: target = 30
    case .sixty, .uncapped: target = 60
    // Frame rate Auto too: chosen together (frame target at 1x, then the 2x rule).
    case .settings, .auto: return AutoFrameRate.bothOnThisMac(rule2x: rule2x).scale
    }
    return choose(name: GpuInfo.read.model ?? "", cores: GpuInfo.read.cores,
                  ramGb: AutoFrameRate.ramGbOfThisMac, targetFps: target)
  }
}

enum FrameRate: String, CaseIterable, Identifiable {
  case settings, auto, thirty = "30", sixty = "60", uncapped
  var id: String { rawValue }
  var title: String { title(resolution: .auto) }
  /// Auto shows what the game will pick on this Mac at that internal-resolution choice
  /// ("Auto (60 FPS on this Mac)", "Auto (uncapped at 2× on this Mac)").
  func title(resolution: InternalResolution, rule2x: Bool = true) -> String {
    switch self {
    case .settings: return "Use game settings"
    case .auto:
      let fps = AutoFrameRate.onThisMac(resolution: resolution, rule2x: rule2x)
      if fps == 0 { return resolution == .auto ? "Auto (uncapped at 2× on this Mac)" : "Auto (uncapped on this Mac)" }
      return "Auto (\(fps) FPS on this Mac)"
    case .thirty: return "30 FPS"
    case .sixty: return "60 FPS"
    case .uncapped: return "Uncapped"
    }
  }
  /// The dev-session --fps value; nil for .settings and .auto (auto goes through --env instead,
  /// because tools/fable2_dev_session.py --fps only takes 30/60/uncapped).
  var argument: String? { self == .settings || self == .auto ? nil : rawValue }
  /// Auto is the default only when nothing was saved yet (new installs); a saved choice stays.
  static func saved(in defaults: SettingsStore) -> FrameRate {
    guard let raw = defaults.string(forKey: "frameRate") else { return .auto }
    return FrameRate(rawValue: raw) ?? .settings
  }
}

/// How the "Turbo 2x on/off" notice is shown (FABLE2_TURBO_NOTICE, docs/turbo.md).
enum TurboNotice: String, CaseIterable, Identifiable {
  case overlay, game
  var id: String { rawValue }
  var title: String {
    switch self {
    case .overlay: return "Overlay (Fable style)"
    case .game: return "In-game message (experimental)"
    }
  }
  static func saved(in defaults: SettingsStore) -> TurboNotice {
    TurboNotice(rawValue: defaults.string(forKey: "turboNotice") ?? "") ?? .overlay
  }
}

/// Audio while turbo is on (FABLE2_TURBO_AUDIO, [patches] turbo_audio; docs/turbo.md "Turbo audio").
enum TurboAudio: String, CaseIterable, Identifiable {
  case natural, highPitch = "high-pitch", normal
  var id: String { rawValue }
  var title: String {
    switch self {
    case .natural: return "Sped up, natural pitch"
    case .highPitch: return "Sped up, high pitch"
    case .normal: return "Normal speed"
    }
  }
  static func saved(in defaults: SettingsStore) -> TurboAudio {
    TurboAudio(rawValue: defaults.string(forKey: "turboAudio") ?? "") ?? .natural
  }
}

/// Turbo speed (FABLE2_TURBO_SPEED, [patches] turbo_scale): 1.1x to 4.0x in 0.1 steps; 1.0 is Off (the game
/// refuses F7, 7 and the controller button: the Original preset).
enum TurboSpeed {
  static let range: ClosedRange<Double> = 1.0...4.0
  static let off = 1.0
  static let defaultValue = 2.0
  /// Clamped to the range and rounded to tenths (the game quantizes the same way).
  static func clean(_ value: Double) -> Double {
    (min(max(value, range.lowerBound), range.upperBound) * 10).rounded() / 10
  }
  static func saved(in defaults: SettingsStore) -> Double {
    guard let value = defaults.object(forKey: "turboSpeed") as? Double else { return defaultValue }
    return clean(value)
  }
  static func label(_ value: Double) -> String { value == off ? "Off" : String(format: "%.1f×", value) }
}

/// Slow-motion speed (FABLE2_SLOW_SPEED, [patches] slow_scale): 0.25x to 0.9x in 0.05 steps (F8); 1.0 is Off (the
/// game refuses F8 and the controller button: the Original preset).
enum SlowSpeed {
  static let range: ClosedRange<Double> = 0.25...1.0
  static let off = 1.0
  static let defaultValue = 0.5
  /// Clamped to the range and rounded to twentieths (the game rounds the same way).
  static func clean(_ value: Double) -> Double {
    (min(max(value, range.lowerBound), range.upperBound) * 20).rounded() / 20
  }
  static func saved(in defaults: SettingsStore) -> Double {
    guard let value = defaults.object(forKey: "slowSpeed") as? Double else { return defaultValue }
    return clean(value)
  }
  static func label(_ value: Double) -> String { value == off ? "Off" : String(format: "%.2f×", value) }
}

/// Settings presets (docs/reference.md "Presets"). Original: as close to the Xbox 360 as possible (1×, 30 FPS,
/// intros, no extras, turbo and slow motion off). Enhanced (the default): the original feel at the resolution and
/// frame rate this Mac can hold, plus turbo and slow motion. Custom: a saved slot; any change to a preset setting
/// (LaunchSettings.presetRules) saves the current values there and selects it. Original and Enhanced are fixed in
/// code, so switching presets never loses the Custom values. Invisible fixes are never part of a preset.
enum SettingsPreset: String, CaseIterable, Identifiable {
  case original, enhanced, custom
  var id: String { rawValue }
  var title: String { rawValue.capitalized }
}

/// One setting a preset decides: copies it from the preset's values, or tells whether it differs from them.
struct PresetRule {
  let key: String
  let setOriginal: (inout LaunchSettings) -> Void
  let copy: (inout LaunchSettings, LaunchSettings) -> Void
  let differs: (LaunchSettings, LaunchSettings) -> Bool
  static func make<T: Equatable>(_ key: String, _ path: WritableKeyPath<LaunchSettings, T>, original: T) -> PresetRule {
    PresetRule(key: key, setOriginal: { $0[keyPath: path] = original },
               copy: { $0[keyPath: path] = $1[keyPath: path] },
               differs: { $0[keyPath: path] != $1[keyPath: path] })
  }
}

/// Internal resolution (FABLE2_RES_SCALE; game config [graphics] resolution_scale,
/// docs/resolution-scale.md). Always passed explicitly so the launcher, not the TOML, decides.
enum InternalResolution: String, CaseIterable, Identifiable {
  case auto, native = "1", double = "2", triple = "3"
  var id: String { rawValue }
  var title: String { title(frameRate: .auto) }
  /// Auto shows what the game will pick on this Mac at that frame-rate choice
  /// ("Auto (2× on this Mac)", "Auto (2× uncapped on this Mac)" when the 2x rule uncaps it).
  func title(frameRate: FrameRate, rule2x: Bool = true) -> String {
    switch self {
    case .auto:
      if frameRate == .auto || frameRate == .settings {
        let both = AutoFrameRate.bothOnThisMac(rule2x: rule2x)
        return "Auto (\(both.scale)×\(both.fps == 0 ? " uncapped" : "") on this Mac)"
      }
      return "Auto (\(AutoResolution.onThisMac(frameRate: frameRate))× on this Mac)"
    case .native: return "Native (1280×720)"
    case .double: return "2× (2560×1440)"
    case .triple: return "3× (3840×2160)"
    }
  }
  static func saved(in defaults: SettingsStore) -> InternalResolution {
    // Auto is the default when nothing was saved (2026-10-14); a saved choice stays.
    InternalResolution(rawValue: defaults.string(forKey: "resolutionScale") ?? "") ?? .auto
  }
  /// The scale the game will render at (Auto: as resolved for this Mac).
  func effectiveScale(frameRate: FrameRate, rule2x: Bool = true) -> Int {
    self == .auto ? AutoResolution.onThisMac(frameRate: frameRate, rule2x: rule2x) : Int(rawValue) ?? 1
  }
  /// CLI value: auto, native, 2 or 3 (1 is accepted for native).
  init?(argument: String) {
    if argument == "native" { self = .native } else { self.init(rawValue: argument) }
  }
}

/// Upscaling of the game image to the window (FABLE2_UPSCALING -> metal_presenter_use_metalfx;
/// game config [graphics] upscaling; docs/resolution-scale.md section 22). Off is the faithful
/// bilinear path. MetalFX Spatial is look-changing and only acts when the window is larger than
/// the internal resolution (e.g. 2× in 4K fullscreen).
enum Upscaling: String, CaseIterable, Identifiable {
  case off, metalfxSpatial = "metalfx-spatial"
  var id: String { rawValue }
  var title: String {
    switch self {
    case .off: return "Bilinear (original)"
    case .metalfxSpatial: return "MetalFX"
    }
  }
  /// Reads the saved choice; falls back to the pre-2026-10-12 "upscaler" key (bilinear|metalfx).
  static func saved(in defaults: SettingsStore) -> Upscaling {
    if let value = Upscaling(rawValue: defaults.string(forKey: "upscaling") ?? "") { return value }
    return defaults.string(forKey: "upscaler") == "metalfx" ? .metalfxSpatial : .off
  }
  /// CLI value: off or metalfx-spatial; the old --upscaler values bilinear/metalfx are accepted.
  init?(argument: String) {
    switch argument {
    case "bilinear": self = .off
    case "metalfx": self = .metalfxSpatial
    default: self.init(rawValue: argument)
    }
  }
}

/// Frame generation (FABLE2_FRAME_INTERP; src/core/fable2_frame_gen.h, docs/macos-metal.md "Frame generation"):
/// MetalFX frame interpolation shows a generated frame between every two real ones (30 rendered -> 60 shown). Needs
/// the 30 FPS cap (the game leaves it off at 60 FPS or uncapped), macOS 26 and a supported GPU. Changes how motion
/// looks and adds about 20 ms of delay, so it is off in Original and Enhanced (Custom only).
enum FrameGeneration: String, CaseIterable, Identifiable {
  case off, on
  var id: String { rawValue }
  var title: String {
    switch self {
    case .off: return "Off"
    case .on: return "30 → 60"
    }
  }
  static func saved(in defaults: SettingsStore) -> FrameGeneration {
    FrameGeneration(rawValue: defaults.string(forKey: "frameGeneration") ?? "") ?? .off
  }
  /// Why this Mac can't use it (nil: it can): macOS 26 and MTLFXFrameInterpolatorDescriptor.supportsDevice.
  static let unsupportedReason: String? = {
    guard #available(macOS 26.0, *) else { return "Needs macOS 26" }
    guard let device = MTLCreateSystemDefaultDevice(), MTLFXFrameInterpolatorDescriptor.supportsDevice(device) else {
      return "Not supported on this Mac"
    }
    return nil
  }()
  /// Why it would stay off with these settings (nil: it runs): unsupported Mac, or a frame rate other than 30 FPS
  /// (Auto: what it picks on this Mac; "Use game settings" is not known here).
  static func blockedReason(frameRate: FrameRate, resolution: InternalResolution, rule2x: Bool) -> String? {
    if let reason = unsupportedReason { return reason }
    switch frameRate {
    case .thirty, .settings: return nil
    case .auto: return AutoFrameRate.onThisMac(resolution: resolution, rule2x: rule2x) == 30 ? nil : "Needs 30 FPS"
    case .sixty, .uncapped: return "Needs 30 FPS"
    }
  }
}

/// Bloom/blur/glow effect size at a higher internal resolution (FABLE2_EFFECTS_NATIVE ->
/// draw_resolution_scaled_texture_offsets; game config [graphics] effects_native_scale).
/// Original keeps the effects' texel offsets at native 1x size (the original look); full
/// resolution shrinks them with the internal resolution (narrower glow, shifted right/down by the half-texel
/// centring; docs/bug-patterns.md #37). Dev-only since 2026-10-22 (Diagnostics > Graphics), not part of a preset.
enum EffectsSize: String, CaseIterable, Identifiable {
  case original, full
  var id: String { rawValue }
  var title: String {
    switch self {
    case .original: return "Native 1× (original look)"
    case .full: return "Full resolution"
    }
  }
  static func saved(in defaults: SettingsStore) -> EffectsSize {
    EffectsSize(rawValue: defaults.string(forKey: "effectsSize") ?? "") ?? .original
  }
}

/// Display mode (FABLE2_FULLSCREEN -> the SDK fullscreen cvar; game config [graphics]
/// fullscreen). Fullscreen is a macOS full-screen Space. Always passed explicitly, and the
/// variable beats a `fullscreen =` line in fable_2.toml, so the launcher decides.
enum DisplayMode: String, CaseIterable, Identifiable {
  case windowed, fullscreen
  var id: String { rawValue }
  var title: String {
    switch self {
    case .windowed: return "Windowed"
    case .fullscreen: return "Fullscreen"
    }
  }
  static func saved(in defaults: SettingsStore) -> DisplayMode {
    DisplayMode(rawValue: defaults.string(forKey: "displayMode") ?? "") ?? .windowed
  }
}

/// Flight recorder presets (docs/telemetry-design-20261008.md, "Implemented in phase 1"). The
/// recorder is on by default in the game; these only pick what is extra.
enum TracePreset: String, CaseIterable, Identifiable {
  case standard, off, loading, gpu, everything
  var id: String { rawValue }
  var title: String {
    switch self {
    case .standard: return "Flight recorder (default)"
    case .off: return "Off (FABLE2_TRACE=off)"
    case .loading: return "Loading trace (load/wait/io detail)"
    case .gpu: return "GPU timing trace"
    case .everything: return "Everything, streamed to disk (all:2)"
    }
  }
  /// --env items for tools/fable2_dev_session.py.
  var env: [String] {
    switch self {
    case .standard: return []
    case .off: return ["FABLE2_TRACE=off"]
    case .loading: return ["FABLE2_LOADING_TRACE=1"]
    case .gpu: return ["FABLE2_GPU_TIME_TRACE=1"]
    case .everything: return ["FABLE2_TRACE=all:2"]
    }
  }
  static func saved(in defaults: SettingsStore) -> TracePreset {
    TracePreset(rawValue: defaults.string(forKey: "tracePreset") ?? "") ?? .standard
  }
}

/// Which app to play (local agent docs): the frozen playtest copy in out/playtest (agents never
/// rebuild it) or the latest build in out/build (replaced by every agent build). Latest is the default: a
/// fresh checkout has no playtest copy (2026-10-15: the old Playtest default failed every launch there).
enum BuildChoice: String, CaseIterable, Identifiable {
  case playtest, latest
  var id: String { rawValue }
  /// Settings key of the one-time build-choice migration: below 1, a saved "playtest" (the old default,
  /// possibly never picked on purpose) becomes "latest" once; a later deliberate Playtest pick is kept.
  static let migrationKey = "buildChoiceMigration"
  static func saved(in defaults: SettingsStore) -> BuildChoice {
    let version = (defaults.object(forKey: migrationKey) as? Int) ?? 0
    if version < 1 {
      if defaults.string(forKey: "build") == BuildChoice.playtest.rawValue {
        defaults.set(BuildChoice.latest.rawValue, forKey: "build")
      }
      defaults.set(1, forKey: migrationKey)
    }
    return BuildChoice(rawValue: defaults.string(forKey: "build") ?? "") ?? .latest
  }
  /// The build a launch actually uses: Playtest without a playtest copy falls back to Latest (never fails).
  func effective(playtestExists: Bool) -> BuildChoice {
    self == .playtest && !playtestExists ? .latest : self
  }
}

/// "NAME=VALUE NAME2=VALUE2" from the Extra env field; entries that are not NAME=VALUE are skipped, and so is
/// FABLE2_TEST_BACKGROUND (test tools only: a saved value would start every Play game in the background).
func parseExtraEnv(_ text: String) -> [String] {
  text.split(whereSeparator: { $0 == " " || $0 == "\n" || $0 == "\t" }).map(String.init).filter { item in
    guard let eq = item.firstIndex(of: "="), eq != item.startIndex else { return false }
    return item[..<eq] != "FABLE2_TEST_BACKGROUND" && item[..<eq].allSatisfy { $0.isLetter || $0.isNumber || $0 == "_" }
  }
}

// MARK: - Settings

/// Where a setting is shown.
/// Everyday tabs: General, Display, Controls, Mods. Performance, Fixes and Diagnostics show only with Dev Options on
/// (SettingsModel.developerMode, 2026-10-19).
enum SettingsTab: String, CaseIterable, Identifiable {
  case general, display, controls, mods, performance, fixes, diagnostics
  var id: String { rawValue }
  var title: String { rawValue.capitalized }
  /// Shown only with Dev Options on.
  var devOnly: Bool { self == .performance || self == .fixes || self == .diagnostics }
  /// The tabs shown for a Dev Options state, in order.
  static func visible(developer: Bool) -> [SettingsTab] { allCases.filter { developer || !$0.devOnly } }
  var symbol: String {
    switch self {
    case .general: return "gearshape"
    case .display: return "display"
    case .controls: return "gamecontroller"
    case .mods: return "puzzlepiece.extension"
    case .performance: return "speedometer"
    case .fixes: return "wrench.and.screwdriver"
    case .diagnostics: return "stethoscope"
    }
  }
}

/// One on/off setting: its storage key, the `--<cli>` / `--no-<cli>` launch flags, where it
/// is shown, and a one-line plain-English explanation. The default is LaunchSettings()'s value.
struct Flag: Identifiable {
  let key: String
  let path: WritableKeyPath<LaunchSettings, Bool>
  let cli: String
  let tab: SettingsTab?  // nil: shown in the main window
  let section: String
  let title: String
  let detail: String
  var id: String { key }
  var defaultValue: Bool { LaunchSettings()[keyPath: path] }
}

/// Everything a launch uses except the save pick and its write-back slot. Defaults are the
/// launcher's defaults for a new install.
struct LaunchSettings: Equatable {
  // General
  /// Off by default (2026-10-15): the original logo movies play, the faithful choice. Settings are saved only when
  /// changed, so an install that never touched it follows this default; a saved choice is kept.
  var skipIntro = false
  var boot = true
  /// Off by default since 2026-10-22 (Enhanced keeps the original game's items; Custom can turn it on).
  var unlockExtras = false
  /// Mods (opt-in, off by default): the Character Editor in the F6 menu (docs/character-editor.md).
  var characterEditor = false
  var turboNotice = TurboNotice.overlay
  var turboSpeed = TurboSpeed.defaultValue
  var slowSpeed = SlowSpeed.defaultValue
  var turboAudio = TurboAudio.natural
  // Display
  var frameRate = FrameRate.auto
  var resolution = InternalResolution.auto
  var upscaling = Upscaling.off
  var frameGeneration = FrameGeneration.off
  var display = DisplayMode.windowed
  var effects = EffectsSize.original
  var fpsCounter = false
  var auto2x = true
  // Controls (tools/macos/Shared/ControlsView.swift)
  var controls = ControlSettings()
  // Performance
  var portableShaderCache = true
  var nativeShaders = true
  var nsSpecialize = true
  var nativeXlate = true
  var nativeXlateVS = true
  var nativeTess = true
  var nativePoints = true
  var nativeRects = true
  var terrainAdaptive = false
  var terrainLayerMatch = true  // docs/adaptive-terrain-flashes-20261014.md
  var occlusionReal = true  // docs/occlusion-queries-20261014.md
  var nativeTessBlended = true
  var nativeFirst = true
  var nativePointsMemexport = true
  var shaderAutoLearn = true
  var learnAppend = true
  var ns2Archive = true
  var xferPrewarm = true
  var nativeGeomPersist = true  // docs/bug-patterns.md row 102
  var shaderPrepBank = true  // docs/shader-precompile-20261015.md
  var rttPrewarm = true
  var startupParallel = true
  var preparedDrawSlim = true
  var texKeyMemo = true
  var drawPrepLite = true
  var drawPrepLite2 = true
  var loadCheckExact = true
  var nativeSaveCompress = true  // docs/load-fixes-20261016.md
  var nativeInflate = true  // docs/load-fixes-20261016.md part G
  // Fixes
  var furFix = true
  var memexportReadback = true
  var texspaceNative = true
  var rtShrink = true
  var rtt1x = true
  var dhrStorageFormat = true
  var shadowsNative = true
  var deferredClears = true
  var aliasClearNative = true
  var mainDepthNative = true
  var passMerge = true
  var passMergeTexDirect = true
  var passMergeClearDst = true
  var tileResolve = true
  var tileResolveExtent = true
  var tileResolveDrawnGate = true
  var tileResolveClear = true
  var tileResolvePrewarm = true
  var mipLodProof = true
  var tileResolveKeepOpen = false
  var texRename = true
  var texRenameNoop = true
  var texUnchanged = false
  var rttNarrowCpuFix = true
  var scaledResolvePageFill = true
  var tileEdram = true
  var effectsFootprint = true
  var effectsFootprintVariant = true
  var clearLoadAction = true
  var ssmMerge = false
  var ssmStencilFold = true
  var ringReinitFix = true
  var presentDrawables2 = true  // docs/macos-metal.md "Present queue"
  var vsyncPhaseLock = true  // docs/macos-metal.md "Vsync phase lock"
  var turboFineDelay = true  // docs/turbo.md "Precise sleeps"
  var fixLodSizeCache = false
  var fastPacing = false  // docs/load-fixes-20261016.md section F (pacing change, opt-in; replaces "Shorter load fades")
  // Diagnostics
  var tracePreset = TracePreset.standard
  var hitchTrace = true
  var rttVerify = false
  var tileResolveVerify = false
  var texRenameVerify = false
  var texUnchangedVerify = false
  var rtShrinkVerify = false
  var nsSpecializeVerify = false
  var texturePrecommitVerify = false
  /// Input recording (src/diagnostics/fable2_session_journal.h, docs/reference.md "Input recording"). Off by default.
  var recordInput = false
  var extraEnv = ""
  // Main window
  /// Original, Enhanced (default) or Custom (SettingsPreset; stored as "preset").
  var preset = SettingsPreset.enhanced
  var build = BuildChoice.latest
  /// Whether the game build these settings launch has the shader converter; set by the app from the build it
  /// launches (hasConverter(macOSFolder:)), never saved. False: the requiredWithoutConverter switches are not
  /// passed as =0.
  var converterBuiltIn = true

  static let flags: [Flag] = [
    Flag(key: "skipIntro", path: \.skipIntro, cli: "skip-intro", tab: .general, section: "Startup",
         title: "Skip intro movies",
         detail: "No logo movies; shorter title fade."),
    Flag(key: "boot", path: \.boot, cli: "boot", tab: nil, section: "",
         title: "Boot straight into save", detail: "Load the save from the main menu automatically."),
    Flag(key: "unlockExtras", path: \.unlockExtras, cli: "unlock-extras", tab: .general, section: "Game",
         title: "Unlock Collector's Edition and website gear",
         detail: "Bonus items from the Collector's Edition and the old website."),
    Flag(key: "characterEditor", path: \.characterEditor, cli: "character-editor", tab: .mods, section: "Mods",
         title: "Character Editor (F6 menu)",
         detail: "Edit morality, looks, gold and renown from the F6 menu. Backs up the save first."),
    Flag(key: "fpsCounter", path: \.fpsCounter, cli: "fps-counter", tab: .display, section: "Frame rate",
         title: "Show FPS counter", detail: "Its state at launch; F9 toggles it in game."),
    Flag(key: "auto2x", path: \.auto2x, cli: "auto-2x", tab: .display, section: "Frame rate",
         title: "Auto: 2× uncapped when 2× can't hold 60",
         detail: "Auto picks 2× uncapped on Macs that run 2× at 45 FPS or more."),
    Flag(key: "portableShaderCache", path: \.portableShaderCache, cli: "portable-shader-cache",
         tab: .performance, section: "Shader cache", title: "Portable shader cache",
         detail: "Survives macOS updates. Off: tied to this Mac's GPU and exact macOS version."),
    Flag(key: "nativeShaders", path: \.nativeShaders, cli: "native-shaders", tab: .performance,
         section: "Shader cache", title: "Native shaders",
         detail: "Optimized Metal builds of the game's shaders. Off: translated shaders only."),
    Flag(key: "nativeXlate", path: \.nativeXlate, cli: "native-xlate", tab: .performance,
         section: "Shader cache", title: "Native shader translator",
         detail: "Translates the game's pixel and vertex shaders straight to Metal. Off: the converter path."),
    Flag(key: "nativeXlateVS", path: \.nativeXlateVS, cli: "native-xlate-vs", tab: .performance,
         section: "Shader cache", title: "Native translator: vertex shaders too",
         detail: "With the native translator on, also its vertex shaders. Off: converter vertex shaders."),
    Flag(key: "nativeTess", path: \.nativeTess, cli: "native-tess", tab: .performance,
         section: "Shader cache", title: "Native tessellation",
         detail: "Terrain tessellation on Metal's tessellator with native domain shaders, where that is bit-identical to the emulation. Off: emulation."),
    Flag(key: "nativePoints", path: \.nativePoints, cli: "native-points", tab: .performance,
         section: "Shader cache", title: "Native point sprites",
         detail: "Point sprites (snow, particles) expanded by a native mesh shader. Off: geometry-shader emulation."),
    Flag(key: "nativeFirst", path: \.nativeFirst, cli: "native-first", tab: .performance,
         section: "Shader cache", title: "Native shaders from the first frame (no converter)",
         detail: "Guest shaders are compiled from the native translation when first needed; DXIL and Metal Shader Converter never run for them. Off: converter shaders first, native swapped in later."),
    Flag(key: "nativePointsMemexport", path: \.nativePointsMemexport, cli: "native-points-memexport", tab: .performance,
         section: "Shader cache", title: "Native point sprites: memory-exporting shaders too",
         detail: "Point lists whose vertex shader writes guest memory (the dog-fur exporter) also take the native mesh expansion (needs native shaders from the first frame). Off: geometry-shader emulation."),
    Flag(key: "nativeTessBlended", path: \.nativeTessBlended, cli: "native-tess-blended", tab: .performance,
         section: "Shader cache", title: "Native tessellation: blended terrain layers",
         detail: "The blended terrain layers on Metal's tessellator too (triangle order inside a draw may differ where it ties in depth). Off: emulation."),
    Flag(key: "occlusionReal", path: \.occlusionReal, cli: "occlusion-real", tab: .performance,
         section: "Render targets", title: "Real occlusion queries",
         detail: "The game's occlusion tests get the GPU's real answer, so hidden objects are skipped (as on the Xbox 360). Off: every test answers visible (older builds)."),
    Flag(key: "terrainAdaptive", path: \.terrainAdaptive, cli: "terrain-adaptive", tab: .performance,
         section: "Shader cache", title: "Adaptive terrain tessellation (fixes cracks natively)",
         detail: "The game's own terrain detail levels on the native tessellator, with correct edges (no cracks). Off: every terrain patch at full detail."),
    Flag(key: "terrainLayerMatch", path: \.terrainLayerMatch, cli: "terrain-layer-match", tab: .performance,
         section: "Shader cache", title: "Adaptive terrain: layers follow the ground's detail (no white flashes)",
         detail: "With adaptive terrain on, the blended ground layers use the same detail level and path as the ground under them, so they never fall behind it. Off: layers at full detail as on the Xbox 360 (white flashes at the view's edge)."),
    Flag(key: "nativeRects", path: \.nativeRects, cli: "native-rects", tab: .performance,
         section: "Shader cache", title: "Native rectangles",
         detail: "Rectangle lists (full-screen passes) expanded by the native vertex shaders. Off: geometry-shader emulation."),
    Flag(key: "nsSpecialize", path: \.nsSpecialize, cli: "ns-specialize", tab: .performance,
         section: "Hot-shader specialization", title: "Specialize hot shaders",
         detail: "Faster variants of the most-used shaders, built in the background."),
    Flag(key: "shaderAutoLearn", path: \.shaderAutoLearn, cli: "shader-autolearn", tab: .performance,
         section: "Hot-shader specialization", title: "Learn hot shaders while playing",
         detail: "Finds what to specialize during play; faster GPU after about a minute."),
    Flag(key: "learnAppend", path: \.learnAppend, cli: "learn-append", tab: .performance,
         section: "Hot-shader specialization", title: "Keep learned shader data across sessions",
         detail: "Append-only learn journal, so each session's learning is kept."),
    Flag(key: "ns2Archive", path: \.ns2Archive, cli: "ns2-archive", tab: .performance,
         section: "Hot-shader specialization", title: "Save specialized pipelines for the next launch",
         detail: "Written soon after they are built and at window close (NS2 archive)."),
    Flag(key: "xferPrewarm", path: \.xferPrewarm, cli: "xfer-prewarm", tab: .performance,
         section: "Startup and CPU", title: "Prebuild transfer pipelines at startup",
         detail: "Builds earlier sessions' render-target transfer pipelines in the background: no first-use hitches."),
    Flag(key: "nativeGeomPersist", path: \.nativeGeomPersist, cli: "native-geom-persist", tab: .performance,
         section: "Startup and CPU", title: "Remember particle and tessellation pipelines",
         detail: "Records native point, rectangle and tessellation pipelines so the next launch prebuilds them in the background."),
    Flag(key: "shaderPrepBank", path: \.shaderPrepBank, cli: "shader-prep-bank", tab: .performance,
         section: "Startup and CPU", title: "Prepare all of the disc's shaders at install",
         detail: "The install's shader step also translates and compiles every shader in the game's own shader bank, so new effects do not stutter the first time they appear. Off: only shaders this Mac has already seen."),
    Flag(key: "rttPrewarm", path: \.rttPrewarm, cli: "rtt-prewarm", tab: .performance,
         section: "Startup and CPU", title: "Prebuild fused resolve pipelines at startup",
         detail: "Builds the resolve-to-texture pipelines in the background: no first-launch load hitches."),
    Flag(key: "startupParallel", path: \.startupParallel, cli: "startup-parallel", tab: .performance,
         section: "Startup and CPU", title: "Parallel startup shader preparation",
         detail: "Builds startup pipelines and cached shaders on all cores: faster first launch at a new scale."),
    Flag(key: "preparedDrawSlim", path: \.preparedDrawSlim, cli: "prepared-draw-slim", tab: .performance,
         section: "Startup and CPU", title: "Lighter per-draw bookkeeping",
         detail: "Less CPU on the GPU command thread."),
    Flag(key: "texKeyMemo", path: \.texKeyMemo, cli: "tex-key-memo", tab: .performance,
         section: "Startup and CPU", title: "Remember texture lookups",
         detail: "Reuses each draw's texture key work instead of redoing it (less GPU-thread CPU, most at 2x/3x)."),
    Flag(key: "drawPrepLite", path: \.drawPrepLite, cli: "draw-prep-lite", tab: .performance,
         section: "Startup and CPU", title: "Skip unchanged per-draw work",
         detail: "Less GPU-thread CPU: skips per-draw copies and rechecks of unchanged state."),
    Flag(key: "drawPrepLite2", path: \.drawPrepLite2, cli: "draw-prep-lite2", tab: .performance,
         section: "Startup and CPU", title: "Skip unchanged per-draw work (round 2)",
         detail: "Less GPU-thread CPU: reuses texture and sampler lookups and packs constants with fewer copies."),
    Flag(key: "loadCheckExact", path: \.loadCheckExact, cli: "load-check-exact", tab: .performance,
         section: "Startup and CPU", title: "Texture loads follow the draw's check",
         detail: "A texture the game rewrites while a draw is being prepared loads at its next use, inside the upload batch."),
    Flag(key: "nativeSaveCompress", path: \.nativeSaveCompress, cli: "native-save-compress", tab: .performance,
         section: "Startup and CPU", title: "Native save compressor",
         detail: "Saves (autosaves at area changes, quick saves) compress with a native version of the game's own search: the same bytes, written sooner. Off: the recompiled search."),
    Flag(key: "nativeInflate", path: \.nativeInflate, cli: "native-inflate", tab: .performance,
         section: "Startup and CPU", title: "Native decompression",
         detail: "Loads decompress the game's data with a native version of its own zlib inflate loop: the same bytes, sooner (about 0.1 s per area change). Off: the recompiled loop."),
    Flag(key: "furFix", path: \.furFix, cli: "fur-fix", tab: .fixes, section: "Dog",
         title: "Dog-fur flash fix",
         detail: "Keeps GPU-written vertex data when a page is re-uploaded (memexport preserve)."),
    Flag(key: "memexportReadback", path: \.memexportReadback, cli: "memexport-readback", tab: .fixes,
         section: "Dog", title: "GPU → game memory sync",
         detail: "Copies GPU-written data back into game memory once the GPU finishes (memexport readback)."),
    Flag(key: "texspaceNative", path: \.texspaceNative, cli: "texspace-native", tab: .fixes, section: "Dog",
         title: "Dog chest seam fix at 2×/3×",
         detail: "Character texture-space passes (dog lighting, morphs) run at native 1×."),
    Flag(key: "rtShrink", path: \.rtShrink, cli: "rt-shrink", tab: .fixes, section: "Render targets",
         title: "Smaller render targets",
         detail: "Sized to the rows the game uses: about 2 GB less memory at 2×, 3.8 GB at 3×."),
    Flag(key: "rtt1x", path: \.rtt1x, cli: "rtt-1x", tab: .fixes, section: "Render targets",
         title: "Resolve straight into textures (RTT)",
         detail: "At native 1×: fewer texture reloads, less CPU per frame."),
    Flag(key: "dhrStorageFormat", path: \.dhrStorageFormat, cli: "dhr-storage-format", tab: .fixes,
         section: "Render targets", title: "Direct resolves for precision-variant formats",
         detail: "The main HDR target's resolves skip the EDRAM dump and write their texture directly: one large reload less per frame indoors."),
    Flag(key: "shadowsNative", path: \.shadowsNative, cli: "shadows-native", tab: .fixes,
         section: "Render targets", title: "Shadow maps at console resolution",
         detail: "At 2×/3× shadows keep their original size (faithful, less GPU). Off: scaled shadow maps."),
    Flag(key: "deferredClears", path: \.deferredClears, cli: "deferred-clears", tab: .fixes,
         section: "Render targets", title: "Deferred clears",
         detail: "Resolve clears are drawn in the next pass instead of a separate pass (less memory traffic)."),
    Flag(key: "aliasClearNative", path: \.aliasClearNative, cli: "alias-clear-native", tab: .fixes,
         section: "Render targets", title: "Native alias clears",
         detail: "The game's 4×MSAA clear quads become plain clears of the real target: fewer passes and copies. Same pixels."),
    Flag(key: "mainDepthNative", path: \.mainDepthNative, cli: "main-depth-native", tab: .fixes,
         section: "Render targets", title: "Native main-depth clears",
         detail: "The shadow-mask and main-depth clears through a 4×MSAA alias (a depth value read as colour, stencil-only clears) are done on the real targets: no 4× copies. Same pixels. Needs native alias clears."),
    Flag(key: "passMerge", path: \.passMerge, cli: "pass-merge", tab: .fixes, section: "Render targets",
         title: "Fewer pass breaks",
         detail: "Keeps the main pass open across more texture uploads and draws clears inside the next pass. Same pixels. Off turns both items below off."),
    Flag(key: "passMergeTexDirect", path: \.passMergeTexDirect, cli: "pass-merge-tex-direct", tab: .fixes,
         section: "Render targets", title: "Texture uploads without a pass break",
         detail: "A texture whose guest memory still needs uploading stays on the pre-commit route when the upload needs no copy in the open frame."),
    Flag(key: "passMergeClearDst", path: \.passMergeClearDst, cli: "pass-merge-clear-dst", tab: .fixes,
         section: "Render targets", title: "Clears before copies in one pass",
         detail: "A target's resolve clear is drawn first in the pass that also copies data into it, instead of its own pass."),
    Flag(key: "effectsFootprint", path: \.effectsFootprint, cli: "effects-footprint", tab: .fixes,
         section: "Render targets", title: "Smooth bloom at 2x/3x (no grating)",
         detail: "With original-size effects, each blur and bloom tap covers its whole console texel instead of one of its 2x2 (3x3) pixels, so thin bright things glow smoothly instead of through stripes. Costs GPU time on effect passes."),
    Flag(key: "effectsFootprintVariant", path: \.effectsFootprintVariant, cli: "effects-footprint-variant",
         tab: .fixes, section: "Render targets", title: "Smooth bloom: only in the shaders that need it",
         detail: "The smooth-bloom code is compiled only into the shader variants of draws it can change (a scaled colour texture read with texel offsets). Other draws, most of the scene, run without it. Same pixels; about 0.5 ms less GPU per frame at 2x."),
    Flag(key: "clearLoadAction", path: \.clearLoadAction, cli: "clear-load-action", tab: .fixes,
         section: "Render targets", title: "Whole-target clears as pass clears",
         detail: "A deferred clear that covers a pass's whole target becomes that pass's clear instead of a load plus a clear quad. Same pixels, less memory traffic."),
    Flag(key: "ssmMerge", path: \.ssmMerge, cli: "ssm-merge", tab: .fixes,
         section: "Render targets", title: "Shadow-mask depth copy in the mask pass",
         detail: "The main-depth copy and stencil clear before the screen-space shadow mask are drawn inside the mask pass instead of a pass of their own, and the mask pass skips loading a depth buffer it overwrites. Same pixels, less memory traffic. Off by default: about 0.1 ms slower on an M5 Max at 2x; meant for Macs short on memory bandwidth."),
    Flag(key: "ssmStencilFold", path: \.ssmStencilFold, cli: "ssm-stencil-fold", tab: .fixes,
         section: "Render targets", title: "Shadow-mask stencil reset without its own pass",
         detail: "The stencil reset after the screen-space shadow mask is not drawn as a pass: the copies that move the mask's depth buffer into the main pass read the reset value directly, and nothing reads that buffer's stencil afterwards. Same pixels, less memory traffic."),
    Flag(key: "tileResolve", path: \.tileResolve, cli: "tile-resolve", tab: .fixes,
         section: "Render targets", title: "Resolves inside the draw pass",
         detail: "Colour copy-outs run as tile shaders before the pass ends: no separate compute pass. Same bytes. Off turns the four items below off."),
    Flag(key: "mipLodProof", path: \.mipLodProof, cli: "miplod-proof", tab: .fixes,
         section: "Render targets", title: "Mip-chain resolves inside the draw pass",
         detail: "Proves from the draw's vertices which mip levels a downsample pass can read, limits its texture view to them, and lets its resolve into the next level run as a tile shader. Same pixels."),
    Flag(key: "tileResolveExtent", path: \.tileResolveExtent, cli: "tile-resolve-extent", tab: .fixes,
         section: "Render targets", title: "Tile resolves: pass only as tall as drawn",
         detail: "Draw passes open only as tall as their draws reach, so a tile resolve skips untouched rows. Same pixels."),
    Flag(key: "tileResolveDrawnGate", path: \.tileResolveDrawnGate, cli: "tile-resolve-drawn-gate", tab: .fixes,
         section: "Render targets", title: "Tile resolves: drawn-area gate",
         detail: "Chooses tile or compute by the untouched part of the pass. Off: the older render-area ratio."),
    Flag(key: "tileResolveClear", path: \.tileResolveClear, cli: "tile-resolve-clear", tab: .fixes,
         section: "Render targets", title: "Tile resolves: copy and clear in one pass",
         detail: "The resolve's clear is drawn in the same pass after the copy, instead of a pass of its own. Same pixels."),
    Flag(key: "tileResolvePrewarm", path: \.tileResolvePrewarm, cli: "tile-resolve-prewarm", tab: .fixes,
         section: "Render targets", title: "Tile resolves: build pipelines at startup",
         detail: "Builds the tile-resolve pipelines in the background at launch so the first frames use them."),
    Flag(key: "tileResolveKeepOpen", path: \.tileResolveKeepOpen, cli: "tile-resolve-keep-open", tab: .fixes,
         section: "Render targets", title: "Tile resolves: keep the pass open (experimental)",
         detail: "After a tile resolve the pass stays open for the next draws when they use the same targets."),
    Flag(key: "texRename", path: \.texRename, cli: "tex-rename", tab: .fixes,
         section: "Render targets", title: "Texture renaming",
         detail: "A texture reloaded after the frame already sampled it gets fresh memory instead of ending the render pass. Same pixels. Off turns the two items below off."),
    Flag(key: "texRenameNoop", path: \.texRenameNoop, cli: "tex-rename-noop", tab: .fixes,
         section: "Render targets", title: "Texture renaming: skip no-op loads",
         detail: "Planned reloads that will not write anything (already up to date, or kept) no longer end the render pass."),
    Flag(key: "texUnchanged", path: \.texUnchanged, cli: "tex-unchanged", tab: .fixes,
         section: "Render targets", title: "Texture renaming: skip unchanged reloads (experimental)",
         detail: "At 2×/3×, a reload is skipped when nothing wrote the texture's exact memory since its last load. Same bytes; no gain measured."),
    Flag(key: "rttNarrowCpuFix", path: \.rttNarrowCpuFix, cli: "rtt-narrow-cpu-fix", tab: .fixes,
         section: "Render targets", title: "Resolve tracking: CPU-write race fix",
         detail: "A CPU write that lands while a resolve is being recorded always invalidates textures. Off: the older behaviour."),
    Flag(key: "scaledResolvePageFill", path: \.scaledResolvePageFill, cli: "scaled-resolve-page-fill", tab: .fixes,
         section: "Render targets", title: "2×/3× resolves: keep neighbouring data",
         detail: "At 2×/3× a resolve first uploads the game data around it in the same memory page, as at 1×. Fixes the Bower Lake water losing its ripples in straight-edged patches. Off: the older behaviour."),
    Flag(key: "tileEdram", path: \.tileEdram, cli: "tile-edram", tab: .fixes, section: "Render targets",
         title: "Tile EDRAM transfers",
         detail: "Depth copies inside the draw pass. Off turns every tile-EDRAM change off."),
    Flag(key: "ringReinitFix", path: \.ringReinitFix, cli: "ring-reinit-fix", tab: .fixes, section: "Boot",
         title: "Ring re-init fix",
         detail: "Boot-hang workaround; the SDK's own ring sync also fixes it, so off alone changes nothing."),
    Flag(key: "presentDrawables2", path: \.presentDrawables2, cli: "present-drawables-2", tab: .fixes,
         section: "Display", title: "Lower display latency",
         detail: "Queues at most 2 frames for the display instead of 3: about 15 ms less input lag, same smoothness."),
    Flag(key: "vsyncPhaseLock", path: \.vsyncPhaseLock, cli: "vsync-phase-lock", tab: .fixes, section: "Display",
         title: "Sync frames to the display",
         detail: "Times each frame to the display refresh: steady pacing every launch and less input lag."),
    Flag(key: "turboFineDelay", path: \.turboFineDelay, cli: "turbo-fine-delay", tab: .fixes, section: "Turbo",
         title: "Turbo: precise sleeps",
         detail: "Short sleeps keep their length under turbo. Off: they spin and use several CPU cores."),
    Flag(key: "fixLodSizeCache", path: \.fixLodSizeCache, cli: "fix-lod-size-cache", tab: .fixes,
         section: "Game bug fixes (changes original behaviour)",
         title: "Fix game texture-detail cache bug (pale textures)",
         detail: "The game sometimes keeps a far-away, low-detail texture on nearby objects until the camera moves (the pale root at Bower Lake, about 1 load in 4). On: the game's size check uses the right distance. Off: original game."),
    Flag(key: "fastPacing", path: \.fastPacing, cli: "fast-pacing", tab: .general, section: "Game",
         title: "Skip cosmetic waits (changes original pacing)",
         detail: "Shortens fixed screen pauses, never loading."),
    Flag(key: "hitchTrace", path: \.hitchTrace, cli: "hitch-trace", tab: .diagnostics, section: "Logging",
         title: "Hitch trace",
         detail: "Logs a [hitch] line in fable_2.log for each stutter: a frame 1.5× the recent median and 40 ms+, or any 100 ms+."),
    Flag(key: "rttVerify", path: \.rttVerify, cli: "rtt-verify", tab: .diagnostics, section: "Verify modes",
         title: "Verify resolve-to-texture",
         detail: "Reference untile at every skipped texture load, byte compare. Slow."),
    Flag(key: "tileResolveVerify", path: \.tileResolveVerify, cli: "tile-resolve-verify",
         tab: .diagnostics, section: "Verify modes", title: "Verify tile resolves",
         detail: "Runs the compute resolve after each tile resolve and compares the bytes. Slow."),
    Flag(key: "texRenameVerify", path: \.texRenameVerify, cli: "tex-rename-verify",
         tab: .diagnostics, section: "Verify modes", title: "Verify texture renaming",
         detail: "Re-runs each renamed texture load at its original position and compares the bytes. Ends render passes; slow."),
    Flag(key: "texUnchangedVerify", path: \.texUnchangedVerify, cli: "tex-unchanged-verify",
         tab: .diagnostics, section: "Verify modes", title: "Verify skipped unchanged reloads",
         detail: "Runs each skipped reload as a reference load and compares it with the kept texture. Slow."),
    Flag(key: "rtShrinkVerify", path: \.rtShrinkVerify, cli: "rt-shrink-verify", tab: .diagnostics,
         section: "Verify modes", title: "Verify smaller render targets",
         detail: "Checks every addressed row before each transfer. Slow."),
    Flag(key: "nsSpecializeVerify", path: \.nsSpecializeVerify, cli: "ns-specialize-verify",
         tab: .diagnostics, section: "Verify modes", title: "Verify specialized shaders",
         detail: "Re-checks every specialized-shader hit against the game's registers."),
    Flag(key: "texturePrecommitVerify", path: \.texturePrecommitVerify, cli: "texture-precommit-verify",
         tab: .diagnostics, section: "Verify modes", title: "Verify texture pre-commit",
         detail: "Compares pre-committed texture sources with the shared memory they came from. Slow."),
    Flag(key: "recordInput", path: \.recordInput, cli: "record-input", tab: .diagnostics, section: "Input recording",
         title: "Record input",
         detail: "Saves controller and keyboard input to logs/recordings, one file per game, with a copy of the save it starts from. Play recording plays it back."),
  ]

  /// Default-on switches that are passed only when turned off (NAME=0), so a default launch has the
  /// same command line as before they got a toggle.
  static let killSwitches: [(WritableKeyPath<LaunchSettings, Bool>, String)] = [
    // docs/resolution-scale.md §25 (the 2x rule)
    (\.auto2x, "FABLE2_AUTO_2X"),
    (\.nativeShaders, "FABLE2_NATIVE_SHADERS"), (\.nsSpecialize, "FABLE2_NS_SPECIALIZE"),
    (\.shadowsNative, "FABLE2_SHADOWS_NATIVE"), (\.deferredClears, "FABLE2_DEFERRED_CLEARS"),
    (\.aliasClearNative, "FABLE2_ALIAS_CLEAR_NATIVE"), (\.mainDepthNative, "FABLE2_MAIN_DEPTH_NATIVE"),
    (\.passMerge, "FABLE2_PASS_MERGE"), (\.passMergeTexDirect, "FABLE2_PASS_MERGE_TEX_DIRECT"),
    (\.passMergeClearDst, "FABLE2_PASS_MERGE_CLEAR_DST"),
    (\.dhrStorageFormat, "FABLE2_DHR_STORAGE_FORMAT"),  // docs/hle-census-20261013.md section 11
    (\.tileEdram, "FABLE2_TILE_EDRAM"), (\.ringReinitFix, "FABLE2_RING_REINIT_FIX"),
    (\.turboFineDelay, "FABLE2_TURBO_FINE_DELAY"),  // docs/turbo.md "Precise sleeps"
    (\.presentDrawables2, "FABLE2_PRESENT_DRAWABLES"),  // docs/macos-metal.md "Present queue" (=0: 3 drawables)
    (\.vsyncPhaseLock, "FABLE2_VSYNC_PHASE_LOCK"),  // docs/macos-metal.md "Vsync phase lock"
    (\.hitchTrace, "FABLE2_HITCH_TRACE"), (\.nativeXlate, "FABLE2_NATIVE_XLATE"),
    (\.nativeXlateVS, "FABLE2_NATIVE_XLATE_VS"), (\.nativeTess, "FABLE2_NATIVE_TESS"),
    (\.nativeRects, "FABLE2_NATIVE_RECTS"), (\.nativeTessBlended, "FABLE2_NATIVE_TESS_BLENDED"), (\.nativePoints, "FABLE2_NATIVE_POINTS"),
    (\.nativeFirst, "FABLE2_NATIVE_FIRST"), (\.nativePointsMemexport, "FABLE2_NATIVE_POINTS_MEMEXPORT"),
    // docs/tile-resolve-20261013.md (default on since 2026-10-13; the four below only matter with it on).
    (\.tileResolve, "FABLE2_TILE_RESOLVE"),
    (\.tileResolveExtent, "FABLE2_TILE_RESOLVE_EXTENT"),
    (\.tileResolveDrawnGate, "FABLE2_TILE_RESOLVE_DRAWN_GATE"),
    (\.tileResolveClear, "FABLE2_TILE_RESOLVE_CLEAR"),
    (\.tileResolvePrewarm, "FABLE2_TILE_RESOLVE_PREWARM"),
    // docs/tex-rename-20261013.md
    (\.texRename, "FABLE2_TEX_RENAME"), (\.texRenameNoop, "FABLE2_TEX_RENAME_NOOP"),
    (\.texUnchanged, "FABLE2_TEX_UNCHANGED"), (\.rttNarrowCpuFix, "FABLE2_RTT_NARROW_CPU_FIX"),
    // docs/lake-water-20261014.md
    (\.scaledResolvePageFill, "FABLE2_SCALED_RESOLVE_PAGE_FILL"),
    // docs/mip-lod-20261013.md
    (\.mipLodProof, "FABLE2_MIPLOD_PROOF"),
    // docs/bloom-banding-20261014.md
    (\.effectsFootprint, "FABLE2_EFFECTS_FOOTPRINT"),
    // docs/ssmask-20261014.md
    (\.effectsFootprintVariant, "FABLE2_EFFECTS_FOOTPRINT_VARIANT"),
    (\.clearLoadAction, "FABLE2_CLEAR_LOAD_ACTION"),
    // docs/ssm-merge-20261015.md
    (\.ssmStencilFold, "FABLE2_SSM_STENCIL_FOLD"),
    // docs/occlusion-queries-20261014.md
    (\.occlusionReal, "FABLE2_OCCLUSION_REAL"),
    // docs/adaptive-terrain-flashes-20261014.md (only matters with terrainAdaptive on)
    (\.terrainLayerMatch, "FABLE2_TERRAIN_LAYER_MATCH"),
  ]
  /// Native-path switches a build without the shader converter needs (docs/no-converter.md, "Forced switches"):
  /// there is no converter path to fall back to, so the game ignores NAME=0 there and the settings show them on
  /// and disabled ("Required in this build"). Builds with the converter keep them editable.
  static let requiredWithoutConverter: Set<String> = [
    "nativeShaders", "nativeXlate", "nativeXlateVS", "nativeFirst", "nativeTess", "nativeTessBlended",
    "nativePoints", "nativePointsMemexport", "nativeRects",
  ]

  /// Whether the game app whose Contents/MacOS folder is `macOS` has the shader converter: converter builds
  /// bundle libmetalirconverter.dylib there, no-converter (release, portable) builds never do (the packagers
  /// refuse one). A missing folder counts as a converter build (nothing to restrict).
  static func hasConverter(macOSFolder macOS: String) -> Bool {
    let fm = FileManager.default
    var isDir: ObjCBool = false
    guard fm.fileExists(atPath: macOS + "/fable_2", isDirectory: &isDir) else { return true }
    return fm.fileExists(atPath: macOS + "/libmetalirconverter.dylib")
  }

  /// Default-off diagnostics that are passed only when turned on (NAME=1).
  static let verifyModes: [(WritableKeyPath<LaunchSettings, Bool>, String)] = [
    (\.rttVerify, "FABLE2_RTT_VERIFY"), (\.rtShrinkVerify, "FABLE2_RT_SHRINK_VERIFY"),
    (\.nsSpecializeVerify, "FABLE2_NS_SPECIALIZE_VERIFY"),
    (\.texturePrecommitVerify, "FABLE2_TEXTURE_PRECOMMIT_VERIFY"),
    (\.terrainAdaptive, "FABLE2_TERRAIN_ADAPTIVE"),
    // docs/tile-resolve-20261013.md (keep-open: default off, no gain measured).
    (\.tileResolveVerify, "FABLE2_TILE_RESOLVE_VERIFY"),
    (\.tileResolveKeepOpen, "FABLE2_TILE_RESOLVE_KEEP_OPEN"),
    (\.texRenameVerify, "FABLE2_TEX_RENAME_VERIFY"),
    (\.texUnchangedVerify, "FABLE2_TEX_UNCHANGED_VERIFY"),
    // docs/ssm-merge-20261015.md (default off: +0.1 ms GPU at 2x on the M5 Max; for bandwidth-bound Macs).
    (\.ssmMerge, "FABLE2_SSM_MERGE"),
    // Input recording (src/diagnostics/fable2_session_journal.h; the game picks the file name).
    (\.recordInput, "FABLE2_RECORD_INPUT"),
  ]

  static func flags(in tab: SettingsTab) -> [Flag] { flags.filter { $0.tab == tab } }

  /// The stored settings with the stored preset applied (Original and Enhanced: their fixed values; Custom: the saved
  /// Custom values). Before presets (2026-10-22, no "preset" stored): Enhanced when the settings match it, else
  /// Custom, saved once as the Custom slot.
  static func load(from defaults: SettingsStore) -> LaunchSettings {
    migrateEffectsSize(in: defaults)
    var s = loadStored(from: defaults)
    if let preset = SettingsPreset(rawValue: defaults.string(forKey: "preset") ?? "") {
      s.applyPreset(preset, custom: savedCustom(in: defaults))
    } else if s.matches(.enhanced) {
      s.preset = .enhanced
    } else {
      s.preset = .custom
      s.storeCustom(to: defaults)
      defaults.set(SettingsPreset.custom.rawValue, forKey: "preset")
    }
    return s
  }

  /// Stored as "effectsDefaultsVersion". 1 (2026-10-22): effects at native 1x became the dev-only default, so a
  /// stored "Full resolution" (which shrinks and shifts glows) is reset to native once; later choices are kept.
  static let effectsDefaultsVersion = 1
  static func migrateEffectsSize(in defaults: SettingsStore) {
    guard (defaults.object(forKey: "effectsDefaultsVersion") as? Int ?? 0) < effectsDefaultsVersion else { return }
    if defaults.object(forKey: "effectsSize") != nil { defaults.set(nil, forKey: "effectsSize") }
    defaults.set(effectsDefaultsVersion, forKey: "effectsDefaultsVersion")
  }

  /// The per-setting keys only (no preset handling).
  static func loadStored(from defaults: SettingsStore) -> LaunchSettings {
    var s = LaunchSettings()
    for flag in flags {
      if let value = defaults.object(forKey: flag.key) as? Bool { s[keyPath: flag.path] = value }
    }
    // "Shorter load fades" (2026-10-16) became part of "Skip cosmetic waits": a saved "on" carries over.
    if defaults.object(forKey: "fastPacing") == nil, defaults.object(forKey: "shortLoadFades") as? Bool == true {
      s.fastPacing = true
    }
    s.turboNotice = TurboNotice.saved(in: defaults)
    s.turboSpeed = TurboSpeed.saved(in: defaults)
    s.slowSpeed = SlowSpeed.saved(in: defaults)
    s.turboAudio = TurboAudio.saved(in: defaults)
    s.frameRate = FrameRate.saved(in: defaults)
    s.resolution = InternalResolution.saved(in: defaults)
    s.upscaling = Upscaling.saved(in: defaults)
    s.frameGeneration = FrameGeneration.saved(in: defaults)
    s.display = DisplayMode.saved(in: defaults)
    s.effects = EffectsSize.saved(in: defaults)
    s.tracePreset = TracePreset.saved(in: defaults)
    s.extraEnv = defaults.string(forKey: "extraEnv") ?? ""
    s.controls = ControlSettings.load(from: defaults)
    s.build = BuildChoice.saved(in: defaults)
    s.preset = SettingsPreset(rawValue: defaults.string(forKey: "preset") ?? "") ?? .enhanced
    return s
  }

  /// Writes the keys that changed since `old` (unchanged keys stay unset, as before the split).
  func save(to defaults: SettingsStore, old: LaunchSettings) {
    for flag in Self.flags where self[keyPath: flag.path] != old[keyPath: flag.path] {
      defaults.set(self[keyPath: flag.path], forKey: flag.key)
    }
    func put<T: Equatable>(_ new: T, _ previous: T, _ key: String, _ value: Any) {
      if new != previous { defaults.set(value, forKey: key) }
    }
    put(turboNotice, old.turboNotice, "turboNotice", turboNotice.rawValue)
    put(turboSpeed, old.turboSpeed, "turboSpeed", turboSpeed)
    put(slowSpeed, old.slowSpeed, "slowSpeed", slowSpeed)
    put(turboAudio, old.turboAudio, "turboAudio", turboAudio.rawValue)
    put(frameRate, old.frameRate, "frameRate", frameRate.rawValue)
    put(resolution, old.resolution, "resolutionScale", resolution.rawValue)
    put(upscaling, old.upscaling, "upscaling", upscaling.rawValue)
    put(frameGeneration, old.frameGeneration, "frameGeneration", frameGeneration.rawValue)
    put(display, old.display, "displayMode", display.rawValue)
    put(effects, old.effects, "effectsSize", effects.rawValue)
    put(tracePreset, old.tracePreset, "tracePreset", tracePreset.rawValue)
    put(extraEnv, old.extraEnv, "extraEnv", extraEnv)
    put(build, old.build, "build", build.rawValue)
    put(preset, old.preset, "preset", preset.rawValue)
    controls.save(to: defaults, old: old.controls)
  }

  /// `--<cli>` / `--no-<cli>` for an on/off setting; false when `arg` is not one.
  mutating func applyFlag(_ arg: String) -> Bool {
    for flag in Self.flags {
      if arg == "--" + flag.cli { self[keyPath: flag.path] = true; return true }
      if arg == "--no-" + flag.cli { self[keyPath: flag.path] = false; return true }
    }
    return false
  }
}

// MARK: - Presets

extension LaunchSettings {
  /// Controller turbo and slow-motion buttons (part of the turbo options a preset decides).
  var padTurbo: PadButton {
    get { controls.padButton(.turbo) }
    set { if newValue != controls.padButton(.turbo) { controls.setPad(newValue, for: .turbo) } }
  }
  var padSlow: PadButton {
    get { controls.padButton(.slow) }
    set { if newValue != controls.padButton(.slow) { controls.setPad(newValue, for: .slow) } }
  }

  /// The settings a preset decides, with their Original values. Enhanced is LaunchSettings()'s value (the default
  /// install). Everything else (invisible fixes, performance, window, keys, controls mapping, diagnostics) is not
  /// part of a preset. docs/reference.md "Presets" lists them (tests/launcher_presets_tests.py checks it).
  static let presetRules: [PresetRule] = [
    .make("resolutionScale", \.resolution, original: .native),
    .make("frameRate", \.frameRate, original: .thirty),
    .make("auto2x", \.auto2x, original: true),
    .make("upscaling", \.upscaling, original: .off),
    .make("frameGeneration", \.frameGeneration, original: .off),
    .make("skipIntro", \.skipIntro, original: false),
    .make("fastPacing", \.fastPacing, original: false),
    .make("unlockExtras", \.unlockExtras, original: false),
    .make("characterEditor", \.characterEditor, original: false),
    .make("fixLodSizeCache", \.fixLodSizeCache, original: false),
    .make("turboSpeed", \.turboSpeed, original: TurboSpeed.off),
    .make("slowSpeed", \.slowSpeed, original: SlowSpeed.off),
    .make("turboAudio", \.turboAudio, original: .natural),
    .make("turboNotice", \.turboNotice, original: .overlay),
    .make("padTurbo", \.padTurbo, original: PadButton.none),
    .make("padSlow", \.padSlow, original: PadButton.none),
    .make("stickDeadzoneLeft", \.controls.stickDeadzoneLeft, original: nil),
    .make("stickDeadzoneRight", \.controls.stickDeadzoneRight, original: nil),
    .make("walkDeadzone", \.controls.walkDeadzone, original: nil),
  ]

  /// The preset's values (Custom: the defaults, never applied).
  static func presetValues(_ preset: SettingsPreset) -> LaunchSettings {
    var s = LaunchSettings()
    if preset == .original { for rule in presetRules { rule.setOriginal(&s) } }
    s.preset = preset
    return s
  }

  /// Whether every preset setting has the preset's value (Custom always matches).
  func matches(_ preset: SettingsPreset) -> Bool {
    guard preset != .custom else { return true }
    let target = Self.presetValues(preset)
    return !Self.presetRules.contains { $0.differs(self, target) }
  }

  /// Selects a preset and applies its settings (nothing else changes): Original and Enhanced their fixed values,
  /// Custom the saved Custom values (none saved: the current values stay).
  mutating func applyPreset(_ preset: SettingsPreset, custom: LaunchSettings?) {
    if let target = preset == .custom ? custom : Self.presetValues(preset) {
      for rule in Self.presetRules { rule.copy(&self, target) }
    }
    self.preset = preset
  }

  /// Settings key of the saved Custom preset: a dictionary of the preset settings' keys (those that differ from
  /// Enhanced), written whenever a preset setting changes (SettingsModel.settings).
  static let customKey = "customPreset"

  /// Settings key of the saved named presets: name -> dictionary as the Custom preset (Save Preset…).
  static let namedKey = "savedPresets"
  /// Names the built-in presets keep (case-insensitive): a saved preset can't take them.
  static func isReservedPresetName(_ name: String) -> Bool {
    SettingsPreset.allCases.contains { $0.title.lowercased() == name.lowercased() }
  }

  /// The preset settings as a dictionary of setting keys (only those that differ from Enhanced).
  var presetDictionary: [String: Any] {
    var custom = LaunchSettings()
    for rule in Self.presetRules { rule.copy(&custom, self) }
    let memory = MemorySettingsStore()
    custom.save(to: memory, old: LaunchSettings())
    memory.set(nil, forKey: "padDefaultsVersion")
    return memory.values
  }
  /// Settings read from a preset dictionary (only the preset settings are meaningful).
  static func fromPresetDictionary(_ values: [String: Any]) -> LaunchSettings {
    var dict = values
    dict["padDefaultsVersion"] = PadAction.defaultsVersion  // never migrated as old defaults
    return loadStored(from: MemorySettingsStore(dict))
  }

  /// The saved Custom values (only the preset settings are used); nil when none were saved.
  static func savedCustom(in store: SettingsStore) -> LaunchSettings? {
    (store.object(forKey: customKey) as? [String: Any]).map(fromPresetDictionary)
  }

  /// Saves this settings' preset settings as the Custom preset.
  func storeCustom(to store: SettingsStore) { store.set(presetDictionary, forKey: Self.customKey) }

  /// The saved named presets.
  static func namedPresets(in store: SettingsStore) -> [String: [String: Any]] {
    store.object(forKey: namedKey) as? [String: [String: Any]] ?? [:]
  }

  /// Whether a preset setting differs between the two.
  func presetSettingsDiffer(from other: LaunchSettings) -> Bool {
    Self.presetRules.contains { $0.differs(self, other) }
  }

  /// --preset original|enhanced|custom|NAME (a saved preset) for one CLI launch, snapshot, --print-env or --play
  /// (never stored); false when NAME is unknown.
  mutating func applyCLIPreset(_ value: String, store: SettingsStore) -> Bool {
    if let preset = SettingsPreset(rawValue: value) {
      applyPreset(preset, custom: Self.savedCustom(in: store))
      return true
    }
    guard let values = Self.namedPresets(in: store)[value] else { return false }
    applyPreset(.custom, custom: Self.fromPresetDictionary(values))
    return true
  }
}

// MARK: - Game environment (the one settings -> game mapping)

extension LaunchSettings {
  /// NAME=VALUE items every launch passes, in order (the Dev Launcher's --env items after the frame-rate
  /// and skip-intro arguments). Free-form extra environment comes last, so it wins.
  var settingsEnvironment: [String] {
    var env: [String] = []
    // Keep GPU-written (memexport) data when a page is re-uploaded: fixes the dog-fur flashes
    // (2026-10-07 playtest fixes). On by default in the game; always passed explicitly.
    env.append("FABLE2_MEMEXPORT_PRESERVE=\(furFix ? 1 : 0)")
    // Copy GPU-written (memexport) data back into game memory once the GPU finishes, so guest
    // RAM matches the GPU (2026-10-07 playtest fixes, fix B). On by default in the game.
    env.append("FABLE2_MEMEXPORT_READBACK=\(memexportReadback ? 1 : 0)")
    // Internal resolution and upscaling (docs/resolution-scale.md); explicit so the launcher,
    // not [graphics] in the TOML, decides.
    env.append("FABLE2_RES_SCALE=\(resolution.rawValue)")
    env.append("FABLE2_UPSCALING=\(upscaling.rawValue)")
    // Frame generation (docs/macos-metal.md "Frame generation"); always explicit. The game turns it off unless it
    // runs at the 30 FPS cap on a supported Mac.
    env.append("FABLE2_FRAME_INTERP=\(frameGeneration == .on ? 1 : 0)")
    // Bloom/blur effect size at 2x/3x (docs/resolution-scale.md section 12); explicit so the
    // launcher, not [graphics] effects_native_scale in the TOML, decides.
    env.append("FABLE2_EFFECTS_NATIVE=\(effects == .original ? 1 : 0)")
    // Windowed or fullscreen (docs/resolution-scale.md, display options); explicit so the
    // launcher, not fable_2.toml or [graphics] in the TOML, decides.
    env.append("FABLE2_FULLSCREEN=\(display == .fullscreen ? 1 : 0)")
    // Turbo on/off notice style (docs/turbo.md); the game default is overlay.
    env.append("FABLE2_TURBO_NOTICE=\(turboNotice.rawValue)")
    // Turbo speed (F7) and audio during turbo (docs/turbo.md). Always explicit so the launcher, not
    // [patches] turbo_scale / turbo_audio in the TOML, decides; FABLE2_TURBO_SPEED does not start turbo on.
    env.append("FABLE2_TURBO_SPEED=\(String(format: "%.1f", TurboSpeed.clean(turboSpeed)))")
    // Slow-motion speed (F8, docs/turbo.md "Slow mode"); does not start slow mode on.
    env.append("FABLE2_SLOW_SPEED=\(String(format: "%.2f", SlowSpeed.clean(slowSpeed)))")
    env.append("FABLE2_TURBO_AUDIO=\(turboAudio.rawValue)")
    // Collector's Edition + website items (docs/patches.md). Always explicit so the toggle,
    // not [patches] unlock_ce / unlock_website in the TOML, decides.
    env.append("FABLE2_UNLOCK_EXTRAS=\(unlockExtras ? 1 : 0)")
    // On-screen FPS counter (src/core/fable2_fps_counter.h); F9 still toggles it in game.
    env.append("FABLE2_FPS_COUNTER=\(fpsCounter ? 1 : 0)")
    // Hot-shader specialization learns during play (docs/native-shaders.md).
    env.append("FABLE2_NS_SPECIALIZE_AUTOLEARN=\(shaderAutoLearn ? 1 : 0)")
    // Resolve straight into the sampled texture at native 1x (docs/resolution-scale.md
    // section 20): fewer texture reloads, less GPU-thread CPU. On by default in the game.
    env.append("FABLE2_RTT_1X=\(rtt1x ? 1 : 0)")
    // Render targets sized to the rows the game uses instead of the whole EDRAM height
    // (docs/resolution-scale.md section 21): about 2 GB less memory at 2x, 3.8 GB at 3x.
    env.append("FABLE2_RT_SHRINK=\(rtShrink ? 1 : 0)")
    // Learn file as an append-only journal, so the session's grown shader cache is published (docs/native-shaders.md).
    env.append("FABLE2_NS_LEARN_APPEND=\(learnAppend ? 1 : 0)")
    // Build the EDRAM transfer pipelines of earlier sessions in the background at startup (docs/frame-pacing-20261012.md).
    env.append("FABLE2_XFER_PREWARM=\(xferPrewarm ? 1 : 0)")
    // Native point/rectangle/tessellation pipelines recorded and archived for the next launch (docs/bug-patterns.md).
    env.append("FABLE2_NATIVE_GEOM_PERSIST=\(nativeGeomPersist ? 1 : 0)")
    // The headless shader prep also compiles the disc's shader bank (docs/shader-precompile-20261015.md).
    env.append("FABLE2_SHADER_PREP_BANK=\(shaderPrepBank ? 1 : 0)")
    // Fused resolve pipelines built in the background at startup (docs/native-shaders.md, "First launch").
    env.append("FABLE2_RTT_PREWARM=\(rttPrewarm ? 1 : 0)")
    // Startup pipelines and cached shader installs spread over the cores (docs/native-shaders.md, "First launch").
    env.append("FABLE2_STARTUP_PARALLEL=\(startupParallel ? 1 : 0)")
    // Save specialized shader pipelines promptly and at window close, so the next launch loads them (docs/native-shaders.md).
    env.append("FABLE2_NS2_ARCHIVE=\(ns2Archive ? 1 : 0)")
    // Lighter per-draw bookkeeping on the GPU command thread (docs/archive/perf-gpucmd-20261012.md).
    env.append("FABLE2_PREPARED_DRAW_SLIM=\(preparedDrawSlim ? 1 : 0)")
    // Texture key memo and per-draw trims on the GPU command thread (docs/gpucmd-cpu-20261014.md).
    env.append("FABLE2_TEX_KEY_MEMO=\(texKeyMemo ? 1 : 0)")
    env.append("FABLE2_DRAW_PREP_LITE=\(drawPrepLite ? 1 : 0)")
    env.append("FABLE2_DRAW_PREP_LITE2=\(drawPrepLite2 ? 1 : 0)")
    // A "no load" texture check stays exact: CPU writes after it load at the next use (docs/gpucmd-cpu4-20261014.md).
    env.append("FABLE2_LOAD_CHECK_EXACT=\(loadCheckExact ? 1 : 0)")
    // Native longest-match search of the game's save compressor: byte-identical saves, less save time (docs/load-fixes-20261016.md).
    env.append("FABLE2_NATIVE_SAVE_COMPRESS=\(nativeSaveCompress ? 1 : 0)")
    // Native zlib inflate_fast for the game's loader: byte-identical output, less load CPU (docs/load-fixes-20261016.md).
    env.append("FABLE2_NATIVE_INFLATE=\(nativeInflate ? 1 : 0)")
    // Character texture-space passes (dog lighting, morph) at 1x when scaled: no seam down the dog's
    // chest at 2x/3x (docs/resolution-scale.md section 23).
    env.append("FABLE2_TEXSPACE_NATIVE=\(texspaceNative ? 1 : 0)")
    // Opt-in game-logic fix: the texture streamer's per-model size cache tests the minimum distance it
    // computes from (docs/pale-root-20261014.md section 5). Default off (original game); always explicit.
    env.append("FABLE2_FIX_LOD_SIZE_CACHE=\(fixLodSizeCache ? 1 : 0)")
    // Opt-in pacing change, "Skip cosmetic waits" (docs/load-fixes-20261016.md section F): sets the defaults of
    // FABLE2_LOAD_FADE_FRAMES and FABLE2_SAVE_NOTICE_MS in the game. Always explicit. The sub-switches are not
    // passed, so an explicit value in Extra environment still wins over the umbrella.
    env.append("FABLE2_FAST_PACING=\(fastPacing ? 1 : 0)")
    // Opt-in mod: the Character Editor entry in the F6 menu (docs/character-editor.md). Default off; always explicit.
    env.append("FABLE2_CHARACTER_EDITOR=\(characterEditor ? 1 : 0)")
    // Shader cache keyed by a fixed converter target (apple7:15.0) instead of this Mac's GPU and exact macOS
    // version, so it survives macOS updates (docs/native-shaders.md, "Converter target"). Off = native.
    env.append("FABLE2_MSC_MIN_TARGET=\(portableShaderCache ? "portable" : "native")")
    // Default-on kill switches (=0 only when turned off) and default-off verify modes (=1 only when on):
    // docs/reference.md, docs/resolution-scale.md, docs/native-shaders.md, docs/frame-pacing-20261012.md.
    for (path, name) in LaunchSettings.killSwitches where !self[keyPath: path] {
      // A no-converter build ignores these at =0 (and logs it); don't pass them there.
      if !converterBuiltIn,
         let flag = LaunchSettings.flags.first(where: { $0.path == path }),
         LaunchSettings.requiredWithoutConverter.contains(flag.key) { continue }
      env.append(name + "=0")
    }
    for (path, name) in LaunchSettings.verifyModes where self[keyPath: path] { env.append(name + "=1") }
    // Keyboard map, mouse look and hotkeys (Controls tab, ControlsView.swift).
    env += controls.environment
    // Diagnostics: flight-recorder preset, then free-form extra environment (last wins).
    env += tracePreset.env + parseExtraEnv(extraEnv)
    return env
  }

  /// Frame pacing items: what tools/fable2_dev_session.py launch() sets for --fps 30/60/uncapped, and the
  /// Dev Launcher's own items for Auto (the game picks 60, 30 or, by the 2x rule, uncapped from the GPU,
  /// src/core/fable2_fps_auto.h; vsync on unless the game picks uncapped). "Use game settings" sets nothing.
  var frameRateEnvironment: [String] {
    switch frameRate {
    case .settings: return []
    case .auto: return ["FABLE2_FPS_60=auto", "REX_VSYNC=1"]
    case .thirty: return ["FABLE2_FPS_60=0", "REX_VSYNC=1"]
    case .sixty: return ["FABLE2_FPS_60=1", "REX_VSYNC=1"]
    case .uncapped: return ["FABLE2_FPS_60=0", "REX_VSYNC=0"]
    }
  }

  /// The whole environment for a direct launch (player app), in the order the dev session builds it:
  /// frame pacing, skip intro, then the settings items (later items win). Skip intro is explicit (=0 too),
  /// because the portable launcher otherwise turns it on by default (Fable2SkipIntroDefault).
  var gameEnvironment: [String] {
    frameRateEnvironment + ["FABLE2_SKIP_INTRO_MOVIES=\(skipIntro ? 1 : 0)"] + settingsEnvironment
  }
}
