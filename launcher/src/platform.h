// The file names that differ between the Windows and the macOS launcher, in one place.
//   Windows: <folder>\RaCACiT Launcher.exe, builds\vNN\RaCACiT.exe
//   macOS:   <folder>/RaCACiT Launcher.app, builds/vNN/RaCACiT
// <folder> holds data, game, builds, mods and backups on both.
#pragma once

#ifdef _WIN32
#define RACIT_GAME_EXE L"RaCACiT.exe"
#define RACIT_LAUNCHER_FILE L"RaCACiT Launcher.exe"
#else
#define RACIT_GAME_EXE L"RaCACiT"
#define RACIT_LAUNCHER_FILE L"RaCACiT Launcher.app"
#endif
