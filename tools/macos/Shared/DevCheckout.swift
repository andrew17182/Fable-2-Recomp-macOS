// Developer mode and the source checkout it works with (docs/player-app.md "Developer mode"). The one Fable II app
// has two roles: the player (release/portable app) and the Dev Launcher (tools/macos/build_dev_launcher.sh). In the
// player role, Settings > Advanced > Developer mode shows everything the Dev Launcher has; features that need the
// repository (dev sessions, playtest/latest builds, kits, exports, out/ folders) work once a source checkout is known.
import AppKit
import SwiftUI

enum AppRole {
  /// The Dev Launcher role (Info.plist Fable2DevLauncher, set by tools/macos/build_dev_launcher.sh).
  static let devLauncher = Bundle.main.object(forInfoDictionaryKey: "Fable2DevLauncher") as? Bool ?? false
  /// Developer mode's default when nothing is stored (the dev build: on).
  static let developerModeDefault = devLauncher
    || (Bundle.main.object(forInfoDictionaryKey: "Fable2DeveloperModeDefault") as? Bool ?? false)
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

/// Settings > Advanced (player role) and Settings > Diagnostics (Dev Launcher role): developer mode and the checkout.
struct DeveloperSection: View {
  @Binding var developerMode: Bool
  /// The Dev Launcher role is always in developer mode.
  var locked = false
  @State private var refresh = 0
  @State private var error = ""

  var body: some View {
    Section {
      Toggle(isOn: locked ? .constant(true) : $developerMode) {
        VStack(alignment: .leading, spacing: 2) {
          Text("Developer mode")
          Text(locked ? "This is the Fable II Launcher build: developer mode is always on."
               : "Shows everything the Fable II Launcher has: Window > Fable II Launcher (save picker, dev sessions, "
                 + "playtest and latest builds) and Window > Developer (benchmark, flash kit, exports, folders).")
            .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
      }
      .disabled(locked)
      if developerMode || locked {
        LabeledContent("Source checkout") {
          VStack(alignment: .trailing, spacing: 2) {
            Text(DevCheckout.path ?? "none").font(.caption).lineLimit(1).truncationMode(.middle).textSelection(.enabled)
            Text(DevCheckout.source).font(.caption2).foregroundStyle(.secondary)
          }
          .id(refresh)
        }
        HStack {
          if DevCheckout.path == nil {
            Text("Features that use the repository need a source checkout.").font(.caption).foregroundStyle(.secondary)
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
      Text("The launcher's dev sessions, save library, playtest and latest builds, benchmark, flash kit and exports "
           + "run the tools in a Fable II Recomp source checkout. Choose yours (the folder with CMakeLists.txt).")
        .fixedSize(horizontal: false, vertical: true)
      HStack {
        Spacer()
        Button("Source Checkout…") { onChoose() }.buttonStyle(.borderedProminent)
      }
    }
    .padding(20)
    .frame(width: 520)
  }
}
