#!/bin/bash
# Build the native macOS release. Arguments are passed to CMake configuration.
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$project_root"
if [[ "$(uname -s)" != Darwin ]]; then
    echo "This script builds on macOS." >&2
    exit 1
fi
case "$(uname -m)" in
    arm64)
        platform=mac-arm64
        sdk_sha256=1192638b51a6963aa6a4f24b77ebcb90c16f7fd0b91b311e11a90100f5274326
        ;;
    x86_64)
        platform=mac-amd64
        sdk_sha256=a71b12e8c91d7f9084683166c59a176634e6adc711f2c60f80cc4a286aa13642
        ;;
    *) echo "Unsupported Mac architecture." >&2; exit 1 ;;
esac

# Private CMake downloaded by the setup app (Install.command); used first when present.
if [[ -x "$project_root/out/tools/cmake/CMake.app/Contents/bin/cmake" ]]; then
    export PATH="$project_root/out/tools/cmake/CMake.app/Contents/bin:$PATH"
fi
stage() { echo "[fable2-build] stage=$1: $2"; }

# Check game files and tools before downloading anything.
missing=()
if [[ ! -f default.xex || ! -d data ]]; then
    missing+=("Game files missing: default.xex and data/ from your Fable II disc must be in $project_root")
fi
for tool in cmake clang++ python3 curl ditto shasum; do
    command -v "$tool" >/dev/null || missing+=("Missing build tool: $tool")
done
if (( ${#missing[@]} )); then
    echo "Cannot build yet:" >&2
    printf '  - %s\n' "${missing[@]}" >&2
    echo 'Double-click Install.command to set everything up, or see README "Getting started".' >&2
    exit 1
fi

# Apple Silicon development defaults to Metal; explicit Vulkan remains available.
backend="${FABLE2_MACOS_BACKEND:-auto}"
case "$backend" in
    auto) [[ "$platform" == mac-arm64 ]] && backend=metal || backend=vulkan ;;
    metal|vulkan) ;;
    *) echo "FABLE2_MACOS_BACKEND must be auto, metal, or vulkan." >&2; exit 1 ;;
esac
if [[ "$backend" == metal ]]; then
    exec python3 "$project_root/tools/build_macos_metal.py" --cmake-args "$@"
fi

# Upstream now tracks thirdparty/rexglue-sdk as a source submodule.
# Keep downloaded Mac binaries outside that checkout.
stage sdk "code generation"
sdk_root="${REXGLUE_SDK_ROOT:-$project_root/out/sdk/$platform}"
if [[ ! -x "$sdk_root/bin/rexglue" ]]; then
    if [[ -n "${REXGLUE_SDK_ROOT:-}" ]]; then
        echo "REXGLUE_SDK_ROOT does not contain bin/rexglue: $sdk_root" >&2
        exit 1
    fi
    archive="$project_root/out/downloads/rexglue-sdk-0.10.0-$platform.zip"
    mkdir -p "$(dirname "$archive")" "$project_root/out/sdk"
    curl --fail --location --show-error \
        "https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-$platform.zip" \
        --output "$archive"
    printf '%s  %s\n' "$sdk_sha256" "$archive" | shasum -a 256 -c -
    ditto -x -k "$archive" "$project_root/out/sdk"
fi

# The source ZIP omits generated/rexglue.cmake. Generate only that missing
# integration file in a temporary scaffold; keep the game's manifest and code.
if [[ ! -f generated/rexglue.cmake ]]; then
    scaffold="$(mktemp -d "${TMPDIR:-/tmp}/fable2-scaffold.XXXXXX")"
    trap 'rm -rf "$scaffold"' EXIT
    "$sdk_root/bin/rexglue" init --project-name fable_2 \
        --xex-path "$project_root/default.xex" --project-root "$scaffold"
    mkdir -p generated
    cp "$scaffold/generated/rexglue.cmake" generated/rexglue.cmake
fi

# Switching between the source-built and the prebuilt SDK (FABLE2_SDK_FROM_SOURCE,
# docs/sdk-from-source.md): the cache pins rexglue_DIR, SDL3_DIR, ... to the old
# root, so start a fresh configure there (object files are kept).
build_cache="$project_root/out/build/$platform-release/CMakeCache.txt"
if [[ -f "$build_cache" ]] && ! grep -qxF "REXGLUE_SDK_ROOT:UNINITIALIZED=$sdk_root" "$build_cache"; then
    echo "ReXGlue SDK root changed to $sdk_root; reconfiguring."
    rm -f "${build_cache:?}"
fi

# CPU baseline for Apple Silicon: -mcpu=apple-m1 (every Apple Silicon Mac);
# FABLE2_CPU_TARGET=generic restores the old -march=armv8-a.
cpu_args=()
if [[ "$platform" == mac-arm64 ]]; then
    cpu_flags=-mcpu=apple-m1
    [[ "${FABLE2_CPU_TARGET:-}" == generic ]] && cpu_flags=-march=armv8-a
    cpu_args=(-DCMAKE_C_FLAGS="$cpu_flags" -DCMAKE_CXX_FLAGS="$cpu_flags")
fi
configure() {
    cmake --preset "$platform-release" -G 'Unix Makefiles' \
        -DCMAKE_PREFIX_PATH="$sdk_root" -DREXGLUE_SDK_ROOT="$sdk_root" \
        ${cpu_args[@]+"${cpu_args[@]}"} "$@"
}
configure "$@"
cmake --build "out/build/$platform-release" --target fable_2_codegen
# Reconfigure after code generation so CMake sees the new sources.cmake.
configure "$@"
stage game "building the game"
# The Metal orchestrator needs only the shared executable, not a Vulkan app.
if [[ "${FABLE2_MACOS_GAME_ONLY:-0}" == 1 ]]; then
    cmake --build "out/build/$platform-release" --target fable_2 \
        --parallel "${FABLE2_BUILD_JOBS:-4}"
    exit 0
fi
cmake --build "out/build/$platform-release" --target fable_2_app \
    --parallel "${FABLE2_BUILD_JOBS:-4}"
if [[ "$platform" == mac-arm64 && "${FABLE2_MACOS_TERRAIN_FIX:-1}" != 0 ]]; then
    python3 tools/macos_terrain_fix.py "out/build/$platform-release/librexgpu-xenos.dylib"
fi
echo "Built: $project_root/out/build/$platform-release/Fable II.app"
