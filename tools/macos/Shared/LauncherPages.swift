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
  static let height: CGFloat = 560
  /// Width of every pop-up menu in a form row (the label left, the menu at the right edge).
  static let pickerWidth: CGFloat = 270
}

/// A form row: label, then a pop-up menu of fixed width at the right edge (a selection never resizes it).
struct FixedPicker<Value: Hashable, Content: View>: View {
  let title: String
  @Binding var selection: Value
  let content: Content
  init(_ title: String, selection: Binding<Value>, @ViewBuilder content: () -> Content) {
    self.title = title
    _selection = selection
    self.content = content()
  }
  var body: some View {
    LabeledContent(title) {
      Picker(title, selection: $selection) { content }
        .labelsHidden()
        .frame(width: LauncherLayout.pickerWidth, alignment: .trailing)
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
      }
      Picker("Save", selection: $selection) {
        if hasSaves {
          ForEach(choices) { Text($0.label).tag($0.id) }
        } else {
          Text("No saves").tag(SaveChoice.titleID)
        }
      }
      .labelsHidden()
      .frame(width: Self.menuWidth, alignment: .leading)  // fixed: a long save label truncates
      .disabled(!hasSaves)
      .help("Newest save first.")
    }
  }
}

/// Resolution and Frame rate (one section), Skip intro movies (its own section), then Turbo speed and Turbo audio.
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

/// Turbo speed (slider and stepper) and Turbo audio: on the front page since 2026-10-19 (were in Settings >
/// General; Turbo notice stays there).
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
    .help("Game speed while turbo is on. F7 or 7 toggles turbo; a controller button can too (Controls).")
    FixedPicker("Turbo audio", selection: $model.settings.turboAudio) {
      ForEach(TurboAudio.allCases) { mode in Text(mode.title).tag(mode) }
    }
    .help("Sound while turbo is on.")
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
