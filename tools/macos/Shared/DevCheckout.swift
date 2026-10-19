// Dev Options and the source checkout it works with (docs/player-app.md "Developer mode"). The one Fable II app
// has two roles: the player (release/portable app) and the Dev Launcher (tools/macos/build_dev_launcher.sh). Dev
// Options (Settings, SettingsModel.developerMode, off by default in both roles since 2026-10-19) shows the dev tabs
// and tools; features that need the repository (dev sessions, playtest/latest builds, kits, exports, out/ folders)
// work once a source checkout is known.
import AppKit
import SwiftUI

enum AppRole {
  /// The Dev Launcher role (Info.plist Fable2DevLauncher, set by tools/macos/build_dev_launcher.sh).
  static let devLauncher = Bundle.main.object(forInfoDictionaryKey: "Fable2DevLauncher") as? Bool ?? false
}

enum DevCheckout {
  /// Where a chosen checkout is remembered: the player's settings file in the player role, UserDefaults in the Dev
  /// Launcher role (set at start).
  static var store: SettingsStore = UserDefaults.standard
  static let key = "sourceCheckout"

  static func isCheckout(_ path: String) -> Bool {
    let fm = FileManager.default
    return fm.fileExists(atPath: path + "/tools/fable2_dev_session.py") && fm.fileExists(atPath: path + "/CMakeLists.txt")
  }

  /// The checkout: FABLE2_REPO_ROOT, the build's Fable2RepoRoot, the one chosen in Settings, else the first folder
  /// above this app that is a checkout (an app built inside or next to the repo). nil: none known.
  static var path: String? {
    if let env = ProcessInfo.processInfo.environment["FABLE2_REPO_ROOT"], !env.isEmpty { return env }
    if let built = Bundle.main.object(forInfoDictionaryKey: "Fable2RepoRoot") as? String, !built.isEmpty { return built }
    if let chosen = store.string(forKey: key), !chosen.isEmpty { return chosen }
    return detected
  }

  /// How `path` was found (shown in Settings).
  static var source: String {
    if let env = ProcessInfo.processInfo.environment["FABLE2_REPO_ROOT"], !env.isEmpty { return "FABLE2_REPO_ROOT" }
    if Bundle.main.object(forInfoDictionaryKey: "Fable2RepoRoot") as? String != nil { return "set by the build" }
    if let chosen = store.string(forKey: key), !chosen.isEmpty { return "chosen" }
    return detected != nil ? "found next to this app" : "none"
  }

  static var detected: String? {
    var url = Bundle.main.bundleURL.deletingLastPathComponent()
    for _ in 0..<6 {
      for candidate in [url.path, url.appendingPathComponent("Fable-2-Recomp-macOS").path] where isCheckout(candidate) {
        return candidate
      }
      url = url.deletingLastPathComponent()
    }
    return nil
  }

  /// Asks for a checkout folder; returns an error text, or nil when one was stored (or the panel was cancelled).
  static func choose() -> String? {
    let panel = NSOpenPanel()
    panel.title = "Choose the Fable II Recomp source checkout"
    panel.message = "The folder with CMakeLists.txt and tools/fable2_dev_session.py."
    panel.canChooseFiles = false
    panel.canChooseDirectories = true
    guard panel.runModal() == .OK, let url = panel.url else { return nil }
    guard isCheckout(url.path) else { return "\(url.path) is not a Fable II Recomp checkout (no tools/fable2_dev_session.py)." }
    store.set(url.path, forKey: key)
    return nil
  }

  static func clear() { store.set(nil, forKey: key) }
}

/// Settings > Diagnostics (Dev Options on, both roles): the source checkout, the front-page dev buttons and the
/// starred saves in the save menu.
struct DeveloperSection: View {
  @ObservedObject var model: SettingsModel
  @State private var refresh = 0
  @State private var error = ""

  var body: some View {
    Section {
      Toggle("Show Update Playtest Build", isOn: $model.showUpdatePlaytest)
        .help("Adds the button to the launcher page. It copies the latest build to out/playtest.")
      Toggle("Show starred saves in save menu", isOn: $model.showStarredSaves)
        .help("Adds starred library saves and the newest library save to the save menu.")
      Group {
        LabeledContent("Source checkout") {
          VStack(alignment: .trailing, spacing: 2) {
            Text(DevCheckout.path ?? "none").font(.caption).lineLimit(1).truncationMode(.middle).textSelection(.enabled)
            Text(DevCheckout.source).font(.caption2).foregroundStyle(.secondary)
          }
          .id(refresh)
        }
        HStack {
          if DevCheckout.path == nil {
            Text("No source checkout").font(.caption).foregroundStyle(.secondary)
          }
          Spacer()
          Button("Source Checkout…") { error = DevCheckout.choose() ?? ""; refresh += 1 }
          if DevCheckout.source == "chosen" { Button("Forget") { DevCheckout.clear(); refresh += 1 } }
        }
        if !error.isEmpty { Text(error).font(.caption).foregroundStyle(.red) }
      }
    } header: { Text("Developer") }
  }
}

/// A short note in place of a repository feature when no checkout is known.
struct NeedsCheckoutView: View {
  var onChoose: () -> Void
  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      Text("Needs a source checkout").font(.title3.weight(.semibold))
      Text("Choose the Fable II Recomp source folder.")
        .help("The folder with CMakeLists.txt. Dev sessions, the save library, builds and kits use it.")
      HStack {
        Spacer()
        Button("Source Checkout…") { onChoose() }.buttonStyle(.borderedProminent)
      }
    }
    .padding(20)
    .frame(width: 520)
  }
}
