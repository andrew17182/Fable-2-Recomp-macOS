// The launcher window's pages, shared by both roles (2026-10-19): the front page parts (Start at Title Screen, Load
// Saved Game and the save menu,
// resolution and frame rate, Skip intro, turbo) and the Settings page inside the same window. One button, bottom-left
// on both pages (PageBar), toggles them ("Settings" / "Launcher"); ⌘, opens Settings. Settings tabs: General,
// Display, Controls, Mods; Performance, Fixes and Diagnostics only with Dev Options on (SettingsModel.developerMode).
// Fixed layout (maintainer, 2026-10-19): click targets never move or resize with state, labels or toggles. One window
// size for both pages (LauncherLayout), fixed-width pickers (FixedPicker) and tabs (SettingsTabBar); a Settings tab
// taller than the page scrolls.
import AppKit
import SwiftUI

/// The launcher window's fixed content size, both pages and both roles.
enum LauncherLayout {
  static let width: CGFloat = 620
  static let height: CGFloat = 610  // 560 until the preset row (2026-10-22)
  /// Width of every pop-up menu in a form row (the label left, the menu at the right edge).
  static let pickerWidth: CGFloat = 270
}

/// A form row: label, then a pop-up menu of fixed width at the right edge (a selection never resizes it).
struct FixedPicker<Value: Hashable, Content: View>: View {
  let title: String
  /// A second line under the title (pass one for every state, so the row height never changes).
  let caption: String?
  @Binding var selection: Value
  let content: Content
  init(_ title: String, caption: String? = nil, selection: Binding<Value>, @ViewBuilder content: () -> Content) {
    self.title = title
    self.caption = caption
    _selection = selection
    self.content = content()
  }
  var body: some View {
    LabeledContent {
      Picker(title, selection: $selection) { content }
        .labelsHidden()
        .frame(width: LauncherLayout.pickerWidth, alignment: .trailing)
    } label: {
      if let caption = caption {
        VStack(alignment: .leading, spacing: 2) {
          Text(title)
          Text(caption).font(.caption).foregroundStyle(.secondary)
        }
      } else {
        Text(title)
      }
    }
  }
}

/// Short tooltips shared by the front page and the Display tab.
enum ResolutionHelp {
  static let resolution = "The size the game renders at. Auto picks it for this Mac."
  static let frameRate = "Auto picks the frame rate for this Mac. The original game runs at 30 FPS."
}

/// One entry of the save menu: a save id with its short label. "title" is the pick with no save (Start at Title
/// Screen; never listed in the menu since 2026-10-19).
struct SaveChoice: Identifiable, Hashable {
  let id: String
  let label: String
  static let titleID = "title"
}

/// The preset row at the top of the front page (2026-10-22): Original · Enhanced · Custom, then the Presets menu
/// (Save Preset…, the saved presets, Rename, Delete), both fixed width. Every control stays enabled; changing a preset setting
/// selects Custom (SettingsModel.settings). The menu's label shows the loaded saved preset ("Name (edited)").
struct PresetPicker: View {
  @ObservedObject var model: SettingsModel
  static let width: CGFloat = 250
  static let menuWidth: CGFloat = 170
  var body: some View {
    let selection = Binding(get: { model.settings.preset }, set: { model.choosePreset($0) })
    LabeledContent("Preset") {
      HStack(spacing: 10) {
        Picker("Preset", selection: selection) {
          ForEach(SettingsPreset.allCases) { Text($0.title).tag($0) }
        }
        .labelsHidden()
        .pickerStyle(.segmented)
        .frame(width: Self.width)
        .help("Original: as on the Xbox 360. Enhanced: higher resolution and frame rate, plus turbo. "
              + "Custom: your own settings.")
        Menu {
          Button("Save Preset…") { model.promptSavePreset() }
          if !model.savedPresetNames.isEmpty {
            Divider()
            ForEach(model.savedPresetNames, id: \.self) { name in
              Button(name) { model.loadPreset(named: name) }
            }
            Divider()
            Menu("Rename") {
              ForEach(model.savedPresetNames, id: \.self) { name in
                Button(name + "…") { model.promptRenamePreset(name) }
              }
            }
            Menu("Delete") {
              ForEach(model.savedPresetNames, id: \.self) { name in
                Button(name) { model.promptDeletePreset(named: name) }
              }
            }
          }
        } label: {
          Text(model.presetMenuTitle).lineLimit(1).truncationMode(.tail)
        }
        .menuStyle(.borderlessButton)
        .fixedSize(horizontal: true, vertical: false)
        .frame(width: Self.menuWidth, alignment: .trailing)
        .clipped()
        .help("Save the current settings as a preset, or load one.")
      }
    }
  }
}

