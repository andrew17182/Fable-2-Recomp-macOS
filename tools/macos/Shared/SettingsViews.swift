// Settings tab views shared by both roles of the app (see LaunchSettings.swift). Settings live inside the launcher
// window (2026-10-19, LauncherPages.swift): SettingsPage shows the tabs Dev Options allows, and each role adds its
// own sections through `extra`.
import AppKit
import SwiftUI

/// Which app shows the settings: the Dev Launcher (CLI hints in tooltips, dev-session footers) or the
/// player app (plain wording).
enum SettingsVariant {
  case dev, player
}

/// The launcher window's two pages: the front page and Settings (one button toggles them; ⌘, opens Settings).
enum LauncherPage { case launcher, settings }

/// The settings and where they are stored. The Dev Launcher's LauncherModel subclasses it.
class SettingsModel: ObservableObject {
  let store: SettingsStore
  /// Front page or Settings (not remembered: the app opens on the front page).
  @Published var page = LauncherPage.launcher
  /// The Settings tab shown (remembered).
  @Published var settingsTab: SettingsTab { didSet { if persist { store.set(settingsTab.rawValue, forKey: "settingsTab") } } }
  /// Dev Options (2026-10-19, was "Developer mode"): the Performance, Fixes and Diagnostics tabs and the dev
  /// launcher features (build choice, save library, Tools menu, Update Playtest Build option, the Fable II Launcher
  /// window in the player role). Off by default in every role; remembered. CLI modes (--launch, --print-args) never
  /// read it.
  @Published var developerMode: Bool {
    didSet {
      if persist { store.set(developerMode, forKey: "developerMode") }
      if !developerMode && settingsTab.devOnly { settingsTab = .general }
      onDeveloperModeChange?(developerMode)
    }
  }
  /// Shows "Update Playtest Build" on the front page (Dev Options > Diagnostics > Developer). Off by default.
  @Published var showUpdatePlaytest: Bool {
    didSet { if persist { store.set(showUpdatePlaytest, forKey: "showUpdatePlaytest") } }
  }
  /// The save menu also lists starred library saves and the newest library save (Dev Options > Diagnostics >
  /// Developer, 2026-10-19). Off by default: the menu shows the live slots only.
  @Published var showStarredSaves: Bool {
    didSet { if persist { store.set(showStarredSaves, forKey: "showStarredSaves") } }
  }
  /// Set by the player role (opens the Dev Launcher model when Dev Options turns on).
  var onDeveloperModeChange: ((Bool) -> Void)?
  /// Playback (2026-10-22, docs/reference.md "Input recording and replay"): the recording picked in Settings >
  /// Diagnostics > Play recording (its file name in logs/recordings; "" = None, the front page shows no Run Playback).
  /// Remembered. Only roles that can play back (playbackSupported) show the picker.
  @Published var playbackRecording: String {
    didSet { if persist { store.set(playbackRecording, forKey: "playbackRecording") } }
  }
  /// The recordings in logs/recordings, newest first (refreshRecordings).
  @Published var recordings: [RecordingItem] = []
  /// Whether this role can play a recording back (both roles since 2026-10-22: the Dev Launcher through
  /// tools/fable2_dev_session.py --mode replay, the player app natively, Player/main.swift PlaybackSession).
  var playbackSupported: Bool { false }
  /// The running playback's replay status (the game's logs/replay-status.json: starting, running, waiting, complete,
  /// failed, cancelled); nil when the role's game is not a playback. --snapshot --replaying: running.
  @Published var replayStatus: String? = commandLine.contains("--snapshot") && commandLine.contains("--replaying")
    ? "running" : nil
  /// The front page's playback slot: "▶ Replaying" while the playback drives the game, else Run Playback for the
  /// picked recording.
  var playbackSlot: PlaybackSlot {
    if let status = replayStatus, ["starting", "running", "waiting"].contains(status) { return .replaying }
    return selectedRecording.map { .ready($0.label) } ?? .hidden
  }
  /// The picked recording while its file is there.
  var selectedRecording: RecordingItem? { recordings.first { $0.id == playbackRecording } }
  /// Run Playback (front page): the role's playback launch.
  func runPlayback() {}
  /// Re-reads logs/recordings (the Diagnostics tab, the front page and every save-list refresh).
  func refreshRecordings() {
    guard playbackSupported else { return }
    let list = InputRecordings.list()
    if list != recordings { recordings = list }
  }
  /// False during a --snapshot: page, tab and Dev Options set for the picture are not stored.
  let persist = !commandLine.contains("--snapshot")
  /// --snapshot ... --dev-options on|off: Dev Options for that picture only (never stored).
  static var snapshotDeveloper: Bool? {
    guard commandLine.contains("--snapshot"), let value = argument(after: "--dev-options") else { return nil }
    return value == "on"
  }
  /// The saved preset last loaded or saved (Custom shows its name), and whether Custom changed since.
  @Published var customName: String? { didSet { if persist { store.set(customName, forKey: "customPresetName") } } }
  @Published var customEdited: Bool { didSet { if persist { store.set(customEdited, forKey: "customPresetEdited") } } }
  /// Saved named presets (Save Preset…): name -> preset settings (LaunchSettings.presetDictionary).
  @Published private(set) var savedPresets: [String: [String: Any]]
  var savedPresetNames: [String] { savedPresets.keys.sorted { $0.localizedStandardCompare($1) == .orderedAscending } }
  /// The Presets menu's label: the loaded saved preset while Custom is selected.
  var presetMenuTitle: String {
    guard settings.preset == .custom, let name = customName else { return "Presets" }
    return customEdited ? name + " (edited)" : name
  }
  var visibleTabs: [SettingsTab] { SettingsTab.visible(developer: developerMode) }
  @Published var settings: LaunchSettings {
    didSet {
      // The build choice decides which app runs, so whether it has the converter (assigning inside didSet does
      // not call it again).
      if let probe = converterProbe, settings.build != oldValue.build {
        settings.converterBuiltIn = probe(settings.build)
      }
      // A change to a preset setting (not a preset switch, which changes the preset too) saves the values as the
      // Custom preset and selects it (docs/reference.md "Presets").
      // On Original or Enhanced that starts a new Custom (replacing the unsaved one; saved presets are untouched).
      if settings.preset == oldValue.preset && settings.presetSettingsDiffer(from: oldValue) {
        if settings.preset != .custom {
          settings.preset = .custom
          customName = nil
          customEdited = false
        } else if customName != nil {
          customEdited = true  // a loaded saved preset shows "(edited)" until saved
        }
        if persist { settings.storeCustom(to: store) }
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
    var loaded = LaunchSettings.load(from: store)
    // --snapshot: on/off switches given as --<cli> / --no-<cli> apply to that picture only (never stored).
    if commandLine.contains("--snapshot") { for arg in commandLine { _ = loaded.applyFlag(arg) } }
    // --preset for one snapshot, --print-env or --play (assigned before init ends: never stored).
    if ["--snapshot", "--print-env", "--play"].contains(where: commandLine.contains), let preset = argument(after: "--preset") {
      _ = loaded.applyCLIPreset(preset, store: store)
    }
    customName = store.string(forKey: "customPresetName")
    customEdited = store.object(forKey: "customPresetEdited") as? Bool ?? false
    savedPresets = LaunchSettings.namedPresets(in: store)
    settings = loaded
    let developer = Self.snapshotDeveloper ?? (store.object(forKey: "developerMode") as? Bool ?? false)
    developerMode = developer
    showUpdatePlaytest = store.object(forKey: "showUpdatePlaytest") as? Bool ?? false
    showStarredSaves = store.object(forKey: "showStarredSaves") as? Bool ?? false
    // --snapshot --recording NAME: that pick for the picture only.
    playbackRecording = (commandLine.contains("--snapshot") ? argument(after: "--recording") : nil)
      ?? store.string(forKey: "playbackRecording") ?? ""
    let tab = SettingsTab(rawValue: store.string(forKey: "settingsTab") ?? "") ?? .general
    settingsTab = !developer && tab.devOnly ? .general : tab
  }

  /// The front page's preset picker: Original and Enhanced apply their fixed values, Custom the saved Custom values.
  /// Nothing is lost: Custom stays saved while another preset is selected.
  func choosePreset(_ preset: SettingsPreset) {
    guard preset != settings.preset else { return }
    let custom = LaunchSettings.savedCustom(in: store)
    var s = settings
    s.applyPreset(preset, custom: custom)
    settings = s
    // Custom chosen before any was saved: the current values become it.
    if preset == .custom && custom == nil && persist { settings.storeCustom(to: store) }
  }

  /// Saves the current preset settings under `name` (an existing one is overwritten) and selects it as Custom.
  /// False for an empty name or a built-in one (Original, Enhanced, Custom).
  @discardableResult func savePreset(named raw: String) -> Bool {
    guard let name = Self.cleanPresetName(raw) else { return false }
    var all = savedPresets
    all[name] = settings.presetDictionary
    savedPresets = all
    store.set(all, forKey: LaunchSettings.namedKey)
    var s = settings
    s.preset = .custom
    settings = s
    settings.storeCustom(to: store)
    customName = name
    customEdited = false
    return true
  }

  /// Applies a saved preset: its values become Custom, selected, with its name shown.
  @discardableResult func loadPreset(named name: String) -> Bool {
    guard let values = savedPresets[name] else { return false }
    var s = settings
    s.applyPreset(.custom, custom: LaunchSettings.fromPresetDictionary(values))
    settings = s
    settings.storeCustom(to: store)
    customName = name
    customEdited = false
    return true
  }

  /// Deletes a saved preset (the current values stay; Custom no longer shows its name).
  func deletePreset(named name: String) {
    var all = savedPresets
    guard all.removeValue(forKey: name) != nil else { return }
    savedPresets = all
    store.set(all, forKey: LaunchSettings.namedKey)
    if customName == name { customName = nil; customEdited = false }
  }

  /// A preset name as stored (trimmed); nil when empty or a built-in name (Original, Enhanced, Custom).
  static func cleanPresetName(_ raw: String) -> String? {
    let name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
    return name.isEmpty || LaunchSettings.isReservedPresetName(name) ? nil : name
  }

  /// The Save Preset… name suggestion: the loaded preset's name while Custom is selected, else the first free
  /// "Custom N".
  var suggestedPresetName: String {
    if settings.preset == .custom, let name = customName { return name }
    var n = 1
    while savedPresets[ "Custom \(n)"] != nil { n += 1 }
    return "Custom \(n)"
  }

  /// Renames a saved preset. False for an unknown preset or an invalid name, or a name another preset has unless
  /// `replace` (that preset is then replaced).
  @discardableResult func renamePreset(_ old: String, to raw: String, replace: Bool = false) -> Bool {
    guard let values = savedPresets[old], let name = Self.cleanPresetName(raw) else { return false }
    if name == old { return true }
    if savedPresets[name] != nil && !replace { return false }
    var all = savedPresets
    all.removeValue(forKey: old)
    all[name] = values
    savedPresets = all
    store.set(all, forKey: LaunchSettings.namedKey)
    if customName == old { customName = name } else if customName == name { customName = nil; customEdited = false }
    return true
  }

  /// Asks for a name (field prefilled); returns it once valid and, when another preset has it, after
  /// confirming the replacement. nil when cancelled.
  private func askPresetName(title: String, button: String, initial: String, taken: (String) -> Bool) -> String? {
    var initial = initial
    while true {
      let alert = NSAlert()
      alert.messageText = title
      let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 24))
      field.placeholderString = "Name"
      field.stringValue = initial
      alert.accessoryView = field
      alert.window.initialFirstResponder = field
      alert.addButton(withTitle: button)
      alert.addButton(withTitle: "Cancel")
      guard alert.runModal() == .alertFirstButtonReturn else { return nil }
      initial = field.stringValue
      guard let name = Self.cleanPresetName(field.stringValue) else {
        let note = NSAlert()
        note.messageText = "Choose another name."
        note.informativeText = "Names can't be empty or Original, Enhanced or Custom."
        note.runModal()
        continue
      }
      if taken(name) {
        let confirm = NSAlert()
        confirm.messageText = "Replace \u{201C}\(name)\u{201D}?"
        confirm.addButton(withTitle: "Replace")
        confirm.addButton(withTitle: "Cancel")
        guard confirm.runModal() == .alertFirstButtonReturn else { continue }
      }
      return name
    }
  }

