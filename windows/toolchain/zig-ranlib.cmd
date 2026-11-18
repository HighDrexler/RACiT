@echo off
rem A plain line, not an if (...) else (...) block: cmd fails to parse a block
rem whose expanded %* is long. zig comes from this repo's tools\venv if it has
rem one, else from rac-acit's (the shared toolchain), unless ZIG_EXE is set.
if not defined ZIG_EXE if exist "%~dp0..\..\tools\venv\Lib\site-packages\ziglang\zig.exe" set "ZIG_EXE=%~dp0..\..\tools\venv\Lib\site-packages\ziglang\zig.exe"
if not defined ZIG_EXE set "ZIG_EXE=%~dp0..\..\..\rac-acit\tools\venv\Lib\site-packages\ziglang\zig.exe"
"%ZIG_EXE%" ranlib %*
