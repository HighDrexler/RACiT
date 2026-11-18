# Native build for the Android APK: SDL2 (from source), glslang (fetched),
# then the game project itself, which pulls in the ps3recomp runtime and the
# lifted PPU/SPU code and produces libmain.so.
cmake_minimum_required(VERSION 3.22)
project(racacit_android C CXX)

get_filename_component(RACACIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
message(STATUS "RaC ACiT repo: ${RACACIT_ROOT}")

# ---- SDL2 (window, input, audio, Vulkan surface) ---------------------------
set(SDL_SHARED ON  CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
add_subdirectory("${RACACIT_ROOT}/tools/SDL2-src" "${CMAKE_BINARY_DIR}/SDL2")

# ---- glslang: HLSL -> SPIR-V for the RSX shader decompilers ----------------
# tools/glslang-src if present, else fetched at configure time (needs git).
include(FetchContent)
set(ENABLE_OPT              OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL             ON  CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS           OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL          OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
if(EXISTS "${RACACIT_ROOT}/tools/glslang-src/CMakeLists.txt")
    # Vendored copy (the source bundle ships one): no git or network needed.
    add_subdirectory("${RACACIT_ROOT}/tools/glslang-src" "${CMAKE_BINARY_DIR}/glslang" EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG        15.1.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(glslang)
endif()
foreach(t glslang SPIRV glslang-default-resource-limits)
    if(TARGET ${t} AND NOT TARGET glslang::${t})
        add_library(glslang::${t} ALIAS ${t})
    endif()
endforeach()

# ---- code generation for the game and the runtime ---------------------------
# API29 provides native ELF TLS. -Bsymbolic binds internal calls and data
# directly within the game library. Keep Cortex-A53 erratum handling. NDK29 plus
# explicit 16KiB ELF alignment supports modern Android loaders; runtime page-size
# assumptions must be checked separately.
# As the Windows build: no frame pointer (a register for the lifted code), no errno
# from math calls, and no stack canary in every lifted function with a local array.
# Every 64-bit Android CPU: plain ARMv8.0, whose atomics call a helper that checks once for
# LSE, so an older CPU runs slowly rather than dying on its first LSE instruction.
# RACIT_MARCH=armv8.2-a+fp16+dotprod+rcpc in the environment builds for newer CPUs only.
# Code scheduled for the big cores that run the game.
if(NOT "$ENV{RACIT_MARCH}" STREQUAL "")
    add_compile_options(-march=$ENV{RACIT_MARCH})
else()
    add_compile_options(-march=armv8-a -moutline-atomics)
endif()
add_compile_options(-mtune=cortex-a715)
add_compile_options(-fomit-frame-pointer -fno-math-errno -fno-stack-protector)
# LLVM stops following a pointer's uses at 100 and then calls it captured: in a lifted function, where every
# guest register is a use of ctx, that threw away what `restrict` says, and each store and asm reloaded them.
add_compile_options("SHELL:-mllvm -capture-tracking-max-uses-to-explore=100000")
# Profile-guided build: RACIT_PGO=gen instruments, RACIT_PGO=<file>.profdata uses a merged run.
if("$ENV{RACIT_PGO}" STREQUAL "gen")
    add_compile_options(-fprofile-generate "SHELL:-mllvm -disable-vp=true")
    add_compile_definitions(RACIT_PGO_GEN)
    add_link_options(-fprofile-generate)
elseif(NOT "$ENV{RACIT_PGO}" STREQUAL "")
    add_compile_options("-fprofile-use=$ENV{RACIT_PGO}" -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
                        -Wno-backend-plugin "SHELL:-mllvm -pgso=false")
    add_link_options("-fprofile-use=$ENV{RACIT_PGO}")
endif()
add_link_options(-Wl,-Bsymbolic -Wl,-z,max-page-size=16384)

# ---- the game ---------------------------------------------------------------
set(PS3RECOMP_RSX_VULKAN ON CACHE BOOL "" FORCE)
set(PS3RECOMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PS3RECOMP_DIR "${RACACIT_ROOT}/tools/ps3recomp")
set(RECOMP_DIR "${RACACIT_ROOT}/game/src/recomp" CACHE PATH "" FORCE)
set(SPU_DIR    "${RACACIT_ROOT}/game/spu"        CACHE PATH "" FORCE)
# Pinned Vulkan headers if present (the NDK sysroot's own work too).
if(EXISTS "${RACACIT_ROOT}/tools/Vulkan-Headers/include/vulkan/vulkan.h")
    set(PS3RECOMP_VULKAN_INCLUDE_DIR "${RACACIT_ROOT}/tools/Vulkan-Headers/include" CACHE PATH "" FORCE)
endif()
add_subdirectory("${RACACIT_ROOT}/game" "${CMAKE_BINARY_DIR}/game")
