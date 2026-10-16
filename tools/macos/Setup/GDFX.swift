// Xbox 360 disc image (XDVDFS / GDFX) reader used by "Fable II Setup" to copy the game
// files out of the user's own disc image. No external tools are needed.
//
// The directory walk is a Swift port of the GDFX reader in upstream's tools/x360extract
// (himdo/Fable-2-Recomp commit 2e12f32, "made a custom xex extractor to help with first
// time running of the game"; MIT licence, see LICENSE). Notable details carried over:
//   - header: "MICROSOFT*XBOX*MEDIA" magic, root sector (u32 LE) at +0x14, root size at +0x18;
//   - file data lives at (header offset - 0x10000) + sector * 2048;
//   - directory entries: u16 left, u16 right, u32 sector, u32 size, u8 attributes, u8 name
//     length, name; padded to 4 bytes. A directory can span several 0x800-byte sectors:
//     each sector's batch ends with 0xFF padding and the next batch starts at the next
//     sector boundary, so an 0xFFFFFFFF word is either the end of one batch or the end.
// Additions here: the known XGD2/XGD3 header offsets are tried before the full scan.
import Foundation

enum DiscError: LocalizedError {
  case open(String), noHeader, read(Int64), badName(String), write(String)
  var errorDescription: String? {
    switch self {
    case .open(let path): return "Cannot open the disc image: \(path)"
    case .noHeader: return "This file does not look like an Xbox 360 disc image (no GDFX header found)."
    case .read(let offset): return "Read failed at byte \(offset) of the disc image."
    case .badName(let name): return "Unexpected file name in the disc image: \(name)"
    case .write(let path): return "Cannot write \(path)"
    }
  }
}

struct DiscEntry {
  let name: String
  let path: String        // relative path inside the disc, "/" separated
  let sector: UInt32
  let size: UInt64
  let isDirectory: Bool
  var children: [DiscEntry] = []

  /// All files below (or equal to) this entry.
  var files: [DiscEntry] {
    isDirectory ? children.flatMap { $0.files } : [self]
  }
  var totalSize: UInt64 { files.reduce(0) { $0 + $1.size } }
}

final class DiscImage {
  static let magic = Array("MICROSOFT*XBOX*MEDIA".utf8)
  /// Header offsets of XGD2 (most Xbox 360 games, including Fable II), XGD3, XGD1 and
  /// plain GDFX dumps. If none matches, the whole image is scanned.
  static let knownHeaderOffsets: [Int64] = [0xFDA0000, 0x2090000, 0x18310000, 0x10000]

  let path: String
  let length: Int64
  private let fd: Int32
  private(set) var headerOffset: Int64 = -1
  private var baseOffset: Int64 { headerOffset - 0x10000 }
  private(set) var rootSector: UInt32 = 0
  private(set) var rootSize: UInt32 = 0

  convenience init(path: String) throws {
    let fd = open(path, O_RDONLY)
    if fd < 0 { throw DiscError.open(path) }
    try self.init(fd: fd, path: path)
  }

  /// An image already opened by the caller (the player app hands its open file to FableIIPrepare, --iso-fd, so the
  /// extractor never opens a path in a privacy-protected folder itself). Takes ownership of `fd`.
  init(fd: Int32, path: String) throws {
    self.path = path
    self.fd = fd
    var info = stat()
    fstat(fd, &info)
    length = Int64(info.st_size)
    headerOffset = try findHeader()
    let header = try read(at: headerOffset, count: 0x24)
    rootSector = Self.u32(header, 0x14)
    rootSize = Self.u32(header, 0x18)
  }

  deinit { close(fd) }

  func read(at offset: Int64, count: Int) throws -> [UInt8] {
    var buffer = [UInt8](repeating: 0, count: count)
    let got = buffer.withUnsafeMutableBytes { pread(fd, $0.baseAddress, count, off_t(offset)) }
    if got != count { throw DiscError.read(offset) }
    return buffer
  }

  static func u32(_ bytes: [UInt8], _ at: Int) -> UInt32 {
    UInt32(bytes[at]) | UInt32(bytes[at + 1]) << 8 | UInt32(bytes[at + 2]) << 16 | UInt32(bytes[at + 3]) << 24
  }

  private func hasMagic(at offset: Int64) -> Bool {
    guard offset >= 0x10000, offset + 0x24 <= length,
          let bytes = try? read(at: offset, count: Self.magic.count) else { return false }
    return bytes == Self.magic
  }

