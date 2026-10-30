#include "store.h"
#include "iso.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <thread>
#include <unordered_set>

// ---- settings files ------------------------------------------------------------
static bool line_key(const std::string& line, std::string* key, std::string* val, bool* commented)
{
    std::string l = trim(line);
    *commented = false;
    if (!l.empty() && l[0] == '#') { *commented = true; l = l.substr(1); }
    const size_t eq = l.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    const std::string k = trim(l.substr(0, eq));
    for (char c : k) if (!(isalnum((unsigned char)c) || c == '_')) return false;
    *key = k;
    std::string v = trim(l.substr(eq + 1));
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    *val = v;
    return true;
}

static bool same_key(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

bool SettingsFile::load()
{
    lines.clear();
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    const std::string t = read_text(path);
    size_t i = 0;
    while (i < t.size()) {
        size_t j = t.find('\n', i);
        if (j == std::string::npos) j = t.size();
        std::string l = t.substr(i, j - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        i = j + 1;
    }
    return true;
}

bool SettingsFile::save() const
{
    std::string t;
    for (const auto& l : lines) { t += l; t += "\r\n"; }
    return write_text_atomic(path, t);
}

std::string SettingsFile::get(const std::string& key, const std::string& def) const
{
    std::string r = def, k, v; bool c;
    for (const auto& l : lines) if (line_key(l, &k, &v, &c) && !c && same_key(k, key)) r = v;   // the last one wins, as in the game
    return r;
}

bool SettingsFile::has(const std::string& key) const
{
    std::string k, v; bool c;
    for (const auto& l : lines) if (line_key(l, &k, &v, &c) && !c && same_key(k, key)) return true;
    return false;
}

void SettingsFile::put(const std::string& key, const std::string& value)
{
    std::string k, v; bool c; bool done = false;
    for (auto& l : lines) if (line_key(l, &k, &v, &c) && !c && same_key(k, key)) { l = key + "=" + value; done = true; }
    if (done) return;
    for (auto& l : lines) if (line_key(l, &k, &v, &c) && c && same_key(k, key) && trim(l).rfind("#" + k, 0) == 0) { l = key + "=" + value; return; }
    static const char* head = "# --- Set from the launcher ---";
    if (std::find(lines.begin(), lines.end(), head) == lines.end()) {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
        lines.push_back(head);
    }
    lines.push_back(key + "=" + value);
}

void SettingsFile::remove(const std::string& key)
{
    std::string k, v; bool c;
    for (auto& l : lines) if (line_key(l, &k, &v, &c) && !c && same_key(k, key)) l = "#" + l;
}

// ---- store -----------------------------------------------------------------------
static const char* kSettingsTemplate =
    "# Ratchet & Clank: A Crack in Time (PC recompilation) -- game settings\r\n"
    "# NAME=value lines, the same names as a release's settings.txt. The launcher's\r\n"
    "# Settings screen and the in-game menu (F1) both edit this file; lines you add\r\n"
    "# by hand are kept.\r\n"
    "#\r\n"
    "# GAME = the folder that contains your disc dump's PS3_GAME folder (set by Import).\r\n"
    "# --- Cutscenes ---\r\n"
    "RACACIT_START_SKIPS_MOVIES=1\r\n"
    "RACACIT_ALL_CUTSCENES_SKIPPABLE=1\r\n";

static const char* kIniTemplate =
    "# RACiT Launcher options (NAME=value)\r\n"
    "# Look for a new release on GitHub when the launcher opens (at most every 6 hours): 1 = yes\r\n"
    "check_updates=1\r\n"
    "# The launcher window while the game runs: minimize, hide or stay\r\n"
    "while_playing=minimize\r\n"
    "# Back up the saves each time Play is pressed: 1 = yes\r\n"
    "backup_on_play=1\r\n"
    "# How many save backups and old logs to keep\r\n"
    "keep_backups=8\r\n"
    "keep_logs=10\r\n";

Store::Store(fs::path r) : root(std::move(r))
{
    std::error_code ec;
    for (const fs::path& d : { data(), cache(), logs(), builds_dir(), mods_dir(), backups(), assets() }) fs::create_directories(d, ec);
    settings.path = data() / L"settings.txt";
    ini.path = root / L"launcher.ini";
    if (!settings.load()) { write_text_atomic(settings.path, kSettingsTemplate); settings.load(); }
    if (!ini.load()) { write_text_atomic(ini.path, kIniTemplate); ini.load(); }
}

void Store::reload() { settings.load(); ini.load(); }

static uint32_t le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }

static std::map<std::string, std::string> parse_sfo(const std::string& d)
{
    std::map<std::string, std::string> out;
    const auto* u = (const unsigned char*)d.data();
    if (d.size() < 20 || memcmp(u, "\0PSF", 4)) return out;
    const uint32_t keys = le32(u + 8), vals = le32(u + 12), n = le32(u + 16);
    for (uint32_t i = 0; i < n && 20 + i * 16 + 16 <= d.size(); i++) {
        const unsigned char* e = u + 20 + i * 16;
        const uint32_t ko = keys + le16(e), fmt = le16(e + 2), len = le32(e + 4), vo = vals + le32(e + 12);
        if (ko >= d.size() || vo + len > d.size()) continue;
        std::string k(d.c_str() + ko);
        if (fmt == 0x0204 || fmt == 0x0004) {
            std::string v(d.data() + vo, len);
            while (!v.empty() && v.back() == '\0') v.pop_back();
            out[k] = v;
        }
    }
    return out;
}
static std::map<std::string, std::string> read_sfo(const fs::path& p) { return parse_sfo(read_text(p, 1 << 20)); }

GameInfo Store::game() const
{
    GameInfo g;
    const std::string gs = settings.get("GAME");
    g.root = gs.empty() ? game_copy() : upath(gs);
    std::error_code ec;
    if (!fs::is_directory(g.root / L"PS3_GAME", ec)) {
        g.status = gs.empty() ? "Not imported yet" : "PS3_GAME not found in " + gs;
        return g;
    }
    auto sfo = read_sfo(g.root / L"PS3_GAME" / L"PARAM.SFO");
    g.title = sfo["TITLE"]; g.title_id = sfo["TITLE_ID"]; g.app_ver = sfo["APP_VER"];
    if (!g.title_id.empty() && g.title_id != "BCUS98124") {
        g.status = "This is " + g.title_id + " (" + g.title + "), not A Crack in Time USA (BCUS98124)";
        return g;
    }
    const fs::path eb = g.root / L"PS3_GAME" / L"USRDIR" / L"EBOOT.elf";
    const std::string head = read_text(eb, 4);
    if (head.size() < 4) { g.need_decrypt = true; g.status = "No EBOOT.elf yet"; return g; }
    if (head.compare(0, 4, "\x7F" "ELF") != 0) { g.need_decrypt = true; g.status = "EBOOT.elf is still encrypted"; return g; }
    g.ok = true;
    g.status = "Ready";
    return g;
}

fs::path Store::game_folder_from_pick(const fs::path& picked, std::string* why)
{
    std::error_code ec;
    auto has = [&](const fs::path& d) { return fs::is_directory(d / L"PS3_GAME", ec); };
    if (has(picked)) return picked;
    if (lower(u8(picked.filename())) == "ps3_game" && fs::is_directory(picked, ec)) return picked.parent_path();
    // USRDIR itself (where EBOOT.BIN / EBOOT.elf are): PS3_GAME's parent.
    if (lower(u8(picked.filename())) == "usrdir" && fs::exists(picked.parent_path() / L"PARAM.SFO", ec)) return picked.parent_path().parent_path();
    if (fs::exists(picked / L"PARAM.SFO", ec) && fs::is_directory(picked / L"USRDIR", ec)) return picked.parent_path();
    // A dump often sits one or two folders further down ("Name (USA)\Name (USA)\PS3_GAME").
    for (auto& a : fs::directory_iterator(picked, ec)) {
        if (!a.is_directory(ec)) continue;
        if (has(a.path())) return a.path();
        for (auto& b : fs::directory_iterator(a.path(), ec)) if (b.is_directory(ec) && has(b.path())) return b.path();
    }
    if (why) *why = "No PS3_GAME folder in " + u8(picked) + " (pick the folder that contains PS3_GAME).";
    return {};
}

std::string Store::use_game_in_place(const fs::path& picked)
{
    std::string why;
    const fs::path g = game_folder_from_pick(picked, &why);
    if (g.empty()) return why;
    settings.put("GAME", u8(g));
    if (!settings.save()) return "Could not write " + u8(settings.path);
    refresh_art();
    return {};
}

std::string Store::import_iso(const fs::path& iso, Progress* p, std::string* summary)
{
    p->set_line("Reading " + u8(iso.filename()));
    IsoImage img;
    std::string r = img.open(iso);
    if (!r.empty()) return r;
    const IsoFile* sfo = img.find("PS3_GAME/PARAM.SFO");
    if (!sfo) return u8(iso.filename()) + " is not a PS3 game disc image (no PS3_GAME\\PARAM.SFO in it).";
    const std::string psf("\0PSF", 4);
    std::string d = img.read(*sfo);
    std::string how = "a decrypted image";
    if (d.compare(0, 4, psf) != 0) {
        if (!img.has_encrypted_regions()) return u8(iso.filename()) + ": its PARAM.SFO cannot be read (a damaged image?).";
        fs::path kf;
        const std::string key = find_dkey(iso, &kf);
        if (key.empty())
            return "This .iso is encrypted (a Redump dump). Its disc key unlocks it: put the .dkey file that comes with the "
                   "dump beside the .iso, named like it (" + u8(iso.stem()) + ".dkey), then import again.";
        if (!(r = img.set_key(key)).empty()) return u8(kf.filename()) + ": " + r;
        d = img.read(*sfo);
        if (d.compare(0, 4, psf) != 0) return u8(kf.filename()) + " does not unlock this .iso (the key of another disc?).";
        how = "a Redump image, unlocked with " + u8(kf.filename());
    }
    const auto info = parse_sfo(d);
    const std::string id = info.count("TITLE_ID") ? info.at("TITLE_ID") : "";
    if (id != "BCUS98124")
        return "This is " + (id.empty() ? std::string("an unknown game") : id + " (" + (info.count("TITLE") ? info.at("TITLE") : "") + ")") +
               ", not A Crack in Time USA (BCUS98124).";
    // PS3_GAME and the disc's PS3_DISC.SFB; not PS3_UPDATE (system software for a console).
    std::vector<const IsoFile*> want; uint64_t need = 0;
    for (auto& f : img.files())
        if (lower(f.path).rfind("ps3_game/", 0) == 0 || lower(f.path) == "ps3_disc.sfb") { want.push_back(&f); need += f.size; }
    p->total = need;
    std::error_code ec;
    fs::create_directories(game_copy(), ec);
    ULARGE_INTEGER free_{};
    if (GetDiskFreeSpaceExW(game_copy().c_str(), &free_, nullptr, nullptr) && free_.QuadPart < need + (512ull << 20))
        return "Not enough space: the game needs " + format_size(need) + ", " + format_size(free_.QuadPart) + " free on this drive.";
    // An EBOOT.elf already in the launcher's copy is kept for the new one.
    const fs::path old_elf = game_copy() / L"PS3_GAME" / L"USRDIR" / L"EBOOT.elf";
    bool k = false;
    if (fs::exists(old_elf, ec) && !fs::exists(pending_elf(), ec) && check_elf(old_elf, &k).empty())
        fs::rename(old_elf, pending_elf(), ec);
    delete_tree(game_copy() / L"PS3_GAME");
    for (const IsoFile* f : want) {
        p->set_line(f->path);
        fs::path out = game_copy();
        size_t a = 0;
        for (size_t b; (b = f->path.find('/', a)) != std::string::npos; a = b + 1) out /= upath(f->path.substr(a, b - a));
        out /= upath(f->path.substr(a));
        if (!(r = img.extract(*f, out, p)).empty()) { delete_tree(game_copy() / L"PS3_GAME"); return r; }
    }
    // A wrong key or region table shows here: the disc's EBOOT.BIN starts "SCE\0".
    if (const IsoFile* eb = img.find("PS3_GAME/USRDIR/EBOOT.BIN"))
        if (img.read(*eb, 4) != std::string("SCE\0", 4)) {
            delete_tree(game_copy() / L"PS3_GAME");
            return "The image did not unpack correctly (its EBOOT.BIN is not readable). Is the .iso complete?";
        }
    settings.remove("GAME");   // the launcher's own copy (game\)
    if (!settings.save()) return "Could not write " + u8(settings.path);
    refresh_art();
    std::string elf_note;
    if (fs::exists(pending_elf(), ec)) {
        const std::string e = add_elf(pending_elf());
        elf_note = e.empty() ? " EBOOT.elf is in place." : " EBOOT.elf: " + e;
    }
    if (summary) *summary = "Imported " + format_size(need) + " from " + how + "." + elf_note;
    return {};
}

std::string Store::check_elf(const fs::path& elf, bool* known)
{
    *known = false;
    const std::string h = read_text(elf, 64);
    if (h.size() >= 4 && h.compare(0, 4, std::string("SCE\0", 4)) == 0)
        return "This is the locked EBOOT.BIN (or a copy of it). Unlock it with RPCS3 first: Utilities > Decrypt PS3 Binaries.";
    if (h.size() < 20 || h.compare(0, 4, "\x7F" "ELF") != 0) return u8(elf.filename()) + " is not an ELF program.";
    if (h[4] != 2 || h[5] != 2 || (unsigned char)h[18] != 0 || (unsigned char)h[19] != 0x15) return u8(elf.filename()) + " is not a PS3 program.";
    *known = sha256_file(elf) == "406bded328d9dbdfa9684ed392554ca34c76f4ed95aa21685f622d3ab2b04592";
    return {};
}

std::string Store::add_elf(const fs::path& elf)
{
    bool known = false;
    std::string r = check_elf(elf, &known);
    if (!r.empty()) return r;
    const GameInfo g = game();
    std::error_code ec;
    const bool have_game = fs::is_directory(g.root / L"PS3_GAME", ec);
    const fs::path to = have_game ? g.root / L"PS3_GAME" / L"USRDIR" / L"EBOOT.elf" : pending_elf();
    if (!(fs::exists(to, ec) && fs::equivalent(elf, to, ec))) {
        fs::create_directories(to.parent_path(), ec);
        if (!fs::copy_file(elf, to, fs::copy_options::overwrite_existing, ec)) return "Could not copy it to " + u8(to) + ": " + ec.message();
    }
    if (have_game) fs::remove(pending_elf(), ec);
    return {};
}

std::string Store::refresh_art()
{
    const GameInfo g = game();
    std::error_code ec;
    const fs::path pg = g.root / L"PS3_GAME";
    int n = 0;
    fs::create_directories(assets(), ec);
    if (fs::exists(pg / L"ICON0.PNG", ec)) { fs::copy_file(pg / L"ICON0.PNG", icon(), fs::copy_options::overwrite_existing, ec); n += !ec; }
    if (fs::exists(pg / L"PIC1.PNG", ec)) { fs::copy_file(pg / L"PIC1.PNG", background(), fs::copy_options::overwrite_existing, ec); n += !ec; }
    if (fs::exists(pg / L"PIC0.PNG", ec)) { fs::copy_file(pg / L"PIC0.PNG", logo(), fs::copy_options::overwrite_existing, ec); }
    return n ? std::string() : "The game folder has no ICON0.PNG / PIC1.PNG.";
}

// ---- an existing release folder ----------------------------------------------------
static int version_of_name(const std::string& name)
{
    // RaCACiT_v65, v65, 65
    for (size_t i = 0; i < name.size(); i++) {
        if ((name[i] == 'v' || name[i] == 'V') && i + 1 < name.size() && isdigit((unsigned char)name[i + 1])) return atoi(name.c_str() + i + 1);
    }
    return !name.empty() && isdigit((unsigned char)name[0]) ? atoi(name.c_str()) : 0;
}

std::vector<fs::path> Store::find_old_installs() const
{
    std::vector<std::pair<int, fs::path>> v;
    std::error_code ec;
    const fs::path parent = root.parent_path();
    for (auto& d : fs::directory_iterator(parent, ec)) {
        if (!d.is_directory(ec) || fs::equivalent(d.path(), root, ec)) continue;
        if (fs::exists(d.path() / L"RaCACiT.exe", ec) && fs::exists(d.path() / L"settings.txt", ec))
            v.push_back({ version_of_name(u8(d.path().filename())), d.path() });
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;   // the newest three (a release folder holds dozens)
    for (auto& e : v) if (out.size() < 3) out.push_back(e.second);
    return out;
}

static bool launcher_owned_key(const std::string& k)
{
    static const char* own[] = { "PS3_HDD0_ROOT", "RSX_SHADER_CACHE", "RSX_PIPELINE_CACHE", "RSX_PIPELINE_LIST",
                                 "RACACIT_SETTINGS_FILE", "RACACIT_LOG_FILE", "RACACIT_MODS" };
    for (const char* o : own) if (same_key(k, o)) return true;
    return false;
}

std::string Store::migrate_from(const fs::path& old, Progress* p, std::string* summary)
{
    SettingsFile os; os.path = old / L"settings.txt";
    if (!os.load()) return "No settings.txt in " + u8(old);
    const std::string mods_on = settings.get("RACACIT_MODS_ENABLED");
    const std::string hdd_src = os.get("PS3_HDD0_ROOT");
    // The old file's lines (its comments too); what the launcher sets itself is left as a comment.
    SettingsFile ns = settings;
    ns.lines.clear();
    std::string k, v; bool c;
    for (const auto& l : os.lines) {
        if (line_key(l, &k, &v, &c) && !c && launcher_owned_key(k)) ns.lines.push_back("# (the launcher sets this) " + l);
        else ns.lines.push_back(l);
    }
    if (!mods_on.empty()) ns.put("RACACIT_MODS_ENABLED", mods_on);
    settings = ns;
    if (!settings.save()) return "Could not write " + u8(settings.path);
    std::string s = "Settings from " + u8(old.filename()) + ".";

    // Saves and the rest of the PS3 hard drive.
    std::error_code ec;
    const fs::path src = hdd_src.empty() ? old / L"dev_hdd0" : upath(hdd_src);
    if (fs::is_directory(src, ec) && !fs::equivalent(src, hdd(), ec)) {
        if (!save_slots().empty()) backup_saves("before-migrate", nullptr);
        p->set_line("Measuring " + u8(src));
        p->done = 0; p->total = tree_size(src);
        const std::string r = copy_tree(src, hdd(), p);
        if (!r.empty()) return r;
        s += " Saves and HDD data copied (" + format_size(p->total) + ").";
    } else s += " No dev_hdd0 to copy.";

    int nc = 0;
    for (const wchar_t* cf : { L"shadercache.bin", L"pipelinecache.bin", L"pipelinelist.bin" })
        if (fs::exists(old / cf, ec)) { fs::copy_file(old / cf, cache() / cf, fs::copy_options::overwrite_existing, ec); nc += !ec; }
    if (nc) s += " Shader caches copied.";

    // Its build too, unless that version is installed already (the launcher's own
    // copy of it may be newer: launcher support added).
    int ver = 0, have = version_of_name(u8(old.filename()));
    bool installed = false;
    for (auto& b : builds()) if (b.version == have) installed = true;
    if (installed) s += " Build v" + std::to_string(have) + " is installed already (kept).";
    else if (fs::exists(old / L"RaCACiT.exe", ec) && install_build_folder(old, &ver, builds().empty()).empty())
        s += " Build v" + std::to_string(ver) + " installed.";
    refresh_art();
    if (summary) *summary = s;
    return {};
}

// ---- an earlier launcher ---------------------------------------------------------------
// Beside this one, one folder further in, or in a folder next to the one this is in: a zip
// unpacked with Windows' "Extract all" nests the launcher in a folder named like itself.
// One named 0.8 first (what players come from; test copies may sit beside it), then played
// ones (a launcher only opened has nothing to bring), then the newest.
std::vector<fs::path> Store::find_old_launchers() const
{
    struct Found { bool named; bool played; fs::file_time_type when; fs::path dir; };
    std::vector<Found> v;
    std::error_code ec;
    auto consider = [&](const fs::path& d) {
        if (!fs::exists(d / L"RaCACiT Launcher.exe", ec) || !fs::exists(d / L"data" / L"settings.txt", ec)) return;
        if (fs::equivalent(d, root, ec)) return;
        for (const auto& f : v) if (fs::equivalent(f.dir, d, ec)) return;
        v.push_back({ u8(d.filename()).find("0.8") != std::string::npos,
                      fs::is_directory(d / L"data" / L"dev_hdd0" / L"home" / L"00000001" / L"savedata", ec),
                      fs::last_write_time(d / L"data" / L"settings.txt", ec), d });
    };
    auto scan = [&](const fs::path& parent) {
        std::error_code e1;
        for (fs::directory_iterator a(parent, fs::directory_options::skip_permission_denied, e1), e; !e1 && a != e; a.increment(e1)) {
            std::error_code e2;
            if (!a->is_directory(e2)) continue;
            consider(a->path());
            for (fs::directory_iterator b(a->path(), fs::directory_options::skip_permission_denied, e2); !e2 && b != e; b.increment(e2)) {
                std::error_code e3;
                if (b->is_directory(e3)) consider(b->path());
            }
        }
    };
    const fs::path parent = root.parent_path();
    scan(parent);
    if (parent.parent_path() != parent) scan(parent.parent_path());
    std::sort(v.begin(), v.end(), [](const Found& a, const Found& b) {
        if (a.named != b.named) return a.named;
        return a.played != b.played ? a.played : a.when > b.when; });
    std::vector<fs::path> out;
    for (auto& f : v) out.push_back(f.dir);
    return out;
}

std::string Store::migrate_from_launcher(const fs::path& old, Progress* p, std::string* summary)
{
    std::error_code ec;
    if (fs::equivalent(old, root, ec)) return "That is this launcher's own folder.";
    SettingsFile os; os.path = old / L"data" / L"settings.txt";
    if (!os.load()) return "No data\\settings.txt in " + u8(old) + " (pick the folder that holds RaCACiT Launcher.exe).";
    std::string s;

    // Saves and the rest of the PS3 hard drive. This launcher's saves are backed up first, and
    // nothing is overwritten if that fails. Settings come last: a failed copy leaves them as they were.
    const fs::path src = old / L"data" / L"dev_hdd0";
    if (fs::is_directory(src, ec) && !fs::equivalent(src, hdd(), ec)) {
        const std::string b = backup_saves("before-migrate", nullptr);
        if (!b.empty()) return "Could not back up this launcher's saves first: " + b;
        p->set_line("Measuring " + u8(src));
        p->done = 0; p->total = tree_size(src);
        const std::string r = copy_tree(src, hdd(), p);
        if (!r.empty()) return r;
        s += "Saves and HDD data copied (" + format_size(p->total) + ").";
    } else s += "No saves to copy.";

    int nc = 0;
    fs::create_directories(cache(), ec);
    for (const wchar_t* cf : { L"shadercache.bin", L"pipelinecache.bin", L"pipelinelist.bin" })
        if (fs::exists(old / L"data" / L"cache" / cf, ec)) {
            fs::copy_file(old / L"data" / L"cache" / cf, cache() / cf, fs::copy_options::overwrite_existing, ec);
            nc += !ec;
        }
    if (nc) s += " Shader caches copied.";

    // Its save backups: zips, and the folders made when tar.exe failed (Store::backup_saves).
    int nb = 0;
    fs::create_directories(backups(), ec);
    { std::error_code e1;
      for (fs::directory_iterator f(old / L"backups", e1), e; !e1 && f != e; f.increment(e1)) {
          std::error_code e2;
          const fs::path to = backups() / f->path().filename();
          if (u8(f->path().filename()).rfind("saves-", 0) != 0 || fs::exists(to, e2)) continue;
          if (f->is_directory(e2)) { Progress bp; nb += copy_tree(f->path(), to, &bp).empty(); }
          else if (f->path().extension() == L".zip") { fs::copy_file(f->path(), to, e2); nb += !e2; }
      } }
    if (nb) s += " " + std::to_string(nb) + " save backups copied.";

    // The options in launcher.ini.
    std::string k, v; bool c;
    SettingsFile oi; oi.path = old / L"launcher.ini";
    if (oi.load()) {
        for (const auto& l : oi.lines) if (line_key(l, &k, &v, &c) && !c) ini.put(k, v);
        ini.save();
    }

    // Settings: its lines, what the launcher sets itself left as a comment. The game: this
    // launcher's if it works already; else the old one's GAME; else the old one's own copy, used
    // where it is (about 25 GB, and the old folder stays as it is).
    const GameInfo here = game();
    const std::string here_game = settings.get("GAME");
    SettingsFile ns = settings;
    ns.lines.clear();
    for (const auto& l : os.lines) {
        if (line_key(l, &k, &v, &c) && !c && launcher_owned_key(k)) ns.lines.push_back("# (the launcher sets this) " + l);
        else ns.lines.push_back(l);
    }
    const std::string old_game = os.get("GAME");
    const fs::path old_copy = old / L"game";
    if (here.ok) {
        if (here_game.empty()) ns.remove("GAME"); else ns.put("GAME", here_game);
        s += " Game: this launcher's own, kept.";
    } else if (!old_game.empty() && fs::is_directory(upath(old_game) / L"PS3_GAME", ec)) {
        s += " Game: " + old_game + ", as before.";
    } else if (fs::is_directory(old_copy / L"PS3_GAME", ec)) {
        ns.put("GAME", u8(old_copy));
        s += " Game: played from " + u8(old_copy) + ", not copied. Keep that folder, or import the .iso again here.";
    } else {
        if (!old_game.empty()) ns.remove("GAME");
        s += " No game there: import it in Set up.";
    }
    settings = ns;
    if (!settings.save()) return "Could not write " + u8(settings.path);
    refresh_art();
    if (summary) *summary = "From " + u8(old.filename()) + ": " + s;
    return {};
}

// ---- builds ------------------------------------------------------------------------
std::vector<BuildInfo> Store::builds() const
{
    std::vector<BuildInfo> out;
    std::error_code ec;
    for (auto& d : fs::directory_iterator(builds_dir(), ec)) {
        if (!d.is_directory(ec)) continue;
        const std::string n = u8(d.path().filename());
        if (n.size() < 2 || n[0] != 'v' || !fs::exists(d.path() / L"RaCACiT.exe", ec)) continue;
        BuildInfo b; b.version = atoi(n.c_str() + 1); b.dir = d.path();
        std::string rd = read_text(d.path() / L"README.txt", 400);
        b.notes_first_line = trim(rd.substr(0, rd.find('\n')));
        if (b.version > 0) out.push_back(b);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.version > b.version; });
    return out;
}

BuildInfo Store::current_build() const
{
    // The newest build that ships with the launcher, always.
    const auto all = builds();
    return all.empty() ? BuildInfo{} : all.front();
}

bool Store::set_current(int version) { return write_text_atomic(builds_dir() / L"current.txt", std::to_string(version) + "\r\n"); }

std::string Store::install_build_folder(const fs::path& from, int* version, bool make_current)
{
    std::error_code ec;
    if (!fs::exists(from / L"RaCACiT.exe", ec)) return "No RaCACiT.exe in " + u8(from);
    int v = atoi(trim(read_text(from / L"version.txt", 64)).c_str());
    if (!v) v = version_of_name(u8(from.filename()));
    if (!v) {   // "... Windows build v65 ..." in the README
        const std::string rd = read_text(from / L"README.txt", 4096);
        const size_t at = rd.find("build v");
        if (at != std::string::npos) v = atoi(rd.c_str() + at + 7);
    }
    if (v <= 0) return "Cannot tell which build " + u8(from) + " is (no version.txt).";
    const fs::path dst = builds_dir() / (L"v" + std::to_wstring(v));
    const fs::path tmp = builds_dir() / (L".v" + std::to_wstring(v) + L".new");
    delete_tree(tmp);
    fs::create_directories(tmp, ec);
    for (const wchar_t* f : { L"RaCACiT.exe", L"README.txt", L"version.txt", L"RaCACiT.map" })
        if (fs::exists(from / f, ec)) {
            fs::copy_file(from / f, tmp / f, fs::copy_options::overwrite_existing, ec);
            if (ec) { delete_tree(tmp); return "Copying " + narrow(f) + " failed: " + ec.message(); }
        }
    write_text_atomic(tmp / L"version.txt", std::to_string(v) + "\r\n");
    delete_tree(dst);
    fs::rename(tmp, dst, ec);
    if (ec) { delete_tree(tmp); return "Could not put the build in place (is it running?): " + ec.message(); }
    if (version) *version = v;
    if (make_current) set_current(v);
    return {};
}

// ---- updates ---------------------------------------------------------------------------
// GET https://api.github.com/repos/HighDrexler/RACiT/releases/latest (no sign-in; GitHub allows
// 60 asks an hour per connection). Only an address on this repository's own releases is ever
// downloaded. RACACIT_UPDATE_TEST_API, for the launcher's tests, stands in for GitHub, and only
// on this PC: http://127.0.0.1:port/...
static const char* kReleasesApi = "https://api.github.com/repos/HighDrexler/RACiT/releases/latest";
static const char* kReleasePages = "https://github.com/HighDrexler/RACiT/releases/";
static const char* kReleaseDownloads = "https://github.com/HighDrexler/RACiT/releases/download/";

static std::string test_api()
{
    const char* e = getenv("RACACIT_UPDATE_TEST_API");
    return e && !strncmp(e, "http://127.0.0.1:", 17) ? e : "";
}
static bool under(const std::string& url, const std::string& base)
{
    return url.rfind(base, 0) == 0 && url.find("..") == std::string::npos && url.find('\\') == std::string::npos && url.find('@') == std::string::npos;
}
static bool release_download_ok(const std::string& url)
{
    const std::string t = test_api();
    if (!t.empty()) return under(url, t.substr(0, t.find('/', 7) + 1));
    return under(url, kReleaseDownloads);
}

// The release's text as plain text: no Markdown marks, not too long for the screen (the rest
// is on the release's page).
static std::string plain_notes(const std::string& md)
{
    std::string out;
    size_t i = 0;
    while (i < md.size() && out.size() < 900) {
        size_t j = md.find('\n', i);
        if (j == std::string::npos) j = md.size();
        std::string l = md.substr(i, j - i);
        i = j + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t at = 0;
        while (at < l.size() && l[at] == '#') at++;
        if (at) l = trim(l.substr(at));
        for (const char* mark : { "**", "__", "`" })
            for (size_t k; (k = l.find(mark)) != std::string::npos;) l.erase(k, strlen(mark));
        if (!(l.empty() && (out.empty() || (out.size() >= 2 && out.compare(out.size() - 2, 2, "\n\n") == 0)))) out += l + "\n";
    }
    bool cut = i < md.size();
    if (out.size() > 900) {
        size_t k = out.rfind(' ', 900);
        out.resize(k == std::string::npos ? 900 : k);
        cut = true;
    }
    out = trim(out);
    return cut ? out + " ..." : out;
}

std::string Store::fetch_latest_release(ReleaseInfo* out)
{
    *out = ReleaseInfo();
    const std::string test = test_api();
    std::string body; int status = 0;
    const std::string r = http_get(test.empty() ? kReleasesApi : test, &body, nullptr, nullptr, &status,
                                   L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n");
    if (!r.empty()) {
        if (status == 0) return "Could not reach GitHub. Check your internet connection and try again.";
        if (status == 403 || status == 429) return "GitHub is limiting how often this connection can ask (60 times an hour). Try again later.";
        if (status == 404) return "There is no release on GitHub yet.";
        return "GitHub answered " + std::to_string(status) + ". Try again later.";
    }
    Json j; std::string err;
    if (!json_parse(body, &j, &err) || j.type != Json::Obj) return "GitHub's answer could not be read. Try again later.";
    out->tag = trim(j.str("tag_name"));
    if (out->tag.empty()) return "GitHub's answer has no release in it. Try again later.";
    out->name = trim(j.str("name", out->tag));
    if (out->name.empty()) out->name = out->tag;
    out->notes = plain_notes(j.str("body"));
    const std::string page = j.str("html_url");
    if (under(page, kReleasePages) || !test.empty()) out->page = page;
    // The asset: a .zip with "launcher" in its name. A .7z is listed for the message only.
    for (const Json& a : j["assets"].a) {
        const std::string n = a.str("name"), low = lower(n), url = a.str("browser_download_url");
        const bool zip = low.size() > 4 && low.compare(low.size() - 4, 4, ".zip") == 0;
        if (out->asset.empty() && zip && low.find("launcher") != std::string::npos && release_download_ok(url)) {
            out->asset = n; out->url = url; out->size = (uint64_t)a.num("size");
            const std::string d = lower(a.str("digest"));
            if (d.rfind("sha256:", 0) == 0) out->sha256 = d.substr(7);
        } else if (!n.empty()) out->other_assets += (out->other_assets.empty() ? "" : ", ") + n;
    }
    return {};
}

static std::vector<long> version_parts(const std::string& s)
{
    std::vector<long> v;
    size_t i = !s.empty() && (s[0] == 'v' || s[0] == 'V') ? 1 : 0;
    while (i < s.size() && isdigit((unsigned char)s[i])) {
        long n = 0;
        while (i < s.size() && isdigit((unsigned char)s[i]) && n < 100000000) n = n * 10 + (s[i++] - '0');
        v.push_back(n);
        if (i + 1 < s.size() && s[i] == '.' && isdigit((unsigned char)s[i + 1])) i++;
        else break;
    }
    return v;
}

int Store::compare_versions(const std::string& a, const std::string& b)
{
    const auto x = version_parts(a), y = version_parts(b);
    if (x.empty() || y.empty()) return x.empty() ? (y.empty() ? 0 : -1) : 1;   // a tag that is no version is never newer
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

std::string Store::installed_release(const std::string& launcher_version) const
{
    const std::string done = trim(read_text(builds_dir() / L"release.txt", 64));
    return compare_versions(done, launcher_version) > 0 ? done : launcher_version;
}

void Store::clean_update_leftovers()
{
    delete_tree(root / L".update");
    // The launcher this one replaced: it may still be on its way out.
    fs::path old = exe_path(); old += L".old";
    std::error_code ec;
    if (!fs::exists(old, ec)) return;
    std::thread([old] {
        for (int i = 0; i < 40; i++) {
            if (DeleteFileW(old.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return;
            Sleep(250);
        }
    }).detach();
}

std::string Store::download_release(const ReleaseInfo& rel, Progress* p)
{
    if (rel.url.empty() || !release_download_ok(rel.url)) return "This release has no launcher .zip to install.";
    const fs::path dir = root / L".update";
    delete_tree(dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return "Could not write to this folder (" + u8(root) + "): " + ec.message();
    const uint64_t room = free_space(root);
    if (rel.size && room && room < rel.size * 4) {
        delete_tree(dir);
        return "Not enough free space: the update needs about " + format_size(rel.size * 4) + " on this drive, and " + format_size(room) + " is free.";
    }
    const fs::path part = dir / L"release.zip.part", zip = dir / L"release.zip";
    p->set_line("Downloading " + rel.asset);
    p->done = 0; p->total = rel.size;
    int status = 0;
    std::string r = http_get(rel.url, nullptr, &part, p, &status);
    if (!r.empty()) {
        delete_tree(dir);
        if (r == "cancelled") return r;
        return status == 0 || r == "the download broke off" ? "The download did not finish. Check your internet connection and try again. Nothing was changed." :
               "The download failed: " + r + ". Nothing was changed.";
    }
    const uint64_t got = fs::file_size(part, ec);
    if (rel.size && got != rel.size) {
        delete_tree(dir);
        return "The download did not finish or is the wrong size (" + format_size(got) + ", should be " + format_size(rel.size) + "). Try again. Nothing was changed.";
    }
    if (rel.sha256.size() == 64) {
        p->set_line("Checking the download");
        p->done = 0; p->total = got;
        const std::string have = sha256_file(part, p);
        if (p->cancel) { delete_tree(dir); return "cancelled"; }
        if (have != rel.sha256) { delete_tree(dir); return "The download does not match GitHub's checksum. Nothing was changed."; }
    }
    fs::rename(part, zip, ec);
    if (ec) { delete_tree(dir); return "Could not keep the download: " + ec.message(); }
    return {};
}

// Which part of a release a zip entry is, under the folder it is packed in (or none): the
// launcher exe, README.txt, mods\<id>\... and builds\vNN\.... Everything else -- a data\, game\,
// backups\ or launcher.ini by mistake among it -- is never even unpacked.
static bool release_part(const std::string& rel_name)
{
    const std::string n = lower(rel_name);
    if (n == "readme.txt") return true;
    if (n.find('/') == std::string::npos) return n.size() > 4 && n.compare(n.size() - 4, 4, ".exe") == 0 && n.find("launcher") != std::string::npos;
    if (n.rfind("mods/", 0) == 0) return n.size() > 5 && n.find('/', 5) != std::string::npos;
    if (n.rfind("builds/v", 0) == 0) {
        const size_t e = n.find('/', 8);
        return e != std::string::npos && e > 8 && n.find_first_not_of("0123456789", 8) == e;
    }
    return false;
}

static bool looks_like_program(const fs::path& p)
{
    const std::string h = read_text(p, 2);
    return h.size() >= 2 && h[0] == 'M' && h[1] == 'Z';
}

std::string Store::install_release(const ReleaseInfo& rel, Progress* p, std::string* summary, bool* restart)
{
    *restart = false;
    std::error_code ec;
    const fs::path dir = root / L".update", zip = dir / L"release.zip", fresh = dir / L"new", aside = dir / L"old";
    auto stop = [&](const std::string& why) { delete_tree(dir); return why; };
    if (process_running_under(builds_dir())) return stop("The game is running. Close it, then install the update.");

    // 1. Unpack beside everything.
    p->set_line("Reading the release");
    std::vector<ZipEntry> all;
    std::string r = zip_list(zip, &all);
    if (!r.empty()) return stop("The release could not be read: " + r + ". Nothing was changed.");
    std::string base;   // the folder everything is packed in, if any
    auto is_main = [](const std::string& s) { return lower(s).rfind("builds/", 0) == 0 || (s.find('/') == std::string::npos && release_part(s) && lower(s) != "readme.txt"); };
    for (const auto& e : all) {
        if (is_main(e.name)) { base.clear(); break; }
        const size_t s = e.name.find('/');
        if (s != std::string::npos && is_main(e.name.substr(s + 1))) { base = e.name.substr(0, s + 1); break; }
    }
    uint64_t need = 0;
    std::vector<std::pair<ZipEntry, std::string>> take;
    for (const auto& e : all) {
        if (e.is_dir || e.name.rfind(base, 0) != 0) continue;
        const std::string n = e.name.substr(base.size());
        if (release_part(n)) { take.push_back({ e, n }); need += e.size; }
    }
    const uint64_t room = free_space(root);
    if (room && room < need + (64u << 20)) return stop("Not enough free space: unpacking needs " + format_size(need) + " on this drive, and " + format_size(room) + " is free.");
    p->done = 0; p->total = need;
    for (const auto& t : take) {
        p->set_line("Unpacking " + t.second);
        r = zip_unpack_entry(zip, t.first, fresh / widen(t.second), p);
        if (r == "cancelled") return stop(r);
        if (!r.empty()) return stop("The release could not be unpacked: " + r + ". Nothing was changed.");
    }

    // 2. What came out: a build, a launcher, or it is not a release.
    p->total = 0; p->set_line("Checking the release");
    std::vector<fs::path> new_builds;
    int top_build = 0;
    for (auto& d : fs::directory_iterator(fresh / L"builds", ec)) {
        const int v = atoi(u8(d.path().filename()).c_str() + 1);
        if (!d.is_directory(ec) || v <= 0) continue;
        new_builds.push_back(d.path());
        top_build = std::max(top_build, v);
    }
    for (const auto& b : new_builds)
        if (!looks_like_program(b / L"RaCACiT.exe")) return stop("The release's build " + u8(b.filename()) + " has no game program. Nothing was changed.");
    fs::path new_launcher;
    for (auto& f : fs::directory_iterator(fresh, ec))
        if (f.is_regular_file(ec) && lower(u8(f.path().extension())) == ".exe") new_launcher = f.path();
    if (new_builds.empty() && new_launcher.empty()) return stop("That release holds no game build and no launcher. Nothing was changed.");
    // The launcher in the zip must at least start on this PC before it replaces this one.
    const fs::path self = exe_path().parent_path() == root ? exe_path() : root / L"RaCACiT Launcher.exe";
    if (!new_launcher.empty() && sha256_file(new_launcher) == sha256_file(self)) new_launcher.clear();   // the same one
    if (!new_launcher.empty()) {
        if (!looks_like_program(new_launcher)) return stop("The release's launcher is not a program. Nothing was changed.");
        const int code = run_hidden(L"\"" + new_launcher.wstring() + L"\" --self-test", fresh, &p->cancel);
        if (p->cancel) return stop("cancelled");
        if (code != 0) return stop("The release's new launcher did not start on this PC (" + std::to_string(code) + "). Nothing was changed.");
    }

    // 3. The saves, first. No backup, no update.
    if (p->cancel) return stop("cancelled");
    if (process_running_under(builds_dir())) return stop("The game is running. Close it, then install the update.");
    p->set_line("Backing up your saves");
    fs::path made;
    r = backup_saves("before-update", &made);
    if (!r.empty()) return stop("Backing up your saves failed, so nothing was installed: " + r);

    // 4. The swap: each piece renamed in, its old copy kept until all are in. Past here Cancel does nothing.
    p->set_line("Installing");
    fs::create_directories(aside, ec);
    struct Moved { fs::path dst, old; };
    std::vector<Moved> moved;
    int nth = 0;
    auto undo = [&] {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code e2;
            delete_tree(it->dst);
            if (!it->old.empty()) fs::rename(it->old, it->dst, e2);
        }
    };
    auto put = [&](const fs::path& from, const fs::path& dst, fs::path old = {}) -> bool {
        std::error_code e2;
        const bool had = fs::exists(dst, e2);
        if (had) {
            if (old.empty()) old = aside / std::to_wstring(nth++);
            fs::rename(dst, old, e2);
            if (e2) return false;
        }
        fs::create_directories(dst.parent_path(), e2);
        e2.clear();
        fs::rename(from, dst, e2);
        if (e2) { std::error_code e3; if (had) fs::rename(old, dst, e3); return false; }
        moved.push_back({ dst, had ? old : fs::path() });
        return true;
    };
    bool ok = true;
    for (const auto& b : new_builds) ok = ok && put(b, builds_dir() / b.filename());
    for (auto& m : fs::directory_iterator(fresh / L"mods", ec)) ok = ok && put(m.path(), mods_dir() / m.path().filename());
    if (fs::exists(fresh / L"README.txt", ec)) ok = ok && put(fresh / L"README.txt", root / L"README.txt");
    fs::path self_old = self; self_old += L".old";
    if (ok && !new_launcher.empty()) {
        // A running exe can be renamed but not deleted: this one goes aside beside itself, and the next
        // start deletes it. The new launcher takes its name, whatever the player calls it.
        DeleteFileW(self_old.c_str());
        ok = put(new_launcher, self, self_old);
    }
    if (!ok) {
        undo();
        return stop("A file could not be replaced (is something using this folder?), so your install was put back as it was.");
    }

    // 5. In. Only the new build and the one before it stay (Play always starts the newest here).
    if (top_build) {
        set_current(top_build);
        int before = 0;
        for (const auto& b : builds()) if (b.version < top_build) before = std::max(before, b.version);
        for (const auto& b : builds()) if (b.version != top_build && b.version != before) delete_tree(b.dir);
    }
    write_text_atomic(builds_dir() / L"release.txt", rel.tag + "\r\n");
    *restart = !new_launcher.empty();
    std::string s = rel.name + " is installed.";
    if (top_build) s += " Play starts build v" + std::to_string(top_build) + ".";
    s += " Your saves, settings, shader caches and game were left as they were";
    s += made.empty() ? "." : "; a save backup was made first (" + u8(made.filename()) + ").";
    if (summary) *summary = s;
    delete_tree(dir);
    return {};
}

// ---- mods ------------------------------------------------------------------------------
static std::string sanitize_id(const std::string& s)
{
    std::string o;
    for (char c : s) o += (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') ? c : '_';
    return o.empty() ? "mod" : o;
}

std::vector<ModInfo> Store::mods() const
{
    std::vector<ModInfo> out;
    std::error_code ec;
    for (auto& d : fs::directory_iterator(mods_dir(), ec)) {
        if (!d.is_directory(ec)) continue;
        const std::string n = u8(d.path().filename());
        if (n.empty() || n[0] == '.') continue;
        ModInfo m; m.dir = d.path(); m.id = n; m.name = n;
        const fs::path j = d.path() / L"mod.json";
        if (fs::exists(j, ec)) {
            Json js; std::string err;
            if (json_parse(read_text(j, 1 << 16), &js, &err)) {
                m.name = js.str("name", n); m.version = js.str("version"); m.author = js.str("author");
                m.description = js.str("description");
                // "settings": {"NAME": "value"}: game settings while the mod is on. Not the ones
                // the launcher sets itself (paths, log) -- those stay the launcher's.
                for (auto& kv : js["settings"].o) {
                    const Json& v = kv.second;
                    const std::string val = v.type == Json::Str ? v.s : v.type == Json::Bool ? (v.b ? "1" : "0")
                                          : v.type == Json::Num ? std::to_string((long long)v.n) : std::string();
                    if (!kv.first.empty() && !launcher_owned_key(kv.first) && !same_key(kv.first, "GAME")) m.settings.push_back({ kv.first, val });
                }
            } else m.problem = "mod.json is not valid JSON (" + err + ").";
        }
        m.has_files = fs::is_directory(d.path() / L"files", ec);
        if (m.problem.empty() && !m.has_files && m.settings.empty()) m.problem = "No files\\ folder and no settings: nothing to change.";
        out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return lower(a.name) < lower(b.name); });
    return out;
}

std::vector<std::string> Store::enabled_mods() const
{
    std::vector<std::string> l;
    const std::string s = settings.get("RACACIT_MODS_ENABLED");
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
        const std::string id = trim(s.substr(i, j - i));
        if (!id.empty()) l.push_back(id);
        i = j + 1;
    }
    return l;
}

void Store::set_enabled_mods(const std::vector<std::string>& ids)
{
    std::string s;
    for (auto& id : ids) { if (!s.empty()) s += ","; s += id; }
    settings.put("RACACIT_MODS_ENABLED", s);
    settings.save();
}

void Store::set_mod_enabled(const std::string& id, bool on)
{
    auto l = enabled_mods();
    l.erase(std::remove(l.begin(), l.end(), id), l.end());
    if (on) l.insert(l.begin(), id);   // the newest turned on wins (as on Android)
    set_enabled_mods(l);
}

void Store::move_mod(const std::string& id, int dir)
{
    auto l = enabled_mods();
    auto it = std::find(l.begin(), l.end(), id);
    if (it == l.end()) return;
    const long i = it - l.begin(), j = i + dir;
    if (j < 0 || j >= (long)l.size()) return;
    std::swap(l[i], l[j]);
    set_enabled_mods(l);
}

std::string Store::install_mod_folder(const fs::path& dir, std::string* id_out)
{
    std::error_code ec;
    std::string id = u8(dir.filename());
    if (fs::exists(dir / L"mod.json", ec)) {
        Json js; std::string err;
        if (!json_parse(read_text(dir / L"mod.json", 1 << 16), &js, &err)) return "mod.json is not valid JSON (" + err + ").";
        id = js.str("id", id);
    }
    id = sanitize_id(id);
    const fs::path dst = mods_dir() / widen(id);
    if (fs::exists(dst, ec) && fs::equivalent(dst, dir, ec)) { if (id_out) *id_out = id; return {}; }
    delete_tree(dst);
    Progress p;
    std::string r = copy_tree(dir, dst, &p);
    if (!r.empty()) return r;
    if (id_out) *id_out = id;
    return {};
}

std::string Store::install_mod_zip(const fs::path& zip, std::string* id_out)
{
    const fs::path tmp = mods_dir() / L".incoming";
    delete_tree(tmp);
    std::string r = zip_extract(zip, tmp);
    if (!r.empty()) { delete_tree(tmp); return r; }
    std::error_code ec;
    fs::path src = tmp;
    if (!fs::exists(src / L"mod.json", ec) && !fs::is_directory(src / L"files", ec)) {
        std::vector<fs::path> kids;
        for (auto& d : fs::directory_iterator(tmp, ec)) kids.push_back(d.path());
        if (kids.size() == 1 && fs::is_directory(kids[0], ec)) src = kids[0];
    }
    std::string id = src == tmp ? sanitize_id(u8(zip.stem())) : u8(src.filename());
    if (fs::exists(src / L"mod.json", ec)) {
        Json js; std::string err;
        if (!json_parse(read_text(src / L"mod.json", 1 << 16), &js, &err)) { delete_tree(tmp); return "mod.json is not valid JSON (" + err + ")."; }
        id = js.str("id", id);
    }
    id = sanitize_id(id);
    const fs::path dst = mods_dir() / widen(id);
    delete_tree(dst);
    fs::rename(src, dst, ec);
    if (ec) { Progress p; r = copy_tree(src, dst, &p); if (!r.empty()) { delete_tree(tmp); return r; } }
    delete_tree(tmp);
    if (id_out) *id_out = id;
    return {};
}

std::string Store::remove_mod(const std::string& id)
{
    set_mod_enabled(id, false);
    return delete_tree(mods_dir() / widen(id)) ? std::string() : "Could not delete the mod's folder.";
}

// ---- saves -----------------------------------------------------------------------------
std::vector<fs::path> Store::save_slots() const
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& d : fs::directory_iterator(saves(), ec))
        if (d.is_directory(ec) && u8(d.path().filename()).rfind("BCUS98124", 0) == 0) out.push_back(d.path());
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<fs::path> Store::backup_list() const
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& f : fs::directory_iterator(backups(), ec)) {
        const std::string n = u8(f.path().filename());
        if (n.rfind("saves-", 0) != 0) continue;
        if ((f.is_regular_file(ec) && f.path().extension() == L".zip") || f.is_directory(ec)) out.push_back(f.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string Store::backup_saves(const std::string& why, fs::path* made)
{
    if (made) made->clear();
    if (save_slots().empty()) return {};
    const std::string stem = "saves-" + stamp_now() + "-" + why;
    fs::path got = backups() / widen(stem + ".zip");
    std::string r = zip_create(saves(), got);
    if (!r.empty()) {
        /* tar.exe missing (older or trimmed Windows), or failing on the path:
         * the save folders copied as they are instead. On Play this failure
         * used to stop the game from starting at all (GitHub #5). Listed,
         * restored and pruned like a zip. */
        std::error_code ec; fs::remove(got, ec);
        got = backups() / widen(stem);
        Progress p;
        const std::string r2 = copy_tree(saves(), got, &p);
        if (!r2.empty()) { delete_tree(got); return r + "; a plain copy failed too: " + r2; }
    }
    if (made) *made = got;
    const int keep = std::max(1, atoi(ini.get("keep_backups", "8").c_str()));
    auto all = backup_list();
    for (size_t i = 0; i + keep < all.size(); i++) delete_tree(all[i]);
    return {};
}

std::string Store::export_saves(const fs::path& zip)
{
    if (save_slots().empty()) return "There are no saves yet.";
    return zip_create(saves(), zip);
}

static std::vector<fs::path> find_slots(const fs::path& dir)
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
        if (it->is_directory(ec) && u8(it->path().filename()).rfind("BCUS98124", 0) == 0) { out.push_back(it->path()); it.disable_recursion_pending(); }
    }
    return out;
}

std::string Store::import_saves(const fs::path& zip)
{
    std::error_code dec;
    if (fs::is_directory(zip, dec)) {               /* a folder backup (tar.exe failed when it was made) */
        const auto slots = find_slots(zip);
        if (slots.empty()) return "No A Crack in Time saves (BCUS98124...) in that backup.";
        std::string r = backup_saves("before-import", nullptr);
        if (!r.empty()) return "Backing up the current saves failed: " + r;
        fs::create_directories(saves(), dec);
        for (auto& s : slots) {
            const fs::path dst = saves() / s.filename();
            delete_tree(dst);
            Progress p;
            r = copy_tree(s, dst, &p);
            if (!r.empty()) return r;
        }
        return {};
    }
    const fs::path tmp = data() / L".save-import";
    delete_tree(tmp);
    std::string r = zip_extract(zip, tmp);
    if (!r.empty()) { delete_tree(tmp); return r; }
    const auto slots = find_slots(tmp);
    if (slots.empty()) { delete_tree(tmp); return "No A Crack in Time saves (BCUS98124...) in that zip."; }
    r = backup_saves("before-import", nullptr);
    if (!r.empty()) { delete_tree(tmp); return "Backing up the current saves failed: " + r; }
    std::error_code ec;
    fs::create_directories(saves(), ec);
    for (auto& s : slots) {
        const fs::path dst = saves() / s.filename();
        delete_tree(dst);
        fs::rename(s, dst, ec);
        if (ec) { Progress p; r = copy_tree(s, dst, &p); if (!r.empty()) { delete_tree(tmp); return r; } }
    }
    delete_tree(tmp);
    return {};
}

std::string Store::restore_backup(const fs::path& zip) { return import_saves(zip); }

// ---- logs -----------------------------------------------------------------------------
void Store::rotate_log()
{
    std::error_code ec;
    if (!fs::exists(log(), ec) || fs::file_size(log(), ec) == 0) return;
    fs::create_directories(logs(), ec);
    fs::rename(log(), logs() / widen("log-" + stamp_now() + ".txt"), ec);
    auto all = old_logs();
    const int keep = std::max(1, atoi(ini.get("keep_logs", "10").c_str()));
    for (size_t i = keep; i < all.size(); i++) fs::remove(all[i], ec);
}

std::vector<fs::path> Store::old_logs() const
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& f : fs::directory_iterator(logs(), ec))
        if (f.is_regular_file(ec) && f.path().extension() == L".txt") out.push_back(f.path());
    std::sort(out.rbegin(), out.rend());
    return out;
}

