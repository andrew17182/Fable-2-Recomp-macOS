// The app's command line (both roles of the one Fable II app: player and Dev Launcher, docs/player-app.md
// "Developer mode"). A global outside main.swift, so it is initialized on first use from any file.
import Foundation

let commandLine = Array(CommandLine.arguments.dropFirst())

/// The value after a `--flag value` pair, or nil.
func argument(after name: String) -> String? {
  commandLine.firstIndex(of: name).flatMap { $0 + 1 < commandLine.count ? commandLine[$0 + 1] : nil }
}
