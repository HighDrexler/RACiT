# Windows build (personal use): the same game project as Linux/Android, with
# SDL2 for window/input/audio and the Vulkan RSX backend. Cross-compiled from
# Linux with tools/zig-win/toolchain.cmake, or built natively with clang.
cmake_minimum_required(VERSION 3.22)
project(racacit_windows C CXX)

get_filename_component(RACACIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# ---- SDL2 (prebuilt by scripts/build_windows.sh into tools/sdl2-win) -------
list(APPEND CMAKE_PREFIX_PATH "${RACACIT_ROOT}/tools/sdl2-win")
find_package(SDL2 REQUIRED CONFIG)

# ---- glslang (vendored): HLSL -> SPIR-V ------------------------------------
set(ENABLE_OPT              OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL             ON  CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS           OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL          OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
add_subdirectory("${RACACIT_ROOT}/tools/glslang-src" "${CMAKE_BINARY_DIR}/glslang" EXCLUDE_FROM_ALL)
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
set(PS3RECOMP_VULKAN_INCLUDE_DIR "${RACACIT_ROOT}/tools/Vulkan-Headers/include" CACHE PATH "" FORCE)
add_subdirectory("${RACACIT_ROOT}/game" "${CMAKE_BINARY_DIR}/game")
