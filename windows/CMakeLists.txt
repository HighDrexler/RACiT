# Windows build (personal use): the same game project as Linux/Android, with
# SDL2 for window/input/audio and the Vulkan RSX backend. Cross-compiled from
# Linux with tools/zig-win/toolchain.cmake, or built natively with clang.
cmake_minimum_required(VERSION 3.22)
project(nexus_windows C CXX)

get_filename_component(RACACIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Toolchains and vendored libraries (SDL2, glslang, Vulkan headers, zlib) are
# shared with RACiT: they stay in rac-acit/tools and are never copied or moved.
# Only the runtime (tools/ps3recomp) is this port's own. A folder that exists
# under this repository's tools/ wins over the shared one.
get_filename_component(_shared "${RACACIT_ROOT}/../rac-acit/tools" ABSOLUTE)
set(NEXUS_SHARED_TOOLS "${_shared}" CACHE PATH "Shared toolchain folder (rac-acit/tools)")
function(nexus_tool out name)
    if(EXISTS "${RACACIT_ROOT}/tools/${name}")
        set(${out} "${RACACIT_ROOT}/tools/${name}" PARENT_SCOPE)
    else()
        set(${out} "${NEXUS_SHARED_TOOLS}/${name}" PARENT_SCOPE)
    endif()
endfunction()
nexus_tool(NEXUS_SDL2_DIR    sdl2-win)
nexus_tool(NEXUS_GLSLANG_DIR glslang-src)
nexus_tool(NEXUS_VKHDR_DIR   Vulkan-Headers)
nexus_tool(NEXUS_ZLIB_DIR    zlib)
set(NEXUS_ZLIB_DIR "${NEXUS_ZLIB_DIR}" CACHE PATH "" FORCE)

# zig cc keeps a frame pointer in every function by default, a register the lifted
# code could use; and on this target clang assumes math calls may set errno, so sqrt()
# stayed a real call (to zig's software square root) even with AVX2 on. Neither changes
# a result: x86's square root is correctly rounded, as the software one was.
if(NOT MSVC)
    add_compile_options(-fomit-frame-pointer -fno-math-errno)
endif()

# ---- SDL2 (prebuilt by scripts/build_windows.sh into tools/sdl2-win) -------
list(APPEND CMAKE_PREFIX_PATH "${NEXUS_SDL2_DIR}")
find_package(SDL2 REQUIRED CONFIG)

# ---- glslang (vendored): HLSL -> SPIR-V ------------------------------------
set(ENABLE_OPT              OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL             ON  CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS           OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL          OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
add_subdirectory("${NEXUS_GLSLANG_DIR}" "${CMAKE_BINARY_DIR}/glslang" EXCLUDE_FROM_ALL)
foreach(t glslang SPIRV glslang-default-resource-limits)
    if(TARGET ${t} AND NOT TARGET glslang::${t})
        add_library(glslang::${t} ALIAS ${t})
    endif()
endforeach()

# ---- the game ----------------------------------------------------------------
set(PS3RECOMP_RSX_VULKAN   ON  CACHE BOOL "" FORCE)
set(PS3RECOMP_WIN_USE_SDL2 ON  CACHE BOOL "" FORCE)
set(PS3RECOMP_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(PS3RECOMP_DIR "${RACACIT_ROOT}/tools/ps3recomp")
set(RECOMP_DIR "${RACACIT_ROOT}/game/src/recomp" CACHE PATH "" FORCE)
set(SPU_DIR    "${RACACIT_ROOT}/game/spu"        CACHE PATH "" FORCE)
set(PS3RECOMP_VULKAN_INCLUDE_DIR "${NEXUS_VKHDR_DIR}/include" CACHE PATH "" FORCE)
add_subdirectory("${RACACIT_ROOT}/game" "${CMAKE_BINARY_DIR}/game")
