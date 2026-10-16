// Controls tab of Settings, shared by the Dev Launcher and the player app (registered in LaunchSettings.swift:
// SettingsTab.controls, LaunchSettings.controls; shown by SettingsViews.swift). Everything for the tab lives
// here so the shared files only register it.
//
// - Keyboard -> gamepad: the game's keyboard_gamepad_map cvar (src/input/keyboard_gamepad.h), passed as
//   REX_KEYBOARD_GAMEPAD_MAP (the SDK's REX_<CVAR> environment route, which beats fable_2.toml and the
//   fable2_config.toml [input] seed; only the command line and the console beat it). Key names are the
//   SDK's rex::ui::ParseVirtualKey names (tests/controls_tab_tests.py checks the list below against them).
// - Mouse look: the mouse_look / mouse_look_scale cvars (REX_MOUSE_LOOK, REX_MOUSE_LOOK_SCALE).
// - Hotkeys: the SDK overlay binds are cvars (bind_console, bind_settings, bind_debug_overlay,
//   bind_achievements; recomp defaults in src/input/recomp_hotkeys.h), so they are rebindable here
//   (REX_BIND_*; the settings key is also mouse_unlock_key). F5/F6/F7/F9 and 5/6/7 are fixed in
//   src/input/window_keyboard_mouse.h and only listed.
// - Controllers: macOS remaps controller buttons system-wide (System Settings > Game Controllers); this
//   tab links there and lists connected controllers (GameController framework). View-button switch:
//   FABLE2_VIEW_BUTTON_TO_GAME (src/input/controller_hotkeys.h).
import AppKit
import GameController
import SwiftUI

// MARK: - Key names and guest inputs

enum KeyNames {
  // BEGIN sdk-key-names (rex::ui::ParseVirtualKey: kKeyNames in the SDK's src/ui/keybinds.cpp; exact case)
  static let all: [String] = [
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Backtick", "Minus", "Plus", "Comma", "Period", "Semicolon", "Slash", "Backslash", "LBracket", "RBracket",
    "Quote",
    "Escape", "Return", "Space", "Tab", "Backspace", "Delete", "Insert", "Home", "End", "PageUp", "PageDown",
    "Left", "Right", "Up", "Down",
    "Shift", "Control", "Alt",
    "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9",
    "NumpadEnter", "NumpadPlus", "NumpadMinus", "NumpadStar", "NumpadSlash",
    "PrintScreen", "Pause", "CapsLock", "NumLock", "ScrollLock",
    "LMB", "RMB", "MMB",
  ]
  // END sdk-key-names
  static let valid = Set(all)

  // BEGIN mac-keycodes (NSEvent.keyCode, kVK_* -> SDK name; the game sees the same key through SDL's
  // scancode -> rex::ui::TranslateSDLScancode, so keypad Enter is Return, Help is Insert, Clear is NumLock)
  static let byKeyCode: [UInt16: String] = [
    0: "A", 1: "S", 2: "D", 3: "F", 4: "H", 5: "G", 6: "Z", 7: "X", 8: "C", 9: "V", 11: "B", 12: "Q", 13: "W",
    14: "E", 15: "R", 16: "Y", 17: "T", 18: "1", 19: "2", 20: "3", 21: "4", 22: "6", 23: "5", 24: "Plus",
    25: "9", 26: "7", 27: "Minus", 28: "8", 29: "0", 30: "RBracket", 31: "O", 32: "U", 33: "LBracket", 34: "I",
    35: "P", 36: "Return", 37: "L", 38: "J", 39: "Quote", 40: "K", 41: "Semicolon", 42: "Backslash", 43: "Comma",
    44: "Slash", 45: "N", 46: "M", 47: "Period", 48: "Tab", 49: "Space", 50: "Backtick", 51: "Backspace",
    53: "Escape", 56: "Shift", 57: "CapsLock", 58: "Alt", 59: "Control", 60: "Shift", 61: "Alt", 62: "Control",
    64: "F17", 67: "NumpadStar", 69: "NumpadPlus", 71: "NumLock", 75: "NumpadSlash", 76: "Return",
    78: "NumpadMinus", 79: "F18", 80: "F19", 82: "Numpad0", 83: "Numpad1", 84: "Numpad2", 85: "Numpad3",
    86: "Numpad4", 87: "Numpad5", 88: "Numpad6", 89: "Numpad7", 90: "F20", 91: "Numpad8", 92: "Numpad9",
    96: "F5", 97: "F6", 98: "F7", 99: "F3", 100: "F8", 101: "F9", 103: "F11", 105: "F13", 106: "F16",
    107: "F14", 109: "F10", 111: "F12", 113: "F15", 114: "Insert", 115: "Home", 116: "PageUp", 117: "Delete",
    118: "F4", 119: "End", 120: "F2", 121: "PageDown", 122: "F1", 123: "Left", 124: "Right", 125: "Down",
    126: "Up",
  ]
  // END mac-keycodes

