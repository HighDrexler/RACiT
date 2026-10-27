<h1 align="center">
  <img src="assets/readme/racit-wordmark.svg" alt="RACiT" width="420">
</h1>

<h3 align="center"><a href="https://discord.gg/p6rypUkvb">Join the discord for updates!</a></h3>

<p align="center">
  <img src="assets/readme/Racitim1.jpg" alt="Two armored characters inside a gold circular frame against a green and gold background." width="360">
</p>

## About

RACiT is Recomped A Crack in Time.

RACiT is a project to bring **Ratchet & Clank: A Crack in Time** (the greatest game ever made) to Windows and Android through static recompilation. It takes the original PS3 code and converts it to native C/C++, using ps3recomp.

The Android target is ARM64. There is also a Linux build for debugging (Android and Linux builds not yet public)

## Features

- Vulkan-based RSX renderer
- DualSense (PS5 controller) speaker and trigger mod integrated
- General controller support for most windows compatible controllers.
- Mouse and keyboard controls (bindable in the launcher)
- Added toggles for enhanced internal resolution and graphics settings

## What you'll need

Game data isn't included. You'll need your own decrypted dump of the **North American disc version (`BCUS98124`)**, including:
 !!A LEGALLY DUMPED VERSION OF RATCHET AND CLANK FUTURE: A CRACK IN TIME (NO ASSESTS ARE OR WILL EVER BE INCLUDED WITH THIS PROJECT)!!

- `EBOOT.ELF`
- `USRDIR`

-  After downloading the launcher, extract and run the .exe, which will take you through the setup process, which is importing your .iso directly or directing the launcher to your PS3GAME folder.
 The game will also check for a decrypted EBOOT.elf, if your dumps .elf is showing as encrypted, the easier way to fix is with RPSC3.

## Current status

This is a work in progress.

RACiT is an unofficial project and is not affiliated with Sony, Insomniac Games, or the game's rights holders.
Saves are currently not transferable between windows and android. Feature will be added in the future to sync saves between devices
