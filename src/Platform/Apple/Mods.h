// The mod list as the engine mounts it (docs/ios-port/MOD-MANAGER.md, "Mounting").
//
// <mods root>/modlist.json says which mods are enabled and in what order (lowest priority first); each mod is a folder
// <mods root>/<id>/ whose content/ is laid out like the game folder. This reads the list, checks the folders and produces the
// lines of the generated fsgame.ltx that make the engine mount them. It reads a few small files and never writes anything.
//
// No SDL and no engine dependency, so the host test (tools/mods_mount_test) uses it as it is.
#pragma once

#include <string>
#include <vector>

namespace AppleShell
{
struct ModMount
{
    std::string id;
    std::string content_dir;           // native path of <mods root>/<id>/content
    std::vector<std::string> archives; // *.db* files below content/db (names relative to it), for the log
    bool has_gamedata = false;         // content/ has a gamedata folder
    long long file_count = -1;         // mod.json's file_count, -1 when it has none (the engine counts what it really mounts)
};

struct ModResolution
{
    std::string mods_root;
    std::string modlist;                // <mods root>/modlist.json
    bool mods_off = false;              // --mods-off: nothing is read, nothing is mounted
    bool modlist_found = false;
    std::string error;                  // modlist.json is unreadable or not valid: no mod is mounted
    std::vector<ModMount> mounts;       // enabled and present, lowest priority first (the later one wins a file)
    std::vector<std::string> notes;     // why an entry was left out: "id: folder not found"
    unsigned listed = 0;                // entries in modlist.json
    unsigned disabled = 0;              //   of which disabled
    unsigned unlisted_folders = 0;      // folders in the mods root that modlist.json does not name (disabled, not mounted)
};

// Reads <mods_root>/modlist.json. A missing file is an empty list; an invalid one is an empty list with `error` set. A bad
// entry is skipped with a note and does not stop the others. `mods_off` skips everything.
ModResolution ResolveMods(const std::string& mods_root, bool mods_off);

// One line per mount for the generated fsgame.ltx, "$mod_<id>$ = true | false | <content dir>\n": CLocatorAPI::mount_mod
// takes the content folder (db/ archives like db\addons, gamedata/ files registered below $fs_root$\gamedata\).
std::string ModFsGameLines(const ModResolution& resolution);

// Removing mod folders, for the scripted test runs (`mods_delete`, `mods_clear` in main.cpp) and for the launcher. These take no path: they act
// on the mods root the shell resolved (`SetModsRoot`, called by AppleShell::Init with Paths::mods_root) and on nothing else. They refuse unless
//   - that folder's name is exactly "Mods", and
//   - it has a marker: a regular file modlist.json, or it is empty, or everything in it is a mod folder (a folder holding a mod.json).
// RemoveModFolder(id) also needs <root>/<id> to be a real folder (not a link) holding a mod.json. Entries are removed with remove_all, which
// never follows a symbolic link: a link is removed as a link.
struct ModsRemoval
{
    std::vector<std::string> removed; // full paths of the entries removed
    std::vector<std::string> refused; // "<what>: <why>"
};
void SetModsRoot(const std::string& mods_root);
// <mods root>/<id>: `id` must be a plain name (no separators, not "." / ".." / empty). Nothing happens when it does not exist.
ModsRemoval RemoveModFolder(const std::string& id);
// Every entry of the mods root (mod folders, modlist.json, .downloads, ...), leaving the mods root itself.
ModsRemoval ClearModsRoot();

// The load order and what each mod brings, for the shell log: a summary line, then one line per mount and per note.
std::vector<std::string> DescribeMods(const ModResolution& resolution);
} // namespace AppleShell
