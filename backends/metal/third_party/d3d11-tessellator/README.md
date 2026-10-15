# D3D11 fixed-function tessellator reference (vendored)

`tessellator.cpp` and `tessellator.hpp` are Microsoft's D3D11 tessellator reference
("Microsoft D3D11 Fixed Function Tessellator Reference - May 7, 2012"), unmodified.

- Source: https://github.com/microsoft/DirectX-Specs, path `d3d/archive/images/d3d11/`
- Commit: `a610933eac2103cd502bacfb5193ae7f02c6fa54` (default branch `master`, fetched 2026-10-13)
- Licence: MIT, `Copyright (c) Microsoft Corporation` — the repository's `LICENSE-CODE`, copied here as
  `LICENSE` (and to `backends/metal/licenses/D3D11-Tessellator-LICENSE`). The files carry no licence header of
  their own. The repository's main `LICENSE` is CC-BY-4.0 and covers the specification documents, not this code.

Used by the native tessellation path (`FABLE2_NATIVE_TESS`, docs/native-translator.md) to generate the same
domain points and triangle order as the D3D11 tessellation that the converter's emulation reproduces.