  /// How a key name is shown (the stored name is always the SDK's).
  static func display(_ name: String) -> String {
    switch name {
    case "Backtick": return "` (Backtick)"
    case "Minus": return "-"
    case "Plus": return "= (Plus)"
    case "Comma": return ","
    case "Period": return "."
    case "Semicolon": return ";"
    case "Slash": return "/"
    case "Backslash": return "\\"
    case "LBracket": return "["
    case "RBracket": return "]"
    case "Quote": return "'"
    case "Up": return "↑"
    case "Down": return "↓"
    case "Left": return "←"
    case "Right": return "→"
    case "Alt": return "Option"
    case "PageUp": return "Page Up"
    case "PageDown": return "Page Down"
    case "CapsLock": return "Caps Lock"
    case "LMB": return "Left Click"
    case "RMB": return "Right Click"
    case "MMB": return "Middle Click"
    case "None": return "None"
    default:
      if name.hasPrefix("Numpad") && name.count > 6 { return "Keypad " + name.dropFirst(6) }
      return name
    }
  }

  /// The "Other key" menu: every SDK name, grouped (NumpadEnter is the same key as Return in the game).
  static let menuGroups: [(title: String, names: [String])] = [
    ("Letters", all.filter { $0.count == 1 && $0.first!.isLetter }),
    ("Digits", all.filter { $0.count == 1 && $0.first!.isNumber }),
    ("Function Keys", all.filter { $0.count > 1 && $0.first == "F" && $0.dropFirst().allSatisfy(\.isNumber) }),
    ("Arrows and Editing", ["Up", "Down", "Left", "Right", "Space", "Return", "Tab", "Escape", "Backspace",
                            "Delete", "Insert", "Home", "End", "PageUp", "PageDown"]),
    ("Symbols", ["Backtick", "Minus", "Plus", "LBracket", "RBracket", "Backslash", "Semicolon", "Quote", "Comma",
                 "Period", "Slash"]),
    ("Modifiers and Locks", ["Shift", "Control", "Alt", "CapsLock", "NumLock", "ScrollLock", "PrintScreen", "Pause"]),
    ("Keypad", all.filter { $0.hasPrefix("Numpad") && $0 != "NumpadEnter" }),
    ("Mouse Buttons", ["LMB", "RMB", "MMB"]),
  ]
}

/// One guest gamepad input: its name in the map (what FillGuestTarget in src/input/keyboard_gamepad.h accepts)
/// and how the table shows it.
struct GuestInput: Identifiable {
  let name: String
  let title: String
  var id: String { name }
}

enum KeyboardMap {
  // BEGIN guest-inputs (canonical target names, table order)
  static let inputs: [GuestInput] = [
    GuestInput(name: "A", title: "A"),
    GuestInput(name: "B", title: "B"),
    GuestInput(name: "X", title: "X"),
    GuestInput(name: "Y", title: "Y"),
    GuestInput(name: "LB", title: "Left bumper (LB)"),
    GuestInput(name: "RB", title: "Right bumper (RB)"),
    GuestInput(name: "LT", title: "Left trigger (LT)"),
    GuestInput(name: "RT", title: "Right trigger (RT)"),
    GuestInput(name: "Up", title: "D-pad up"),
    GuestInput(name: "Down", title: "D-pad down"),
    GuestInput(name: "Left", title: "D-pad left"),
    GuestInput(name: "Right", title: "D-pad right"),
    GuestInput(name: "Pause", title: "Start (pause menu)"),
    GuestInput(name: "Select", title: "Back (Select)"),
    GuestInput(name: "L3", title: "Left stick click (L3)"),
    GuestInput(name: "R3", title: "Right stick click (R3)"),
    GuestInput(name: "StickUp", title: "Left stick up (forward)"),
    GuestInput(name: "StickDown", title: "Left stick down (back)"),
    GuestInput(name: "StickLeft", title: "Left stick left"),
    GuestInput(name: "StickRight", title: "Left stick right"),
  ]
  // END guest-inputs

  /// Other spellings the game accepts (case-insensitive) -> canonical name.
  static let aliases: [String: String] = [
    "LSHOULDER": "LB", "RSHOULDER": "RB", "LTRIGGER": "LT", "RTRIGGER": "RT", "DUP": "Up", "DDOWN": "Down",
    "DLEFT": "Left", "DRIGHT": "Right", "START": "Pause", "BACK": "Select", "VIEW": "Select", "LTHUMB": "L3",
    "RTHUMB": "R3", "LU": "StickUp", "LD": "StickDown", "LL": "StickLeft", "LR": "StickRight",
  ]

