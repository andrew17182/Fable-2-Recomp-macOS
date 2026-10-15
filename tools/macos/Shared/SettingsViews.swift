// Settings tab views shared by the Dev Launcher and the player app (see LaunchSettings.swift). Each app
// builds its own Settings window from SettingsTabView: the Dev Launcher shows every tab as before; the
// player app calls the Diagnostics tab "Advanced" and adds its own sections through `extra`.
import AppKit
import SwiftUI

/// Which app shows the settings: the Dev Launcher (CLI hints in tooltips, dev-session footers) or the
/// player app (plain wording).
enum SettingsVariant {
  case dev, player
}

/// The settings and where they are stored. The Dev Launcher's LauncherModel subclasses it.
class SettingsModel: ObservableObject {
  let store: SettingsStore
  @Published var settings: LaunchSettings {
    didSet {
      // The build choice decides which app runs, so whether it has the converter (assigning inside didSet does
      // not call it again).
      if let probe = converterProbe, settings.build != oldValue.build {
        settings.converterBuiltIn = probe(settings.build)
      }
      if settings != oldValue { settings.save(to: store, old: oldValue) }
    }
  }
  /// Set by the app: whether the game build a choice launches has the shader converter
  /// (LaunchSettings.hasConverter(macOSFolder:)). Without converter, the requiredWithoutConverter switches are
  /// shown on and disabled and not passed as =0.
  var converterProbe: ((BuildChoice) -> Bool)? {
    didSet { refreshConverter() }
  }

  /// Re-reads the probe (after a rebuild or a playtest update changed the app on disk).
  func refreshConverter() {
    guard let probe = converterProbe else { return }
    let has = probe(settings.build)
    if has != settings.converterBuiltIn { settings.converterBuiltIn = has }
  }

  init(store: SettingsStore) {
    self.store = store
    settings = LaunchSettings.load(from: store)
  }

  func binding(_ path: WritableKeyPath<LaunchSettings, Bool>) -> Binding<Bool> {
    Binding(get: { self.settings[keyPath: path] }, set: { self.settings[keyPath: path] = $0 })
  }

  /// Turns every switch of one Settings tab back to its default.
  func resetFlags(in tab: SettingsTab) {
    var s = settings
    for flag in LaunchSettings.flags(in: tab) { s[keyPath: flag.path] = flag.defaultValue }
    settings = s
  }

  /// Re-reads the settings from the store (another window of the same app may have changed them).
  func reloadSettings() {
    let stored = LaunchSettings.load(from: store)
    var s = stored
    s.converterBuiltIn = settings.converterBuiltIn
    if s != settings { settings = s }
  }

  /// Every setting back to its default (the build choice, a Dev Launcher main-window pick, is kept).
  func resetAll() {
    var s = LaunchSettings()
    s.build = settings.build
    s.converterBuiltIn = settings.converterBuiltIn
    settings = s
  }

  var isDefault: Bool {
    var s = LaunchSettings()
    s.build = settings.build
    s.converterBuiltIn = settings.converterBuiltIn
    return settings == s
  }

  /// A switch the launched build forces on (no shader converter, docs/no-converter.md): shown on and disabled.
  func forcedOn(_ flag: Flag) -> Bool {
    !settings.converterBuiltIn && LaunchSettings.requiredWithoutConverter.contains(flag.key)
  }
}

/// A switch with its one-line explanation (also the tooltip).
struct FlagRow: View {
  @ObservedObject var model: SettingsModel
  let flag: Flag
  var variant: SettingsVariant = .dev
  static let forcedNote = "Required in this build: it has no shader converter to fall back to."
  var body: some View {
    let forced = model.forcedOn(flag)
    Toggle(isOn: forced ? .constant(true) : model.binding(flag.path)) {
      VStack(alignment: .leading, spacing: 2) {
        Text(flag.title)
        Text(forced ? Self.forcedNote : flag.detail).font(.caption).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      }
    }
    .disabled(forced)
    .help(forced ? flag.detail + " " + Self.forcedNote
          : variant == .dev
          ? flag.detail + " (--" + (flag.defaultValue ? "no-" : "") + flag.cli + " to change for one CLI launch)"
          : flag.detail)
  }
}

