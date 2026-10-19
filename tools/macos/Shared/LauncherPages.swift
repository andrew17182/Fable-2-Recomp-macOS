// The launcher window's pages, shared by both roles (2026-10-19): the front page parts (save menu with Load Save,
// resolution and frame rate, Skip intro) and the Settings page inside the same window. One button toggles the two
// pages ("Settings" / "Launcher"); ⌘, opens Settings. Settings tabs: General, Display, Controls, Mods; Performance,
// Fixes and Diagnostics only with Dev Options on (SettingsModel.developerMode).
import AppKit
import SwiftUI

/// Short tooltips shared by the front page and the Display tab.
enum ResolutionHelp {
  static let resolution = "The size the game renders at. Auto picks it for this Mac."
  static let frameRate = "Auto picks the frame rate for this Mac. The original game runs at 30 FPS."
}

/// One entry of the save menu: "title" (Title Screen) or a save id, with its short label.
struct SaveChoice: Identifiable, Hashable {
  let id: String
  let label: String
  static let titleID = "title"
  static let title = SaveChoice(id: titleID, label: "Title Screen")
}

/// Save menu and Load Save (the default button). Entries newest first, then Title Screen.
struct SaveMenu: View {
  let choices: [SaveChoice]
  @Binding var selection: String
  let busy: Bool
  let disabled: Bool
  let load: () -> Void

  var body: some View {
    HStack(spacing: 10) {
      Picker("Save", selection: $selection) {
        ForEach(choices.filter { $0.id != SaveChoice.titleID }) { Text($0.label).tag($0.id) }
        if choices.count > 1 { Divider() }
        Text(SaveChoice.title.label).tag(SaveChoice.titleID)
      }
      .labelsHidden()
      .frame(maxWidth: .infinity, alignment: .leading)
      .help("Newest save first. Title Screen: start at the title (New Game).")
      Button(action: load) {
        Text(busy ? "Working…" : "Load Save").fontWeight(.semibold).frame(minWidth: 90)
      }
      .buttonStyle(.borderedProminent)  // the accent-coloured default button (Return), every role
      .tint(.accentColor)
      .controlSize(.large)
      .keyboardShortcut(.defaultAction)
      .disabled(disabled)
      .help(selection == SaveChoice.titleID ? "Start at the title screen. ↩" : "Load this save. ↩")
    }
  }
}

/// Resolution and Frame rate (one section), then Skip intro movies (its own section).
struct FrontPageOptions: View {
  @ObservedObject var model: SettingsModel
  var body: some View {
    Section {
      Picker("Resolution", selection: $model.settings.resolution) {
        ForEach(InternalResolution.allCases) { scale in
          Text(scale.title(frameRate: model.settings.frameRate, rule2x: model.settings.auto2x)).tag(scale)
        }
      }
      .help(ResolutionHelp.resolution)
      Picker("Frame rate", selection: $model.settings.frameRate) {
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
  }
}

/// The button that switches pages: "Settings" on the front page, "Launcher" on Settings.
struct PageButton: View {
  @ObservedObject var model: SettingsModel
  var body: some View {
    if model.page == .launcher {
      Button { model.page = .settings } label: { Label("Settings", systemImage: "gearshape") }
        .help("⌘,")
    } else {
      Button { model.page = .launcher } label: { Label("Launcher", systemImage: "play.rectangle") }
        .keyboardShortcut(.cancelAction)
        .help("Back to the launcher (Esc)")
    }
  }
}

/// The Settings page: tab bar, the tab, then Dev Options and the page button.
struct SettingsPage: View {
  @ObservedObject var model: SettingsModel
  var variant: SettingsVariant { model.developerMode ? .dev : .player }
  /// Role-specific sections appended to a tab.
  var extra: (SettingsTab) -> AnyView = { _ in AnyView(EmptyView()) }

  var body: some View {
    VStack(spacing: 0) {
      Picker("Settings", selection: $model.settingsTab) {
        ForEach(model.visibleTabs) { Text($0.title).tag($0) }
      }
      .pickerStyle(.segmented)
      .labelsHidden()
      // Seven tabs (Dev Options) fill the width at the small size; four keep their natural width.
      .controlSize(model.developerMode ? .small : .regular)
      .fixedSize(horizontal: !model.developerMode, vertical: true)
      .padding(.horizontal, 20)
      .padding(.top, 12)
      .padding(.bottom, 2)
      SettingsTabView(model: model, tab: model.settingsTab, variant: variant, extra: extra)
        .id(model.settingsTab)
      Divider()
      HStack {
        Toggle("Dev Options", isOn: $model.developerMode)
          .toggleStyle(.switch)
          .controlSize(.small)
          .help("Performance, Fixes and Diagnostics tabs, and developer tools.")
        Spacer()
        PageButton(model: model)
      }
      .padding(.horizontal, 20)
      .padding(.vertical, 12)
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
