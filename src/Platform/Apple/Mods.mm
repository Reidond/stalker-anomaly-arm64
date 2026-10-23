// The mod list as the engine mounts it. See Mods.h.
#include "Mods.h"

#import <Foundation/Foundation.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>

namespace AppleShell
{
namespace
{
bool IsDirectory(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(tolower((unsigned char)c));
    return s;
}

// The id names a folder and an fsgame.ltx alias: letters, digits, '.', '_' and '-', not starting with '.' ("." and ".." would
// leave the mods root; ".downloads" is the downloads folder).
bool ValidId(const std::string& id)
{
    if (id.empty() || id.size() > 128 || id[0] == '.')
        return false;
    for (unsigned char c : id)
        if (!isalnum(c) && c != '.' && c != '_' && c != '-')
            return false;
    return true;
}

// A line of fsgame.ltx is "id = a | b | c": these cannot be part of a path in it.
bool PathFitsFsGame(const std::string& path)
{
    return path.find_first_of("|=\n\r") == std::string::npos;
}

// *.db* and *.xdb* files below `dir` (names relative to it, '/' separators), depth first in name order, like the engine scans them.
void CollectArchives(const std::string& dir, const std::string& prefix, std::vector<std::string>& out)
{
    DIR* d = opendir(dir.c_str());
    if (!d)
        return;
    std::vector<std::pair<std::string, bool>> entries;
    while (dirent* e = readdir(d))
    {
        if (e->d_name[0] == '.')
            continue;
        const std::string full = dir + "/" + e->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0)
            continue;
        entries.emplace_back(e->d_name, S_ISDIR(st.st_mode));
    }
    closedir(d);
    std::sort(entries.begin(), entries.end());
    for (const auto& [name, is_dir] : entries)
    {
        if (is_dir)
        {
            CollectArchives(dir + "/" + name, prefix + name + "/", out);
            continue;
        }
        const std::string lower = Lower(name);
        const size_t dot = lower.rfind('.');
        if (dot != std::string::npos && (lower.compare(dot, 3, ".db") == 0 || lower.compare(dot, 4, ".xdb") == 0))
            out.push_back(prefix + name);
    }
}

// The top-level entries of content/ whose lower-cased names are "db" and "gamedata" (archives made on Windows keep their capitals).
void InspectContent(const std::string& content, ModMount& mount)
{
    DIR* d = opendir(content.c_str());
    if (!d)
        return;
    while (dirent* e = readdir(d))
    {
        if (e->d_name[0] == '.')
            continue;
        const std::string name = Lower(e->d_name);
        const std::string full = content + "/" + e->d_name;
        if (!IsDirectory(full))
            continue;
        if (name == "db")
            CollectArchives(full, "", mount.archives);
        else if (name == "gamedata")
            mount.has_gamedata = true;
    }
    closedir(d);
}

// mod.json's file_count, for the log only (the engine counts what it mounts); -1 when it is not there.
long long ReadFileCount(const std::string& mod_json)
{
    NSData* data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:mod_json.c_str()]];
    if (!data)
        return -1;
    id root = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![root isKindOfClass:NSDictionary.class])
        return -1;
    id count = ((NSDictionary*)root)[@"file_count"];
    return [count isKindOfClass:NSNumber.class] ? [(NSNumber*)count longLongValue] : -1;
}
} // namespace

