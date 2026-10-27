// The launcher's data: where everything lives, the settings file, the game,
// builds and updates, mods, saves, logs, and starting the game.
//
// <launcher folder>
//   RaCACiT Launcher.exe
//   launcher.ini            the launcher's own options (update address, ...)
//   builds\v65\             RaCACiT.exe (+ README.txt, version.txt) per installed build
//   builds\current.txt      which one Play starts
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

struct ModInfo {
    std::string id, name, version, author, description, problem;
    fs::path dir;
    bool has_files = false;                                       // a files\ folder to overlay
    std::vector<std::pair<std::string, std::string>> settings;   // mod.json "settings"
};

struct BuildInfo {
    int version = 0;
    fs::path dir;
    std::string notes_first_line;
};

class Store {
public:
    explicit Store(fs::path root);
    fs::path root;
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
    // check_elf: "" = a PS3 program; *known = the USA disc's own (the one the build is made from).
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

    // Builds: installed under builds\vNN, one is current.
    std::vector<BuildInfo> builds() const;
    BuildInfo current_build() const;
    bool set_current(int version);
    std::string install_build_folder(const fs::path& from, int* version, bool make_current);
    std::string install_build_zip(const fs::path& zip, int* version, bool make_current);
    std::string remove_build(int version);

    // Updates (off until launcher.ini has update_url=): a JSON manifest.
    std::string update_url() const { return trim(ini.get("update_url")); }
    std::string check_update(int* latest, std::string* notes, std::string* url, std::string* sha256, uint64_t* size);
    std::string download_update(const std::string& url, const std::string& sha256, int version, Progress* p);

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
    std::string launch(void** process);
    // After the game ends: a build that wrote log.txt beside its exe (v65 and
    // older) -- moved to data\log.txt.
    void collect_log();
    fs::path launched_dir;
};