  /// The game's built-in layout (fable2::config::kDefaultKeyboardGamepadMap, src/core/fable2_config.h).
  static let builtInDefault =
    "E:A,X:B,Z:X,C:Y,W:StickUp,S:StickDown,A:StickLeft,D:StickRight,Escape:Pause,M:Select,Q:LT,Tab:RT,"
    + "Up:Up,Down:Down,Left:Left,Right:Right"

  static func canonicalTarget(_ name: String) -> String? {
    let u = name.trimmingCharacters(in: .whitespaces).uppercased()
    if let input = inputs.first(where: { $0.name.uppercased() == u }) { return input.name }
    return aliases[u]
  }

  /// Bindings per input (table order kept by `serialize`) and the entries the game would skip.
  static func parse(_ text: String) -> (bindings: [String: [String]], invalid: [String]) {
    var bindings: [String: [String]] = [:]
    var invalid: [String] = []
    for entry in text.split(separator: ",").map(String.init) where !entry.isEmpty {
      guard let colon = entry.firstIndex(of: ":") else { invalid.append(entry); continue }
      let key = String(entry[..<colon])
      guard KeyNames.valid.contains(key), let target = canonicalTarget(String(entry[entry.index(after: colon)...]))
      else { invalid.append(entry); continue }
      if !(bindings[target] ?? []).contains(key) { bindings[target, default: []].append(key) }
    }
    return (bindings, invalid)
  }

  /// "Key:Input,..." in table order.
  static func serialize(_ bindings: [String: [String]]) -> String {
    inputs.flatMap { input in (bindings[input.name] ?? []).map { "\($0):\(input.name)" } }.joined(separator: ",")
  }

  /// The built-in layout in table order (the stored default).
  static let defaultMap = serialize(parse(builtInDefault).bindings)

  /// Inputs other than `input` that have `key`.
  static func owners(of key: String, except input: String, in bindings: [String: [String]]) -> [String] {
    inputs.map(\.name).filter { $0 != input && (bindings[$0] ?? []).contains(key) }
  }

  /// Binds `key` to `input` (added to its keys, or replacing them). Other inputs that had the key lose it;
  /// with `swap` they get the input's previous keys instead (a replace that trades places).
  static func bind(_ key: String, to input: String, add: Bool, swap: Bool,
                   in bindings: [String: [String]]) -> [String: [String]] {
    var b = bindings
    let previous = b[input] ?? []
    for owner in owners(of: key, except: input, in: bindings) {
      b[owner] = (b[owner] ?? []).filter { $0 != key }
      if swap && !add { b[owner, default: []].append(contentsOf: previous.filter { !(b[owner] ?? []).contains($0) }) }
    }
    if add { if !previous.contains(key) { b[input, default: []].append(key) } } else { b[input] = [key] }
    return b
  }
}

// MARK: - Hotkeys

/// A built-in hotkey. `cvar` set: an SDK overlay bind, rebindable (REX_<CVAR>); nil: fixed in code.
struct Hotkey: Identifiable {
  let title: String
  let keys: String
  let detail: String
  var cvar: String? = nil
  var id: String { cvar ?? title }
}

enum Hotkeys {
  /// Rebindable SDK overlay binds and their recomp defaults (src/input/recomp_hotkeys.h; SDK names).
  static let rebindable: [Hotkey] = [
    Hotkey(title: "Console", keys: "Backtick", detail: "The SDK console (cvars, log).", cvar: "bind_console"),
    Hotkey(title: "Settings menu", keys: "4",
           detail: "The SDK settings overlay. Also frees the mouse while mouse look is on.", cvar: "bind_settings"),
    Hotkey(title: "Debug overlay", keys: "3", detail: "Frame-time and GPU stats.", cvar: "bind_debug_overlay"),
    Hotkey(title: "Achievements", keys: "8", detail: "The achievements overlay.", cvar: "bind_achievements"),
  ]
  /// Fixed hotkeys (src/input/window_keyboard_mouse.h, src/input/controller_hotkeys.h, SDL's window menu).
  static let fixed: [Hotkey] = [
    Hotkey(title: "Quick save", keys: "F5 or 5", detail: "Same as Save and Continue in the pause menu."),
    Hotkey(title: "Lua script menu", keys: "F6 or 6", detail: "Opens the script selector (or runs f6_lua_path)."),
    Hotkey(title: "Turbo on/off", keys: "F7 or 7", detail: "The whole game runs faster (General > Turbo speed, Turbo audio, Turbo notice)."),
    Hotkey(title: "FPS counter", keys: "F9", detail: "Shows or hides the frame-rate counter."),
    Hotkey(title: "Full screen on/off", keys: "⌃⌘F", detail: "The window's full-screen Space (or the green button)."),
    Hotkey(title: "Quit", keys: "⌘Q", detail: "Closes the game (it does not save)."),
    Hotkey(title: "Controller: quick save", keys: "Share", detail: "Xbox Series controllers."),
    Hotkey(title: "Controller: screenshot", keys: "View", detail: "Saves a PNG in the Screenshots folder."),
  ]
  /// Keys the game also uses as fixed hotkeys (number keys only while no overlay is open).
  static let fixedKeys: [String: String] = [
    "F5": "Quick save", "5": "Quick save", "F6": "Lua script menu", "6": "Lua script menu",
    "F7": "Turbo", "7": "Turbo", "F9": "FPS counter",
  ]
}

