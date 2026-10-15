// Start Fresh / Restore archive (docs/release-app.md "Start Fresh"): move everything this app owns into a
// timestamped archive so the next launch is a first launch again, without a new macOS user account. Nothing is
// deleted: items are moved (renamed) into <state>/Archive/<yyyy-MM-dd_HH-mm-ss>/, with fresh.json listing where each
// one came from. Restore moves an archive back (after archiving the current state, so nothing is lost); deleting an
// archive is a separate, confirmed action.
//
// Scope (only this app's own paths):
//   - its state folder: game files (GameData), build tree and generated code (Build), the built game (Game), logs,
//     shader caches (cache), settings and launcher plists, configs; the saves too unless "Keep my saves";
//   - its UserDefaults domain (exported into the archive, then removed), its saved window state, its
//     ~/Library/Caches folder and the per-user Metal shader cache of its bundle id (DARWIN_USER_CACHE_DIR/<id>).
// The system parts are touched only for a bundle id under org.fable2recomp., and never when the state folder comes
// from FABLE2_APP_STATE_ROOT (tests) unless FABLE2_FRESH_SYSTEM_ROOT gives stand-in folders for them.
import Foundation

enum StartFresh {
  struct Item: Codable, Equatable {
    var label: String
    var original: String  // absolute path it came from
    var archived: String  // path relative to the archive folder
    var kind: String = "path"  // "path" or "defaults" (an exported UserDefaults domain)
  }

  struct Record: Codable {
    var created: String
    var bundleID: String
    var stateRoot: String
    var keptSaves: Bool
    var reason: String
    var items: [Item]
  }

  struct Archive: Identifiable, Equatable {
    let name: String
    let path: String
    let created: Date
    let keptSaves: Bool
    let reason: String
    let bytes: Int64
    var id: String { name }
  }

  enum Failure: Error, CustomStringConvertible {
    case running, notOwned(String), io(String)
    var description: String {
      switch self {
      case .running: return "Fable II is running. Quit the game first."
      case .notOwned(let path): return "Refusing to touch \(path): it is not this app's folder."
      case .io(let text): return text
      }
    }
  }

  static let archiveFolderName = "Archive"
  /// State-folder entries never moved: the archives themselves and the lock the running app holds.
  static let keptEntries: Set<String> = [archiveFolderName, "app.lock", ".DS_Store"]
  static let savesEntry = "saves"

  /// What Start Fresh moves, with a label each (shown in the confirmation sheet). `stateRoot` must be the app's own
  /// state folder.
  static func plan(stateRoot: String, bundleID: String, keepSaves: Bool,
                   environment: [String: String] = ProcessInfo.processInfo.environment) -> [Item] {
    let fm = FileManager.default
    var items: [Item] = []
    let labels: [String: String] = [
      "GameData": "Game files extracted from your disc image",
      "Build": "Build folder (generated code, compiled objects, CMake)",
      "Game": "The game built on this Mac",
      "logs": "Logs",
      "cache": "Shader cache",
      "saves": "Saves",
      "launcher.plist": "Launcher choices",
      "player-settings.plist": "Settings",
    ]
    let entries = ((try? fm.contentsOfDirectory(atPath: stateRoot)) ?? []).sorted()
    for name in entries where !keptEntries.contains(name) {
      if name == savesEntry && keepSaves { continue }
      items.append(Item(label: labels[name] ?? name, original: stateRoot + "/" + name, archived: "state/" + name))
    }
    for (label, path, key) in systemPaths(bundleID: bundleID, environment: environment) where fm.fileExists(atPath: path) {
      items.append(Item(label: label, original: path, archived: "system/" + key))
    }
    if let domain = defaultsDomain(bundleID: bundleID, environment: environment), domain.exists {
      items.append(Item(label: "App preferences (UserDefaults \(bundleID))", original: domain.description,
                        archived: "system/\(bundleID).defaults.plist", kind: "defaults"))
    }
    return items
  }

  /// Whether system locations may be touched, and where they are (stand-ins under FABLE2_FRESH_SYSTEM_ROOT in tests).
  static func systemRoot(bundleID: String, environment: [String: String]) -> String?? {
    guard bundleID.hasPrefix("org.fable2recomp.") else { return .none }
    if let root = environment["FABLE2_FRESH_SYSTEM_ROOT"], !root.isEmpty { return .some(root) }
    if let state = environment["FABLE2_APP_STATE_ROOT"], !state.isEmpty { return .none }  // tests: state only
    return .some(nil)  // the real locations
  }

