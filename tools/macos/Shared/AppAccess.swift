// macOS privacy folders (TCC, System Settings > Privacy & Security > Files and Folders) and App Translocation, for
// the release app (docs/release-app.md "Protected folders").
//
// Why: macOS asks the player before an app reads Downloads, Desktop, Documents, iCloud Drive, other cloud storage or
// removable and network volumes. A process reading its own app bundle is not asked, but helper processes outside the
// bundle are: /usr/bin/python3 running the build kit from an app left in ~/Downloads (also when macOS runs it
// translocated) triggered "Fable II would like to access files in your Downloads folder". Rule since 2026-10-19:
// the apps never trigger a privacy prompt. So the app copies the build kit out of its bundle with its own process,
// hands the disc image (a user pick) to the extractor as an open file, never copies, moves or trashes its own bundle
// (the old "Move to Applications" offer trashed the original in Downloads, which asked for Downloads access), and
// explains a denial (Choose Disc Image…, Privacy Settings, Try Again).
//
// Pure functions (paths and errno in, verdicts out) so tests can drive them: FableIIPlayer --access-check PATH and
// --bundle-location [PATH] print them as JSON. Test switch: FABLE2_TEST_PROTECTED_DIRS (colon-separated folders that
// count as Downloads).
import AppKit
import Darwin

enum AppAccess {
  static var environment: [String: String] { ProcessInfo.processInfo.environment }

  // MARK: Protected folders

  /// Test stand-ins for Downloads (a chmod-ed scratch folder then exercises the privacy UI).
  static var testProtected: [String] {
    (environment["FABLE2_TEST_PROTECTED_DIRS"] ?? "").split(separator: ":").map(String.init).filter { $0.hasPrefix("/") }
  }

  static func standard(_ path: String) -> String {
    var p = (path as NSString).standardizingPath
    if p.hasPrefix("/private/var/") || p.hasPrefix("/private/tmp/") { p = String(p.dropFirst("/private".count)) }
    return p
  }

  static func inside(_ path: String, _ folder: String) -> Bool {
    let p = standard(path), f = standard(folder)
    return p == f || p.hasPrefix(f.hasSuffix("/") ? f : f + "/")
  }

  /// The privacy-protected place `path` is in ("Downloads", "Desktop", "Documents", "iCloud Drive", "cloud storage",
  /// "an external or network volume"), or nil.
  static func protectedFolder(for path: String, home: String = NSHomeDirectory(), extra: [String]? = nil) -> String? {
    for folder in extra ?? testProtected where inside(path, folder) { return "Downloads" }
    let named: [(String, String)] = [("Downloads", "Downloads"), ("Desktop", "Desktop"), ("Documents", "Documents"),
                                     ("Library/Mobile Documents", "iCloud Drive"), ("Library/CloudStorage", "cloud storage")]
    for (relative, name) in named where inside(path, home + "/" + relative) { return name }
    if inside(path, "/Volumes") { return "an external or network volume" }
    return nil
  }

  // MARK: Errors

  enum Problem: Equatable {
    /// macOS privacy blocked the access (EPERM/EACCES inside a protected place).
    case privacy(folder: String, path: String)
    /// Plain file permissions (EACCES or EPERM elsewhere).
    case permissions(path: String)
    case missing(path: String)
    case other(path: String, code: Int32)

    var path: String {
      switch self {
      case .privacy(_, let p), .permissions(let p), .missing(let p), .other(let p, _): return p
      }
    }
    var kind: String {
      switch self {
      case .privacy: return "privacy"
      case .permissions: return "permissions"
      case .missing: return "missing"
      case .other: return "other"
      }
    }
    var folder: String? { if case .privacy(let f, _) = self { return f } else { return nil } }
    var json: [String: Any] { ["kind": kind, "path": path, "folder": (folder as Any?) ?? NSNull()] }
  }

  /// What an errno from accessing `path` means. nil for 0.
  static func classify(code: Int32, path: String, home: String = NSHomeDirectory(), extra: [String]? = nil) -> Problem? {
    switch code {
    case 0: return nil
    case EPERM, EACCES:
      if let folder = protectedFolder(for: path, home: home, extra: extra) { return .privacy(folder: folder, path: path) }
      return .permissions(path: path)
    case ENOENT, ENOTDIR: return .missing(path: path)
    default: return .other(path: path, code: code)
    }
  }

  /// Tries to read `path` (opens a file, lists a folder) and classifies a failure. nil: readable.
  static func check(_ path: String, home: String = NSHomeDirectory(), extra: [String]? = nil) -> Problem? {
    var code: Int32 = 0
    var directory: ObjCBool = false
    if FileManager.default.fileExists(atPath: path, isDirectory: &directory), directory.boolValue {
      if let dir = opendir(path) { closedir(dir) } else { code = errno }
    } else {
      let fd = open(path, O_RDONLY | O_CLOEXEC)
      if fd >= 0 { close(fd) } else { code = errno }
    }
    return classify(code: code, path: path, home: home, extra: extra)
  }