ModResolution ResolveMods(const std::string& mods_root, bool mods_off)
{
    @autoreleasepool
    {
        ModResolution r;
        r.mods_root = mods_root;
        r.modlist = mods_root + "/modlist.json";
        r.mods_off = mods_off;
        if (mods_off || mods_root.empty())
            return r;
        if (!PathFitsFsGame(mods_root))
        {
            r.error = "the mods folder path contains a character that fsgame.ltx cannot hold (| = or a line break), no mod is mounted";
            return r;
        }

        NSData* data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:r.modlist.c_str()]];
        if (!data)
        {
            struct stat st;
            r.modlist_found = stat(r.modlist.c_str(), &st) == 0;
            if (r.modlist_found)
                r.error = "modlist.json cannot be read, no mod is mounted";
            return r;
        }
        r.modlist_found = true;

        NSError* error = nil;
        id root = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
        if (!root)
        {
            r.error = std::string("modlist.json is not valid JSON (") + (error.localizedDescription.UTF8String ?: "?") + "), no mod is mounted";
            return r;
        }
        if (![root isKindOfClass:NSDictionary.class])
        {
            r.error = "modlist.json is not a JSON object, no mod is mounted";
            return r;
        }
        NSDictionary* dict = root;
        if (id schema = dict[@"schema"]; schema)
        {
            if (![schema isKindOfClass:NSNumber.class] || [(NSNumber*)schema integerValue] != 1)
            {
                r.error = "modlist.json has a schema this build does not know, no mod is mounted";
                return r;
            }
        }
        NSArray* mods = dict[@"mods"];
        if (![mods isKindOfClass:NSArray.class])
        {
            r.error = "modlist.json has no \"mods\" array, no mod is mounted";
            return r;
        }

        std::set<std::string> seen;
        for (id entry in mods)
        {
            ++r.listed;
            if (![entry isKindOfClass:NSDictionary.class])
            {
                r.notes.push_back("entry " + std::to_string(r.listed) + ": not an object, skipped");
                continue;
            }
            id name = ((NSDictionary*)entry)[@"id"];
            id enabled = ((NSDictionary*)entry)[@"enabled"];
            const std::string id_text = [name isKindOfClass:NSString.class] ? std::string([(NSString*)name UTF8String]) : std::string();
            if (!ValidId(id_text))
            {
                r.notes.push_back("entry " + std::to_string(r.listed) + ": \"" + id_text + "\" is not a usable id, skipped");
                continue;
            }
            if (!seen.insert(Lower(id_text)).second)
            {
                r.notes.push_back(id_text + ": listed twice, the first entry counts");
                continue;
            }
            if (![enabled isKindOfClass:NSNumber.class] || ![(NSNumber*)enabled boolValue])
            {
                ++r.disabled;
                continue;
            }

            const std::string folder = mods_root + "/" + id_text;
            const std::string content = folder + "/content";
            if (!IsDirectory(folder))
            {
                r.notes.push_back(id_text + ": enabled, but the mod folder is gone, skipped");
                continue;
            }
            if (!IsDirectory(content))
            {
                r.notes.push_back(id_text + ": enabled, but the folder has no content/, skipped");
                continue;
            }
            ModMount mount;
            mount.id = id_text;
            mount.content_dir = content;
            InspectContent(content, mount);
            mount.file_count = ReadFileCount(folder + "/mod.json");
            r.mounts.push_back(std::move(mount));
        }

        // folders the list does not name are disabled mods (a folder dropped in by hand, or a list that lost its entry)
        if (DIR* d = opendir(mods_root.c_str()))
        {
            while (dirent* e = readdir(d))
            {
                if (e->d_name[0] == '.' || !IsDirectory(mods_root + "/" + e->d_name))
                    continue;
                if (!seen.count(Lower(e->d_name)))
                    ++r.unlisted_folders;
            }
            closedir(d);
        }
        return r;
    }
}

std::string ModFsGameLines(const ModResolution& resolution)
{
    std::string out;
    for (const ModMount& m : resolution.mounts)
        out += "$mod_" + m.id + "$ = true | false | " + m.content_dir + "\n";
    return out;
}

