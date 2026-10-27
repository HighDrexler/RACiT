# Windows build with zig's bundled clang (x86_64-windows-gnu): no Visual
# Studio, no MinGW install. zig comes from pip (`pip install ziglang`); point
# ZIG_EXE at it, or keep the default, the repository's tools\venv.
#   cmake -S windows -B game/build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=windows/toolchain/zig-windows.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(DEFINED ENV{ZIG_EXE})
    set(ZIG_EXE "$ENV{ZIG_EXE}")
else()
    get_filename_component(ZIG_EXE "${CMAKE_CURRENT_LIST_DIR}/../../tools/venv/Lib/site-packages/ziglang/zig.exe" ABSOLUTE)
endif()
if(NOT EXISTS "${ZIG_EXE}")
    message(FATAL_ERROR "zig not found at ${ZIG_EXE}: pip install ziglang into tools\venv, or set ZIG_EXE")
endif()
set(CMAKE_C_COMPILER   "${ZIG_EXE}" cc  -target x86_64-windows-gnu)
set(CMAKE_CXX_COMPILER "${ZIG_EXE}" c++ -target x86_64-windows-gnu)
set(CMAKE_RC_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-rc.cmd")
set(CMAKE_AR     "${CMAKE_CURRENT_LIST_DIR}/zig-ar.cmd")
set(CMAKE_RANLIB "${CMAKE_CURRENT_LIST_DIR}/zig-ranlib.cmd")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
