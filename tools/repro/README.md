# Reproducible / AV-clean Windows builds (no code-signing cert)

Goal: every release build of `fable_2.exe` + `rexruntime.dll` (+ plugins) should
scan clean on VirusTotal / AV cloud services **without** paying for a
code-signing certificate.

## Why an unsigned build gets flagged

AV vendors flag a file when it is (a) **novel** (a hash they've never seen),
(b) **unsigned**, and (c) **unusual in profile**. A large, freshly-rebuilt,
unsigned runtime DLL with no version info hits all three — and because a plain
rebuild changes the timestamp + PDB GUID, *every build is a brand-new novel
hash*, so the file never accumulates a benign reputation.

## The strategy (no cert)

1. **Normal PE profile** — the biggest *immediate* lever for a novel unsigned
   binary. A 10,140-export, version-less runtime DLL is an abnormal profile
   that AV generic heuristics (BitDefender's "Ulise" ML) latch onto. Trim the
   export table to a sane size, add version resources, and the file looks like
   a normal product library.
2. **Stable hash** — make a rebuild of the same source produce the *same*
   bytes. Then the shipped hash is fixed, gets scanned many times, and AV
   cloud/ML systems learn it is benign. The second-biggest lever.
3. **Population** — actually ship it so it gets observed; optionally pro-upload
   the stable hash to VirusTotal to seed its baseline. This is what *removes*
   the generic flags for good (the ML needs to see it many times as benign).

## What this does

### `clean_pe.py` (the build orchestrator)
Run as `fable_2`'s `POST_BUILD` over the staged release dir. It does three
things, in order:

1. **Adds a `VS_VERSION_INFO` to each ReXGlue DLL that lacks one** (the
   prebuilt SDK DLLs ship with only a manifest). `add_version_resource.py`
   grafts a well-formed version resource into the existing `.rsrc` section
   (company / product / version), preserving the manifest, so a large unsigned
   DLL reads as a real product rather than a dropped payload. Applied post-hoc,
   so it works on the prebuilt DLLs without rebuilding the SDK.
2. **Trims the `rexruntime.dll` export table** to the curated set — every
   symbol a consumer actually imports (`fable_2.exe`, the GPU plugins) plus the
   whole `rex::` public API. That drops ~6,200 of the ~10,140 exports that
   `WINDOWS_EXPORT_ALL_SYMBOLS` leaks (std, RTTI, operator, unused `__imp_`),
   leaving ~3,919. The `.def` is regenerated from the *current* consumers each
   build, so it can't go stale. The exported *code* is untouched — only the
   export directory is rewritten — so consumers still resolve (verified: 0
   missing imports).
3. **Normalizes** every staged exe/dll (pins the volatile PE bytes, below).

It is idempotent and prints the final hashes.

### `normalize_pe.py` (the core normalizer)
Post-build step that pins the volatile bytes so the hash depends only on
(source + compiler + flags):

| Volatile field | Where | Pinned to |
|---|---|---|
| PE `TimeDateStamp` | `IMAGE_FILE_HEADER` | fixed constant |
| CodeView entry timestamp | debug directory | fixed constant |
| RSDS PDB GUID | CodeView blob | fixed constant |
| RSDS PDB path | CodeView blob | `<basename>.pdb` (relative) |

It is idempotent, size-preserving, and skips files that aren't present. The
`__FILE__` strings (panic/assert messages) are made machine-independent at
*compile* time with `-ffile-prefix-map`, so they don't need patching.

Run it manually on any PE:

```
python tools/repro/normalize_pe.py fable_2.exe rexruntime.dll [--stamp 0x6554B000]
```

### Build integration (`CMakeLists.txt`)
- `fable_2` (and `fable_2_recomp`) compile with
  `-ffile-prefix-map=<sourcedir>= -fdebug-prefix-map=...` so embedded paths are
  relative to the repo root.
- A `VS_VERSION_INFO` resource (`fable2_version.rc`) is added to `fable_2.exe`
  (skipped automatically if no `CMAKE_RC_COMPILER` is configured).
- A `POST_BUILD` command runs `clean_pe.py` on the staged dir *after* staging,
  so each `build.cmd -release fable_2` leaves a stable-hash, profile-clean set
  of files (trimmed exports + pinned PE) in `out/build/win-amd64-release/`.

The DLLs (`rexruntime.dll`, `rexgpu-xenos.dll`) may be prebuilt from the SDK;
the normalizer works on their final bytes regardless of how they were built.

## Verifying reproducibility

```
build.cmd -r fable_2
sha256sum out/build/win-amd64-release/fable_2.exe out/build/win-amd64-release/rexruntime.dll
# touch a source or force a relink, rebuild:
build.cmd -r fable_2
sha256sum out/build/win-amd64-release/fable_2.exe out/build/win-amd64-release/rexruntime.dll
# -> identical hashes for identical source
```

## Further (optional) hardening

- **Pro-upload** the stable hash to VirusTotal after each release (populates it
  with zero end users).
- **Ship it / submit false-positive reports** — the remaining generic flags
  (BitDefender "Ulise") are a novelty/population score. The profile is already
  normalized (version resource + trimmed exports + manifest + no path leakage),
  and that is *all* the profile levers have; trimming 6,200 exports moved the
  count 0, which is proof the score is novelty/unsigned-dominated. So the count
  only trends down as the stable hash accumulates benign sightings (ship it,
  pro-upload it) and/or BitDefender whitelists the specific hash via a
  false-positive report. See `plans/av-clean-builds.md`.

Note: the version resource is added **post-hoc** to the prebuilt DLLs by
`add_version_resource.py` (no SDK rebuild needed). The SDK CMake also carries
the change so a from-source SDK build is clean too.
- **Code-sign later** if you ever want a hard trust signal: it layers on top of
  all of the above.