// MARK: - Settings

/// The Controls tab's settings (LaunchSettings.controls). Stored under their own keys in the same store.
struct ControlSettings: Equatable {
  var keyboardMap = KeyboardMap.defaultMap
  var mouseLook = false
  var mouseLookScale = 256  // cvar range 1...4096
  /// Overlay binds by cvar name; a missing entry is the recomp default. "None" unbinds.
  var binds: [String: String] = [:]
  var viewButtonToGame = false

  static let scaleRange = 1...4096

  func bind(_ cvar: String) -> String {
    binds[cvar] ?? Hotkeys.rebindable.first { $0.cvar == cvar }?.keys ?? "None"
  }

  static func load(from store: SettingsStore) -> ControlSettings {
    var c = ControlSettings()
    if let map = store.string(forKey: "keyboardMap") { c.keyboardMap = map }
    if let on = store.object(forKey: "mouseLook") as? Bool { c.mouseLook = on }
    if let scale = store.object(forKey: "mouseLookScale") as? Int { c.mouseLookScale = scale }
    if let binds = store.object(forKey: "hotkeyBinds") as? [String: String] { c.binds = binds }
    if let on = store.object(forKey: "viewButtonToGame") as? Bool { c.viewButtonToGame = on }
    return c
  }

  func save(to store: SettingsStore, old: ControlSettings) {
    if keyboardMap != old.keyboardMap { store.set(keyboardMap, forKey: "keyboardMap") }
    if mouseLook != old.mouseLook { store.set(mouseLook, forKey: "mouseLook") }
    if mouseLookScale != old.mouseLookScale { store.set(mouseLookScale, forKey: "mouseLookScale") }
    if binds != old.binds { store.set(binds, forKey: "hotkeyBinds") }
    if viewButtonToGame != old.viewButtonToGame { store.set(viewButtonToGame, forKey: "viewButtonToGame") }
  }

  /// NAME=VALUE items (part of LaunchSettings.settingsEnvironment). The map and mouse look are always explicit
  /// so the launcher, not fable_2.toml or fable2_config.toml, decides; binds only when changed.
  var environment: [String] {
    var env = ["REX_KEYBOARD_GAMEPAD_MAP=\(keyboardMap)", "REX_MOUSE_LOOK=\(mouseLook)",
               "REX_MOUSE_LOOK_SCALE=\(min(max(mouseLookScale, Self.scaleRange.lowerBound), Self.scaleRange.upperBound))"]
    for hotkey in Hotkeys.rebindable {
      guard let cvar = hotkey.cvar, let key = binds[cvar], key != hotkey.keys else { continue }
      env.append("REX_\(cvar.uppercased())=\(key)")
      // The settings key also frees the mouse (mouse_unlock_key, src/main.cpp); keep them the same key.
      if cvar == "bind_settings" { env.append("REX_MOUSE_UNLOCK_KEY=\(key == "None" ? "" : key)") }
    }
    if viewButtonToGame { env.append("FABLE2_VIEW_BUTTON_TO_GAME=1") }
    return env
  }

  enum CLIResult { case notMine, ok(consumed: Int), error(String) }