// ---- shader caches ------------------------------------------------------------------------
// Each build ships the caches it was played with, in the formats the game writes:
// shadercache.bin (translated shaders: "RSXSPV01", then records { u64 k1, u64 k2, u32 words,
// u32 words ^ 0x5350565A, u32 spirv[words] }) and pipelinelist.bin (the pipeline variants
// seen: "RSXPLS01", u32 record size, u32 0, then records of that size). They were copied into
// data\cache only when it had none, so a player who started on an older build never got a
// newer build's caches: every area compiled its shaders again during play, build after build.
// Now a build's entries are merged in once (cache\seeded.txt: the last build merged); the
// game itself drops the oldest when a file outgrows its cap. pipelinecache.bin is the
// graphics driver's own: copied only when there is none.
namespace {
const char kSpvMagic[8] = { 'R', 'S', 'X', 'S', 'P', 'V', '0', '1' };
const char kPlistMagic[8] = { 'R', 'S', 'X', 'P', 'L', 'S', '0', '1' };

struct SpvRec { uint64_t a, b, off; uint32_t n; };   // words at off + 24

// The whole records of a shader cache, as the game reads it (it stops at the first bad one);
// *end = where the last one ends. false: not a shader cache.
bool spv_records(const fs::path& p, std::vector<SpvRec>* out, uint64_t* end)
{
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    std::ifstream f(p, std::ios::binary);
    char m[8];
    if (ec || !f.read(m, 8) || memcmp(m, kSpvMagic, 8)) return false;
    uint64_t off = 8;
    unsigned char h[24];
    while (off + 24 <= size && f.read((char*)h, 24)) {
        SpvRec r; uint32_t check;
        memcpy(&r.a, h, 8); memcpy(&r.b, h + 8, 8); memcpy(&r.n, h + 16, 4); memcpy(&check, h + 20, 4);
        if (check != (r.n ^ 0x5350565Au) || r.n == 0 || r.n > (64u << 20) / 4u || off + 24 + (uint64_t)r.n * 4 > size) break;
        r.off = off;
        out->push_back(r);
        off += 24 + (uint64_t)r.n * 4;
        if (!f.seekg((std::streamoff)off)) break;
    }
    *end = off;
    return true;
}

// The records of `from` that `to` lacks, appended to it. false: nothing could be merged.
bool merge_shader_cache(const fs::path& from, const fs::path& to)
{
    std::vector<SpvRec> src, dst;
    uint64_t src_end = 0, dst_end = 0;
    if (!spv_records(from, &src, &src_end)) return false;
    std::error_code ec;
    if (!spv_records(to, &dst, &dst_end)) return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    // A torn tail (the game killed mid-write) would hide everything after it: cut it first.
    if (fs::file_size(to, ec) != dst_end) { fs::resize_file(to, dst_end, ec); if (ec) return false; }
    std::set<std::pair<uint64_t, uint64_t>> have;
    for (const SpvRec& r : dst) have.insert({ r.a, r.b });
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::app);
    std::vector<char> buf;
    for (const SpvRec& r : src) {
        if (!have.insert({ r.a, r.b }).second) continue;
        buf.resize(24 + (size_t)r.n * 4);
        if (!in.seekg((std::streamoff)r.off) || !in.read(buf.data(), (std::streamsize)buf.size())) break;
        out.write(buf.data(), (std::streamsize)buf.size());
    }
    return (bool)out;
}

