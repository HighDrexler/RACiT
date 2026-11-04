RACiT Launcher 0.9.8 -- Ratchet & Clank Future: A Crack in Time (PC), Windows
================================================================================

Everything the game needs comes in this folder: the launcher, the game build
(builds\vNN) and its shader caches. The only thing you add is your own copy of
A Crack in Time (USA, BCUS98124):
  1. the game: its disc image (.iso), or the dumped game's folder (USRDIR)
  2. EBOOT.elf, the game's main program, unlocked with RPCS3

SET UP
1. Extract the zip anywhere (with about 27 GB free on that drive for the
   game) and open "RaCACiT Launcher.exe". Windows may warn about an unknown
   app (it is not signed): More info > Run anyway.
2. The game, either way:
   - "1. Import the game: a .iso": pick your .iso. The launcher unpacks the
     game into its game\ folder. The .iso is not changed; you can delete it
     after. A Redump .iso is encrypted: keep the .dkey file that comes with
     it beside the .iso, with the same name (Game.iso + Game.dkey), and the
     launcher unlocks it while importing. A decrypted .iso needs no key.
   - "or pick the game's folder (USRDIR)": a dumped game folder, played
     where it is. Pick its PS3_GAME\USRDIR folder (PS3_GAME, or the folder
     holding it, works too). If a decrypted EBOOT.elf is already in USRDIR,
     the launcher finds it and set up is done.
3. "2. Add EBOOT.elf": pick your EBOOT.elf.
   You must provide your own legally dumped copy of Ratchet & Clank Future:
   A Crack in Time, and the EBOOT.elf file from your dump. If your EBOOT.elf
   reads as encrypted, RPCS3 has a decryption tool available. RACiT does
   not and will not assist with acquiring the .iso dump of the game.
   Decrypting with RPCS3 (rpcs3.net):
     a. Install RPCS3 and set it up with the PS3 system software (its
        quickstart guide shows how).
     b. In RPCS3: Utilities > Decrypt PS3 Binaries.
     c. Pick EBOOT.BIN from the game's PS3_GAME\USRDIR folder. No EBOOT.BIN
        to hand? Do step 2 first; then "Show EBOOT.BIN" in the launcher opens
        the folder it is in.
     d. RPCS3 writes EBOOT.elf next to it: pick that one.
   Steps 2 and 3 work in either order.
4. Play. Settings (display, graphics, controller) can be changed any time
   from the main menu, or in the game: F1, the touchpad on a DualSense, or
   hold Select (View) for a moment on any other controller.
Coming from RACiT Launcher 0.8? Set up offers to bring it over (or lets you pick
its folder): its saves, settings, save backups and shader caches are copied, and
its game is played from where it is, so keep that folder or import the .iso again.
The 0.8 folder itself is not changed.

UPDATES
The launcher looks for a new release on GitHub when it opens (at most every 6
hours; More > Check for updates looks now, and can turn this off). If there is
one, the main menu says so. You see what is new, download it, and confirm:
the launcher backs up your saves, puts in the new game build and launcher, and
starts again. Your saves, settings, shader caches, game and any mods you added
stay as they are. A failed or cancelled download changes nothing.

Each Play backs up your saves first, then starts the game; the launcher
minimises and comes back when the game ends.

Needs: Windows 10 or 11 (64-bit) and a graphics card with Vulkan support (any
recent driver). Nothing else to install.

SCREENS
  Play              backs up your saves, then starts the game
  Settings          display, graphics, performance, controller, keyboard and mouse
  Saves             back up, export to .zip, import, restore, bring over a 0.8
                    launcher or an older release
  More              your game, mods, logs (for bug reports), updates, help
Controls: mouse; keyboard (arrows, Enter, Esc); or a controller (D-pad/stick,
Cross/A select, Circle/B back, START plays). The controller is released while
the game runs.

FOLDERS
  builds\           the game build Play starts (the newest one here); an update
                    keeps the one before it too
  game\             your game, unpacked from the .iso
  data\             settings.txt, dev_hdd0 (the PS3 hard drive with your
                    saves), cache (shaders), log.txt and logs
  backups\          save backups
  mods\             mods; DualSense Plus comes with the launcher (off)

KEYBOARD AND MOUSE
  Move W A S D (Left Alt held: walk)    Camera  the mouse (click in the game
  Jump Space / Enter                            window first), or I J K L
  Fire left click                       Strafe  right click
  Melee F                               Quick select  Q
  Circle E / Backspace                  L2 Left Ctrl     R2 Left Shift
  L3 V                                  R3 middle click / B
  Pause Esc / P                         Map Tab / M
  D-pad arrow keys, or 1 2 3 4 (up, right, down, left)
  In the game, F1 opens the settings (on a controller: the touchpad, or hold
  Select).
  Settings > Key bindings changes any of them: pick a button, then press the
  key or mouse button for it. Its "By key" view puts one key on several
  buttons at once (say H for D-pad right and R2). Mouse speed and invert are
  in Settings too. Keyboard and mouse work alongside a controller.

CONTROLLERS
  Any controller works as it is: Xbox, PlayStation, Switch Pro, Steam Input's
  virtual pad and most others, with the PS3's own layout and button prompts.
  DualSense Plus (Settings > Controller, or More > Mods) adds a PS5
  DualSense's extras: game sounds on its speaker, haptic feedback, adaptive
  triggers, the light bar, motion, and fire on R2 (Rift Apart layout) with
  prompts to match. Turning it off keeps your DualSense choices for later.
