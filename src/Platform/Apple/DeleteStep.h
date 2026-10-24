// The `delete <path>` step of the --script runner (src/Platform/App/main.cpp): removes what a test run put into the game folder, so that
// a run on a device can leave it as it found it (the device has no way to delete a file from outside, and devicectl must never be asked to).
// Header only and free of engine and shell dependencies, so tools/delete_step_test can check it on any host.
//
// What it removes: one file, or one empty folder (std::filesystem::remove: never recursive), below
//   gamedata/        (any case, any depth),
//   db/addons/ and db/patches/   (any case; the two folders themselves too, when empty),
//   and the folder db itself when it is empty (a case variant like DB/ that an unzip made).
// It refuses anything else, in particular every stock archive and folder of db/ (files.db0, shaders.db0, levels, textures, meshes, sounds, configs,
// mods), absolute paths, `..`, and any path with a symbolic link in it (the target's folders are checked one by one, not only the target).
#pragma once

#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace AppleShell
{
enum class DeleteResult
{
    Removed,
    NotThere,
    Refused,
    Kept, // exists, allowed, but remove() did not remove it (a folder with something in it)
};

inline std::string DeleteLower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 'relative' is a path below the game folder (`gamedata/Scripts/x.script`, `db/addons/pack.db0`); 'message' says why when it is refused or kept.
inline DeleteResult DeleteBelowGameFolder(const std::string& data_root, const std::string& relative, std::string& message)
{
    namespace fs = std::filesystem;
    message.clear();
    if (relative.empty() || relative.find('\0') != std::string::npos)
    {
        message = "an empty path";
        return DeleteResult::Refused;
    }
    const fs::path rel(relative);
    if (!rel.is_relative())
    {
        message = "not a relative path";
        return DeleteResult::Refused;
    }
    std::vector<std::string> parts;
    for (const fs::path& part : rel)
    {
        const std::string name = part.string();
        if (name.empty() || name == "." || name == "..")
        {
            message = "a path with '.', '..' or an empty part";
            return DeleteResult::Refused;
        }
        parts.push_back(name);
    }
    const std::string top = DeleteLower(parts[0]);
    const bool in_gamedata = top == "gamedata";
    const bool db_itself = top == "db" && parts.size() == 1;
    const bool in_addon_folder = top == "db" && parts.size() >= 2 && (DeleteLower(parts[1]) == "addons" || DeleteLower(parts[1]) == "patches");
    if (!in_gamedata && !db_itself && !in_addon_folder)
    {
        message = "only gamedata/, db/addons/ and db/patches/ (and an empty db) may be removed from";
        return DeleteResult::Refused;
    }

    // every folder on the way, and the target, must be a real file or folder: a link in the path could lead out of the game folder
    fs::path walk(data_root);
    std::error_code ec;
    for (const std::string& name : parts)
    {
        walk /= name;
        const fs::file_status status = fs::symlink_status(walk, ec);
        if (ec || !fs::exists(status))
            return DeleteResult::NotThere;
        if (fs::is_symlink(status))
        {
            message = "a link in the path";
            return DeleteResult::Refused;
        }
    }
    if (fs::remove(walk, ec))
        return DeleteResult::Removed;
    message = ec ? ec.message() : "not removed";
    return DeleteResult::Kept;
}
} // namespace AppleShell
