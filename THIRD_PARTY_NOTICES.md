# Third-party notices

This project's own code is under the MIT licence in [LICENSE](LICENSE). Fable II is © Microsoft / Lionhead Studios;
no game content is included. The components below keep their own licences.

A built `Fable II.app` carries the licence texts in `Contents/Resources/Licenses/` (copied by
`tools/package_macos_app.py`). In the repository they are in the places listed below; components downloaded at build
time (`backends/metal/dependencies.json`, `tools/build_sdk_from_source.py`) have their texts in their own source trees.

## In this repository

| Component | Where | Licence | Text |
|---|---|---|---|
| Fable 2 Recomp (upstream, himdo) | most of `src/`, configs, manifests | MIT | [LICENSE](LICENSE) |
| XeniOS / Xenia (Metal backend sources) | `backends/metal/port/` | BSD-3-Clause | `backends/metal/licenses/XeniOS-LICENSE` |
| ReXGlue SDK (patches) | `backends/metal/*.patch`, `patches/sdk/` | BSD-3-Clause | `backends/metal/licenses/ReXGlue-LICENSE` |
| D3D11 reference tessellator (Microsoft) | `backends/metal/third_party/d3d11-tessellator/` | MIT | `backends/metal/licenses/D3D11-Tessellator-LICENSE` |
| toml++ | `thirdparty/tomlplusplus/` | MIT | `thirdparty/tomlplusplus/LICENSE` |
| Vulkan headers, Vulkan Video headers (Khronos) | `thirdparty/vulkan/`, `thirdparty/vk_video/` (Windows build only) | Apache-2.0 | SPDX header in each file; full text at https://www.apache.org/licenses/LICENSE-2.0 |

## Downloaded at build time and shipped in the app

| Component | Ends up in | Licence |
|---|---|---|
| ReXGlue SDK runtime and recompiler | `librexruntime.dylib`, `rexglue` | BSD-3-Clause (includes Xenia, BSD-3-Clause) |
| SDL3 | static in the runtime and the game | zlib |
| FFmpeg (libavcodec, libavutil, libavformat; XMA decoder only, no GPL parts) | static in the runtime | LGPL-2.1-or-later |
| libmspack | static in the runtime | LGPL-2.1 |
| fmt, spdlog, o1heap, Dear ImGui, inja, toml++, aes_128 | static / headers in the runtime | MIT |
| CLI11 | `rexglue` recompiler | BSD-3-Clause |
| utfcpp | runtime | Boost Software License 1.0 |
| xxHash | runtime | BSD-2-Clause |
| Tracy client | `libTracyClient.dylib` | BSD-3-Clause |
| SIMDe | headers in the runtime | MIT |
| DirectXShaderCompiler (dxilconv, xenios-jp fork) | `libdxilconv.dylib` (not in a `FABLE2_NO_CONVERTER` build) | University of Illinois/NCSA, plus its third-party notices (`backends/metal/licenses/DirectXShaderCompiler-*`) |
| Apple Metal Shader Converter 3.1.1 | `libmetalirconverter.dylib` (not in a `FABLE2_NO_CONVERTER` build, which uses only the Apache-2.0 runtime headers) | Apple's Metal Shader Converter licence (library); Apache-2.0 (headers); acknowledgements in `Acknowledgements.rtf` |
| metal-cpp | compiled into `librexgpu-metal.dylib` | Apache-2.0 |
| DirectX-Headers | Metal plugin build | MIT |

LGPL components: the complete source of the runtime, including the exact FFmpeg and libmspack sources and the build
scripts that link them, is downloaded and built by `tools/build_sdk_from_source.py` from pinned, checksummed
tarballs, so a modified FFmpeg or libmspack can be substituted and the runtime rebuilt.
