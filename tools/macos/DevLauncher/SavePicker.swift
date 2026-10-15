// Save picker of the Dev Launcher (2026-10-14): readable titles, grouping, test-save hiding, stars, renames, search.
//
// The data comes from `tools/fable2_dev_session.py list`; tools/fable2_save_picker.py adds each library entry's
// friendly place/quest names, its group (LevelName, QuestName, hero folder) and the test-save flag. Stars and custom
// titles are metadata only, in <main checkout>/out/save-library/.picker.json (the list's `picker_meta`); the save
// folders are never touched.
//
// CLI check: FableIIDevLauncher --picker-rows LIST.json [--query TEXT] [--show-tests] [--meta PICKER.json]
// prints the rows the picker would show as JSON (tests/save_picker_tests.py).
import AppKit
import Foundation
import SwiftUI

struct SaveItem: Identifiable, Hashable {
  let id: String      // "title", "live:Hero000", "library:<name>"
  let title: String   // automatic title ("Place · Quest · Oct 7, 2:45 pm" for library entries)
  let detail: String
  let folder: String  // HeroNNN of the save ("" for title)
  var name = ""       // library entry name
  var group = ""      // grouping key (library)
  var test = false    // made by an agent test session
  var hero = ""       // "Hero 1 · Hero000"
  var search = ""     // lowercased text the search field matches

  static let parse: DateFormatter = {
    let f = DateFormatter()
    f.locale = Locale(identifier: "en_US_POSIX")
    f.dateFormat = "yyyy-MM-dd HH:mm:ss"
    return f
  }()
  static let show: DateFormatter = {
    let f = DateFormatter()
    f.locale = Locale(identifier: "en_US_POSIX")
    f.dateFormat = "MMM d, h:mm a"
    f.amSymbol = "am"
    f.pmSymbol = "pm"
    return f
  }()

  /// "2026-10-07 14:45:08" -> "Oct 7, 2:45 pm" (unchanged when it does not parse).
  static func friendlyDate(_ text: String?) -> String {
    guard let text = text, !text.isEmpty else { return "" }
    guard let date = parse.date(from: text) else { return text }
    return show.string(from: date)
  }

