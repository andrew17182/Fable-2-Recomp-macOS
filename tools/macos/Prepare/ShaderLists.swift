// Rebuilds the game's shader list (cache/shaders/shareable/4D5307F1.xsh) and pipeline list (.metal.xpso) from a
// shader recipe plus the player's own disc files (docs/offline-shader-seed.md). The same algorithm as
// tools/fable2_shader_bank.py `rebuild`: the recipe names, per shader, a slice of a decompressed entry of
// data/Shaders/ShadersRelease.sbk (zlib), the vertex-fetch dwords D3D patched at bind time and a SHA-256 of the
// result; no microcode is in the recipe.
//
//   FableIIPrepare --target <GameData> --shader-lists <recipe.json> --cache <state>/cache
//       Exit 0 = written (logs "[shader-lists] ..."), 1 = failed.
import Compression
import CryptoKit
import Foundation

enum ShaderLists {
  static let title = "4D5307F1"
  static let xshMagic: UInt32 = 0x4853_4558, xshVersion: UInt32 = 0x2020_1219

  static func be32(_ d: [UInt8], _ at: Int) -> Int {
    Int(d[at]) << 24 | Int(d[at + 1]) << 16 | Int(d[at + 2]) << 8 | Int(d[at + 3])
  }

  /// The decompressed entries of one ShaderBankFile (layout: tools/fable2_shader_bank.py read_bank).
  static func readBank(_ url: URL) throws -> [[UInt8]] {
    let d = [UInt8](try Data(contentsOf: url))
    guard d.count > 16, String(decoding: d[0..<14], as: UTF8.self) == "ShaderBankFile" else {
      throw NSError(domain: "ShaderLists", code: 1, userInfo: [NSLocalizedDescriptionKey: "\(url.path) is not a shader bank"])
    }
    var first = 0
    while first + 1 < d.count {
      guard let found = (first..<(d.count - 1)).first(where: { d[$0] == 0x78 && d[$0 + 1] == 0xDA }) else { break }
      first = found
      for n in 1..<4096 {
        let c = first - 8, b = c - 4 * n, a = b - 4 * n - 4
        if a < 16 { break }
        if be32(d, a) == n && be32(d, b) == 0 {
          var entries: [[UInt8]] = []
          for i in 0..<n {
            let size = be32(d, a + 4 + 4 * i), offset = be32(d, b + 4 * i)
            let s = c + offset
            let csize = be32(d, s + 4)
            // zlib stream: 2-byte header, then raw DEFLATE (COMPRESSION_ZLIB decodes raw DEFLATE).
            let src = Array(d[(s + 10)..<min(d.count, s + 8 + csize)])
            // The bank's streams carry no final block, so decode as a stream and take what it produced.
            var out = [UInt8](repeating: 0, count: size)
            var written = 0
            let stream = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
            defer { stream.deallocate() }
            if compression_stream_init(stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK {
              src.withUnsafeBufferPointer { sp in
                out.withUnsafeMutableBufferPointer { dp in
                  stream.pointee.src_ptr = sp.baseAddress!
                  stream.pointee.src_size = src.count
                  stream.pointee.dst_ptr = dp.baseAddress!
                  stream.pointee.dst_size = size
                  while stream.pointee.dst_size > 0 {
                    let status = compression_stream_process(stream, 0)
                    if status != COMPRESSION_STATUS_OK || stream.pointee.src_size == 0 { break }
                  }
                  written = size - stream.pointee.dst_size
                }
              }
              compression_stream_destroy(stream)
            }
            guard written == size else {
              throw NSError(domain: "ShaderLists", code: 2,
                            userInfo: [NSLocalizedDescriptionKey: "\(url.lastPathComponent) entry \(i): \(written) of \(size) bytes"])
            }
            entries.append(out)
          }
          return entries
        }
      }
      first += 1
    }
    throw NSError(domain: "ShaderLists", code: 3, userInfo: [NSLocalizedDescriptionKey: "\(url.path): no entry table"])
  }

  static func rebuild(recipe: URL, gameData: URL, cache: URL) throws -> (ok: Int, mismatched: Int) {
    guard let r = try JSONSerialization.jsonObject(with: Data(contentsOf: recipe)) as? [String: Any],
          r["version"] as? Int == 1, let shaders = r["shaders"] as? [[String: Any]] else {
      throw NSError(domain: "ShaderLists", code: 4, userInfo: [NSLocalizedDescriptionKey: "unsupported shader recipe"])
    }
    var banks: [String: [[UInt8]]] = [:]
    for name in ["ShadersRelease.sbk", "Shaders.sbk"] {
      let url = gameData.appendingPathComponent("data/Shaders/\(name)")
      if FileManager.default.fileExists(atPath: url.path) { banks[name] = try readBank(url) }
    }
    guard let fallback = banks["ShadersRelease.sbk"] ?? banks.values.first else {
      throw NSError(domain: "ShaderLists", code: 5, userInfo: [NSLocalizedDescriptionKey: "no shader bank in the game files"])
    }
    var out = Data()
    func le32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { out.append(contentsOf: $0) } }
    func le64(_ v: UInt64) { withUnsafeBytes(of: v.littleEndian) { out.append(contentsOf: $0) } }
    le32(xshMagic)
    le32(xshVersion.byteSwapped)
    var ok = 0, bad = 0
    for s in shaders {
      guard let hashText = s["hash"] as? String, let hash = UInt64(hashText, radix: 16), let dwords = s["dwords"] as? Int,
            let entry = s["entry"] as? Int, let offset = s["offset"] as? Int, let digest = s["sha256"] as? String else { bad += 1; continue }
      let entries = banks[s["bank"] as? String ?? ""] ?? fallback
      guard entry < entries.count, offset + 4 * dwords <= entries[entry].count else { bad += 1; continue }
      var ucode = Array(entries[entry][offset..<(offset + 4 * dwords)])
      for patch in s["patches"] as? [[Int]] ?? [] where patch.count == 2 && 4 * patch[0] + 4 <= ucode.count {
        let v = UInt32(truncatingIfNeeded: patch[1]), k = 4 * patch[0]
        ucode[k] = UInt8(v >> 24); ucode[k + 1] = UInt8((v >> 16) & 0xFF)
        ucode[k + 2] = UInt8((v >> 8) & 0xFF); ucode[k + 3] = UInt8(v & 0xFF)
      }
      let got = SHA256.hash(data: ucode).map { String(format: "%02x", $0) }.joined()
      guard got == digest else { bad += 1; continue }
      le64(hash)
      le32(UInt32(dwords) | (s["type"] as? String == "pixel" ? 0x8000_0000 : 0))
      out.append(contentsOf: ucode)
      ok += 1
    }
    let shareable = cache.appendingPathComponent("shaders/shareable")
    try FileManager.default.createDirectory(at: shareable, withIntermediateDirectories: true)
    try out.write(to: shareable.appendingPathComponent("\(title).xsh"), options: .atomic)
    if let pipelines = r["pipelines"] as? [String: Any], let file = pipelines["file"] as? String,
       let b64 = pipelines["data_b64"] as? String, let data = Data(base64Encoded: b64) {
      try data.write(to: shareable.appendingPathComponent(file), options: .atomic)
    }
    return (ok, bad)
  }
}