std::vector<std::string> DescribeMods(const ModResolution& r)
{
    std::vector<std::string> lines;
    if (r.mods_off)
    {
        lines.push_back("mods: off (--mods-off), nothing from " + r.mods_root + " is mounted");
        return lines;
    }
    if (!r.error.empty())
    {
        lines.push_back("mods: " + r.error + " (" + r.modlist + ")");
        return lines;
    }
    std::string order;
    for (const ModMount& m : r.mounts)
        order += (order.empty() ? "" : ", ") + m.id;
    char head[256];
    snprintf(head, sizeof(head), "mods: %zu mounted of %u listed (%u disabled), %u folders not in the list; %s in %s",
             r.mounts.size(), r.listed, r.disabled, r.unlisted_folders, r.modlist_found ? "modlist.json read" : "no modlist.json",
             r.mods_root.c_str());
    lines.push_back(head);
    if (!r.mounts.empty())
        lines.push_back("mods: load order, lowest priority first: " + order + " (the game folder's own gamedata/ wins over all of them)");
    for (size_t i = 0; i < r.mounts.size(); ++i)
    {
        const ModMount& m = r.mounts[i];
        std::string archives;
        for (const std::string& a : m.archives)
            archives += (archives.empty() ? "" : " ") + a;
        const std::string files = m.file_count >= 0 ? std::to_string(m.file_count) + " files (mod.json)" : "file count unknown";
        lines.push_back("mod " + std::to_string(i + 1) + "/" + std::to_string(r.mounts.size()) + " " + m.id + ": " + files + ", gamedata " +
                        (m.has_gamedata ? "yes" : "no") + ", " + std::to_string(m.archives.size()) + " archives" +
                        (archives.empty() ? "" : ": " + archives));
    }
    for (const std::string& n : r.notes)
        lines.push_back("mods: " + n);
    return lines;
}
} // namespace AppleShell

namespace fs = std::filesystem;

namespace
{
// The canonical mods root, or an empty path with the reason in `why`.
fs::path CheckedRoot(const std::string& mods_root, std::string& why)
{
    std::error_code ec;
    if (mods_root.empty())
    {
        why = "no mods root";
        return {};
    }
    const fs::path root = fs::weakly_canonical(fs::path(mods_root), ec);
    if (ec || !fs::is_directory(root, ec))
    {
        why = "the mods root is not a folder";
        return {};
    }
    size_t parts = 0;
    for (const fs::path& part : root)
        if (!part.empty() && part != "/")
            ++parts;
    if (parts < 3)
    {
        why = "the mods root is too close to the top of the file system";
        return {};
    }
    return root;
}

// `entry` is directly inside `root`: its parent resolves to the root itself. The entry's own last component is not resolved, so a
// symbolic link is judged (and removed) as a link.
bool DirectlyInside(const fs::path& root, const fs::path& entry)
{
    std::error_code ec;
    const fs::path parent = fs::weakly_canonical(entry.parent_path(), ec);
    return !ec && parent == root && entry.filename() != "." && entry.filename() != ".." && !entry.filename().empty();
}

void RemoveEntry(const fs::path& root, const fs::path& entry, AppleShell::ModsRemoval& out)
{
    std::error_code ec;
    if (!DirectlyInside(root, entry))
    {
        out.refused.push_back(entry.string() + ": not directly inside the mods root");
        return;
    }
    if (!fs::exists(fs::symlink_status(entry, ec)))
        return;
    // remove_all does not follow symbolic links: a link inside a mod is removed as a link
    fs::remove_all(entry, ec);
    if (ec)
        out.refused.push_back(entry.string() + ": " + ec.message());
    else
        out.removed.push_back(entry.string());
}
} // namespace

namespace AppleShell
{
ModsRemoval RemoveModFolder(const std::string& mods_root, const std::string& id)
{
    ModsRemoval out;
    std::string why;
    const fs::path root = CheckedRoot(mods_root, why);
    if (root.empty())
    {
        out.refused.push_back(id + ": " + why);
        return out;
    }
    if (id.empty() || id == "." || id == ".." || id.find_first_of("/\\") != std::string::npos)
    {
        out.refused.push_back("\"" + id + "\": not a plain folder name");
        return out;
    }
    RemoveEntry(root, root / id, out);
    return out;
}

ModsRemoval ClearModsRoot(const std::string& mods_root)
{
    ModsRemoval out;
    std::string why;
    const fs::path root = CheckedRoot(mods_root, why);
    if (root.empty())
    {
        out.refused.push_back(mods_root + ": " + why);
        return out;
    }
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        entries.push_back(it->path());
    for (const fs::path& entry : entries)
        RemoveEntry(root, entry, out);
    return out;
}
} // namespace AppleShell
