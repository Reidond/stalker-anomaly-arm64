// mods_mount_test: the mod list and the engine's mounting of mods (docs/ios-port/MOD-MANAGER.md, "Mounting"), on generated mods.
//
//   mods_mount_test <scenario>          one scenario; exit status 0 when every check passes
//   mods_mount_test --list              the scenario names
//   mods_mount_test perf <files> <mb>   mounts one generated mod of that many files and size (written for real) and prints the cost
//
// Each scenario builds its own tree in a temporary folder (a stock archive, mods, a manual install in the game's gamedata,
// modlist.json), writes the fsgame.ltx the shell would (the same order as Shell.mm), initializes xrCore with it and looks at what
// the file system serves. xrCore can only be initialized once per process, hence one process per scenario (ctest runs them all).
#include "xrCore/xrCore.h"
#include "xrCore/log_lines.h"
#include "xrCore/stream_reader.h"

#include "Platform/Apple/Mods.h"
#include "Platform/Apple/ShellInternal.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

// Overlay.cpp (the packer of the stock-like archives used here) logs through the shell
void AppleShell::Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

namespace
{
int g_failures = 0;
std::vector<std::string> g_engine_log;

void check(bool ok, const std::string& what)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

void engine_log(LPCSTR line)
{
    g_engine_log.push_back(line);
    printf("    | %s\n", line);
}

// the engine's log lines so far (what Msg wrote, whether or not a log file is open yet)
bool log_has(const std::string& needle)
{
    xr_vector<xr_string> lines;
    if (const u32 count = LogLineCount())
        LogCopyLines(count - 1, count, lines);
    for (const xr_string& l : lines)
        if (l.find(needle) != std::string::npos)
            return true;
    return false;
}

void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// what the file system serves for `name` in the alias `$game_config$` / `$game_scripts$`; "<none>" when it is not there
std::string served(const char* alias, const char* name)
{
    IReader* r = FS.r_open(alias, name);
    if (!r)
        return "<none>";
    std::string s((const char*)r->pointer(), r->length());
    FS.r_close(r);
    return s;
}

// Every folder the tests make lives below a fresh mkdtemp folder (`base`) that carries a marker file; nothing is ever removed unless that
// marker is there and the folder is the one this process made. No test passes or computes a parent path to a removal function.
const char* const kMarker = ".made_by_mods_mount_test";

struct Tree
{
    fs::path base, root, game, mods, run;
};

Tree make_tree(const std::string& tag)
{
    std::string pattern = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/mods_mount_test_XXXXXX";
    if (!mkdtemp(pattern.data()))
    {
        printf("cannot make a temporary folder from %s\n", pattern.c_str());
        exit(2);
    }
    Tree t;
    t.base = fs::canonical(pattern);
    write_file(t.base / kMarker, "made by mods_mount_test (pid " + std::to_string(getpid()) + ")");
    t.root = t.base / tag; // the tag may have a space and a non-ASCII letter
    t.game = t.root / "game";
    t.mods = t.root / "Mods";
    t.run = t.root / "run";
    fs::create_directories(t.game / "db");
    fs::create_directories(t.mods);
    fs::create_directories(t.run / "appdata");
    return t;
}

void remove_tree(const Tree& t)
{
    const std::string name = t.base.filename().string();
    if (name.rfind("mods_mount_test_", 0) != 0 || read_file(t.base / kMarker).find("made by mods_mount_test") != 0)
    {
        printf("not removing %s: it is not a folder this test made\n", t.base.c_str());
        return;
    }
    fs::remove_all(t.base);
}

// An archive in the shell's own format (the Monolith overlay packer): the files of `src` appear below $fs_root$\gamedata\.
void make_archive(const fs::path& src, const fs::path& out_dir, const std::string& name)
{
    fs::create_directories(out_dir);
    if (!AppleShell::Internal::BuildOverlayArchive(src.string(), out_dir.string()))
    {
        printf("cannot pack %s\n", src.c_str());
        exit(2);
    }
    fs::rename(out_dir / "monolith_overlay.db0", out_dir / name);
    fs::remove(out_dir / "monolith_overlay.sig");
}

void mod_file(const Tree& t, const std::string& id, const std::string& relative, const std::string& content)
{
    write_file(t.mods / id / "content" / relative, content);
}

void write_modlist(const Tree& t, const std::vector<std::pair<std::string, bool>>& list)
{
    std::string json = "{\"schema\":1,\"mods\":[";
    for (size_t i = 0; i < list.size(); ++i)
        json += std::string(i ? "," : "") + "{\"id\":\"" + list[i].first + "\",\"enabled\":" + (list[i].second ? "true" : "false") + "}";
    json += "]}";
    write_file(t.mods / "modlist.json", json);
}

// The mods the scenarios share:
//   stock   archive db/stock.db0: configs/shared.ltx, configs/stock_only.ltx, scripts/stock_script.script
//   a       gamedata/configs/shared.ltx, only_a.ltx, scripts/only_a.script
//   b       gamedata/configs/shared.ltx, db/b_patch.db0 (configs/archived.ltx, configs/shared.ltx, configs/b_archive_only.ltx)
//   c       gamedata/configs/only_c.ltx
//   case    GameData/Configs/CaseTest.LTX, GameData/Scripts/MixedCase.Script (capitals as in an archive made on Windows)
//   e       gamedata/configs/only_e.ltx
void make_common(const Tree& t)
{
    const fs::path stock = t.root / "stock_src";
    write_file(stock / "configs/shared.ltx", "stock shared");
    write_file(stock / "configs/stock_only.ltx", "stock only");
    write_file(stock / "scripts/stock_script.script", "-- stock script");
    make_archive(stock, t.game / "db", "stock.db0");

    for (const char* id : {"a", "b", "c", "case", "e"})
        write_file(t.mods / id / "mod.json", std::string("{\"schema\":1,\"id\":\"") + id + "\"}");
    mod_file(t, "a", "gamedata/configs/shared.ltx", "mod a shared");
    mod_file(t, "a", "gamedata/configs/only_a.ltx", "mod a only");
    mod_file(t, "a", "gamedata/scripts/only_a.script", "-- mod a script");
    mod_file(t, "a", "gamedata/configs/.hidden.ltx", "hidden files are not mounted");

    mod_file(t, "b", "gamedata/configs/shared.ltx", "mod b shared");
    const fs::path b_src = t.root / "b_src";
    write_file(b_src / "configs/archived.ltx", "b archive");
    write_file(b_src / "configs/shared.ltx", "b archive shared");
    write_file(b_src / "configs/b_archive_only.ltx", "b archive only");
    make_archive(b_src, t.mods / "b/content/db", "b_patch.db0");

    mod_file(t, "c", "gamedata/configs/only_c.ltx", "mod c only");
    mod_file(t, "case", "GameData/Configs/CaseTest.LTX", "case ok");
    mod_file(t, "case", "GameData/Scripts/MixedCase.Script", "-- mixed case");
    mod_file(t, "e", "gamedata/configs/only_e.ltx", "mod e only");
}

// The generated fsgame.ltx, in the order of Shell.mm: archives, patches, addons, the mods, the game folder's own gamedata.
std::string fsgame_text(const Tree& t, const AppleShell::ModResolution& mods)
{
    std::string s;
    s += "$fs_root$ = false | false | " + t.game.string() + "\n";
    s += "$app_data_root$ = true | false | " + (t.run / "appdata").string() + "\n";
    s += "$arch_dir$ = false | false | $fs_root$ | db\\\n";
    s += "$arch_dir_patches$ = false | true | $arch_dir$ | patches\\\n";
    s += "$arch_dir_addons$ = false | true | $arch_dir$ | addons\\\n";
    s += AppleShell::ModFsGameLines(mods);
    s += "$game_data$ = true | true | $fs_root$ | gamedata\\\n";
    s += "$game_config$ = true | false | $game_data$ | configs\\\n";
    s += "$game_scripts$ = true | false | $game_data$ | scripts\\ | *.script\n";
    s += "$logs$ = true | false | $app_data_root$ | logs\\\n";
    return s;
}

AppleShell::ModResolution start_fs(const Tree& t, bool mods_off = false)
{
    const AppleShell::ModResolution mods = AppleShell::ResolveMods(t.mods.string(), mods_off);
    for (const std::string& line : AppleShell::DescribeMods(mods))
        printf("    # %s\n", line.c_str());
    const fs::path fsgame = t.run / "fsgame.ltx";
    write_file(fsgame, fsgame_text(t, mods));
    Debug._initialize(false);
    Core._initialize("mods_mount_test", engine_log, TRUE, fsgame.c_str());
    check(FS.m_Flags.is(CLocatorAPI::flReady), "file system ready");
    return mods;
}

std::string ids(const AppleShell::ModResolution& m)
{
    std::string s;
    for (const auto& x : m.mounts)
        s += (s.empty() ? "" : ",") + x.id;
    return s;
}

// every file below `dir` with its size, to see that nothing was written into a mod folder
std::map<std::string, uintmax_t> snapshot(const fs::path& dir)
{
    std::map<std::string, uintmax_t> files;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        files[e.path().string()] = e.is_regular_file() ? e.file_size() : 0;
    return files;
}

// ---- scenarios ---------------------------------------------------------------------------------------------------------------------

int priority(bool reversed)
{
    Tree t = make_tree(reversed ? "priority_rev" : "priority");
    make_common(t);
    // c is disabled, e is not listed, d is listed but its folder is gone
    if (reversed)
        write_modlist(t, {{"b", true}, {"c", false}, {"d", true}, {"a", true}});
    else
        write_modlist(t, {{"a", true}, {"c", false}, {"d", true}, {"b", true}});
    const auto mods = start_fs(t);
    check(ids(mods) == (reversed ? "b,a" : "a,b"), "mounted in list order: " + ids(mods));
    check(mods.listed == 4 && mods.disabled == 1, "4 listed, 1 disabled");
    check(mods.unlisted_folders == 2, "2 folders are not in the list (case, e)");
    bool noted = false;
    for (const auto& n : mods.notes)
        noted = noted || n.find("d:") == 0;
    check(noted, "the removed folder of d is noted, the others still mount");

    // a stock file overridden by a mod, and two mods overriding the same file: the later one wins
    check(served("$game_config$", "shared.ltx") == (reversed ? "mod a shared" : "mod b shared"), "the later mod wins shared.ltx: " + served("$game_config$", "shared.ltx"));
    check(served("$game_config$", "stock_only.ltx") == "stock only", "a stock file no mod touches is still served");
    check(served("$game_config$", "only_a.ltx") == "mod a only", "a file only mod a has");
    check(served("$game_scripts$", "only_a.script") == "-- mod a script", "a script of mod a");
    check(served("$game_scripts$", "stock_script.script") == "-- stock script", "a stock script");
    // archives of a mod: their files appear, and the mod's own loose files beat its archive
    check(served("$game_config$", "archived.ltx") == "b archive", "a file from the db archive of mod b");
    check(served("$game_config$", "b_archive_only.ltx") == "b archive only", "... and another one");
    // mod b: db archive, then its loose gamedata; mod a is below or above it as listed
    check(served("$game_config$", "only_c.ltx") == "<none>", "a disabled mod is absent");
    check(served("$game_config$", "only_e.ltx") == "<none>", "an unlisted folder is absent");
    check(served("$game_config$", ".hidden.ltx") == "<none>", "hidden files are not mounted");
    check(log_has("* mod a: 0 archives, 3 files in 2 folders"), "the engine log has one line for mod a with its file count");
    check(log_has("* mod b: 1 archives, 1 files in 1 folders"), "... and for mod b with its archive");

    // listing, which Lua's getFS():file_list_open and the engine's directory scans use
    FS_FileSet set;
    FS.file_list(set, "$game_config$", FS_ListFiles);
    std::set<std::string> names;
    for (const FS_File& f : set)
        names.insert(f.name.c_str());
    check(names.count("only_a.ltx") && names.count("shared.ltx") && names.count("stock_only.ltx") && names.count("archived.ltx"),
          "file_list of $game_config$ shows stock, archive and mod files");
    check(!names.count("only_c.ltx"), "... and not the disabled mod's");
    // the same file through the stream reader (levels, sounds)
    CStreamReader* sr = FS.rs_open("$game_config$", "only_a.ltx");
    check(sr && sr->length() == 10, "rs_open of a mounted file (10 bytes)");
    if (sr)
    {
        char buf[16] = {};
        sr->r(buf, 10);
        check(std::string(buf, 10) == "mod a only", "... reads its bytes");
        FS.r_close(sr);
    }
    Core._destroy();
    remove_tree(t);
    return g_failures;
}

// A mods folder with a space and a non-ASCII letter in its path (the Mac's "Application Support" has a space; a player's folder can have anything)
int odd_paths()
{
    Tree t = make_tree("odd path \xc3\xa9");
    make_common(t);
    write_modlist(t, {{"a", true}, {"b", true}});
    const auto mods = start_fs(t);
    check(ids(mods) == "a,b", "mounted: " + ids(mods));
    check(t.mods.string().find(' ') != std::string::npos, "the mods path has a space");
    check(served("$game_config$", "shared.ltx") == "mod b shared", "files are served from it");
    check(served("$game_config$", "archived.ltx") == "b archive", "... and so is the db archive in it");
    check(served("$game_scripts$", "only_a.script") == "-- mod a script", "... and a script");
    Core._destroy();
    remove_tree(t);
    return g_failures;
}

int manual_wins()
{
    Tree t = make_tree("manual");
    make_common(t);
    write_file(t.game / "gamedata/configs/shared.ltx", "manual shared");
    write_file(t.game / "gamedata/configs/loose_only.ltx", "loose only");
    write_modlist(t, {{"a", true}, {"b", true}});
    const auto mods = start_fs(t);
    check(ids(mods) == "a,b", "a and b mounted");
    check(served("$game_config$", "shared.ltx") == "manual shared", "a file in the game's own gamedata/ wins over every mod");
    check(served("$game_config$", "loose_only.ltx") == "loose only", "a loose file with no mod involved");
    check(served("$game_config$", "only_a.ltx") == "mod a only", "mod files the manual install does not replace are served");
    Core._destroy();
    remove_tree(t);
    return g_failures;
}

int disable(bool disable_b)
{
    Tree t = make_tree(disable_b ? "disable_b" : "disable_a");
    make_common(t);
    write_modlist(t, {{"a", disable_b}, {"b", !disable_b}});
    const auto mods = start_fs(t);
    check(ids(mods) == (disable_b ? "a" : "b"), "only the enabled mod is mounted: " + ids(mods));
    check(served("$game_config$", "shared.ltx") == (disable_b ? "mod a shared" : "mod b shared"), "shared.ltx comes from the enabled one");
    check(served("$game_config$", disable_b ? "archived.ltx" : "only_a.ltx") == "<none>", "the disabled mod's files are absent");
    Core._destroy();
    remove_tree(t);
    return g_failures;
}

int mixed_case()
{
    Tree t = make_tree("case");
    make_common(t);
    write_modlist(t, {{"case", true}});
    const auto mods = start_fs(t);
    check(ids(mods) == "case", "the mod is mounted");
    check(mods.mounts.size() == 1 && mods.mounts[0].has_gamedata, "GameData/ is found as the gamedata folder");
    check(served("$game_config$", "casetest.ltx") == "case ok", "GameData/Configs/CaseTest.LTX opens as casetest.ltx");
    check(served("$game_config$", "CaseTest.Ltx") == "case ok", "... and as CaseTest.Ltx");
    check(served("$game_scripts$", "mixedcase.script") == "-- mixed case", "GameData/Scripts/MixedCase.Script opens as mixedcase.script");
    check(served("$game_config$", "CASETEST.LTX") == "case ok", "... and as CASETEST.LTX");
    Core._destroy();
    remove_tree(t);
    return g_failures;
}

int write_isolation()
{
    Tree t = make_tree("writes");
    make_common(t);
    write_modlist(t, {{"a", true}, {"b", true}});
    const auto before = snapshot(t.mods);
    const auto mods = start_fs(t);
    check(ids(mods) == "a,b", "a and b mounted");
    // the engine writes by the same virtual name it reads (MCM's axr_options.ltx): that must not reach the mod's file
    IWriter* w = FS.w_open("$game_config$", "only_a.ltx");
    check(w != nullptr, "w_open of a name a mod provides");
    if (w)
    {
        w->w_string("written by the game");
        FS.w_close(w);
    }
    check(snapshot(t.mods) == before, "nothing in the mods folder changed (same files, same sizes)");
    check(read_file(t.mods / "a/content/gamedata/configs/only_a.ltx") == "mod a only", "the mod's file still has its bytes");
    check(fs::exists(t.game / "gamedata/configs/only_a.ltx"), "the write went to the game folder's gamedata/ (not to the mod)");
    check(served("$game_config$", "only_a.ltx").find("written by the game") == 0, "and what was written is what the engine now reads");
    Core._destroy();
    remove_tree(t);
    return g_failures;
}

// The folder the removal functions act on, the way the shell hands it over: ANOMALY_MODS_ROOT, then SetModsRoot
void point_at(const fs::path& mods_root)
{
    setenv("ANOMALY_MODS_ROOT", mods_root.c_str(), 1);
    AppleShell::SetModsRoot(getenv("ANOMALY_MODS_ROOT"));
}

// mods_delete / mods_clear: only the shell's mods root, only when it is named Mods and has a marker, only entries directly inside it,
// symbolic links not followed
int removal()
{
    Tree t = make_tree("remove");
    make_common(t);
    write_modlist(t, {{"a", true}, {"b", true}});
    // something outside the mods root that a link inside a mod points to
    const fs::path outside = t.root / "outside";
    write_file(outside / "keep.txt", "must survive");
    fs::create_directories(t.mods / "a/content");
    fs::create_symlink(outside, t.mods / "a/content/link_to_outside");
    fs::create_symlink(outside, t.mods / "link_entry");
    write_file(t.mods / "plain_folder/readme.txt", "a folder without a mod.json");

    AppleShell::SetModsRoot("");
    auto r = AppleShell::ClearModsRoot();
    check(r.removed.empty() && !r.refused.empty() && fs::exists(t.mods / "a"), "nothing is removed before the shell has set a mods root");

    point_at(t.mods);
    for (const char* bad : {"", ".", "..", "../outside", "a/content", "/etc", "a\\b", "outside/", "plain_folder", "link_entry"})
    {
        r = AppleShell::RemoveModFolder(bad);
        check(r.removed.empty() && !r.refused.empty(), std::string("refused: \"") + bad + "\"");
    }
    check(fs::exists(outside / "keep.txt") && fs::exists(t.mods / "a") && fs::exists(t.mods / "plain_folder") && fs::is_symlink(t.mods / "link_entry"),
          "nothing was touched by the refused ids");
    r = AppleShell::RemoveModFolder("nowhere");
    check(r.removed.empty() && r.refused.empty(), "an id that does not exist removes nothing and is no error");

    r = AppleShell::RemoveModFolder("a");
    check(r.removed.size() == 1 && !fs::exists(t.mods / "a"), "mod a (it has a mod.json) is removed");
    check(read_file(outside / "keep.txt") == "must survive", "the link inside mod a was removed as a link: its target is untouched");
    check(fs::exists(t.mods / "b") && fs::exists(t.mods / "modlist.json"), "the other entries are still there");

    r = AppleShell::ClearModsRoot();
    check(r.refused.empty() && fs::is_directory(t.mods) && fs::is_empty(t.mods), "mods_clear leaves an empty mods root: " + std::to_string(r.removed.size()) + " entries removed");
    check(!fs::exists(t.mods / "link_entry") && fs::exists(outside / "keep.txt"), "... a link entry went as a link, its target stayed");
    check(fs::exists(t.game / "db/stock.db0"), "the game folder is untouched");
    r = AppleShell::ClearModsRoot();
    check(r.removed.empty() && r.refused.empty(), "an empty mods root is a mods root: nothing to remove, no error");
    remove_tree(t);
    return g_failures;
}

// Folders that are not a mods root: the functions refuse and delete nothing
int removal_refuses()
{
    Tree t = make_tree("refuse");
    // 1. not named Mods, though it has a modlist.json
    const fs::path other = t.root / "Stuff";
    write_file(other / "modlist.json", "{}");
    write_file(other / "precious.txt", "keep");
    // 2. named Mods but no marker: no modlist.json, and something that is not a mod folder in it
    const fs::path unmarked = t.root / "unmarked" / "Mods";
    write_file(unmarked / "precious.txt", "keep");
    write_file(unmarked / "a/mod.json", "{}");
    // 3. named Mods, no modlist.json, a folder without mod.json
    const fs::path half = t.root / "half" / "Mods";
    write_file(half / "a/mod.json", "{}");
    write_file(half / "docs/readme.txt", "keep");
    // 4. named mods in lower case
    const fs::path lower = t.root / "lower" / "mods";
    write_file(lower / "modlist.json", "{}");
    write_file(lower / "a/mod.json", "{}");
    // 5. a link named Mods that points at a folder with no marker
    const fs::path target = t.root / "linktarget";
    write_file(target / "precious.txt", "keep");
    fs::create_directories(t.root / "viaLink");
    fs::create_symlink(target, t.root / "viaLink" / "Mods");
    // 6. the mods root of the tree itself, to be sure the positive case works with the same code
    write_modlist(t, {});

    struct Case { const char* what; fs::path root; fs::path survivor; };
    const Case cases[] = {
        {"a folder not named Mods (with modlist.json)", other, other / "precious.txt"},
        {"a Mods folder without modlist.json and with a non-mod entry", unmarked, unmarked / "precious.txt"},
        {"a Mods folder where one folder has no mod.json", half, half / "docs/readme.txt"},
        {"a folder named mods in lower case", lower, lower / "a/mod.json"},
        {"a link named Mods to a folder without a marker", t.root / "viaLink" / "Mods", target / "precious.txt"},
    };
    for (const Case& c : cases)
    {
        point_at(c.root);
        auto r = AppleShell::ClearModsRoot();
        check(r.removed.empty() && !r.refused.empty() && fs::exists(c.survivor), std::string("mods_clear refuses: ") + c.what);
        r = AppleShell::RemoveModFolder("a");
        check(r.removed.empty() && fs::exists(c.survivor), std::string("mods_delete deletes nothing: ") + c.what);
    }
    // the same code does work on a real mods root
    point_at(t.mods);
    write_file(t.mods / "m/mod.json", "{}");
    auto r = AppleShell::RemoveModFolder("m");
    check(r.removed.size() == 1 && !fs::exists(t.mods / "m"), "a real mods root (modlist.json present): mods_delete removes a mod folder");
    // a Mods folder with no modlist.json whose entries are all mod folders is a mods root too
    fs::remove(t.mods / "modlist.json");
    write_file(t.mods / "m2/mod.json", "{}");
    r = AppleShell::ClearModsRoot();
    check(r.removed.size() == 1 && fs::is_empty(t.mods), "... and so is one that holds only mod folders");
    remove_tree(t);
    return g_failures;
}

int mods_off()
{
    Tree t = make_tree("off");
    make_common(t);
    write_modlist(t, {{"a", true}, {"b", true}});
    const auto mods = start_fs(t, true);
    check(mods.mounts.empty() && mods.mods_off, "--mods-off mounts nothing");
    check(served("$game_config$", "shared.ltx") == "stock shared", "the stock file is served");
    check(served("$game_config$", "only_a.ltx") == "<none>", "no mod file is");
    Core._destroy();
    remove_tree(t);
    return g_failures;
}

// modlist.json variants: ResolveMods only (no file system)
int bad_lists()
{
    Tree t = make_tree("badlists");
    make_common(t);
    struct Case
    {
        const char* what;
        const char* json;
        bool error;       // expects `error`
        const char* mounts; // expected ids
    };
    const Case cases[] = {
        {"valid", R"({"schema":1,"mods":[{"id":"a","enabled":true}]})", false, "a"},
        {"empty file", "", true, ""},
        {"not JSON", "this is { not json", true, ""},
        {"truncated", R"({"schema":1,"mods":[{"id":"a","enab)", true, ""},
        {"array at the top", R"([{"id":"a","enabled":true}])", true, ""},
        {"newer schema", R"({"schema":2,"mods":[{"id":"a","enabled":true}]})", true, ""},
        {"schema as text", R"({"schema":"1","mods":[{"id":"a","enabled":true}]})", true, ""},
        {"no mods key", R"({"schema":1})", true, ""},
        {"mods is not an array", R"({"schema":1,"mods":{"id":"a"}})", true, ""},
        {"no schema key is accepted", R"({"mods":[{"id":"a","enabled":true}]})", false, "a"},
        {"entry is not an object", R"({"schema":1,"mods":[3,"a",{"id":"b","enabled":true}]})", false, "b"},
        {"id with a path", R"({"schema":1,"mods":[{"id":"../a","enabled":true},{"id":"a/b","enabled":true},{"id":"b","enabled":true}]})", false, "b"},
        {"id of the downloads folder", R"({"schema":1,"mods":[{"id":".downloads","enabled":true},{"id":"b","enabled":true}]})", false, "b"},
        {"id missing or not a string", R"({"schema":1,"mods":[{"enabled":true},{"id":5,"enabled":true},{"id":"b","enabled":true}]})", false, "b"},
        {"duplicate: the first entry counts", R"({"schema":1,"mods":[{"id":"a","enabled":false},{"id":"a","enabled":true},{"id":"b","enabled":true}]})", false, "b"},
        {"enabled missing means disabled", R"({"schema":1,"mods":[{"id":"a"},{"id":"b","enabled":true}]})", false, "b"},
        {"enabled as text means disabled", R"({"schema":1,"mods":[{"id":"a","enabled":"yes"},{"id":"b","enabled":true}]})", false, "b"},
        {"listed but missing folder", R"({"schema":1,"mods":[{"id":"zz","enabled":true},{"id":"b","enabled":true}]})", false, "b"},
        {"unknown fields are ignored", R"({"schema":1,"x":[1],"mods":[{"id":"a","enabled":true,"note":"hi"}]})", false, "a"},
    };
    for (const Case& c : cases)
    {
        write_file(t.mods / "modlist.json", c.json);
        const auto r = AppleShell::ResolveMods(t.mods.string(), false);
        const bool ok = (r.error.empty() != c.error) && ids(r) == c.mounts;
        check(ok, std::string(c.what) + " -> " + (r.error.empty() ? "no error" : "error \"" + r.error + "\"") + ", mounts [" + ids(r) + "]");
    }
    // a missing file is an empty list and no error; an unreadable folder too
    fs::remove(t.mods / "modlist.json");
    auto r = AppleShell::ResolveMods(t.mods.string(), false);
    check(r.error.empty() && r.mounts.empty() && !r.modlist_found, "no modlist.json: no mods, no error");
    r = AppleShell::ResolveMods((t.root / "nowhere").string(), false);
    check(r.error.empty() && r.mounts.empty(), "no mods folder: no mods, no error");
    r = AppleShell::ResolveMods((t.root / "has|bar").string(), false);
    check(!r.error.empty() && r.mounts.empty(), "a mods folder path fsgame.ltx cannot hold is refused");
    // an enabled mod without content/ is left out with a note
    fs::create_directories(t.mods / "empty");
    write_file(t.mods / "modlist.json", R"({"schema":1,"mods":[{"id":"empty","enabled":true},{"id":"a","enabled":true}]})");
    r = AppleShell::ResolveMods(t.mods.string(), false);
    check(ids(r) == "a" && r.notes.size() == 1 && r.notes[0].find("no content/") != std::string::npos, "an enabled folder without content/ is skipped with a note");
    remove_tree(t);
    return g_failures;
}

// An invalid modlist.json: the engine starts with no mods
int bad_list_fs()
{
    Tree t = make_tree("badfs");
    make_common(t);
    write_file(t.mods / "modlist.json", "{ this is not json");
    const auto mods = start_fs(t);
    check(!mods.error.empty() && mods.mounts.empty(), "invalid JSON: an error and no mods");
    check(served("$game_config$", "shared.ltx") == "stock shared", "the game starts with its stock files");
    check(served("$game_config$", "only_a.ltx") == "<none>", "and none of the mods");
    Core._destroy();
    remove_tree(t);
    return g_failures;
}

// Cost of a big mod: `files` files of `mb` megabytes in all, written for real, mounted by the engine.
int perf(int files, int mb)
{
    Tree t = make_tree("perf");
    make_common(t);
    const std::string chunk(1 << 20, 'x');
    const long per_file = std::max(1L, (long)mb * 1024 * 1024 / files);
    printf("  writing %d files, %d MB ...\n", files, mb);
    for (int i = 0; i < files; ++i)
    {
        const fs::path p = t.mods / "big/content/gamedata/textures" / ("dir" + std::to_string(i % 40)) / ("tex_" + std::to_string(i) + ".dds");
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        for (long left = per_file; left > 0; left -= (long)chunk.size())
            out.write(chunk.data(), std::min<long>(left, (long)chunk.size()));
    }
    write_modlist(t, {{"big", true}});
    const auto t0 = std::chrono::steady_clock::now();
    const auto mods = start_fs(t);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("  resolve + engine file system init: %.1f ms (%d files, %d MB)\n", ms, files, mb);
    {
        xr_vector<xr_string> lines;
        if (const u32 count = LogLineCount())
            LogCopyLines(count - 1, count, lines);
        for (const xr_string& l : lines)
            if (l.find("* mod ") == 0 || l.find("FS: ") == 0)
                printf("  engine log: %s\n", l.c_str());
    }
    check(ids(mods) == "big", "mounted");
    check(FS.exist("$game_data$", "textures\\dir0\\tex_0.dds") != nullptr, "a texture of the mod is registered");
    Core._destroy();
    remove_tree(t);
    return g_failures;
}
} // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::map<std::string, int (*)()> scenarios = {
        {"priority", [] { return priority(false); }},
        {"priority_reversed", [] { return priority(true); }},
        {"manual_wins", manual_wins},
        {"odd_paths", odd_paths},
        {"disable_a", [] { return disable(false); }},
        {"disable_b", [] { return disable(true); }},
        {"case", mixed_case},
        {"write_isolation", write_isolation},
        {"mods_off", mods_off},
        {"removal", removal},
        {"removal_refuses", removal_refuses},
        {"bad_lists", bad_lists},
        {"bad_list_fs", bad_list_fs},
    };
    if (argc >= 2 && !strcmp(argv[1], "--list"))
    {
        for (const auto& s : scenarios)
            printf("%s\n", s.first.c_str());
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "perf"))
        return perf(atoi(argv[2]), atoi(argv[3])) ? 1 : 0;
    if (argc < 2 || !scenarios.count(argv[1]))
    {
        printf("usage: mods_mount_test <scenario> | --list | perf <files> <mb>\n");
        return 2;
    }
    printf("mods_mount_test %s\n", argv[1]);
    const int failures = scenarios.at(argv[1])();
    printf("%s (%d failed)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
