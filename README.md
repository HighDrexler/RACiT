<h1 align="center">
  <img src="assets/readme/racit-wordmark.svg" alt="RACiT" width="420">
</h1>

<h3 align="center"><a href="https://discord.gg/p6rypUkvb">Join the discord for updates!</a></h3>

<p align="center">
  <img src="assets/readme/Racitim1.jpg" alt="Two armored characters inside a gold circular frame against a green and gold background." width="560">
</p>

RACiT is an unofficial, personal-use static recompilation of **Ratchet & Clank: A Crack in Time** for PlayStation 3. It is not affiliated with Sony, Insomniac Games, or the game's rights holders.

## Game data requirements

The project does not include game data. You must supply your own decrypted dump of the **North American disc release, title ID `BCUS98124`**, including:

- `EBOOT.ELF`
- The `USRDIR` directory

Detailed installation and launch instructions will be added once confirmed.

## Recompilation features

- **Native recompilation:** PS3 PPU and SPU code is lifted to native C/C++, using ps3recomp.
- **Vulkan rendering:** a Vulkan-based RSX renderer.
- **DualSense integration:** support for the PS5 controller's speaker and triggers.

## Platforms and current status

| Platform | Status |
| --- | --- |
| Windows | Target platform; a launcher preview exists. |
| Android (ARM64) | Target platform. |
| Linux | Build for debugging. |

Work in progress; known issues remain, including a reported level-14 freeze.