/// The switches of one tab and section, in table order.
struct FlagSection: View {
  @ObservedObject var model: SettingsModel
  let tab: SettingsTab
  let section: String
  var variant: SettingsVariant = .dev
  var body: some View {
    Section(section) {
      ForEach(LaunchSettings.flags(in: tab).filter { $0.section == section }) {
        FlagRow(model: model, flag: $0, variant: variant)
      }
    }
  }
}

private struct SettingsContentHeight: PreferenceKey {
  static var defaultValue: CGFloat = 0
  static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}

struct SettingsTabView: View {
  @ObservedObject var model: SettingsModel
  let tab: SettingsTab
  var variant: SettingsVariant = .dev
  /// App-specific sections appended to a tab (player app: launcher options, logs, reset).
  var extra: (SettingsTab) -> AnyView = { _ in AnyView(EmptyView()) }

  /// Tallest a tab may make the Settings window; taller tabs (Fixes, ~765 pt) scroll so they fit a 900-pt
  /// laptop screen.
  static let maxHeight: CGFloat = 600
  @State private var contentHeight: CGFloat = 0

  var body: some View {
    ScrollView {
      Form {
        content
        extra(tab)
      }
        .formStyle(.grouped)
        .scrollDisabled(true)
        .fixedSize(horizontal: false, vertical: true)  // each tab sizes the window to its content
        .background(GeometryReader { proxy in
          Color.clear.preference(key: SettingsContentHeight.self, value: proxy.size.height)
        })
    }
    .scrollBounceBehavior(.basedOnSize)
    .frame(height: contentHeight > 0 ? min(contentHeight, Self.maxHeight) : nil)
    .onPreferenceChange(SettingsContentHeight.self) { contentHeight = $0 }
  }

  private func row(_ flag: Flag) -> FlagRow { FlagRow(model: model, flag: flag, variant: variant) }
  private func section(_ tab: SettingsTab, _ name: String) -> FlagSection {
    FlagSection(model: model, tab: tab, section: name, variant: variant)
  }