  /// Dev Launcher --launch options: --keyboard-map MAP, --mouse-look / --no-mouse-look, --mouse-look-scale N,
  /// --bind CVAR=KEY (bind_console, bind_settings, bind_debug_overlay, bind_achievements),
  /// --view-button-to-game / --no-view-button-to-game.
  mutating func applyCLI(_ arg: String, _ value: String?) -> CLIResult {
    switch arg {
    case "--keyboard-map":
      guard let v = value else { return .error("--keyboard-map expects Key:Input,...") }
      let parsed = KeyboardMap.parse(v)
      if !parsed.invalid.isEmpty { return .error("--keyboard-map: invalid entries " + parsed.invalid.joined(separator: " ")) }
      keyboardMap = v; return .ok(consumed: 1)
    case "--mouse-look": mouseLook = true; return .ok(consumed: 0)
    case "--no-mouse-look": mouseLook = false; return .ok(consumed: 0)
    case "--mouse-look-scale":
      guard let v = value.flatMap(Int.init), Self.scaleRange.contains(v) else {
        return .error("--mouse-look-scale expects 1 to 4096")
      }
      mouseLookScale = v; return .ok(consumed: 1)
    case "--bind":
      let parts = (value ?? "").split(separator: "=", maxSplits: 1).map(String.init)
      guard parts.count == 2, Hotkeys.rebindable.contains(where: { $0.cvar == parts[0] }),
            parts[1] == "None" || KeyNames.valid.contains(parts[1]) else {
        return .error("--bind expects bind_console|bind_settings|bind_debug_overlay|bind_achievements=KEY")
      }
      binds[parts[0]] = parts[1]; return .ok(consumed: 1)
    case "--view-button-to-game": viewButtonToGame = true; return .ok(consumed: 0)
    case "--no-view-button-to-game": viewButtonToGame = false; return .ok(consumed: 0)
    default: return .notMine
    }
  }
}

// MARK: - Key capture

/// What a captured key is for.
enum CaptureTarget: Equatable {
  case input(String, add: Bool)
  case hotkey(String)
}

/// Captures the next key pressed in this app's windows (a local event monitor; the key is swallowed).
/// ⌘ shortcuts pass through, so ⌘W still closes the window.
final class KeyCapture: ObservableObject {
  @Published private(set) var target: CaptureTarget?
  @Published var note: String?
  private var monitor: Any?
  private var handler: ((String) -> Void)?

  func start(_ target: CaptureTarget, handler: @escaping (String) -> Void) {
    stop()
    self.target = target
    self.handler = handler
    note = nil
    monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged]) { [weak self] event in
      guard let self = self else { return event }
      if event.type == .keyDown {
        if event.modifierFlags.contains(.command) { return event }
        if let name = KeyNames.byKeyCode[event.keyCode] { self.finish(name) }
        else { self.note = "That key is not one the game can read. Pick one from the + menu instead." }
        return nil
      }
      // flagsChanged: a modifier went down (Shift, Control, Option, Caps Lock).
      let flag: NSEvent.ModifierFlags?
      switch event.keyCode {
      case 56, 60: flag = .shift
      case 59, 62: flag = .control
      case 58, 61: flag = .option
      case 57: flag = .capsLock
      case 54, 55: self.note = "Command can't be bound: macOS uses it for shortcuts."; return nil
      default: flag = nil
      }
      if let flag = flag, event.modifierFlags.contains(flag) || event.keyCode == 57,
         let name = KeyNames.byKeyCode[event.keyCode] { self.finish(name); return nil }
      return event
    }
  }

  func stop() {
    if let monitor = monitor { NSEvent.removeMonitor(monitor) }
    monitor = nil
    handler = nil
    target = nil
  }

  private func finish(_ name: String) {
    let handler = self.handler
    stop()
    handler?(name)
  }
}

// MARK: - Controllers

/// Connected game controllers (GameController framework), updated on connect/disconnect.
final class ControllerList: ObservableObject {
  @Published private(set) var names: [String] = []
  private var observers: [NSObjectProtocol] = []

  init() {
    refresh()
    for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
      observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
        self?.refresh()
      })
    }
  }

  deinit { observers.forEach(NotificationCenter.default.removeObserver) }

  private func refresh() {
    names = GCController.controllers().map { controller in
      let name = controller.vendorName ?? "Controller"
      return controller.productCategory.isEmpty || name.contains(controller.productCategory)
        ? name : "\(name) (\(controller.productCategory))"
    }
  }

  /// System Settings > Game Controllers (macOS 13+); falls back to System Settings.
  static func openSystemSettings() {
    let pane = URL(string: "x-apple.systempreferences:com.apple.Game-Controller-Settings.extension")!
    if !NSWorkspace.shared.open(pane) { NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:")!) }
  }
}

// MARK: - View

/// A key shown as a small rounded label.
private struct KeyChip: View {
  let name: String
  var warning: String? = nil
  var remove: (() -> Void)? = nil
  var body: some View {
    HStack(spacing: 3) {
      if warning != nil {
        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange).imageScale(.small)
      }
      Text(KeyNames.display(name)).font(.system(.callout, design: .rounded)).lineLimit(1)
      if let remove = remove {
        Button(action: remove) { Image(systemName: "xmark.circle.fill").imageScale(.small) }
          .buttonStyle(.borderless).foregroundStyle(.secondary)
          .help("Remove \(KeyNames.display(name))")
      }
    }
    .padding(.horizontal, 6).padding(.vertical, 2)
    .background(RoundedRectangle(cornerRadius: 5).fill(Color.secondary.opacity(0.15)))
    .help(warning ?? KeyNames.display(name))
  }
}