  /// Place · quest from the backend's friendly names, else the raw codes.
  static func placeAndQuest(_ info: [String: Any]) -> [String] {
    let place = (info["place"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? (info["LevelName"] as? String)
    let quest = (info["quest"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? (info["QuestName"] as? String)
    return [place, quest].compactMap { $0 }
  }

  static func live(_ info: [String: Any]) -> SaveItem {
    let folder = info["folder"] as? String ?? "?"
    let name = info["header_display_name"] as? String ?? folder
    var parts = placeAndQuest(info)
    let when = friendlyDate(info["saved_mtime"] as? String)
    if !when.isEmpty { parts.append("saved " + when) }
    return SaveItem(id: "live:" + folder, title: "\(name) (\(folder))", detail: parts.joined(separator: " · "),
                    folder: folder)
  }

  static func library(_ info: [String: Any]) -> SaveItem {
    let name = info["name"] as? String ?? "?"
    let folder = info["folder"] as? String ?? "Hero000"
    let when = friendlyDate((info["created"] as? String) ?? (info["imported"] as? String) ?? (info["saved_mtime"] as? String))
    var titleParts = placeAndQuest(info)
    if titleParts.isEmpty { titleParts = [name] }
    if !when.isEmpty { titleParts.append(when) }
    let hero = [info["header_display_name"] as? String, folder].compactMap { $0 }.joined(separator: " · ")
    let test = info["test"] as? Bool ?? false
    var detail = [hero]
    if let chapter = (info["Chapter"] as? String).flatMap(Double.init) {
      detail.append(String(format: "Chapter %g", chapter))
    }
    if let note = info["note"] as? String, !note.isEmpty { detail.append(note) }
    detail.append(name)
    let search = ([name, hero, info["LevelName"], info["QuestName"], info["place"], info["quest"], info["note"]]
                  .compactMap { $0 as? String } + titleParts).joined(separator: " ").lowercased()
    return SaveItem(id: "library:" + name, title: titleParts.joined(separator: " · "),
                    detail: detail.joined(separator: " · "), folder: folder, name: name,
                    group: info["group"] as? String ?? name, test: test, hero: hero, search: search)
  }
}

/// Stars and custom titles (.picker.json: {"version": 1, "entries": {NAME: {"starred": bool, "title": str}}}).
struct PickerMeta: Equatable {
  var path = ""
  var starred: Set<String> = []
  var titles: [String: String] = [:]

  static func load(_ path: String) -> PickerMeta {
    var meta = PickerMeta(path: path)
    guard let data = FileManager.default.contents(atPath: path),
          let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let entries = root["entries"] as? [String: [String: Any]] else { return meta }
    for (name, entry) in entries {
      if entry["starred"] as? Bool == true { meta.starred.insert(name) }
      if let title = entry["title"] as? String, !title.isEmpty { meta.titles[name] = title }
    }
    return meta
  }

  /// Writes the file atomically, keeping keys this version does not know.
  func save() -> Bool {
    guard !path.isEmpty else { return false }
    var root: [String: Any] = [:]
    if let data = FileManager.default.contents(atPath: path),
       let old = try? JSONSerialization.jsonObject(with: data) as? [String: Any] { root = old }
    var entries = root["entries"] as? [String: [String: Any]] ?? [:]
    for name in Set(entries.keys).union(starred).union(titles.keys) {
      var entry = entries[name] ?? [:]
      entry["starred"] = starred.contains(name) ? true : nil
      entry["title"] = titles[name]
      entries[name] = entry.isEmpty ? nil : entry
    }
    root["version"] = root["version"] ?? 1
    root["entries"] = entries
    guard let data = try? JSONSerialization.data(withJSONObject: root, options: [.prettyPrinted, .sortedKeys]) else {
      return false
    }
    return (try? data.write(to: URL(fileURLWithPath: path), options: .atomic)) != nil
  }

  func title(_ item: SaveItem) -> String {
    item.name.isEmpty ? item.title : (titles[item.name] ?? item.title)
  }
}

struct SaveGroup: Identifiable {
  let id: String
  let newest: SaveItem
  let older: [SaveItem]
}

/// What the picker lists for one query: starred entries first (always shown), then the rest grouped by
/// (LevelName, QuestName, hero), newest first, test saves only when asked for (or when it is the current pick).
struct PickerRows {
  let starred: [SaveItem]
  let groups: [SaveGroup]
  let hiddenTests: Int

  init(library: [SaveItem], meta: PickerMeta, query: String, showTests: Bool, pick: String = "") {
    let words = query.lowercased().split(separator: " ").map(String.init)
    func matches(_ item: SaveItem) -> Bool {
      let text = item.search + " " + (meta.titles[item.name] ?? "").lowercased()
      return words.allSatisfy { text.contains($0) }
    }
    starred = library.filter { meta.starred.contains($0.name) && matches($0) }
    var hidden = 0
    var order: [String] = []
    var members: [String: [SaveItem]] = [:]
    for item in library where !meta.starred.contains(item.name) && matches(item) {
      if item.test && !showTests && item.id != pick { hidden += 1; continue }
      if members[item.group] == nil { order.append(item.group) }
      members[item.group, default: []].append(item)
    }
    hiddenTests = hidden
    groups = order.map { key in
      let items = members[key]!  // library order is newest first
      return SaveGroup(id: key, newest: items[0], older: Array(items.dropFirst()))
    }
  }

  /// JSON for --picker-rows (tests).
  func json(meta: PickerMeta) -> [String: Any] {
    func row(_ item: SaveItem) -> [String: Any] {
      ["name": item.name, "title": meta.title(item), "detail": item.detail, "test": item.test]
    }
    return ["starred": starred.map(row), "hidden_tests": hiddenTests,
            "groups": groups.map { ["newest": row($0.newest), "older": $0.older.map(row)] }]
  }
}

/// --picker-rows LIST.json [--query TEXT] [--show-tests] [--meta PICKER.json]
func cliPickerRows(_ argv: [String]) -> Int32 {
  func value(_ name: String) -> String? {
    argv.firstIndex(of: name).flatMap { $0 + 1 < argv.count ? argv[$0 + 1] : nil }
  }
  guard let path = value("--picker-rows"), let data = FileManager.default.contents(atPath: path),
        let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
    print("{\"ok\": false, \"error\": \"--picker-rows expects a list JSON file\"}"); return 2
  }
  let result = root["result"] as? [String: Any] ?? root
  let library = (result["library"] as? [[String: Any]] ?? []).map(SaveItem.library)
  let meta = PickerMeta.load(value("--meta") ?? (result["picker_meta"] as? String ?? ""))
  let rows = PickerRows(library: library, meta: meta, query: value("--query") ?? "",
                        showTests: argv.contains("--show-tests"))
  let out = (try? JSONSerialization.data(withJSONObject: rows.json(meta: meta), options: [.prettyPrinted, .sortedKeys]))
  print(String(decoding: out ?? Data(), as: UTF8.self))
  return 0
}

// MARK: - Sheet

struct SavePickerView: View {
  @ObservedObject var model: LauncherModel
  @Environment(\.dismiss) private var dismiss
  @State private var query = ""
  @AppStorage("pickerShowTests") private var showTests = false
  @State private var selection: String?
  @State private var expanded: Set<String> = []
  @State private var renaming: SaveItem?
  @State private var renameText = ""

  private var rows: PickerRows {
    PickerRows(library: model.library, meta: model.meta, query: query, showTests: showTests, pick: model.pick)
  }

  var body: some View {
    let rows = self.rows
    VStack(spacing: 0) {
      HStack(spacing: 12) {
        TextField("Search title, quest or level", text: $query)
          .textFieldStyle(.roundedBorder)
        Toggle("Show test saves", isOn: $showTests)
          .toggleStyle(.checkbox)
          .help("Saves made by agent test sessions (hidden by default; play saves are always shown).")
      }
      .padding(12)
      ScrollViewReader { scroller in
      List(selection: $selection) {
        if query.isEmpty {
          Text("Title screen (live saves, like Fable II.app)").tag("title")
        }
        if !rows.starred.isEmpty {
          Section("Starred") { ForEach(rows.starred) { row($0) } }
        }
        Section(libraryHeader(rows)) {
          ForEach(rows.groups) { group in
            row(group.newest)
            if !group.older.isEmpty {
              DisclosureGroup(isExpanded: expandedBinding(group.id)) {
                ForEach(group.older) { row($0) }
              } label: {
                Text("\(group.older.count) older").font(.caption).foregroundStyle(.secondary)
              }
            }
          }
        }
        let live = model.live.filter { query.isEmpty || ($0.title + " " + $0.detail).lowercased().contains(query.lowercased()) }
        if !live.isEmpty {
          Section("Live save slots") { ForEach(live) { row($0) } }
        }
      }
      .onAppear { DispatchQueue.main.async { scroller.scrollTo(model.pick, anchor: .center) } }
      .contextMenu(forSelectionType: String.self) { ids in
        if let id = ids.first, let item = model.library.first(where: { $0.id == id }) { menu(item) }
      } primaryAction: { ids in
        if let id = ids.first { choose(id) }
      }
      }
      Divider()
      HStack {
        Text("Double-click to pick · right-click to star or rename · stars and titles: \(metaFileName)")
          .font(.caption).foregroundStyle(.secondary)
        Spacer()
        Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
        Button("Choose") { if let id = selection { choose(id) } }
          .keyboardShortcut(.defaultAction)
          .disabled(selection == nil)
      }
      .padding(12)
    }
    .frame(width: 680, height: 600)
    .onAppear {
      selection = model.pick
      // Show the current pick even when it sits in a folded group.
      if let item = model.library.first(where: { $0.id == model.pick }) { expanded.insert(item.group) }
    }
    .alert("Rename save", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
      TextField("Title", text: $renameText)
      Button("Save") { if let item = renaming { model.setTitle(item, renameText) }; renaming = nil }
      Button("Use Automatic Title") { if let item = renaming { model.setTitle(item, "") }; renaming = nil }
      Button("Cancel", role: .cancel) { renaming = nil }
    } message: {
      Text(renaming.map { "\($0.title)\n\($0.name)\nOnly the picker's title changes; the save itself is not touched." } ?? "")
    }
  }

  private var metaFileName: String { "out/save-library/.picker.json" }

  private func libraryHeader(_ rows: PickerRows) -> String {
    var text = "Save library (newest first)"
    if rows.hiddenTests > 0 { text += " · \(rows.hiddenTests) test saves hidden" }
    return text
  }

  private func expandedBinding(_ id: String) -> Binding<Bool> {
    Binding(get: { !query.isEmpty || expanded.contains(id) },
            set: { if $0 { expanded.insert(id) } else { expanded.remove(id) } })
  }

  private func choose(_ id: String) {
    model.pick = id
    dismiss()
  }

  @ViewBuilder private func menu(_ item: SaveItem) -> some View {
    Button(model.meta.starred.contains(item.name) ? "Unstar" : "Star") { model.toggleStar(item) }
    Button("Rename…") { renameText = model.meta.title(item); renaming = item }
    if model.meta.titles[item.name] != nil {
      Button("Use Automatic Title") { model.setTitle(item, "") }
    }
    Divider()
    Button("Show in Finder") { model.reveal(item) }
  }

  private func row(_ item: SaveItem) -> some View {
    let starred = model.meta.starred.contains(item.name)
    let custom = model.meta.titles[item.name] != nil
    return HStack(alignment: .firstTextBaseline, spacing: 8) {
      if !item.name.isEmpty {
        Button { model.toggleStar(item) } label: {
          Image(systemName: starred ? "star.fill" : "star")
            .foregroundStyle(starred ? Color.yellow : Color.secondary)
        }
        .buttonStyle(.borderless)
        .help(starred ? "Unstar" : "Star (starred saves are listed first)")
      }
      VStack(alignment: .leading, spacing: 2) {
        HStack(spacing: 6) {
          Text(model.meta.title(item)).fontWeight(.medium).lineLimit(1)
          if item.test {
            Text("TEST").font(.caption2.weight(.semibold)).foregroundStyle(.secondary)
              .padding(.horizontal, 4).padding(.vertical, 1)
              .overlay(RoundedRectangle(cornerRadius: 3).stroke(Color.secondary.opacity(0.6)))
          }
        }
        Text(custom ? item.title + " · " + item.detail : item.detail)
          .font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
      }
    }
    .padding(.vertical, 1)
    .tag(item.id)
    .id(item.id)
  }
}