  /// A denied access in a helper's output: Python's "[Errno 1] Operation not permitted: '/path'" (PermissionError),
  /// "can't open file '/path': [Errno 1] …" (the interpreter could not read the script), or the build driver's
  /// "[denied] <errno> <path>" line.
  static func classify(output: String, home: String = NSHomeDirectory(), extra: [String]? = nil) -> Problem? {
    let patterns: [(String, Int, Int)] = [  // regex, errno group, path group
      (#"^\[denied\] (\d+) (.+)$"#, 1, 2),
      (#"\[Errno (1|13)\] (?:Operation not permitted|Permission denied): '([^']+)'"#, 1, 2),
      (#"can't open file '([^']+)': \[Errno (1|13)\]"#, 2, 1),
    ]
    for line in output.split(separator: "\n").reversed() {
      let text = String(line)
      for (pattern, codeGroup, pathGroup) in patterns {
        guard let regex = try? NSRegularExpression(pattern: pattern),
              let m = regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
              let codeRange = Range(m.range(at: codeGroup), in: text), let pathRange = Range(m.range(at: pathGroup), in: text),
              let code = Int32(text[codeRange]) else { continue }
        return classify(code: code, path: String(text[pathRange]), home: home, extra: extra)
      }
    }
    return nil
  }

  static func errnoOf(_ error: Error) -> Int32 {
    let ns = error as NSError
    if ns.domain == NSPOSIXErrorDomain { return Int32(ns.code) }
    if let underlying = ns.userInfo[NSUnderlyingErrorKey] as? NSError, underlying.domain == NSPOSIXErrorDomain {
      return Int32(underlying.code)
    }
    if ns.domain == NSCocoaErrorDomain {
      switch ns.code {
      case NSFileReadNoPermissionError, NSFileWriteNoPermissionError: return EACCES
      case NSFileReadNoSuchFileError, NSFileNoSuchFileError: return ENOENT
      default: break
      }
    }
    return EIO
  }

  /// System Settings > Privacy & Security > Files and Folders.
  static let privacySettingsURL = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_FilesAndFolders")!

  // MARK: Where the app runs from

  /// SecTranslocateIsTranslocatedURL / SecTranslocateCreateOriginalPathForURL (Security.framework, public C API
  /// without a Swift overlay), looked up at run time.
  static func translocation(of url: URL) -> (translocated: Bool, original: URL?) {
    typealias IsFn = @convention(c) (CFURL, UnsafeMutablePointer<Bool>, UnsafeMutablePointer<Unmanaged<CFError>?>?) -> Bool
    typealias OriginalFn = @convention(c) (CFURL, UnsafeMutablePointer<Unmanaged<CFError>?>?) -> Unmanaged<CFURL>?
    let heuristic = url.path.contains("/AppTranslocation/")
    guard let handle = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY),
          let isSym = dlsym(handle, "SecTranslocateIsTranslocatedURL"),
          let originalSym = dlsym(handle, "SecTranslocateCreateOriginalPathForURL") else { return (heuristic, nil) }
    var translocated = false
    guard unsafeBitCast(isSym, to: IsFn.self)(url as CFURL, &translocated, nil) else { return (heuristic, nil) }
    guard translocated else { return (heuristic, nil) }
    let original = unsafeBitCast(originalSym, to: OriginalFn.self)(url as CFURL, nil)?.takeRetainedValue() as URL?
    return (true, original)
  }

  static func readOnlyVolume(_ path: String) -> Bool {
    var info = statfs()
    return statfs(path, &info) == 0 && (info.f_flags & UInt32(MNT_RDONLY)) != 0
  }

  struct Location: Equatable {
    let bundlePath: String
    let originalPath: String?  // where the player put the app (translocated apps run from a hidden copy)
    let translocated: Bool
    let folder: String?        // protected folder of the original location
    let readOnly: Bool         // a disc image or another read-only volume
    var inApplications: Bool {
      let p = AppAccess.standard(originalPath ?? bundlePath)
      return p.hasPrefix("/Applications/") || AppAccess.inside(p, NSHomeDirectory() + "/Applications")
    }
    var json: [String: Any] {
      ["bundlePath": bundlePath, "originalPath": (originalPath as Any?) ?? NSNull(), "translocated": translocated,
       "folder": (folder as Any?) ?? NSNull(), "readOnly": readOnly, "inApplications": inApplications]
    }
  }

  /// Where `bundle` runs from. A path with /AppTranslocation/ counts as translocated even when Security.framework
  /// cannot say (tests pass such paths).
  static func location(of bundle: URL, home: String = NSHomeDirectory(), extra: [String]? = nil) -> Location {
    let (translocated, original) = translocation(of: bundle)
    let place = original?.path ?? bundle.path
    let readOnly = !translocated && readOnlyVolume(bundle.path) || (original.map { readOnlyVolume($0.path) } ?? false)
    return Location(bundlePath: bundle.path, originalPath: original?.path, translocated: translocated,
                    folder: protectedFolder(for: place, home: home, extra: extra), readOnly: readOnly)
  }
}
