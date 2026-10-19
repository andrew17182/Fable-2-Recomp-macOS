// Hands activation (front + keyboard focus) to a game a launcher started, for both roles' Play buttons.
//
// macOS 14+ cooperative activation: an app started as a child process is not activated for it, SDL 3 no longer
// activates itself at launch on 14+ (SDL_cocoaevents.m), and an activate call from an app that is not active is
// refused. Only the active app (the launcher, just clicked) can pass activation on, and only to an app that already
// exists. The game shares the launcher's bundle id, so a yield by bundle id right after process.run() went nowhere
// and the game came up in the background. So: once the game's NSRunningApplication appears (KVO on
// NSWorkspace.runningApplications and on its isFinishedLaunching; no fixed wait, the timeout is only a guard), the
// launcher yields to it and activates it as if from the launcher. Callers skip this in background test mode
// (FABLE2_TEST_BACKGROUND=1).
import AppKit

final class GameActivation: NSObject {
  private static var pending: [GameActivation] = []
  private let pid: pid_t
  private let done: (String) -> Void
  private var appsObservation: NSKeyValueObservation?
  private var launchObservation: NSKeyValueObservation?
  /// Held while observed: a KVO observation must be invalidated before its object goes away.
  private var app: NSRunningApplication?
  private var activated = false

  /// `done` gets one log line (main thread): the outcome, or why nothing was activated.
  static func handOff(to pid: pid_t, timeout: TimeInterval = 120, done: @escaping (String) -> Void = { _ in }) {
    let item = GameActivation(pid: pid, done: done)
    pending.append(item)
    item.start(timeout: timeout)
  }

  private init(pid: pid_t, done: @escaping (String) -> Void) {
    self.pid = pid
    self.done = done
  }

  private func start(timeout: TimeInterval) {
    appsObservation = NSWorkspace.shared.observe(\.runningApplications, options: [.initial]) { [weak self] _, _ in
      DispatchQueue.main.async { self?.check() }
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + timeout) { [weak self] in
      self?.finish("game activation: pid \(self?.pid ?? 0) not activated (timed out)")
    }
  }

  private func check() {
    guard launchObservation == nil,
          let app = NSWorkspace.shared.runningApplications.first(where: { $0.processIdentifier == pid }) else { return }
    appsObservation?.invalidate()
    appsObservation = nil
    self.app = app
    activate(app)  // as soon as it exists; again once it has finished launching (an earlier one may be dropped)
    launchObservation = app.observe(\.isFinishedLaunching, options: [.initial]) { [weak self] app, _ in
      DispatchQueue.main.async {
        guard let self = self, app.isFinishedLaunching else { return }
        self.activate(app)
        self.finish("game activation: pid \(self.pid) " + (app.isActive || self.activated ? "activated" : "refused"))
      }
    }
  }

  private func activate(_ app: NSRunningApplication) {
    guard !app.isTerminated else { return }
    NSApp.yieldActivation(to: app)
    if app.activate(from: NSRunningApplication.current, options: []) { activated = true }
  }

  private func finish(_ line: String) {
    guard let index = GameActivation.pending.firstIndex(where: { $0 === self }) else { return }
    appsObservation?.invalidate()
    appsObservation = nil
    launchObservation?.invalidate()
    launchObservation = nil
    app = nil
    GameActivation.pending.remove(at: index)
    done(line)
  }
}