/// The top of the front page (2026-10-19): [Start at Title Screen] [Load Saved Game], equal fixed widths, then the
/// save menu (saves only, newest first) below them at a fixed width. Load Saved Game is the accent default button
/// (Return); with no saves it is disabled (same size) and Start at Title Screen becomes the default.
struct SaveMenu: View {
  let choices: [SaveChoice]
  @Binding var selection: String
  let busy: Bool
  let disabled: Bool
  let startTitle: () -> Void
  let load: () -> Void
  /// Settings > Diagnostics > Record input is on: a red "Recording input" note right of the buttons (its slot is
  /// always there, so nothing moves when it shows). Clicking it opens that setting.
  var recording = false
  var showRecording: () -> Void = {}
  /// Playback (2026-10-22): the same right slot holds Run Playback while a recording is picked (Settings > Diagnostics
  /// > Play recording) and "▶ Replaying" while a playback drives the game (PlaybackSlot). Hidden: the recording note.
  var playback = PlaybackSlot.hidden
  var runPlayback: () -> Void = {}
  static let buttonWidth: CGFloat = 200
  static let spacing: CGFloat = 10
  static var menuWidth: CGFloat { 2 * buttonWidth + spacing }

  var body: some View {
    let hasSaves = !choices.isEmpty
    VStack(alignment: .leading, spacing: Self.spacing) {
      HStack(spacing: Self.spacing) {
        Button(action: startTitle) {
          Text("Start at Title Screen").frame(width: Self.buttonWidth)
        }
        .controlSize(.large)
        .keyboardShortcut(hasSaves ? nil : .defaultAction)
        .disabled(disabled)
        .help("Start at the title screen (intro movies first unless skipped).")
        Button(action: load) {
          Text(busy ? "Working…" : "Load Saved Game").fontWeight(.semibold).frame(width: Self.buttonWidth)
        }
        .buttonStyle(.borderedProminent)  // the accent-coloured default button (Return), every role
        .tint(.accentColor)
        .controlSize(.large)
        .keyboardShortcut(hasSaves ? .defaultAction : nil)
        .disabled(disabled || !hasSaves)
        .help("Load the save below. ↩")
        Spacer(minLength: 0)
        // One fixed slot (RecordingBadge.width): nothing moves when its content changes.
        ZStack(alignment: .trailing) {
          RecordingBadge(on: recording && playback == .hidden, action: showRecording)
          PlaybackButton(slot: playback, disabled: disabled, recording: recording, action: runPlayback)
          ReplayingBadge(on: playback == .replaying)
        }
        .frame(width: RecordingBadge.width, alignment: .trailing)
      }
      HStack(spacing: Self.spacing) {
        Text("Select save game:")
          .frame(width: Self.labelWidth, alignment: .leading)
        Picker("Save", selection: $selection) {
        if hasSaves {
          ForEach(choices) { Text($0.label).tag($0.id) }
        } else {
          Text("No saves").tag(SaveChoice.titleID)
        }
      }
        .labelsHidden()
        .frame(width: Self.menuWidth, alignment: .trailing)  // fixed: a long save label truncates
        .disabled(!hasSaves)
        .help("Newest save first.")
        Spacer(minLength: 0)
      }
    }
  }
  /// The fixed label column left of the save menu.
  static let labelWidth: CGFloat = 118
}

