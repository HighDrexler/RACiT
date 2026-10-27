@echo off
if defined ZIG_EXE ("%ZIG_EXE%" ar %*) else ("%~dp0..\..\tools\venv\Lib\site-packages\ziglang\zig.exe" ar %*)
