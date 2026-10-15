# ensure_recomp_manifest.cmake
#
# Idempotently ensure every external-Lua script under <lua_dir> has a matching
# entry in each dir.manifest, so the in-game VFS (and the game's custom
# `loadfile`) can resolve them at runtime.
#
# The game indexes data/ from dir.manifest at startup and its loadfile()
# resolves paths relative to the data/ root using forward slashes
# (e.g. "scripts/recomp/getPlayerPos.lua"). The manifest stores the same path
# with backslashes and relative to the data/ root
# (e.g. "scripts\recomp\getPlayerPos.lua"). A file that is NOT in the manifest
# is invisible to the VFS, so loadfile() returns nil for it.
#
# Usage (from a CMake custom command):
#   cmake -P ensure_recomp_manifest.cmake
#     -Dlua_dir=<absolute path to the staged src/lua folder>
#     -Dmanifests="<abs path to dir.manifest 1>;<abs path to dir.manifest 2>;..."
#
# For every <name>.lua in lua_dir, the key "scripts\recomp\<name>" is added to
# each manifest (CRLF-terminated) if it is not already a whole line. Existing
# manifest lines are otherwise left untouched, so re-staging / re-running the
# game content never clobbers other entries.

if(NOT DEFINED lua_dir)
  message(FATAL_ERROR "ensure_recomp_manifest: -Dlua_dir=<path> is required")
endif()
if(NOT DEFINED manifests)
  message(FATAL_ERROR "ensure_recomp_manifest: -Dmanifests=<path>... is required")
endif()

if(NOT IS_DIRECTORY "${lua_dir}")
  message(WARNING "ensure_recomp_manifest: lua_dir not found: ${lua_dir}")
  return()
endif()

# Collect the manifest keys we need (one per .lua in lua_dir).
file(GLOB _recomp_lua "${lua_dir}/*.lua")
set(_required_keys "")
foreach(_f IN LISTS _recomp_lua)
  get_filename_component(_name "${_f}" NAME)
  # scripts\recomp\<name>  (\\ -> single backslash in the CMake string)
  list(APPEND _required_keys "scripts\\recomp\\${_name}")
endforeach()

foreach(_man IN LISTS manifests)
  if(NOT EXISTS "${_man}")
    # No manifest yet: start an empty one so the keys still get recorded.
    file(WRITE "${_man}" "")
  endif()

  # Read the manifest as a list of whole lines (strip CR, drop blanks).
  file(STRINGS "${_man}" _lines)
  set(_have_lines "")
  foreach(_l IN LISTS _lines)
    string(REPLACE "\r" "" _l "${_l}")
    if(NOT _l STREQUAL "")
      list(APPEND _have_lines "${_l}")
    endif()
  endforeach()

  set(_missing "")
  foreach(_key IN LISTS _required_keys)
    list(FIND _have_lines "${_key}" _idx)
    if(_idx EQUAL -1)
      list(APPEND _missing "${_key}")
    endif()
  endforeach()

  if(NOT _missing)
    continue()
  endif()

  # Newline bytes for this platform. The game's dir.manifest is CRLF. On
  # Windows CMake's file(APPEND) already translates a lone "\n" into "\r\n",
  # so we emit "\n" there; on POSIX the bytes are written verbatim, so we emit
  # an explicit "\r\n". Either way each appended line ends up CRLF-terminated.
  if(WIN32)
    set(_nl "\n")
  else()
    set(_nl "\r\n")
  endif()

  # If the manifest is non-empty and does not already end with a newline,
  # start a fresh line before appending.
  file(READ "${_man}" _content)
  set(_append "")
  if(NOT _content STREQUAL "" AND NOT _content MATCHES "\n$")
    string(APPEND _append "${_nl}")
  endif()
  foreach(_key IN LISTS _missing)
    string(APPEND _append "${_key}${_nl}")
  endforeach()

  file(APPEND "${_man}" "${_append}")
  message(STATUS "ensure_recomp_manifest: added ${_missing} to ${_man}")
endforeach()
