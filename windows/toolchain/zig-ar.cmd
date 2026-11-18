@echo off
rem A plain line, not an if (...) else (...) block: cmd fails to parse a block
rem whose expanded %* is long (an archive of every glslang object under a deep path).
if not defined ZIG_EXE set "ZIG_EXE=%~dp0..\..\tools\venv\Lib\site-packages\ziglang\zig.exe"
"%ZIG_EXE%" ar %*