  /// Save Preset…: a name field prefilled with the loaded preset's name or "Custom N".
  func promptSavePreset() {
    guard let name = askPresetName(title: "Save Preset", button: "Save", initial: suggestedPresetName,
                                   taken: { self.savedPresets[$0] != nil }) else { return }
    savePreset(named: name)
  }

  /// Rename…: built-ins are not in the list, so they can't be renamed.
  func promptRenamePreset(_ old: String) {
    guard let name = askPresetName(title: "Rename Preset", button: "Rename", initial: old,
                                   taken: { $0 != old && self.savedPresets[$0] != nil }) else { return }
    renamePreset(old, to: name, replace: true)
  }

  /// Delete: asks first.
  func promptDeletePreset(named name: String) {
    let alert = NSAlert()
    alert.messageText = "Delete \u{201C}\(name)\u{201D}?"
    alert.addButton(withTitle: "Delete")
    alert.addButton(withTitle: "Cancel")
    if alert.runModal() == .alertFirstButtonReturn { deletePreset(named: name) }
  }

  /// ⌘, and the Settings button.
  func toggleSettingsPage() { page = page == .launcher ? .settings : .launcher }

  /// The front page's "Recording input" note: Settings > Diagnostics (the setting's tab; General without Dev Options).
  func openRecordingSetting() {
    if developerMode { settingsTab = .diagnostics }
    page = .settings
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

/// A switch with its label; the explanation is the tooltip (2026-10-19: no caption lines under switches).
struct FlagRow: View {
  @ObservedObject var model: SettingsModel
  let flag: Flag
  var variant: SettingsVariant = .dev
  static let forcedNote = "Required in this build (no shader converter)."
  var body: some View {
    let forced = model.forcedOn(flag)
    Toggle(isOn: forced ? .constant(true) : model.binding(flag.path)) {
      VStack(alignment: .leading, spacing: 2) {
        Text(flag.title)
        if forced { Text("Required in this build").font(.caption).foregroundStyle(.secondary) }
      }
    }
    .disabled(forced)
    .help(forced ? flag.detail + " " + Self.forcedNote
          : variant == .dev
          ? flag.detail + " (--" + (flag.defaultValue ? "no-" : "") + flag.cli + " to change for one CLI launch)"
          : flag.detail)
  }
}

/// Frame generation picker (FrameGeneration): always the same two lines; the caption gives the cost, or why it
/// stays off (unsupported Mac: picker disabled and shown Off; another frame rate: disabled, choice kept).
struct FrameGenerationRow: View {
  @ObservedObject var model: SettingsModel
  var body: some View {
    let s = model.settings
    let unsupported = FrameGeneration.unsupportedReason != nil
    let blocked = FrameGeneration.blockedReason(frameRate: s.frameRate, resolution: s.resolution, rule2x: s.auto2x)
    FixedPicker("Frame generation", caption: blocked ?? "Smoother motion, about 20 ms more delay",
                selection: unsupported ? .constant(.off) : $model.settings.frameGeneration) {
      ForEach(FrameGeneration.allCases) { mode in Text(mode.title).tag(mode) }
    }
    .disabled(blocked != nil)
    .help("MetalFX makes a frame between every two: 30 FPS shows as 60. Moving characters can smear. "
          + "Needs the 30 FPS cap and macOS 26.")
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

struct SettingsTabView: View {
  @ObservedObject var model: SettingsModel
  let tab: SettingsTab
  var variant: SettingsVariant = .dev
  /// App-specific sections appended to a tab (player app: launcher options, logs, reset).
  var extra: (SettingsTab) -> AnyView = { _ in AnyView(EmptyView()) }

  /// Fills the page between the tab bar and the page bar (fixed window size, LauncherLayout); a taller tab scrolls.
  var body: some View {
    ScrollView {
      Form {
        content
        extra(tab)
      }
        .formStyle(.grouped)
        .scrollDisabled(true)
        .fixedSize(horizontal: false, vertical: true)
    }
    // No scroll bar: a legacy one appeared only on taller tabs and narrowed their rows, shifting every switch
    // sideways between tabs. Taller tabs still scroll (wheel, trackpad).
    .scrollIndicators(.never)
    .scrollBounceBehavior(.basedOnSize)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
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
        FixedPicker("Turbo notice", selection: $model.settings.turboNotice) {
          ForEach(TurboNotice.allCases) { style in Text(style.title).tag(style) }
        }
        .help("How the turbo (F7) notice is shown.")
      }
    case .mods:
      section(.mods, "Mods")
    case .display:
      Section("Resolution") {
        FixedPicker("Internal resolution", selection: $model.settings.resolution) {
          ForEach(InternalResolution.allCases) { scale in
            Text(scale.title(frameRate: model.settings.frameRate, rule2x: model.settings.auto2x)).tag(scale)
          }
        }
        .help(ResolutionHelp.resolution)
        FixedPicker("Upscaling (window scaling)", selection: $model.settings.upscaling) {
          ForEach(Upscaling.allCases) { mode in Text(mode.title).tag(mode) }
        }
        .help("How the image is scaled to the window. Does not change the render resolution or its cost.")
      }
      Section("Window") {
        FixedPicker("Display", selection: $model.settings.display) {
          ForEach(DisplayMode.allCases) { mode in Text(mode.title).tag(mode) }
        }
        .help("Fullscreen is a macOS full-screen Space.")
      }
      Section("Frame rate") {
        FixedPicker("Frame rate", selection: $model.settings.frameRate) {
          ForEach(FrameRate.allCases) { fps in Text(fps.title(resolution: model.settings.resolution, rule2x: model.settings.auto2x)).tag(fps) }
        }
        .help(ResolutionHelp.frameRate)
        FrameGenerationRow(model: model)
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
      section(.fixes, "Display")
      section(.fixes, "Turbo")
      Section {
        ForEach(LaunchSettings.flags(in: .fixes).filter { $0.section == "Game bug fixes (changes original behaviour)" }) {
          row($0)
        }
      } header: {
        Text("Game bug fixes (changes original behaviour)")
      } footer: {
        Text("Off by default: they change the original game.")
          .font(.caption).foregroundStyle(.secondary)
      }
      Section {
        ForEach(LaunchSettings.flags(in: .fixes).filter { $0.section == "Boot" }) { row($0) }
      } header: {
        Text("Boot")
      } footer: {
        HStack(alignment: .firstTextBaseline) {
          Text("Leave on unless comparing.")
            .font(.caption).foregroundStyle(.secondary)
          Spacer()
          Button("Reset All to Defaults") { model.resetFlags(in: .fixes) }
            .disabled(LaunchSettings.flags(in: .fixes).allSatisfy { model.settings[keyPath: $0.path] == $0.defaultValue })
            .help("Game bug fixes off, the rest on.")
        }
      }
    case .diagnostics:
      Section("Tracing") {
        FixedPicker("Trace preset", selection: $model.settings.tracePreset) {
          ForEach(TracePreset.allCases) { preset in Text(preset.title).tag(preset) }
        }
        .help("Flight-recorder preset (docs/telemetry-design-20261008.md).")
        ForEach(LaunchSettings.flags(in: .diagnostics).filter { $0.section == "Logging" }) { row($0) }
      }
      Section {
        ForEach(LaunchSettings.flags(in: .diagnostics).filter { $0.section == "Input recording" }) { row($0) }
        if model.playbackSupported {
          FixedPicker("Play recording", selection: $model.playbackRecording) {
            Text("None").tag("")
            ForEach(model.recordings) { Text($0.label).tag($0.id) }
            // A remembered pick whose file is gone: shown as missing, never a blank menu.
            if !model.playbackRecording.isEmpty && model.selectedRecording == nil {
              Text("Missing recording").tag(model.playbackRecording)
            }
          }
          .help("Run Playback on the front page plays it with the current settings, from the save it started in. "
                + "Newest first.")
          .onAppear { model.refreshRecordings() }
        }
        HStack {
          Text("One file per game in logs/recordings.").font(.caption).foregroundStyle(.secondary)
          Spacer()
          Button("Show Recordings") { InputRecordings.reveal() }
            .help(InputRecordings.folder)
        }
      } header: {
        Text("Input recording")
      }
      section(.diagnostics, "Verify modes")
      Section("Graphics") {
        Toggle("Effects at full resolution (shifts glows)", isOn: Binding(
          get: { model.settings.effects == .full },
          set: { model.settings.effects = $0 ? .full : .original }))
          .help("At 2× and 3×, bloom, blur and glow kernels shrink with the resolution and shift right and down. "
                + "Off: native 1× size, the original look.")
      }
      Section {
        TextField("Extra environment", text: $model.settings.extraEnv, prompt: Text("NAME=VALUE NAME2=VALUE2"))
          .labelsHidden()
          .font(.system(.body, design: .monospaced))
          .help("Passed last, so it wins over every setting above.")
      } header: {
        Text("Extra environment")
      } footer: {
        if variant == .dev {
          Text("Report: python3 tools/f2diag.py report <session>")
            .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
        } else {
          Text("Overrides the settings above.")
            .font(.caption).foregroundStyle(.secondary)
        }
      }
    }
  }
}

/// Where the game writes input recordings (Settings > Diagnostics > Record input): logs/recordings in the state
/// folder the game uses. Dev Launcher role: the live state (tools/fable2_dev_session.py sets
/// FABLE2_RECORD_INPUT_PATH there for play sessions); player role: the app's own state folder.
enum InputRecordings {
  /// Same rules as Backend.liveState (Dev Launcher) and Paths.state (player), kept here so the shared files
  /// build on their own (tests/controls_tab_tests.py).
  static var folder: String {
    let env = ProcessInfo.processInfo.environment
    let state: String
    if AppRole.devLauncher {
      state = env["FABLE2_LIVE_STATE_ROOT"].flatMap { $0.isEmpty ? nil : $0 }
        ?? NSHomeDirectory() + "/Library/Application Support/Fable II Recomp"
    } else if let root = env["FABLE2_APP_STATE_ROOT"], !root.isEmpty {
      state = root
    } else {
      let name = Bundle.main.object(forInfoDictionaryKey: "Fable2StateFolder") as? String ?? "Fable II Recomp"
      state = NSHomeDirectory() + "/Library/Application Support/" + name
    }
    return state + "/logs/recordings"
  }
  /// Opens the folder in Finder (created first, so the button always works). Our own Application Support folder:
  /// no permission prompt.
  static func reveal() {
    try? FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
    NSWorkspace.shared.open(URL(fileURLWithPath: folder, isDirectory: true))
  }
}

/// One input recording in logs/recordings, for the Play recording picker: the file name (id), its path, and a short
/// label "2026-10-22 10:24:15 · Hero000" (the date and time from the name, the save it started from, or "Title").
struct RecordingItem: Identifiable, Hashable {
  let id: String
  let path: String
  let label: String
  let date: Date
}

extension InputRecordings {
  /// Every *.jsonl in `folder`, newest first (the time in the game's file name, else the file's modification time).
  static func list(in folder: String = InputRecordings.folder) -> [RecordingItem] {
    let names = (try? FileManager.default.contentsOfDirectory(atPath: folder)) ?? []
    let items = names.filter { $0.hasSuffix(".jsonl") && !$0.hasPrefix(".") }.map { name -> RecordingItem in
      let path = folder + "/" + name
      let date = stamp(name) ?? ((try? FileManager.default.attributesOfItem(atPath: path))?[.modificationDate] as? Date)
        ?? .distantPast
      return RecordingItem(id: name, path: path, label: label(name: name, date: date, meta: meta(path)), date: date)
    }
    return items.sorted { $0.date != $1.date ? $0.date > $1.date : $0.id > $1.id }
  }

  /// "Fable II input 2026-10-22 10-24-15[ 2].jsonl" -> that local time.
  static func stamp(_ name: String) -> Date? {
    guard let range = name.range(of: #"\d{4}-\d\d-\d\d \d\d-\d\d-\d\d"#, options: .regularExpression) else { return nil }
    let format = DateFormatter()
    format.locale = Locale(identifier: "en_US_POSIX")
    format.dateFormat = "yyyy-MM-dd HH-mm-ss"
    return format.date(from: String(name[range]))
  }

  /// The recording's AppLaunch metadata (its first line), empty when unreadable.
  static func meta(_ path: String) -> [String: Any] {
    guard let handle = FileHandle(forReadingAtPath: path) else { return [:] }
    defer { try? handle.close() }
    let head = (try? handle.read(upToCount: 4096)) ?? Data()
    guard let line = head.split(separator: UInt8(ascii: "\n"), maxSplits: 1).first,
          let object = try? JSONSerialization.jsonObject(with: Data(line)) as? [String: Any] else { return [:] }
    return object["meta"] as? [String: Any] ?? [:]
  }

  static func label(name: String, date: Date, meta: [String: Any]) -> String {
    let save = (meta["save"] as? String).flatMap { $0.isEmpty ? nil : $0 }
    let start = save ?? (meta["start"] as? String == "save" ? "Save" : "Title")
    guard date != .distantPast else { return (name as NSString).deletingPathExtension + " · " + start }
    let format = DateFormatter()
    format.locale = Locale(identifier: "en_US_POSIX")
    format.dateFormat = "yyyy-MM-dd HH:mm:ss"
    return format.string(from: date) + " · " + start
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
