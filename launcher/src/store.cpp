#include "store.h"
#include "iso.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>

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
    "# Update address: a JSON manifest of builds (README). Empty = updates off.\r\n"
    "update_url=\r\n"
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

std::string Store::install_build_zip(const fs::path& zip, int* version, bool make_current)
{
    const fs::path tmp = builds_dir() / L".incoming";
    delete_tree(tmp);
    std::string r = zip_extract(zip, tmp);
    if (!r.empty()) { delete_tree(tmp); return r; }
    std::error_code ec;
    fs::path src = tmp;
    if (!fs::exists(src / L"RaCACiT.exe", ec))
        for (auto& d : fs::directory_iterator(tmp, ec)) if (d.is_directory(ec) && fs::exists(d.path() / L"RaCACiT.exe", ec)) { src = d.path(); break; }
    if (!fs::exists(src / L"RaCACiT.exe", ec)) { delete_tree(tmp); return "The zip has no RaCACiT.exe."; }
    // The zip's own name says the version when the folder inside does not.
    if (!fs::exists(src / L"version.txt", ec)) {
        int v = version_of_name(u8(src == tmp ? zip.stem() : src.filename()));
        if (!v) v = version_of_name(u8(zip.stem()));
        if (v) write_text_atomic(src / L"version.txt", std::to_string(v) + "\r\n");
    }
    r = install_build_folder(src, version, make_current);
    delete_tree(tmp);
    return r;
}

std::string Store::remove_build(int version)
{
    if (current_build().version == version) return "That is the build Play starts; pick another first.";
    return delete_tree(builds_dir() / (L"v" + std::to_wstring(version))) ? std::string() : "Could not delete it (is it running?).";
}

// ---- updates ---------------------------------------------------------------------------
// The manifest: {"builds": [{"version": 66, "url": "https://.../RaCACiT_v66.zip",
//                            "sha256": "...", "size": 12345, "notes": "..."}, ...]}
// or one {"version": ...} object as "latest". The zip holds RaCACiT.exe (+ README.txt, version.txt).
std::string Store::check_update(int* latest, std::string* notes, std::string* url, std::string* sha256, uint64_t* size)
{
    const std::string u = update_url();
    if (u.empty()) return "Updates are off: no update_url in launcher.ini.";
    std::string body;
    std::string r = http_get(u, &body, nullptr, nullptr);
    if (!r.empty()) return r;
    Json j; std::string err;
    if (!json_parse(body, &j, &err)) return "The update list is not valid JSON (" + err + ").";
    const Json* best = nullptr;
    auto consider = [&](const Json& e) { if (e.type == Json::Obj && (!best || e.num("version") > best->num("version"))) best = &e; };
    for (const auto& e : j["builds"].a) consider(e);
    consider(j["latest"]);
    if (!best) return "The update list has no builds.";
    *latest = (int)best->num("version");
    *notes = best->str("notes");
    *url = best->str("url");
    *sha256 = lower(best->str("sha256"));
    *size = (uint64_t)best->num("size");
    return {};
}

std::string Store::download_update(const std::string& url, const std::string& sha256, int version, Progress* p)
{
    if (sha256.size() != 64) return "The update list gives no SHA-256 for this build: not installing it.";
    const fs::path f = builds_dir() / L".download" / (L"RaCACiT_v" + std::to_wstring(version) + L".zip");
    p->set_line("Downloading build v" + std::to_string(version));
    std::string r = http_get(url, nullptr, &f, p);
    if (!r.empty()) return r;
    p->set_line("Checking the download");
    p->done = 0;
    std::error_code ec; p->total = fs::file_size(f, ec);
    if (sha256_file(f, p) != sha256) { fs::remove(f, ec); return "The download does not match its SHA-256: not installed."; }
    p->set_line("Installing");
    int v = 0;
    r = install_build_zip(f, &v, true);
    fs::remove(f, ec);
    return r;
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
        if (f.is_regular_file(ec) && n.rfind("saves-", 0) == 0 && f.path().extension() == L".zip") out.push_back(f.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string Store::backup_saves(const std::string& why, fs::path* made)
{
    if (made) made->clear();
    if (save_slots().empty()) return {};
    const fs::path z = backups() / widen("saves-" + stamp_now() + "-" + why + ".zip");
    std::string r = zip_create(saves(), z);
    if (!r.empty()) return r;
    if (made) *made = z;
    const int keep = std::max(1, atoi(ini.get("keep_backups", "8").c_str()));
    auto all = backup_list();
    std::error_code ec;
    for (size_t i = 0; i + keep < all.size(); i++) fs::remove(all[i], ec);
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

// ---- launch -----------------------------------------------------------------------------
static std::wstring fwd(const fs::path& p) { std::wstring s = p.wstring(); for (auto& c : s) if (c == L'\\') c = L'/'; return s; }

std::string Store::launch(void** process)
{
    *process = nullptr;
    reload();
    const GameInfo g = game();
    if (!g.ok) return "The game is not ready: " + g.status;
    const BuildInfo b = current_build();
    if (!b.version) return "No build installed (Builds & Updates).";
    std::error_code ec;
    fs::create_directories(hdd(), ec); fs::create_directories(cache(), ec);
    // The build ships its shader caches; a fresh install starts from them
    // instead of compiling every shader during play.
    for (const wchar_t* cf : { L"shadercache.bin", L"pipelinecache.bin", L"pipelinelist.bin" })
        if (!fs::exists(cache() / cf, ec) && fs::exists(b.dir / cf, ec))
            fs::copy_file(b.dir / cf, cache() / cf, ec);
    if (ini.get("backup_on_play", "1") != "0") {
        const std::string r = backup_saves("play", nullptr);
        if (!r.empty()) return "Backing up the saves failed: " + r + " (backup_on_play=0 in launcher.ini skips it).";
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