  /// (label, path, archive name) of the app's system folders.
  static func systemPaths(bundleID: String, environment: [String: String]) -> [(String, String, String)] {
    guard case .some(let root) = systemRoot(bundleID: bundleID, environment: environment) else { return [] }
    let home = NSHomeDirectory()
    let darwinCache: String
    if let root = root { darwinCache = root + "/DarwinUserCache" } else {
      var buffer = [CChar](repeating: 0, count: 1024)
      let n = confstr(_CS_DARWIN_USER_CACHE_DIR, &buffer, buffer.count)
      darwinCache = n > 0 ? String(cString: buffer).trimmingCharacters(in: CharacterSet(charactersIn: "/")) : ""
    }
    let base = root ?? home + "/Library"
    var out: [(String, String, String)] = [
      ("Saved window state", base + "/Saved Application State/\(bundleID).savedState", "saved-state"),
      ("App caches", base + "/Caches/\(bundleID)", "caches"),
    ]
    if !darwinCache.isEmpty {
      out.append(("macOS Metal shader cache of this app", (root == nil ? "/" : "") + darwinCache + "/" + bundleID,
                  "darwin-user-cache"))
    }
    return out
  }

  struct DefaultsDomain: CustomStringConvertible {
    let bundleID: String
    let plistFile: String?  // test stand-in: a plist file instead of the real domain
    var exists: Bool {
      if let file = plistFile { return FileManager.default.fileExists(atPath: file) }
      guard let values = UserDefaults(suiteName: bundleID)?.persistentDomain(forName: bundleID) else { return false }
      return !values.isEmpty
    }
    var description: String { plistFile ?? "defaults:\(bundleID)" }
  }

  static func defaultsDomain(bundleID: String, environment: [String: String]) -> DefaultsDomain? {
    guard case .some(let root) = systemRoot(bundleID: bundleID, environment: environment) else { return nil }
    if let root = root { return DefaultsDomain(bundleID: bundleID, plistFile: root + "/Preferences/\(bundleID).plist") }
    return DefaultsDomain(bundleID: bundleID, plistFile: nil)
  }

  static func timestamp(_ date: Date = Date()) -> String {
    let formatter = DateFormatter()
    formatter.locale = Locale(identifier: "en_US_POSIX")
    formatter.dateFormat = "yyyy-MM-dd_HH-mm-ss"
    return formatter.string(from: date)
  }

  /// Moves `items` into a new archive folder; returns its path. Every move is a rename on the same volume where
  /// possible; FileManager falls back to copy-then-remove across volumes.
  @discardableResult
  static func archive(stateRoot: String, bundleID: String, keepSaves: Bool, reason: String,
                      environment: [String: String] = ProcessInfo.processInfo.environment) throws -> String {
    let fm = FileManager.default
    guard stateRoot.hasPrefix("/"), (stateRoot as NSString).lastPathComponent != "Application Support" else {
      throw Failure.notOwned(stateRoot)
    }
    let items = plan(stateRoot: stateRoot, bundleID: bundleID, keepSaves: keepSaves, environment: environment)
    var name = timestamp()
    var folder = stateRoot + "/" + archiveFolderName + "/" + name
    var n = 2
    while fm.fileExists(atPath: folder) { folder = stateRoot + "/" + archiveFolderName + "/" + name + "-\(n)"; n += 1 }
    name = (folder as NSString).lastPathComponent
    do {
      try fm.createDirectory(atPath: folder + "/state", withIntermediateDirectories: true)
      try fm.createDirectory(atPath: folder + "/system", withIntermediateDirectories: true)
    } catch { throw Failure.io("Could not create the archive folder: \(error.localizedDescription)") }
    var done: [Item] = []
    func writeRecord() {
      let record = Record(created: ISO8601DateFormatter().string(from: Date()), bundleID: bundleID, stateRoot: stateRoot,
                          keptSaves: keepSaves, reason: reason, items: done)
      let encoder = JSONEncoder()
      encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
      if let data = try? encoder.encode(record) { fm.createFile(atPath: folder + "/fresh.json", contents: data) }
    }
    for item in items {
      let target = folder + "/" + item.archived
      do {
        if item.kind == "defaults" {
          let domain = defaultsDomain(bundleID: bundleID, environment: environment)!
          if let file = domain.plistFile {
            try fm.moveItem(atPath: file, toPath: target)
          } else {
            guard run("/usr/bin/defaults", ["export", bundleID, target]) == 0 else {
              throw Failure.io("Could not export the app's preferences (defaults export \(bundleID)).")
            }
            UserDefaults.standard.removePersistentDomain(forName: bundleID)
            _ = run("/usr/bin/defaults", ["delete", bundleID])
          }
        } else {
          try fm.moveItem(atPath: item.original, toPath: target)
        }
        done.append(item)
      } catch {
        writeRecord()
        throw Failure.io("Could not move \(item.original) into the archive: \(error). Items moved so far are in \(folder).")
      }
    }
    writeRecord()
    return folder
  }

