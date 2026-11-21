cmake_minimum_required(VERSION 3.20)
project(RaCACiTRecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/ps3recomp" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
# The lifted code is built for AVX2 + FMA (main() checks the CPU); the runtime too.
# x86-64 hosts: the lifted code is built for AVX2 + FMA (hardware fma for PPC
# fmadd; main() checks the CPU), and so is the runtime. Other hosts keep the
# baseline flags.
set(RACACIT_SIMD_FLAGS "")
# MinGW: the lifted code's extern data (the guest-memory gate, the SPU
# reservation bitmap) is in this image, never a DLL -- address it directly
# instead of through an auto-import stub (.refptr), one load less per access.
set(RACACIT_LIFTED_FLAGS "")
if(WIN32 AND NOT MSVC)
    set(RACACIT_LIFTED_FLAGS "-fno-auto-import")
endif()
# RACACIT_CPU_COMPAT=ON: the older-CPU build, RaCACiT_compat.exe, for CPUs without
# AVX2/FMA/BMI2 (Sandy/Ivy Bridge, AMD FX and older). x86-64-v2 (SSE4.2 + POPCNT,
# Intel Nehalem / AMD Bulldozer, 2008-2011 on). PPC fmadd then goes through
# libm's exact software fma(): the same results, slower. The launcher picks
# this exe on such CPUs (Settings > Performance > Game build). A build tree of
# its own: game/build-win-compat.
option(RACACIT_CPU_COMPAT "Build for x86-64 CPUs without AVX2 (RaCACiT_compat.exe)" OFF)
if(NOT ANDROID AND (WIN32 OR CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X86_64"))
    if(RACACIT_CPU_COMPAT)
        set(RACACIT_SIMD_FLAGS "-msse4.2;-mpopcnt")
        set(PS3RECOMP_AVX2 OFF CACHE BOOL "" FORCE)
    else()
        set(RACACIT_SIMD_FLAGS "-mavx2;-mfma;-mbmi2")
        set(PS3RECOMP_AVX2 ON CACHE BOOL "" FORCE)
    endif()
endif()
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)
# racacit: cellSpurs' native Edge zlib queue needs the vendored zlib headers.
target_include_directories(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/zlib")

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c policy_modules.c jobmods_register.c taskset_policy_bytes.c spu_jobs.c spu_extra.c spu_sub.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# Real libsre (cellSpurs) LLE -- present only after scripts/lift_libsre.py.
set(PRX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prx" CACHE PATH
    "Directory holding firmware-derived PPU sources for this game build")
file(GLOB LIBSRE_SOURCES CONFIGURE_DEPENDS
    "${PRX_DIR}/libsre/*.cpp"
    "${PRX_DIR}/libsre_module.c")
if(LIBSRE_SOURCES)
    message(STATUS "libsre LLE:      enabled")
endif()

# zlib (vendored) -- the Edge zlib inflate queue is done natively.
file(GLOB ZLIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../tools/zlib/*.c")
list(FILTER ZLIB_SOURCES EXCLUDE REGEX "/gz[a-z]+\\.c$")

# Android: SDLActivity loads the game as libmain.so and calls SDL_main.
if(ANDROID)
    set(RACACIT_TARGET_KIND SHARED)
    add_library(${PROJECT_NAME} SHARED)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME main)
else()
    add_executable(${PROJECT_NAME})
endif()
# Discord Rich Presence: the Application ID registered for this port at
# https://discord.com/developers/applications. Public (every copy uses the
# same one); with it built in, the status works for every player with no
# setup. Empty = no Rich Presence.
set(RACACIT_DISCORD_APP_ID "" CACHE STRING "Discord Application ID built into the game")
if(RACACIT_DISCORD_APP_ID)
    set_source_files_properties(discord_rpc.cpp PROPERTIES
        COMPILE_DEFINITIONS "RACACIT_DISCORD_APP_ID=${RACACIT_DISCORD_APP_ID}")
endif()
set(HOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH
    "Directory holding version-selected main.cpp and stubs.cpp")
target_sources(${PROJECT_NAME} PRIVATE
    "${HOST_DIR}/main.cpp"
    ${ZLIB_SOURCES}
    "${HOST_DIR}/stubs.cpp"
    mods.cpp
    button_prompts.cpp
    menu_archive.cpp
    desktop_mouse.cpp
    discord_rpc.cpp
    ${LIBSRE_SOURCES}
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)
# Every hook on the lift. The lift is generated once and patched by
# scripts/patch_lift.py, so a lift patched before a hook was added would build
# without it. Before the game compiles, scripts/check_lift_hooks.py re-runs the
# patcher when it, a hook table under versions/ or the lift is newer than the
# last check, and stops the build if any hook in
# versions/<profile>-hook-symbols.txt is missing. The profile comes from the
# version-selected host sources (scripts/prepare_game_host.py writes it into
# game_version.h); RACACIT_ELF is the executable re-patching needs for an update.
set(RACACIT_GAME_PROFILE "" CACHE STRING "Game profile of RECOMP_DIR (empty: from HOST_DIR/game_version.h, else BCUS98124-1.00)")
set(RACACIT_ELF "" CACHE FILEPATH "Decrypted EBOOT.ELF of RECOMP_DIR's lift (needed to re-patch an update lift)")
set(RACACIT_CHECK_HOOKS ON CACHE BOOL "Re-patch the lift when needed and stop on missing hooks")
if(RACACIT_CHECK_HOOKS)
    set(_racit_profile "${RACACIT_GAME_PROFILE}")
    if(NOT _racit_profile AND EXISTS "${HOST_DIR}/game_version.h")
        file(STRINGS "${HOST_DIR}/game_version.h" _racit_profile_line REGEX "RACACIT_GAME_PROFILE_ID")
        string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" _racit_profile "${_racit_profile_line}")
    endif()
    if(NOT _racit_profile)
        set(_racit_profile "BCUS98124-1.00")
    endif()
    set(_racit_elf "${RACACIT_ELF}")
    if(NOT _racit_elf AND _racit_profile STREQUAL "BCUS98124-1.00" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../input/EBOOT.ELF")
        set(_racit_elf "${CMAKE_CURRENT_SOURCE_DIR}/../input/EBOOT.ELF")
    endif()
    message(STATUS "Lift hooks:      ${_racit_profile} (versions/${_racit_profile}-hook-symbols.txt)")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_racit_root "${CMAKE_CURRENT_SOURCE_DIR}/..")
    file(GLOB _racit_hook_inputs
        "${_racit_root}/scripts/patch_lift.py" "${_racit_root}/scripts/update120_hooks.py"
        "${_racit_root}/scripts/fix_vmx_alias.py" "${_racit_root}/scripts/game_version.py"
        "${_racit_root}/scripts/check_lift_hooks.py" "${_racit_root}/versions/*")
    # Re-patch here, at configure time, before the build has looked at the lifted sources:
    # a source rewritten during the build would not be recompiled. A change to the patcher
    # or a hook list configures (and so re-patches) again.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_racit_hook_inputs})
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -I -X utf8 "${_racit_root}/scripts/check_lift_hooks.py" "${RECOMP_DIR}"
                --profile "${_racit_profile}" --elf "${_racit_elf}" --patch
                --stamp "${CMAKE_CURRENT_BINARY_DIR}/lift-patched.ok"
        RESULT_VARIABLE _racit_patch_result)
    if(NOT _racit_patch_result EQUAL 0)
        message(FATAL_ERROR "The lift in ${RECOMP_DIR} is missing hooks (see above)")
    endif()
    # The build only checks again (it never writes a source): a lift regenerated or edited
    # since this configure, and not patched, stops the build.
    set(_racit_stamp "${CMAKE_CURRENT_BINARY_DIR}/lift-hooks.ok")
    add_custom_command(OUTPUT "${_racit_stamp}"
        COMMAND "${Python3_EXECUTABLE}" -I -X utf8 "${_racit_root}/scripts/check_lift_hooks.py" "${RECOMP_DIR}"
                --profile "${_racit_profile}" --stamp "${_racit_stamp}"
        DEPENDS ${_racit_hook_inputs} ${RECOMP_SOURCES}
        COMMENT "Checking the lift's hooks (${_racit_profile})"
        VERBATIM)
    add_custom_target(racit_lift_hooks ALL DEPENDS "${_racit_stamp}")
    add_dependencies(${PROJECT_NAME} racit_lift_hooks)
endif()
# Android: the dual-screen companion's JNI link (android/app).
if(ANDROID)
    target_sources(${PROJECT_NAME} PRIVATE android_companion.cpp android_profiler.cpp)
endif()

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   SPU_LS_DIAG=0       -- local-store accesses without the runtime's watch and
#                          probe hooks (diagnostics; each was a possible call on
#                          every access, which spills the register file).
#   SPU_LS_IN_CONTEXT=1 -- (Apple arm64) local-store accesses at ctx->ls_store, so
#                          an SPU store is visibly not a register-file store
#                          (spu_context.h); this title creates no raw SPUs.
#   -O3                 -- the SPU job code is the main thread's critical path
#                          (it waits on the job chain every frame).
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS "SPU_USE_DISPATCH_MT;SPU_LS_DIAG=0")
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # x86-64: SSE4.1 lets spu_helpers.h use its pshufb shufb (the byte-loop
        # fallback was ~60% of the audio mixer's time -- the game's music fell
        # three blocks in four behind real time). Every x86-64 CPU since 2008.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NOT ANDROID)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O3;-msse4.1;-ffp-contract=off;${RACACIT_SIMD_FLAGS};${RACACIT_LIFTED_FLAGS}")
            # The hot modules together in the exe (scripts/dev/hot_funcs.py).
            if(WIN32 AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/hot_spu_modules.cmake")
                include("${CMAKE_CURRENT_LIST_DIR}/hot_spu_modules.cmake")
                foreach(t 0 1)
                    foreach(m ${RACACIT_HOT_SPU${t}})
                        set_property(SOURCE "${SPU_DIR}/${m}/spu_recomp.c" APPEND PROPERTY
                                     COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/hot_spu${t}.h")
                    endforeach()
                endforeach()
            endif()
        else()
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
            if(APPLE)
                set_source_files_properties(${SPU_SOURCES} PROPERTIES
                    COMPILE_DEFINITIONS "SPU_USE_DISPATCH_MT;SPU_LS_DIAG=0;SPU_LS_IN_CONTEXT=1")
            endif()
        endif()
    endif()
endif()
# Android: the SPU job code at the Windows build's -O3, and without FP
# contraction (arm64 fuses a*b+c by default; the SPU rounds each step).
if(ANDROID AND SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/prx"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)
# menu_archive.cpp builds the keyboard menu archive itself (RACACIT_MENU_KEYCAPS=1)
# from the compiled menu actions, so they go into the game as bytes.
set(RACACIT_MENU_ACTIONS "${CMAKE_CURRENT_SOURCE_DIR}/menu_keycaps.actions.bin")
file(READ "${RACACIT_MENU_ACTIONS}" RACACIT_MENU_ACTIONS_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," RACACIT_MENU_ACTIONS_BYTES "${RACACIT_MENU_ACTIONS_HEX}")
set(RACACIT_MENU_ACTIONS_INC "${CMAKE_CURRENT_BINARY_DIR}/menu_keycaps_actions.inc")
set(RACACIT_MENU_ACTIONS_OLD "")
if(EXISTS "${RACACIT_MENU_ACTIONS_INC}")
    file(READ "${RACACIT_MENU_ACTIONS_INC}" RACACIT_MENU_ACTIONS_OLD)
endif()
if(NOT RACACIT_MENU_ACTIONS_OLD STREQUAL "${RACACIT_MENU_ACTIONS_BYTES}")
    file(WRITE "${RACACIT_MENU_ACTIONS_INC}" "${RACACIT_MENU_ACTIONS_BYTES}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RACACIT_MENU_ACTIONS}")
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(ZLIB_SOURCES)
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/zlib")
endif()

if(APPLE)
    find_package(ZLIB REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
    target_link_libraries(ps3recomp_runtime PRIVATE ZLIB::ZLIB)
endif()
if(ANDROID)
    # SDL_main / SDL_AndroidGetExternalStoragePath, and logcat.
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2 log android)
endif()

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
        # The toolkit's D3D12 path is still linked in (cellGcmSys reaches it);
        # these ship with every Windows 10/11 install.
        d3d12
        dxgi
        d3dcompiler_47
    )
    # 256 MB main-thread stack: the primary guest thread runs on it, and
    # lifted code recurses as deep as the title does (Windows defaults to
    # 1 MB; guest threads already get 256 MB from sys_ppu_thread).
    if(MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--stack,268435456")
        # A windowed app: no console window behind the game (RACACIT_CONSOLE=1
        # opens one on demand). Keep the ordinary main() entry, reached through
        # racacit_entry (main.cpp): the CPU check, before any global constructor.
        target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--subsystem,windows" "-Wl,-e,racacit_entry")
        # zig writes RaCACiT.pdb beside the exe: crash offsets in log.txt
        # resolve with llvm-symbolizer --obj=RaCACiT.exe 0x140000000+off.
    endif()
    if(RACACIT_CPU_COMPAT)
        set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME RaCACiT_compat)
    else()
        set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME RaCACiT)
    endif()
endif()
# main.cpp's CPU check asks for what this build was compiled for.
if(RACACIT_CPU_COMPAT)
    # The source actually compiled: game/main.cpp for the disc, the generated copy for 1.20.
    set_property(SOURCE "${HOST_DIR}/main.cpp" APPEND PROPERTY COMPILE_DEFINITIONS RACACIT_CPU_COMPAT=1)
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The Vulkan RSX backend (Linux/Android): main.cpp picks it at run time when
# the toolkit was configured with -DPS3RECOMP_RSX_VULKAN=ON.
if(PS3RECOMP_RSX_VULKAN)
    # On main.cpp alone: a target-wide define would change the command line of
    # every lifted translation unit and rebuild hours of generated code.
    set_property(SOURCE "${HOST_DIR}/main.cpp" APPEND PROPERTY COMPILE_DEFINITIONS RACACIT_HAVE_VULKAN=1)
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
# The hot functions together in the exe (scripts/dev/hot_funcs.py): only where they sit changes.
set(RACACIT_HOT_PPU "")
if(WIN32 AND NOT MSVC AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/hot_ppu.h")
    set(RACACIT_HOT_PPU "-include;${CMAKE_CURRENT_LIST_DIR}/hot_ppu.h")
endif()
# Code mods (mods.cpp, docs/MODDING.md): every lifted function gets a 16-byte hook
# pad -- 14 bytes of nop before its entry, a 2-byte nop at it -- so a mod's hook is
# two aligned stores, no instruction decoding. The entry nop costs one cycle a call.
# mod_hookable.h keeps the functions mods hook from being folded into their callers.
set(RACACIT_HOOK_FLAGS "")
if(WIN32 AND NOT MSVC AND NOT ANDROID)
    set(RACACIT_HOOK_FLAGS "-fpatchable-function-entry=16,14;-include;${CMAKE_CURRENT_LIST_DIR}/mod_hookable.h")
endif()
# macOS: the same idea through the linker. mac/macos.order lists the functions a
# profile of the gameplay route sampled (lifted PPU and SPU code and the runtime
# they call), hottest first; ld64 places them together. Names missing from a
# build are skipped. Only where code sits changes.
if(APPLE AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../mac/macos.order")
    target_link_options(${PROJECT_NAME} PRIVATE "LINKER:-order_file,${CMAKE_CURRENT_LIST_DIR}/../mac/macos.order")
endif()
if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;${RACACIT_HOT_PPU};${RACACIT_HOOK_FLAGS};-ffp-contract=off;${RACACIT_SIMD_FLAGS};${RACACIT_LIFTED_FLAGS}")
endif()
# Guest loads and stores of ordinary memory inlined into the lifted code
# (ppu_recomp.h); the runtime keeps calling the out-of-line accessors.
set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS PPU_RECOMP_INLINE_VM)
# ...without a per-access test for the GCM control page: main() sets
# ppu_inl_clip_hole, so ppu_run's window already ends below it.
set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS PPU_INL_NO_HOLE)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