  @ViewBuilder private var content: some View {
    switch tab {
    case .general:
      section(.general, "Startup")
      Section("Game") {
        ForEach(LaunchSettings.flags(in: .general).filter { $0.section == "Game" }) { row($0) }
        Picker("Turbo notice", selection: $model.settings.turboNotice) {
          ForEach(TurboNotice.allCases) { style in Text(style.title).tag(style) }
        }
        .help("How the \"Turbo 2x on/off\" notice (F7) is shown (docs/turbo.md).")
      }
    case .display:
      Section("Resolution") {
        Picker("Internal resolution", selection: $model.settings.resolution) {
          ForEach(InternalResolution.allCases) { scale in
            Text(scale.title(frameRate: model.settings.frameRate, rule2x: model.settings.auto2x)).tag(scale)
          }
        }
        .help("The size the game renders at (docs/resolution-scale.md). Auto picks 1×, 2× or 3× from this "
              + "Mac's GPU, RAM and the frame-rate setting, aiming to hold the frame rate with headroom "
              + "(the game log's [res-auto] line says why).")
        Picker("Upscaling", selection: $model.settings.upscaling) {
          ForEach(Upscaling.allCases) { mode in Text(mode.title).tag(mode) }
        }
        .help("Off: one bilinear filter to the window (original look). MetalFX Spatial: Apple's spatial "
              + "upscaler from the internal resolution to the window; sharper, changes the look, and only "
              + "acts when the window is larger (e.g. 2× in 4K fullscreen: cheaper than 3×).")
        Picker("Bloom and effects", selection: $model.settings.effects) {
          ForEach(EffectsSize.allCases) { mode in Text(mode.title).tag(mode) }
        }
        .disabled(model.settings.resolution.effectiveScale(frameRate: model.settings.frameRate, rule2x: model.settings.auto2x) == 1)
        .help("At 2×/3×: keep bloom, blur and glow at their original size, or shrink them with the resolution. "
              + "No effect at native resolution.")
      }
      Section("Window") {
        Picker("Display", selection: $model.settings.display) {
          ForEach(DisplayMode.allCases) { mode in Text(mode.title).tag(mode) }
        }
        .help("Fullscreen is a macOS full-screen Space.")
      }
      Section("Frame rate") {
        Picker("Frame rate", selection: $model.settings.frameRate) {
          ForEach(FrameRate.allCases) { fps in Text(fps.title(resolution: model.settings.resolution, rule2x: model.settings.auto2x)).tag(fps) }
        }
        .help("Auto: 60 FPS when this Mac's GPU is estimated to average about 54 FPS or more at the chosen "
              + "internal resolution, else 30 FPS. At 2× (and with Auto resolution, when 2× is estimated at 45 FPS "
              + "or more), uncapped instead of 30 (docs/resolution-scale.md §25). The original game runs at 30: "
              + "pick 30 FPS for that.")
        ForEach(LaunchSettings.flags(in: .display)) { row($0) }
      }
    case .controls:
      ControlsTabContent(model: model, variant: variant)
    case .performance:
      section(.performance, "Shader cache")
      section(.performance, "Hot-shader specialization")
      section(.performance, "Startup and CPU")
    case .fixes:
      section(.fixes, "Dog")
      section(.fixes, "Render targets")
      Section {
        ForEach(LaunchSettings.flags(in: .fixes).filter { $0.section == "Game bug fixes (changes original behaviour)" }) {
          row($0)
        }
      } header: {
        Text("Game bug fixes (changes original behaviour)")
      } footer: {
        Text("Off by default: these correct bugs in the original game, so the game no longer behaves exactly as on Xbox 360.")
          .font(.caption).foregroundStyle(.secondary)
      }
      Section {
        ForEach(LaunchSettings.flags(in: .fixes).filter { $0.section == "Boot" }) { row($0) }
      } header: {
        Text("Boot")
      } footer: {
        HStack(alignment: .firstTextBaseline) {
          Text("Leave these on; turn one off only to compare with the old behaviour.")
            .font(.caption).foregroundStyle(.secondary)
          Spacer()
          Button("Reset All to Defaults") { model.resetFlags(in: .fixes) }
            .disabled(LaunchSettings.flags(in: .fixes).allSatisfy { model.settings[keyPath: $0.path] == $0.defaultValue })
            .help("Restore every fix to its default (game bug fixes off, the rest on).")
        }
      }
    case .diagnostics:
      Section("Tracing") {
        Picker("Trace preset", selection: $model.settings.tracePreset) {
          ForEach(TracePreset.allCases) { preset in Text(preset.title).tag(preset) }
        }
        .help("Flight-recorder preset (docs/telemetry-design-20261008.md).")
        ForEach(LaunchSettings.flags(in: .diagnostics).filter { $0.section == "Logging" }) { row($0) }
      }
      section(.diagnostics, "Verify modes")
      Section {
        TextField("Extra environment", text: $model.settings.extraEnv, prompt: Text("NAME=VALUE NAME2=VALUE2"))
          .labelsHidden()
          .font(.system(.body, design: .monospaced))
          .help("Passed last, so it wins over every setting above.")
      } header: {
        Text("Extra environment")
      } footer: {
        if variant == .dev {
          Text("Flight-recorder dumps: <session>/test-state/logs/trace/dumps. "
               + "Report: python3 tools/f2diag.py report <session>")
            .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
        } else {
          Text("Passed to the game last, so it wins over every setting above. For troubleshooting.")
            .font(.caption).foregroundStyle(.secondary)
        }
      }
    }
  }
}

/// Saves one of this app's own windows to a PNG (--snapshot in both apps). CGWindowListCreateImage needs no
/// screen-recording permission for the app's own windows; it is looked up at run time because it is
/// deprecated in favour of ScreenCaptureKit, which needs that permission even for the app's own windows.
enum WindowSnapshot {
  static func capture(_ windowNumber: Int) -> CGImage? {
    typealias Create = @convention(c) (CGRect, UInt32, UInt32, UInt32) -> Unmanaged<CGImage>?
    guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImage") else { return nil }
    let create = unsafeBitCast(symbol, to: Create.self)
    return create(.null, CGWindowListOption.optionIncludingWindow.rawValue, UInt32(windowNumber),
                  CGWindowImageOption([.boundsIgnoreFraming, .bestResolution]).rawValue)?.takeRetainedValue()
  }

  /// Writes `window` to `path` as PNG; false on failure.
  static func write(_ window: NSWindow, to path: String) -> Bool {
    guard let image = capture(window.windowNumber),
          let png = NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:]) else { return false }
    return (try? png.write(to: URL(fileURLWithPath: path))) != nil
  }
}