bool merge_pipeline_list(const fs::path& from, const fs::path& to)
{
    const std::string s = read_text(from, 1u << 28), d = read_text(to, 1u << 28);
    auto rec_size = [](const std::string& t) -> uint32_t {
        if (t.size() < 16 || memcmp(t.data(), kPlistMagic, 8)) return 0;
        uint32_t n; memcpy(&n, t.data() + 8, 4); return n;
    };
    const uint32_t rs = rec_size(s);
    if (!rs) return false;
    std::error_code ec;
    // Another record layout: the game would start a new list over this one anyway.
    if (rec_size(d) != rs) return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    const size_t whole = (d.size() - 16) / rs;
    if (16 + whole * rs != d.size()) { fs::resize_file(to, 16 + whole * rs, ec); if (ec) return false; }   // a torn tail
    std::unordered_set<std::string> have;
    for (size_t i = 0; i < whole; i++) have.insert(d.substr(16 + i * rs, rs));
    std::ofstream out(to, std::ios::binary | std::ios::app);
    for (size_t i = 0; 16 + (i + 1) * rs <= s.size(); i++) {
        std::string r = s.substr(16 + i * rs, rs);
        if (have.insert(r).second) out.write(r.data(), (std::streamsize)rs);
    }
    return (bool)out;
}
}  // namespace

