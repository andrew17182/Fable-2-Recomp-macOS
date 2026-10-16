// Optimized guest shader path (FABLE2_NATIVE_SHADERS, docs/native-shaders.md).
//
// Metal Shader Converter output is AIR that never went through the Metal
// front end's optimization pipeline. Running Apple's `metal-opt -O2` over it
// (the same pass pipeline `metal` uses for MSL) and repacking it with
// `metallib` makes the hot Fable II pixel shaders much faster: the Xenos
// control-flow dispatch loop gets its invariant constant and descriptor loads
// hoisted (LICM / loop rotation), which the driver compiler does not do.
//
// The tools come from the Metal Toolchain (`xcrun --find metal-opt`), which
// the build already requires. Without them every call returns false and the
// caller keeps the plain converter output.

#ifndef REX_GRAPHICS_METAL_METAL_AIR_OPTIMIZER_H_
#define REX_GRAPHICS_METAL_METAL_AIR_OPTIMIZER_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rex {
namespace graphics {
namespace metal {

// Default when FABLE2_NATIVE_SHADERS is unset (docs/native-shaders.md,
// milestone 2 decision).
constexpr uint32_t kNativeShadersDefault = 1;

// 0 = off, 1 = optimize guest pixel shaders. Reads FABLE2_NATIVE_SHADERS once
// ("0" disables, any other non-empty value enables, unset = default).
uint32_t NativeShadersMode();
// FABLE2_NS_EMU_PIPELINES (default on, =0 off): geometry- and
// tessellation-emulation pipelines also get the optimized pixel shader
// (rebuilt in the background and swapped once it is ready). Only with
// NativeShadersMode().
bool NativeShadersEmuPipelines();
// FABLE2_NATIVE_XLATE (docs/native-translator.md, default on since
// 2026-10-13, =0 off): guest pixel and vertex shaders are translated straight to MSL (no DXBC/DXIL/converter code)
// and published as the optimized function; per-shader fallback to the
// converter + -O2 path. Only with NativeShadersMode().
bool NativeXlateMode();
// FABLE2_NATIVE_FIRST (docs/no-converter.md; needs FABLE2_NATIVE_XLATE):
// a guest shader is translated to MSL and compiled when it is first needed,
// and that library is the shader's only one: no DXIL, no Metal Shader
// Converter, no background swap. Shaders the translator refuses still take
// the converter path (or fail loudly in a FABLE2_NO_CONVERTER build).
bool NativeFirstMode();
// False in a build without dxilconv and Metal Shader Converter
// (FABLE2_NO_CONVERTER=1 at build time).
bool ConverterBuiltIn();
// True in a no-converter build (unless FABLE2_NOCONV_ALLOW_OFF=1, a
// diagnostic): the native-path switches (FABLE2_NATIVE_SHADERS, _XLATE,
// _XLATE_VS, _FIRST, _TESS, _TESS_BLENDED, _POINTS, _POINTS_MEMEXPORT,
// _RECTS) ignore =0 and FABLE2_NATIVE_POINTS_ONLY is ignored, because there
// is no converter path to fall back to (docs/no-converter.md).
bool NoConverterForcesNative();
// FABLE2_NATIVE_XLATE_VS (default on when FABLE2_NATIVE_XLATE is on; =0 keeps
// the converter's vertex shaders): native vertex shaders for standard
// pipelines (emulation pipelines keep the converter's vertex stage).
bool NativeXlateVertexMode();
// FABLE2_NATIVE_TESS (default 1 since 2026-10-13, =0 off; docs/native-translator.md,
// "Geometry and tessellation emulation"): quad tessellation draws use native
// paths instead of the converter's mesh-shader emulation. Needs the
// translator on.
// 1: exact. Patches that all have uniform, half-exact factors go to Metal's
//    tessellator (bit-identical to the emulation); others go to the D3D11
//    reference tessellator's pattern (exact D3D semantics, correct edges).
// 2: Metal's tessellator for every draw, blended ones included (Metal's
//    own stitching of transition regions; triangle order not proven).
// 3: the reference pattern for every non-blended draw.
uint32_t NativeTessMode();
// FABLE2_TERRAIN_ADAPTIVE=1 (default off; docs/native-translator.md): the
// terrain keeps the guest's adaptive tessellation factors (no
// metal_fable_terrain_uniform_tessellation clamp); the native paths map the
// edges correctly, so no cracks (mode 1: the reference tessellator for
// non-uniform patches). Needs FABLE2_NATIVE_TESS on.
bool TerrainAdaptiveMode();
// FABLE2_TERRAIN_LAYER_MATCH (default on, only with FABLE2_TERRAIN_ADAPTIVE=1;
// docs/adaptive-terrain-flashes-20261014.md): the blended terrain layers take
// the per-patch factors of the opaque terrain under them, and every terrain
// pass takes the same tessellation path, so a layer never lands behind the
// prepass depth (the white flashes). =0: layers at the uniform maximum.
bool TerrainLayerMatchMode();
// FABLE2_NATIVE_TESS_BLENDED (default on since 2026-10-14, =0 off): blended
// tessellated draws (the continuous terrain layers) also take Metal's
// tessellator in modes 1 and 3 when their factors qualify (uniform, exact in
// half precision). Their triangle order inside a draw can differ from the
// emulation where a draw's own triangles tie in depth (the maintainer accepted it,
// docs/native-translator.md). Non-uniform blended draws stay emulated.
bool NativeTessBlendedMode();
// FABLE2_NATIVE_TESS_SNAP_UV=1 (default off, prototype for the M3 white
// flashes): the native domain shader snaps Metal's tessellator uv to the
// D3D11 1/65536 grid (see MslTranslatorConfig::snap_domain_uv).
bool NativeTessSnapUvMode();
// FABLE2_EFFECTS_FOOTPRINT (default on, =0 off; docs/bloom-banding-20261014.md):
// see MslTranslatorConfig::effects_footprint.
bool EffectsFootprintMode();
// FABLE2_EFFECTS_FOOTPRINT_VARIANT (default on, =0 off; docs/ssmask-20261014.md):
// a pixel shader is translated with the effects footprint only in the
// modification variant of draws where some offset fetch's texture format can
// take it (MetalPipelineCache::kPixelModificationFootprint); other draws use
// a variant without the footprint code, which is the same shader for them.
bool EffectsFootprintVariantMode();
// FABLE2_NATIVE_POINTS (default 2, =0 off; docs/native-translator.md): point-list
// draws that the converter emulates with a geometry shader are expanded
// natively instead. 1: triangle-strip instances, the native VS run per
// corner; 2: a native mesh function, the VS run once per point.
uint32_t NativePointsMode();
// FABLE2_NATIVE_POINTS_MEMEXPORT (default on with FABLE2_NATIVE_POINTS=2 and
// FABLE2_NATIVE_FIRST, =0 off; docs/no-converter.md): point lists whose
// vertex shader exports to memory (the dog-fur exporter F6C546D6) also take
// the native mesh expansion. Its VS runs once per point, as in the
// converter's emulation, so the exports happen exactly once.
bool NativePointsMemexportMode();
// FABLE2_NATIVE_RECTS (default on, =0 off): the same for rectangle lists (the
// fourth vertex mirrored across the longest edge in the native VS).
bool NativeRectsMode();
// FABLE2_NATIVE_XLATE_DUMP=<dir>: writes each translated shader's MSL there
// (diagnostic; game-derived, keep it in ignored folders).
const char* NativeXlateDumpDir();

// FABLE2_NS_KEEP_PLAIN=<hash>[,<hash>...] (guest pixel shader ucode hashes, hex):
// shaders that keep the plain library because -O2 measured slower than plain
// (docs/native-shaders.md, "Shaders kept plain"). Unset = the built-in list;
// "none" (or empty) = optimize everything.
bool NativeShaderKeepPlain(uint64_t ucode_hash);

// True when metal-opt and metallib were found. Looks them up on first call
// (blocking; xcrun + `metal-opt --version`).
bool AirOptimizerAvailable();

// True when cached optimized libraries can be used: the tools were found, or
// FABLE2_NS_CACHE_TOOLCHAIN names the toolchain of a bundled cache (portable
// build without Xcode). Blocks like AirOptimizerAvailable.
bool AirOptimizerCacheReadable();

// True once the lookup has finished (AirOptimizerAvailable won't block).
bool AirOptimizerLookupDone();

// Starts the tool lookup on a background thread so later callers rarely wait.
void StartAirOptimizerLookup();

// Identifies the toolchain (hash of `metal-opt --version`), for cache keys.
uint64_t AirOptimizerToolchainHash();

// Optimizes a single-function metallib produced by Metal Shader Converter.
// Runs two tool processes at utility QoS; call it from a background thread.
// Returns false (out untouched) when the tools are missing, the library has
// more than one module, or a tool fails; *error says why.
bool OptimizeMetallibAir(const std::vector<uint8_t>& metallib,
                         std::vector<uint8_t>& optimized_out,
                         std::string* error);

// FABLE2_NS_SPECIALIZE (metal_ns_specialize.h): runs `metal-opt -O2 -S` on
// the library's module, hands the text to `transform` (constant baking), then
// `metal-opt -O2` and `metallib` again. Background threads only.
bool SpecializeMetallibAir(
    const std::vector<uint8_t>& metallib,
    const std::function<bool(std::string& ll, std::string* error)>& transform,
    std::vector<uint8_t>& specialized_out, std::string* error);

}  // namespace metal
}  // namespace graphics
}  // namespace rex

#endif  // REX_GRAPHICS_METAL_METAL_AIR_OPTIMIZER_H_