/// "● Recording input" in a fixed slot (empty, same size, when recording is off).
struct RecordingBadge: View {
  let on: Bool
  let action: () -> Void
  static let width: CGFloat = 132
  var body: some View {
    Button(action: action) {
      HStack(spacing: 5) {
        Circle().fill(Color.red).frame(width: 8, height: 8)
        Text("Recording input").font(.callout).foregroundStyle(.red)
      }
      .frame(width: Self.width, alignment: .trailing)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .opacity(on ? 1 : 0)
    .disabled(!on)
    .accessibilityHidden(!on)
    .help("Input is being recorded (Settings > Diagnostics).")
  }
}

/// What the front page's right slot shows for playback: nothing (the recording note may show), Run Playback for the
/// picked recording (its label as the caption), or "▶ Replaying" while the playback drives the game.
enum PlaybackSlot: Equatable {
  case hidden
  case ready(String)
  case replaying
}

/// "Run Playback" with the picked recording's date and time under it, in the fixed slot (empty, same size, otherwise).
/// With Record input on, a red dot before the caption stands in for the "Recording input" note it covers (Load Saved
/// Game still records; playback never does).
struct PlaybackButton: View {
  let slot: PlaybackSlot
  let disabled: Bool
  var recording = false
  let action: () -> Void
  var body: some View {
    let label: String? = { if case .ready(let text) = slot { return text } else { return nil } }()
    VStack(alignment: .trailing, spacing: 1) {
      Button(action: action) { Text("Run Playback") }
        .disabled(disabled || label == nil)
      HStack(spacing: 4) {
        if recording {
          Circle().fill(Color.red).frame(width: 6, height: 6).help("Record input is on (not during playback).")
        }
        Text(label.map(Self.caption) ?? " ").font(.caption2).foregroundStyle(.secondary).lineLimit(1)
      }
    }
    .frame(width: RecordingBadge.width, alignment: .trailing)
    .opacity(label == nil ? 0 : 1)
    .accessibilityHidden(label == nil)
    .help(label.map { "Play back \($0) with the current settings." } ?? "")
  }
  /// "2026-10-22 10:24:15 · Hero000" -> "10-22 10:24 · Hero000" (fits the slot; the full label is the tooltip).
  static func caption(_ label: String) -> String {
    let parts = label.components(separatedBy: " · ")
    let stamp = parts[0].split(separator: " ")
    guard stamp.count == 2, stamp[0].count == 10 else { return label }
    let short = String(stamp[0].dropFirst(5)) + " " + String(stamp[1].prefix(5))
    return ([short] + parts.dropFirst()).joined(separator: " · ")
  }
}

/// "▶ Replaying" in the fixed slot while a playback drives the game.
struct ReplayingBadge: View {
  let on: Bool
  var body: some View {
    HStack(spacing: 5) {
      Image(systemName: "play.fill").font(.caption).foregroundStyle(Color.accentColor)
      Text("Replaying").font(.callout).foregroundStyle(Color.accentColor)
    }
    .frame(width: RecordingBadge.width, alignment: .trailing)
    .opacity(on ? 1 : 0)
    .accessibilityHidden(!on)
    .help("A recording is playing back. Esc hands the game back to you.")
  }
}

/// Resolution and Frame rate (one section), Skip intro movies (its own section), then Turbo speed, Slow speed and
/// Turbo audio.
struct FrontPageOptions: View {
  @ObservedObject var model: SettingsModel
  var body: some View {
    Section {
      FixedPicker("Resolution", selection: $model.settings.resolution) {
        ForEach(InternalResolution.allCases) { scale in
          Text(scale.title(frameRate: model.settings.frameRate, rule2x: model.settings.auto2x)).tag(scale)
        }
      }
      .help(ResolutionHelp.resolution)
      FixedPicker("Frame rate", selection: $model.settings.frameRate) {
        ForEach(FrameRate.allCases) { fps in
          Text(fps.title(resolution: model.settings.resolution, rule2x: model.settings.auto2x)).tag(fps)
        }
      }
      .help(ResolutionHelp.frameRate)
    }
    Section {
      Toggle("Skip intro movies", isOn: model.binding(\.skipIntro))
        .help("No logo movies; shorter title fade.")
    }
    Section {
      TurboControls(model: model)
    }
  }
}

/// Turbo speed (slider and stepper), Slow speed (2026-10-21) and Turbo audio: on the front page since 2026-10-19
/// (were in Settings > General; Turbo notice stays there).
struct TurboControls: View {
  @ObservedObject var model: SettingsModel
  var body: some View {
    let speed = Binding(get: { model.settings.turboSpeed }, set: { model.settings.turboSpeed = TurboSpeed.clean($0) })
    LabeledContent("Turbo speed") {
      HStack {
        Slider(value: speed, in: TurboSpeed.range, step: 0.1)
        Stepper(TurboSpeed.label(model.settings.turboSpeed), value: speed, in: TurboSpeed.range, step: 0.1)
          .monospacedDigit()
      }
    }
    .help("Game speed while turbo is on. F7 or 7 toggles turbo; a controller button can too (Controls). Off: no turbo.")
    let slow = Binding(get: { model.settings.slowSpeed }, set: { model.settings.slowSpeed = SlowSpeed.clean($0) })
    LabeledContent("Slow speed") {
      HStack {
        Slider(value: slow, in: SlowSpeed.range, step: 0.05)
        Stepper(SlowSpeed.label(model.settings.slowSpeed), value: slow, in: SlowSpeed.range, step: 0.05)
          .monospacedDigit()
      }
    }
    .help("F8 toggles slow motion; a controller button can too (Controls). Off: no slow motion.")
    FixedPicker("Turbo audio", selection: $model.settings.turboAudio) {
      ForEach(TurboAudio.allCases) { mode in Text(mode.title).tag(mode) }
    }
    .help("Sound while turbo or slow motion is on.")
  }
}

/// The one button that switches pages, in the same spot on both: "Settings" on the front page, "Launcher" on
/// Settings (Esc).
struct PageButton: View {
  @ObservedObject var model: SettingsModel
  var body: some View {
    let settings = model.page == .settings
    Button { model.toggleSettingsPage() } label: {
      Label(settings ? "Launcher" : "Settings", systemImage: settings ? "play.rectangle" : "gearshape")
    }
    .keyboardShortcut(settings ? .cancelAction : nil)
    .help(settings ? "Back to the launcher (Esc)" : "⌘,")
  }
}

/// The bottom bar of both pages: the page button at the bottom-left, then the page's own items. Same height and
/// padding on both pages, so the button does not move when the page switches.
struct PageBar<Trailing: View>: View {
  @ObservedObject var model: SettingsModel
  let trailing: Trailing
  static var height: CGFloat { 28 }
  init(model: SettingsModel, @ViewBuilder trailing: () -> Trailing) {
    self.model = model
    self.trailing = trailing()
  }
  var body: some View {
    VStack(spacing: 0) {
      Divider()
      HStack(spacing: 12) {
        PageButton(model: model)
        trailing
      }
      .frame(minHeight: Self.height)
      .padding(.horizontal, 20)
      .padding(.vertical, 12)
    }
  }
}

/// Settings tabs: left-aligned, every tab the same fixed width. Dev Options appends Performance, Fixes and Diagnostics
/// at the right; the everyday tabs never move or resize (maintainer, 2026-10-19; the segmented control rescaled its
/// segments and once kept the 7-tab widths after going back to 4, overflowing the window).
struct SettingsTabBar: View {
  @ObservedObject var model: SettingsModel
  static let tabWidth: CGFloat = 82
  static let tabHeight: CGFloat = 24