/// A key the user picked that is already bound to another input.
private struct PendingConflict: Identifiable {
  let key: String
  let target: String        // the input being edited
  let owners: [String]      // inputs that have the key now
  let add: Bool             // adding a key (keep the target's keys) or replacing them
  var id: String { key + target }
}

/// The Controls tab's sections (SettingsTabView's content for SettingsTab.controls).
struct ControlsTabContent: View {
  @ObservedObject var model: SettingsModel
  var variant: SettingsVariant = .dev
  @StateObject private var capture = KeyCapture()
  @StateObject private var controllers = ControllerList()
  @State private var conflict: PendingConflict?

  private var controls: ControlSettings { model.settings.controls }
  private var parsed: (bindings: [String: [String]], invalid: [String]) { KeyboardMap.parse(controls.keyboardMap) }

  private func setBindings(_ bindings: [String: [String]]) {
    model.settings.controls.keyboardMap = KeyboardMap.serialize(bindings)
  }

  private func title(_ input: String) -> String {
    KeyboardMap.inputs.first { $0.name == input }?.title ?? input
  }

  /// Why a key bound to `input` may not act as expected (another input, a hotkey, the mouse-unlock key).
  private func warning(for key: String, in input: String, bindings: [String: [String]]) -> String? {
    var notes: [String] = []
    let others = KeyboardMap.inputs.filter { $0.name != input && (bindings[$0.name] ?? []).contains(key) }
    if !others.isEmpty { notes.append("Also bound to " + others.map(\.title).joined(separator: ", ") + ".") }
    if let hotkey = Hotkeys.fixedKeys[key] { notes.append("Also the \(hotkey) hotkey.") }
    for hotkey in Hotkeys.rebindable where controls.bind(hotkey.cvar!) == key {
      notes.append(hotkey.cvar == "bind_settings"
                   ? "The Settings menu key: the game ignores it as a button."
                   : "Also opens the \(hotkey.title.lowercased()).")
    }
    return notes.isEmpty ? nil : notes.joined(separator: " ")
  }

  /// A captured or picked key for an input: bind it, or ask when another input has it.
  private func choose(_ key: String, for input: String, add: Bool) {
    let owners = KeyboardMap.owners(of: key, except: input, in: parsed.bindings)
    if owners.isEmpty { apply(key, to: input, add: add, owners: [], swap: false) }
    else { conflict = PendingConflict(key: key, target: input, owners: owners, add: add) }
  }

  private func apply(_ key: String, to input: String, add: Bool, owners: [String], swap: Bool) {
    setBindings(KeyboardMap.bind(key, to: input, add: add, swap: swap, in: parsed.bindings))
  }

  var body: some View {
    keyboardSection
    mouseSection
    hotkeySection
    controllerSection
  }

  // Keyboard -> gamepad table.
  @ViewBuilder private var keyboardSection: some View {
    let current = parsed
    Section {
      ForEach(KeyboardMap.inputs) { input in
        inputRow(input, bindings: current.bindings)
      }
      if !current.invalid.isEmpty {
        Label("Skipped (the game can't read them): " + current.invalid.joined(separator: ", "),
              systemImage: "exclamationmark.triangle").foregroundStyle(.orange).font(.caption)
      }
    } header: {
      Text("Keyboard → controller")
    } footer: {
      HStack(alignment: .firstTextBaseline) {
        Text(capture.note ?? (variant == .dev
             ? "Click a row, then press a key; + adds another key or a mouse button. Passed as REX_KEYBOARD_GAMEPAD_MAP; the log's [input] line shows the map in use."
             : "Click a row, then press a key; + adds another key or a mouse button. Keys work together with a controller."))
          .font(.caption).foregroundStyle(capture.note == nil ? Color.secondary : Color.orange)
          .fixedSize(horizontal: false, vertical: true)
        Spacer()
        Button("Restore Defaults") { capture.stop(); model.settings.controls.keyboardMap = KeyboardMap.defaultMap }
          .disabled(controls.keyboardMap == KeyboardMap.defaultMap)
          .help("The built-in layout: WASD to move, E/X/Z/C for A/B/X/Y, Q/Tab triggers, Escape pause, M select, arrows for the d-pad.")
      }
    }
    .alert(item: $conflict) { pending in
      let owners = pending.owners.map(title).joined(separator: " and ")
      let previous = parsed.bindings[pending.target] ?? []
      let message = Text("\(KeyNames.display(pending.key)) is already bound to \(owners).")
      if !pending.add && !previous.isEmpty {
        return Alert(
          title: Text("\(KeyNames.display(pending.key)) is in use"),
          message: message + Text(" Swap gives \(owners) \(previous.map(KeyNames.display).joined(separator: ", ")); Replace leaves it without this key."),
          primaryButton: .default(Text("Swap")) {
            apply(pending.key, to: pending.target, add: pending.add, owners: pending.owners, swap: true)
          },
          secondaryButton: .destructive(Text("Replace")) {
            apply(pending.key, to: pending.target, add: pending.add, owners: pending.owners, swap: false)
          })
      }
      return Alert(
        title: Text("\(KeyNames.display(pending.key)) is in use"),
        message: message + Text(" Replace moves it to \(title(pending.target))."),
        primaryButton: .destructive(Text("Replace")) {
          apply(pending.key, to: pending.target, add: pending.add, owners: pending.owners, swap: false)
        },
        secondaryButton: .cancel())
    }
    .onDisappear { capture.stop() }
  }

