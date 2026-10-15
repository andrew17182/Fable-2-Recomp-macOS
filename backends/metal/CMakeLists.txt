cmake_minimum_required(VERSION 3.25)
project(FableMetalGame LANGUAGES CXX OBJCXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)
if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  message(FATAL_ERROR "The Metal backend requires Apple Silicon")
endif()
if(NOT FABLE2_METAL_DEPS)
  message(FATAL_ERROR "Run tools/build_macos_metal.py to prepare pinned Metal dependencies")
endif()
find_package(rexglue CONFIG REQUIRED)
set(SDK "${FABLE2_METAL_DEPS}/sdk-source")

set(DEPS "${FABLE2_METAL_DEPS}")
set(MSC "${DEPS}/metal-shader-converter")
set(DXC "${DEPS}/DirectXShaderCompiler")
include(common-sources.cmake)
include(metal-sources.cmake)
# FABLE2_NO_CONVERTER (docs/no-converter.md): leave dxilconv and Apple's Metal
# Shader Converter out entirely. Guest shaders then only take the native
# Xenos -> MSL translator; anything that still needs the converter fails
# loudly at run time ([converter-use] ... NO-CONVERTER BUILD). The MSC runtime
# headers (Apache-2.0, types and the header-only emulation runtime) are still
# used; the MSC library is neither linked nor shipped.
option(FABLE2_NO_CONVERTER "Build without dxilconv and Metal Shader Converter" OFF)
if(FABLE2_NO_CONVERTER)
  list(REMOVE_ITEM METAL_SOURCES port/rex/graphics/metal/dxbc_to_dxil_converter.cc
                                 port/rex/graphics/metal/metal_shader_converter.cc)
  list(APPEND METAL_SOURCES port/rex/graphics/metal/metal_converter_stubs.cc)
  set(FABLE2_CONVERTER_LIBS "")
else()
  set(FABLE2_CONVERTER_LIBS
      "${FABLE2_METAL_DEPS}/build-dxilconv/lib/libdxilconv.dylib" "${MSC}/lib/libmetalirconverter.dylib")
endif()
add_library(rexgpu-metal SHARED ${COMMON_SOURCES} ${METAL_SOURCES} plugin_main.cpp metal_flags.cpp)
set_target_properties(rexgpu-metal PROPERTIES CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
target_include_directories(rexgpu-metal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/port" "${FABLE2_METAL_DEPS}/generated"
    "${SDK}/include" "${SDK}" "${CMAKE_CURRENT_SOURCE_DIR}" "${FABLE2_METAL_DEPS}"
    "${MSC}/include" "${MSC}/include/metal_irconverter" "${MSC}/include/metal_irconverter_runtime"
    "${DXC}/include" "${DXC}/projects/dxilconv/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/diagnostics")  # fable2_trace_api.h (flight recorder C API)
target_compile_definitions(rexgpu-metal PRIVATE REX_PLATFORM_MAC=1 METAL_SHADER_CONVERTER_AVAILABLE=1
    IR_RUNTIME_METALCPP=1 REXGLUE_BUILD_CONFIG="$<CONFIG>"
    FABLE2_NO_CONVERTER=$<BOOL:${FABLE2_NO_CONVERTER}>)
target_compile_options(rexgpu-metal PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/compat.h"
    -fno-strict-aliasing -ffp-model=strict -fno-char8_t -fblocks -ferror-limit=8)
# No build-machine paths in the shipped plugin (__FILE__ in asserts, debug info): the checkout (including the SDK
# sources under out/metal-build) becomes "./...", the rest of the home folder "~/...". The checkout map goes last
# (debug info takes the last matching option; __FILE__ the longest).
get_filename_component(FABLE2_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." REALPATH)
if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    target_compile_options(rexgpu-metal PRIVATE -ffile-prefix-map=$ENV{HOME}=~)
endif()
target_compile_options(rexgpu-metal PRIVATE -ffile-prefix-map=${FABLE2_REPO_ROOT}=.)
target_link_libraries(rexgpu-metal PRIVATE rex::runtime rex::xxhash ${FABLE2_CONVERTER_LIBS}
    "-framework Metal" "-framework Foundation" "-framework AppKit"
    "-framework QuartzCore" "-framework MetalKit" "-framework MetalFX"
    "-framework ImageIO" "-framework CoreGraphics")  # ImageIO: flash-kit example PNGs (metal_flash_detect.h)

# Diagnostic: offline native Xenos -> MSL translator driver (docs/native-translator.md).
add_executable(fable2-msl-xlate EXCLUDE_FROM_ALL ${COMMON_SOURCES}
    metal_flags.cpp port/rex/graphics/metal/metal_msl_translator.cc tools/msl_xlate_tool.cc)
target_include_directories(fable2-msl-xlate PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/port" "${FABLE2_METAL_DEPS}/generated"
    "${SDK}/include" "${SDK}" "${CMAKE_CURRENT_SOURCE_DIR}" "${FABLE2_METAL_DEPS}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/diagnostics")
target_compile_definitions(fable2-msl-xlate PRIVATE REX_PLATFORM_MAC=1)
target_compile_options(fable2-msl-xlate PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/compat.h"
    -fno-strict-aliasing -ffp-model=strict -fno-char8_t -ferror-limit=8)
target_link_libraries(fable2-msl-xlate PRIVATE rex::runtime rex::xxhash)
