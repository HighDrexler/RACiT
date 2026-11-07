@echo off
rem %* once per line: expanded twice, a long archive list passes cmd's 8191 limit.
if defined ZIG_EXE goto env
"%~dp0..\..\tools\venv\Lib\site-packages\ziglang\zig.exe" ar %*
exit /b %errorlevel%
:env
"%ZIG_EXE%" ar %*