  static func archives(stateRoot: String) -> [Archive] {
    let fm = FileManager.default
    let base = stateRoot + "/" + archiveFolderName
    var out: [Archive] = []
    for name in (try? fm.contentsOfDirectory(atPath: base)) ?? [] {
      let path = base + "/" + name
      guard let data = fm.contents(atPath: path + "/fresh.json"),
            let record = try? JSONDecoder().decode(Record.self, from: data) else { continue }
      let created = ISO8601DateFormatter().date(from: record.created) ?? Date.distantPast
      out.append(Archive(name: name, path: path, created: created, keptSaves: record.keptSaves, reason: record.reason,
                         bytes: allocatedSize(path)))
    }
    return out.sorted { $0.created > $1.created }
  }

  /// Archives the current state (everything, saves included: nothing is lost), then moves `archive`'s items back.
  @discardableResult
  static func restore(_ archive: Archive, stateRoot: String, bundleID: String,
                      environment: [String: String] = ProcessInfo.processInfo.environment) throws -> String {
    let fm = FileManager.default
    guard let data = fm.contents(atPath: archive.path + "/fresh.json"),
          let record = try? JSONDecoder().decode(Record.self, from: data) else {
      throw Failure.io("The archive \(archive.name) has no readable fresh.json.")
    }
    guard record.stateRoot == stateRoot, record.bundleID == bundleID else {
      throw Failure.notOwned(archive.path)
    }
    // When the archive kept its saves, the current saves stay in place (they are the same saves, played on).
    let current = try self.archive(stateRoot: stateRoot, bundleID: bundleID, keepSaves: record.keptSaves,
                                   reason: "before restoring \(archive.name)", environment: environment)
    for item in record.items {
      let source = archive.path + "/" + item.archived
      guard fm.fileExists(atPath: source) else { continue }
      do {
        if item.kind == "defaults" {
          if let file = defaultsDomain(bundleID: bundleID, environment: environment)?.plistFile {
            try fm.createDirectory(atPath: (file as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
            try fm.moveItem(atPath: source, toPath: file)
          } else {
            guard run("/usr/bin/defaults", ["import", bundleID, source]) == 0 else {
              throw Failure.io("Could not import the archived preferences.")
            }
            try fm.removeItem(atPath: source)
          }
        } else {
          guard !fm.fileExists(atPath: item.original) else {
            throw Failure.io("\(item.original) exists already; restore stopped (current state is in \(current)).")
          }
          try fm.createDirectory(atPath: (item.original as NSString).deletingLastPathComponent,
                                 withIntermediateDirectories: true)
          try fm.moveItem(atPath: source, toPath: item.original)
        }
      } catch let failure as Failure {
        throw failure
      } catch {
        throw Failure.io("Could not restore \(item.original): \(error). The state before the restore is in \(current).")
      }
    }
    // The archive is now empty apart from its record: mark it restored rather than deleting anything.
    try? fm.moveItem(atPath: archive.path + "/fresh.json", toPath: archive.path + "/restored.json")
    return current
  }

  /// Permanently removes one archive folder (a separate, confirmed action in the app).
  static func delete(_ archive: Archive, stateRoot: String) throws {
    let base = (stateRoot + "/" + archiveFolderName as NSString).standardizingPath
    let path = (archive.path as NSString).standardizingPath
    guard (path as NSString).deletingLastPathComponent == base else { throw Failure.notOwned(archive.path) }
    do { try FileManager.default.removeItem(atPath: path) } catch {
      throw Failure.io("Could not delete \(archive.name): \(error.localizedDescription)")
    }
  }

  static func allocatedSize(_ path: String) -> Int64 {
    var total: Int64 = 0
    guard let walker = FileManager.default.enumerator(at: URL(fileURLWithPath: path),
                                                      includingPropertiesForKeys: [.totalFileAllocatedSizeKey]) else { return 0 }
    for case let url as URL in walker {
      total += Int64((try? url.resourceValues(forKeys: [.totalFileAllocatedSizeKey]))?.totalFileAllocatedSize ?? 0)
    }
    return total
  }

  @discardableResult
  static func run(_ tool: String, _ arguments: [String]) -> Int32 {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: tool)
    process.arguments = arguments
    process.standardOutput = FileHandle.nullDevice
    process.standardError = FileHandle.nullDevice
    do { try process.run() } catch { return -1 }
    process.waitUntilExit()
    return process.terminationStatus
  }

  /// What Start Fresh cannot reset (shown in the sheet).
  static let notReset = [
    "Apple's Command Line Tools (installed for the whole Mac)",
    "Tools installed with Homebrew or on your PATH (for example CMake)",
    "macOS's Gatekeeper approval of this app (\"Open Anyway\")",
  ]
}
