@echo off
if defined ZIG_EXE ("%ZIG_EXE%" rc %*) else ("%~dp0..\..\tools\venv\Lib\site-packages\ziglang\zig.exe" rc %*)