  var body: some View {
    HStack(spacing: 0) {
      ForEach(model.visibleTabs) { tab in
        let selected = tab == model.settingsTab
        Button { model.settingsTab = tab } label: {
          Text(tab.title)
            .font(.system(size: 12, weight: selected ? .semibold : .regular))
            .lineLimit(1)
            .foregroundStyle(selected ? Color.white : Color.primary)
            .frame(width: Self.tabWidth, height: Self.tabHeight)
            .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Color.accentColor : Color.clear))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? .isSelected : [])
      }
    }
    .padding(2)
    .background(RoundedRectangle(cornerRadius: 8).fill(Color.primary.opacity(0.07)))
    .frame(maxWidth: .infinity, alignment: .leading)
  }
}

/// The Settings page: tab bar, the tab (scrolls when taller than the page), then the page bar (page button, Dev
/// Options).
struct SettingsPage: View {
  @ObservedObject var model: SettingsModel
  var variant: SettingsVariant { model.developerMode ? .dev : .player }
  /// Role-specific sections appended to a tab.
  var extra: (SettingsTab) -> AnyView = { _ in AnyView(EmptyView()) }

  var body: some View {
    VStack(spacing: 0) {
      SettingsTabBar(model: model)
        .padding(.horizontal, 20)
        .padding(.top, 12)
        .padding(.bottom, 2)
      SettingsTabView(model: model, tab: model.settingsTab, variant: variant, extra: extra)
        .id(model.settingsTab)
      PageBar(model: model) {
        Spacer()
        Toggle("Dev Options", isOn: $model.developerMode)
          .toggleStyle(.switch)
          .controlSize(.small)
          .help("Performance, Fixes and Diagnostics tabs, and developer tools.")
      }
    }
  }
}

/// ⌘, ("Settings…" in the app menu): shows the Settings page in the launcher window and brings it to the front.
struct SettingsPageCommand: Commands {
  let model: SettingsModel
  var windowTitles: Set<String>
  var body: some Commands {
    CommandGroup(replacing: .appSettings) {
      Button("Settings…") {
        model.page = .settings
        if let window = NSApp.windows.first(where: { windowTitles.contains($0.title) }) {
          if window.isMiniaturized { window.deminiaturize(nil) }
          window.makeKeyAndOrderFront(nil)
        }
      }
      .keyboardShortcut(",")
    }
  }
}