  private func findHeader() throws -> Int64 {
    for offset in Self.knownHeaderOffsets where hasMagic(at: offset) { return offset }
    // Fallback: scan in 64 MiB chunks with overlap (upstream FindGdfxHeader).
    let chunk = 64 << 20
    var offset: Int64 = 0
    while offset < length {
      let count = Int(min(Int64(chunk), length - offset))
      if count < Self.magic.count || (offset > 0 && count <= Self.magic.count) { break }  // tail already scanned (overlap)
      let bytes = try read(at: offset, count: count)
      var i = 0
      let first = Self.magic[0]
      while i <= count - Self.magic.count {
        if bytes[i] == first && Array(bytes[i..<(i + Self.magic.count)]) == Self.magic {
          let found = offset + Int64(i)
          if found >= 0x10000 { return found }
        }
        i += 1
      }
      offset += Int64(count - Self.magic.count)
    }
    throw DiscError.noHeader
  }

  func offset(ofSector sector: UInt32) -> Int64 { baseOffset + Int64(sector) * 2048 }

  /// Read one directory (all of its sector batches).
  func readDirectory(sector: UInt32, size: UInt32, parent: String) throws -> [DiscEntry] {
    var entries: [DiscEntry] = []
    if size == 0 { return entries }
    let start = offset(ofSector: sector)
    let bytes = try read(at: start, count: Int(size))
    var pos = 0
    let end = Int(size)
    while pos + 14 <= end {
      if Self.u32(bytes, pos) == 0xFFFF_FFFF {
        // End of this sector's batch, or of the whole directory.
        let next = ((pos >> 11) + 1) << 11
        if next < end && bytes[pos..<next].allSatisfy({ $0 == 0xFF }) { pos = next; continue }
        break
      }
      let entrySector = Self.u32(bytes, pos + 4)
      let entrySize = Self.u32(bytes, pos + 8)
      let attributes = bytes[pos + 12]
      let nameLength = Int(bytes[pos + 13])
      if nameLength < 1 || pos + 14 + nameLength > end { break }
      let nameBytes = bytes[(pos + 14)..<(pos + 14 + nameLength)]
      guard nameBytes.allSatisfy({ $0 > 0x20 && $0 < 0x7F }),
            let name = String(bytes: nameBytes, encoding: .ascii) else { break }
      if name == "." || name == ".." || name.contains("/") || name.contains("\\") {
        throw DiscError.badName(name)
      }
      let path = parent.isEmpty ? name : parent + "/" + name
      var entry = DiscEntry(name: name, path: path, sector: entrySector, size: UInt64(entrySize),
                            isDirectory: attributes & 0x10 != 0)
      if entry.isDirectory {
        entry.children = try readDirectory(sector: entrySector, size: entrySize, parent: path)
      }
      entries.append(entry)
      pos += (14 + nameLength + 3) & ~3
    }
    return entries
  }

  func rootEntries() throws -> [DiscEntry] {
    try readDirectory(sector: rootSector, size: rootSize, parent: "")
  }

  /// Copy one file to `destination` (written as "<name>.partial", then renamed).
  /// `progress` receives the number of bytes written since the last call.
  func extract(_ entry: DiscEntry, to destination: URL, progress: (UInt64) -> Void) throws {
    let partial = destination.appendingPathExtension("partial")
    try FileManager.default.createDirectory(at: destination.deletingLastPathComponent(),
                                            withIntermediateDirectories: true)
    let out = open(partial.path, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
    if out < 0 { throw DiscError.write(partial.path) }
    defer { close(out) }
    var remaining = entry.size
    var at = offset(ofSector: entry.sector)
    if at + Int64(entry.size) > length { throw DiscError.read(at) }
    let chunk = 4 << 20
    var buffer = [UInt8](repeating: 0, count: chunk)
    while remaining > 0 {
      let count = Int(min(UInt64(chunk), remaining))
      let got = buffer.withUnsafeMutableBytes { pread(fd, $0.baseAddress, count, off_t(at)) }
      if got != count { throw DiscError.read(at) }
      let wrote = buffer.withUnsafeBytes { write(out, $0.baseAddress, count) }
      if wrote != count { throw DiscError.write(partial.path) }
      at += Int64(count)
      remaining -= UInt64(count)
      progress(UInt64(count))
    }
    if FileManager.default.fileExists(atPath: destination.path) {
      try FileManager.default.removeItem(at: destination)
    }
    try FileManager.default.moveItem(at: partial, to: destination)
  }
}