  private func inputRow(_ input: GuestInput, bindings: [String: [String]]) -> some View {
    let keys = bindings[input.name] ?? []
    let capturing = capture.target == .input(input.name, add: false) || capture.target == .input(input.name, add: true)
    return HStack(spacing: 6) {
      Text(input.title).frame(width: 170, alignment: .leading)
      if capturing {
        Text("Press a key…").foregroundStyle(Color.accentColor)
        Spacer()
        Button("Cancel") { capture.stop() }
      } else {
        if keys.isEmpty { Text("Not bound").foregroundStyle(.tertiary) }
        ForEach(keys, id: \.self) { key in
          KeyChip(name: key, warning: warning(for: key, in: input.name, bindings: bindings)) {
            var b = parsed.bindings
            b[input.name] = keys.filter { $0 != key }
            setBindings(b)
          }
        }
        Spacer(minLength: 4)
        Menu {
          Button("Add a Key…") { capture.start(.input(input.name, add: true)) { choose($0, for: input.name, add: true) } }
          ForEach(KeyNames.menuGroups, id: \.title) { group in
            Menu(group.title) {
              ForEach(group.names, id: \.self) { name in
                Button(KeyNames.display(name)) { choose(name, for: input.name, add: true) }
              }
            }
          }
          Divider()
          Button("Clear") { var b = parsed.bindings; b[input.name] = []; setBindings(b) }.disabled(keys.isEmpty)
        } label: {
          Image(systemName: "plus")
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
        .help("Add another key, a mouse button or any key the game reads; or clear this input.")
      }
    }
    .contentShape(Rectangle())
    .onTapGesture {
      if !capturing { capture.start(.input(input.name, add: false)) { choose($0, for: input.name, add: false) } }
    }
    .help(capturing ? "Press the new key (⌘ shortcuts still work)." : "Click, then press a key to rebind \(input.title).")
  }

  // Mouse look.
  private var mouseSection: some View {
    Section {
      Toggle(isOn: $model.settings.controls.mouseLook) {
        VStack(alignment: .leading, spacing: 2) {
          Text("Mouse look")
          Text("Moving the mouse turns the camera (right stick). Captures the cursor while playing; the Settings menu key (\(KeyNames.display(controls.bind("bind_settings")))) frees it.")
            .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
      }
      LabeledContent("Sensitivity") {
        HStack {
          Slider(value: Binding(
            get: { log2(Double(min(max(controls.mouseLookScale, 32), 2048))) },
            set: { model.settings.controls.mouseLookScale = Int((pow(2, $0) / 8).rounded() * 8) }),
                 in: 5...11) {
            EmptyView()
          } minimumValueLabel: { Text("Low").font(.caption) } maximumValueLabel: { Text("High").font(.caption) }
          TextField("", value: Binding(
            get: { controls.mouseLookScale },
            set: { model.settings.controls.mouseLookScale = min(max($0, ControlSettings.scaleRange.lowerBound),
                                                                 ControlSettings.scaleRange.upperBound) }),
                    format: .number)
            .frame(width: 56).multilineTextAlignment(.trailing).labelsHidden()
        }
      }
      .disabled(!controls.mouseLook)
      .help("Right-stick units per pixel of mouse movement (1–4096, default 256). The console can change mouse_look_scale live.")
    } header: {
      Text("Mouse")
    } footer: {
      HStack {
        Spacer()
        Button("Restore Defaults") {
          model.settings.controls.mouseLook = false
          model.settings.controls.mouseLookScale = 256
        }
        .disabled(!controls.mouseLook && controls.mouseLookScale == 256)
      }
    }
  }

  // Hotkeys: rebindable SDK overlay keys, then the fixed ones.
  private var hotkeySection: some View {
    Section {
      ForEach(Hotkeys.rebindable) { hotkey in hotkeyRow(hotkey) }
      ForEach(Hotkeys.fixed) { hotkey in
        HStack(spacing: 6) {
          VStack(alignment: .leading, spacing: 2) {
            Text(hotkey.title)
            Text(hotkey.detail).font(.caption).foregroundStyle(.secondary)
          }
          Spacer()
          Text(hotkey.keys).font(.system(.callout, design: .rounded)).foregroundStyle(.secondary)
        }
      }
    } header: {
      Text("Hotkeys")
    } footer: {
      HStack(alignment: .firstTextBaseline) {
        Text("The first four can be rebound (click, then press a key). The others are built in; the number keys do nothing while a menu or the console is open.")
          .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        Spacer()
        Button("Restore Defaults") { capture.stop(); model.settings.controls.binds = [:] }
          .disabled(controls.binds.allSatisfy { cvar, key in Hotkeys.rebindable.first { $0.cvar == cvar }?.keys == key })
      }
    }
  }

  private func hotkeyRow(_ hotkey: Hotkey) -> some View {
    let cvar = hotkey.cvar!
    let key = controls.bind(cvar)
    let capturing = capture.target == .hotkey(cvar)
    let clash: String? = {
      if key == "None" { return nil }
      var notes: [String] = []
      let map = parsed.bindings
      let inputs = KeyboardMap.inputs.filter { (map[$0.name] ?? []).contains(key) }.map(\.title)
      if !inputs.isEmpty { notes.append("Also a controller key: " + inputs.joined(separator: ", ") + ".") }
      if let fixed = Hotkeys.fixedKeys[key] { notes.append("Also the \(fixed) hotkey.") }
      let others = Hotkeys.rebindable.filter { $0.cvar != cvar && controls.bind($0.cvar!) == key }.map(\.title)
      if !others.isEmpty { notes.append("Also " + others.joined(separator: ", ") + ".") }
      return notes.isEmpty ? nil : notes.joined(separator: " ")
    }()
    return HStack(spacing: 6) {
      VStack(alignment: .leading, spacing: 2) {
        Text(hotkey.title)
        Text(hotkey.detail).font(.caption).foregroundStyle(.secondary)
      }
      Spacer()
      if capturing {
        Text("Press a key…").foregroundStyle(Color.accentColor)
        Button("Cancel") { capture.stop() }
      } else {
        KeyChip(name: key, warning: clash)
        Menu {
          ForEach(KeyNames.menuGroups.filter { $0.title != "Mouse Buttons" }, id: \.title) { group in
            Menu(group.title) {
              ForEach(group.names, id: \.self) { name in
                Button(KeyNames.display(name)) { model.settings.controls.binds[cvar] = name }
              }
            }
          }
          Divider()
          Button("None (no key)") { model.settings.controls.binds[cvar] = "None" }
          Button("Default (\(KeyNames.display(hotkey.keys)))") { model.settings.controls.binds[cvar] = nil }
        } label: { Image(systemName: "chevron.down") }
          .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
      }
    }
    .contentShape(Rectangle())
    .onTapGesture {
      if !capturing { capture.start(.hotkey(cvar)) { model.settings.controls.binds[cvar] = $0 } }
    }
    .help(variant == .dev ? "SDK cvar \(cvar) (REX_\(cvar.uppercased())). Click, then press a key." : "Click, then press a key.")
  }

  // Controllers.
  private var controllerSection: some View {
    Section {
      VStack(alignment: .leading, spacing: 6) {
        Text("Controllers work directly: connect one before or during play. To remap a controller's buttons, use macOS: System Settings > Game Controllers (system-wide, per controller).")
          .fixedSize(horizontal: false, vertical: true)
        HStack {
          Button("Open Game Controller Settings…") { ControllerList.openSystemSettings() }
          Spacer()
        }
      }
      LabeledContent("Connected") {
        Text(controllers.names.isEmpty ? "None" : controllers.names.joined(separator: ", "))
          .foregroundStyle(.secondary).multilineTextAlignment(.trailing)
      }
      Toggle(isOn: $model.settings.controls.viewButtonToGame) {
        VStack(alignment: .leading, spacing: 2) {
          Text("View button reaches the game as Back")
          Text("Off: View takes a screenshot only. On: it also acts as Back (Select) in the game.")
            .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
      }
      .help(variant == .dev ? "FABLE2_VIEW_BUTTON_TO_GAME=1 when on (src/input/controller_hotkeys.h)." : "")
    } header: {
      Text("Game controllers")
    }
  }
}
