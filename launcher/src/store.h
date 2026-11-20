// The launcher's data: where everything lives, the settings file, the game,
// builds and updates, mods, saves, logs, and starting the game.
//
// <launcher folder>
//   RaCACiT Launcher.exe     (macOS: RaCACiT Launcher.app; builds/vNN/RaCACiT, see platform.h)
//   launcher.ini            the launcher's own options
//   builds\v65\             RaCACiT.exe (+ README.txt, version.txt) per installed build
//   builds\current.txt      which one Play starts
//   builds\release.txt      the release last installed through the launcher (v0.9)
//   .update\                a release being downloaded and unpacked; removed after
//   data\settings.txt       game settings: NAME=value, the same names as a release's
//                           settings.txt; the in-game menu and the launcher both edit it
//   data\dev_hdd0\          the PS3 hard drive; saves in home\00000001\savedata
//   data\cache\             shader and pipeline caches
//   data\log.txt            the last session's log; earlier ones in data\logs\
//   game\                   the game, when it was copied in (else GAME= points at it)
//   mods\<id>\mod.json      mods: files\ mirrors the game folder (PS3_GAME\USRDIR\...);
//                           mod.json "settings": {"NAME": "value"} -- game settings the mod
//                           turns on while it is on (DualSense Plus: RACACIT_DUALSENSE_PLUS=1)
//   backups\                save backups, one per Play
//   assets\                 icon and background art taken from the player's game
#pragma once
#include "util.h"

struct SettingsFile {
    fs::path path;
    std::vector<std::string> lines;
    bool load();
    bool save() const;
    std::string get(const std::string& key, const std::string& def = "") const;
    bool has(const std::string& key) const;
    void put(const std::string& key, const std::string& value);   // a value line, or the "#KEY=" template, or appended
    void remove(const std::string& key);                           // its value lines become comments
};

struct GameInfo {
    bool ok = false;
    bool need_decrypt = false;          // the right game, but no decrypted EBOOT.elf yet
    std::string status;                 // "Ready", or what is wrong
    fs::path root;                      // the folder holding PS3_GAME
    std::string title, title_id, app_ver;
};

// One game the launcher plays. RACiT (A Crack in Time) lives in the launcher folder itself,
// as it always has; another game lives in a folder of its own beside it (nexus\) with the
// same layout (data\, builds\, game\, backups\, assets\, mods\).
struct GameDisc {
    const char* id;              // BCUS98124
    const char* region;          // "USA"
    const wchar_t* exe;          // its program in builds\vNN
    const wchar_t* compat_exe;   // the older-processor one, or nullptr
    const char* elf_sha256;      // the 1.00 EBOOT.elf the build is made from
};
struct GameProfile {
    const char* key;             // "racit", "nexus"
    const char* name;            // "A Crack in Time" (screen titles)
    const char* full_name;       // "Ratchet & Clank Future: A Crack in Time"
    const char* brand;           // the header's small line: "RATCHET & CLANK FUTURE"
    const wchar_t* folder;       // under the launcher folder: L"" = the launcher folder itself
    std::vector<GameDisc> discs;
    bool updates;                // releases on GitHub (Updates screen, the check at start)
    std::vector<const char*> save_ids;   // save folder name prefixes (Into the Nexus saves as NPUA80908)
    const char* version;         // the game's version on screen: RACiT "1.0.1", Into the Nexus "0.1.0"
    std::string disc_list() const;   // "BCUS98124 or BCES00511"
};
extern const GameProfile kRacitProfile, kNexusProfile;
extern const GameProfile* g_prof;   // the game on screen: what the Store helpers check against

extern const char* g_launcher_version;   // app.cpp
// AVX2 + FMA + BMI2 with the OS saving YMM state: what RaCACiT.exe needs (game/main.cpp racacit_cpu_ok).
// Without them Play starts RaCACiT_compat.exe, unless RACACIT_GAME_BUILD says otherwise.
bool cpu_has_avx2();

struct ModInfo {
    std::string id, name, version, author, description, problem;
    fs::path dir;
    bool has_files = false;                                       // a files\ folder to overlay
    fs::path code;                                                // mod.json "code": its DLL (runs inside the game), or empty
    std::vector<std::pair<std::string, std::string>> settings;   // mod.json "settings"
};

struct BuildInfo {
    int version = 0;
    fs::path dir;
    std::string notes_first_line;
};

struct ReleaseInfo {
    std::string tag, name, notes, page;   // v0.9, its title, its text, its page on GitHub
    std::string asset, url, sha256;       // the launcher .zip; empty = the release has none
    std::string other_assets;             // what it has instead, for the message
    uint64_t size = 0;
};

class Store {
public:
    explicit Store(fs::path root, const GameProfile* prof = &kRacitProfile);
    fs::path root;
    const GameProfile* prof;
    SettingsFile settings, ini;

    fs::path data() const { return root / L"data"; }
    fs::path hdd() const { return data() / L"dev_hdd0"; }
    fs::path saves() const { return hdd() / L"home" / L"00000001" / L"savedata"; }
    fs::path cache() const { return data() / L"cache"; }
    fs::path log() const { return data() / L"log.txt"; }
    fs::path logs() const { return data() / L"logs"; }
    fs::path builds_dir() const { return root / L"builds"; }
    fs::path mods_dir() const { return root / L"mods"; }
    fs::path backups() const { return root / L"backups"; }
    fs::path assets() const { return root / L"assets"; }
    fs::path game_copy() const { return root / L"game"; }

    void reload();