void Store::seed_caches(const BuildInfo& b)
{
    std::error_code ec;
    const fs::path mark = cache() / L"seeded.txt";
    const bool merged = atoi(trim(read_text(mark, 64)).c_str()) == b.version;
    bool ok = true;
    for (const wchar_t* cf : { L"shadercache.bin", L"pipelinecache.bin", L"pipelinelist.bin" }) {
        const fs::path from = b.dir / cf, to = cache() / cf;
        if (!fs::exists(from, ec)) continue;
        if (!fs::exists(to, ec)) { fs::copy_file(from, to, ec); continue; }   // a fresh install starts from the build's
        if (merged) continue;
        if (!wcscmp(cf, L"shadercache.bin")) ok = merge_shader_cache(from, to) && ok;
        else if (!wcscmp(cf, L"pipelinelist.bin")) ok = merge_pipeline_list(from, to) && ok;
    }
    if (!merged && ok) write_text_atomic(mark, std::to_string(b.version) + "\r\n");   // else tried again next Play
}

// ---- launch -----------------------------------------------------------------------------
static std::wstring fwd(const fs::path& p) { std::wstring s = p.wstring(); for (auto& c : s) if (c == L'\\') c = L'/'; return s; }

std::string Store::launch(void** process)
{
    *process = nullptr;
    reload();
    const GameInfo g = game();
    if (!g.ok) return "The game is not ready: " + g.status;
    const BuildInfo b = current_build();
    if (!b.version) return "No build installed: reinstall the launcher.";
    std::error_code ec;
    fs::create_directories(hdd(), ec); fs::create_directories(cache(), ec);
    seed_caches(b);
    if (ini.get("backup_on_play", "1") != "0") {
        /* A backup that cannot be made must not keep the game from starting
         * (GitHub #5: "backup save failed, game won't open"); the reason goes
         * to logs\backup_failed.txt. */
        const std::string r = backup_saves("play", nullptr);
        if (!r.empty()) {
            if (FILE* f = _wfopen((logs() / L"backup_failed.txt").c_str(), L"ab")) {
                fprintf(f, "%s  backup on Play failed: %s\r\n", stamp_now().c_str(), r.c_str());
                fclose(f);
            }
        }
    }
    rotate_log();

    std::map<std::wstring, std::wstring> set;
    {   // settings.txt as environment (the game's own reading of it does the same)
        std::string k, v; bool c;
        for (const auto& l : settings.lines)
            if (line_key(l, &k, &v, &c) && !c && !same_key(k, "GAME") && !launcher_owned_key(k)) set[widen(k)] = widen(v);
    }
    {   // Enabled mods' "settings", over settings.txt; the first in the list wins.
        const auto all = mods(); const auto en = enabled_mods();
        for (auto id = en.rbegin(); id != en.rend(); ++id)
            for (auto& m : all)
                if (m.id == *id && m.problem.empty())
                    for (auto& kv : m.settings) {
                        for (auto it = set.begin(); it != set.end(); )
                            it = _wcsicmp(it->first.c_str(), widen(kv.first).c_str()) == 0 ? set.erase(it) : std::next(it);
                        set[widen(kv.first)] = widen(kv.second);
                    }
    }
    set[L"RACACIT_SETTINGS_FILE"] = settings.path.wstring();
    set[L"PS3_HDD0_ROOT"] = hdd().wstring();
    set[L"RSX_SHADER_CACHE"] = (cache() / L"shadercache.bin").wstring();
    set[L"RSX_PIPELINE_CACHE"] = (cache() / L"pipelinecache.bin").wstring();
    set[L"RSX_PIPELINE_LIST"] = (cache() / L"pipelinelist.bin").wstring();
    set[L"RACACIT_LOG_FILE"] = log().wstring();
    set[L"RACACIT_LAUNCHER"] = L"1";
    std::wstring modlist;
    const auto all = mods();
    for (auto& id : enabled_mods())
        for (auto& m : all)
            if (m.id == id && m.problem.empty() && m.has_files) { if (!modlist.empty()) modlist += L";"; modlist += fwd(m.dir / L"files"); }
    if (!modlist.empty()) set[L"RACACIT_MODS"] = modlist;

    std::wstring env;
    if (wchar_t* cur = GetEnvironmentStringsW()) {
        for (wchar_t* e = cur; *e; e += wcslen(e) + 1) {
            const wchar_t* eq = wcschr(e + 1, L'=');
            const std::wstring k = eq ? std::wstring(e, (size_t)(eq - e)) : std::wstring(e);
            bool ours = false;
            for (auto& kv : set) if (_wcsicmp(kv.first.c_str(), k.c_str()) == 0) ours = true;
            if (!ours) { env += e; env += L'\0'; }
        }
        FreeEnvironmentStringsW(cur);
    }
    for (auto& kv : set) { env += kv.first + L"=" + kv.second; env += L'\0'; }
    env += L'\0';

    const fs::path exe = b.dir / L"RaCACiT.exe";
    const fs::path eboot = g.root / L"PS3_GAME" / L"USRDIR" / L"EBOOT.elf";
    std::wstring cl = L"\"" + exe.wstring() + L"\" \"" + eboot.wstring() + L"\"";
    // An older build writes log.txt beside itself: clear the last one so a stale
    // file is never taken for this session's.
    fs::remove(b.dir / L"log.txt", ec);
    launched_dir = b.dir;
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(),
                        b.dir.c_str(), &si, &pi))
        return "Could not start " + u8(exe) + " (error " + std::to_string(GetLastError()) + ").";
    CloseHandle(pi.hThread);
    *process = pi.hProcess;
    return {};
}

void Store::collect_log()
{
    std::error_code ec;
    if (launched_dir.empty()) return;
    const fs::path old = launched_dir / L"log.txt";
    if (fs::exists(old, ec) && fs::file_size(old, ec) > 0) {
        fs::remove(log(), ec);
        fs::rename(old, log(), ec);
        if (ec) fs::copy_file(old, log(), fs::copy_options::overwrite_existing, ec);
    }
    launched_dir.clear();
}