    // The game (the player's own copy).
    GameInfo game() const;
    static fs::path game_folder_from_pick(const fs::path& picked, std::string* why);   // the folder holding PS3_GAME
    std::string use_game_in_place(const fs::path& picked);
    // The disc image: PS3_GAME and PS3_DISC.SFB out of the .iso into game\ (a Redump
    // .iso unlocked with the .dkey beside it). The game is then played from there.
    std::string import_iso(const fs::path& iso, Progress* p, std::string* summary);
    // EBOOT.elf, the game's main program unlocked with RPCS3: checked, then put in
    // PS3_GAME\USRDIR -- or kept in data\ until the game is imported.
    // check_elf: "" = a PS3 program; *known = the program of a disc the build is made from.
    static std::string check_elf(const fs::path& elf, bool* known);
    std::string add_elf(const fs::path& elf);
    fs::path pending_elf() const { return data() / L"EBOOT.elf"; }
    std::string refresh_art();          // ICON0.PNG / PIC1.PNG from PS3_GAME into assets\ (the game's own art)
    fs::path icon() const { return assets() / L"icon0.png"; }
    fs::path background() const { return assets() / L"pic1.png"; }
    fs::path logo() const { return assets() / L"pic0.png"; }   // the game's title logo

    // An existing release folder (RaCACiT_vNN): its settings, saves and caches.
    std::vector<fs::path> find_old_installs() const;
    std::string migrate_from(const fs::path& old, Progress* p, std::string* summary);
    // An earlier launcher (RACiT Launcher 0.8): its settings, saves, caches and save backups;
    // its game is played where it is. The old folder is not changed.
    std::vector<fs::path> find_old_launchers() const;
    std::string migrate_from_launcher(const fs::path& old, Progress* p, std::string* summary);
    // An older install's per-build trophy files into this one's trophy import folder.
    int import_old_trophies(const fs::path& dir, const std::string& from, bool single = false);

    // Builds: installed under builds\vNN, one is current.
    std::vector<BuildInfo> builds() const;
    BuildInfo current_build() const;
    bool set_current(int version);
    std::string install_build_folder(const fs::path& from, int* version, bool make_current);

    // Updates: the newest release on GitHub. Its tag (v0.9) against the installed release
    // says whether it is newer; the launcher .zip it carries is downloaded beside everything
    // (.update\), checked, unpacked, and only then swapped in. data\, game\, backups\ and
    // launcher.ini are never touched; the saves are backed up first.
    static std::string fetch_latest_release(ReleaseInfo* out);   // touches nothing here: fine off the UI thread
    static int compare_versions(const std::string& a, const std::string& b);   // v0.10 > 0.9.1 > v0.9 = 0.9.0; <0, 0, >0
    std::string installed_release(const std::string& launcher_version) const;  // the higher of it and the last release installed
    std::string download_release(const ReleaseInfo& rel, Progress* p);
    // *restart: a new launcher is in place; start it and leave.
    std::string install_release(const ReleaseInfo& rel, Progress* p, std::string* summary, bool* restart);
    void clean_update_leftovers();

    // Mods.
    std::vector<ModInfo> mods() const;
    std::vector<std::string> enabled_mods() const;            // priority order, first wins
    void set_enabled_mods(const std::vector<std::string>& ids);
    void set_mod_enabled(const std::string& id, bool on);
    void move_mod(const std::string& id, int dir);
    std::string install_mod_zip(const fs::path& zip, std::string* id);
    std::string install_mod_folder(const fs::path& dir, std::string* id);
    std::string remove_mod(const std::string& id);

    // Saves.
    std::vector<fs::path> save_slots() const;
    std::vector<fs::path> backup_list() const;               // oldest first
    std::string backup_saves(const std::string& why, fs::path* made);
    std::string export_saves(const fs::path& zip);
    std::string import_saves(const fs::path& zip);
    std::string restore_backup(const fs::path& zip);

    // Logs.
    void rotate_log();                                       // data\log.txt -> data\logs\log-<stamp>.txt (newest 10 kept)
    std::vector<fs::path> old_logs() const;                  // newest first

    // Start the game: checks, a save backup (unless off), the environment.
    // Every settings.txt value also goes in the environment and the game's
    // EBOOT on the command line, so builds that only read the settings.txt
    // beside their exe (v65 and older) get them too.
    // prepare: the shader prepare instead (RACACIT_PREPARE=1: no window, no game, every
    // pipeline in data\cache\pipelinelist.bin built on all cores, then it exits; Windows only).
    std::string launch(void** process, bool prepare = false);   // Windows: a process HANDLE; macOS: a ChildProcess*

    // Shaders (data\cache): pipelinelist.bin (the pipelines recorded), shaders.state (the game
    // writes it each start: the cache ok / empty / refused, the graphics driver), shaders.stamp
    // (each prepare: the build and the driver it was for, the counts), shaders.progress (the
    // prepare's "state done total failed absent", rewritten while it runs).
    int shader_variants() const;
    std::map<std::string, std::string> shader_stamp() const;
    std::map<std::string, std::string> shader_state() const;
    std::string shader_build_tag() const;            // "v122": what a prepare stamps
    enum { SH_READY, SH_NONE, SH_NEVER, SH_DRIVER, SH_BUILD };
    int shader_status() const;
    // state 0 starting, 1 reading, 2 building, 3 done, 4 failed, 5 updating shader text; false: none yet.
    bool shader_progress(int out[5]) const;
    fs::path prepare_log() const { return data() / L"prepare_log.txt"; }
    // The build's shader and pipeline caches into data\cache: copied when there are none,
    // else merged in once per build (launch does this).
    void seed_caches(const BuildInfo& b);
    // After the game ends: a build that wrote log.txt beside its exe (v65 and
    // older) -- moved to data\log.txt.
    void collect_log();
    fs::path launched_dir;
#ifdef __APPLE__
private:
    std::string launch_mac(const GameInfo& g, const BuildInfo& b, std::map<std::wstring, std::wstring>& set, void** process);
#endif
};
