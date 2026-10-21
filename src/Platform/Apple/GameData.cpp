// Game data discovery and validation: checks a data root against the Anomaly 1.5.3 archive manifest.

#include "Shell.h"

#include <sys/stat.h>

#include <cstdio>
#include <TargetConditionals.h>

namespace AppleShell
{
namespace
{
struct ManifestEntry
{
    const char* path;
    uint64_t size;
    bool required;
};

const ManifestEntry kManifest[] = {
#include "GameDataManifest.inc"
};

bool StatFile(const std::string& path, uint64_t& size)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    size = uint64_t(st.st_size);
    return true;
}

bool IsDirectory(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string FormatGB(uint64_t bytes)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buffer;
}

// Looks one folder deeper for a db/ folder: dragging the whole "Anomaly" folder into the app in
// Files/Finder gives Documents/Anomaly/Anomaly/db.
std::string FindNestedRoot(const std::string& root)
{
    std::string found;
    for (const char* name : {"Anomaly", "anomaly", "STALKER Anomaly", "S.T.A.L.K.E.R. Anomaly"})
    {
        std::string candidate = root + "/" + name;
        if (IsDirectory(candidate + "/db"))
        {
            found = candidate;
            break;
        }
    }
    return found;
}

// Lines are kept under ~80 columns; the launcher prints them as they are.
std::string BuildHelp(const GameDataStatus& status)
{
    std::string help;
    if (!status.nested_root.empty())
    {
        help += "The game is one folder too deep, at:\n  " + status.nested_root + "\n";
        help += "Move the contents of that folder (db, gamedata, ...) up into:\n  " + status.root + "\n";
        return help;
    }
    if (status.state == DataState::Incomplete)
    {
        help += "The copy looks interrupted or is a different version. Copy the game again;\n";
        help += "files that are already complete are skipped by tools/ios/push_gamedata.sh.\n\n";
    }
#if TARGET_OS_IOS
    help += "Copy Anomaly 1.5.3 (about " + FormatGB(status.bytes_expected) + " in db/) into this app:\n";
    help += "  Finder: connect the iPad, select it, open the Files tab, then drag the contents\n";
    help += "          of your Anomaly folder (db, gamedata, ...) onto \"Anomaly\".\n";
    help += "  Files:  On My iPad > Anomaly > Anomaly, paste the contents of your Anomaly folder.\n";
    help += "  Script: tools/ios/push_gamedata.sh from the repo (uses devicectl over USB).\n";
    help += "Skip bin/ and tools/ (Windows only). Needs " + FormatGB(status.bytes_expected) + " of free space.\n";
#else
    help += "Point the app at your Anomaly 1.5.3 folder (the one that contains db/):\n";
    help += "  --data-root /path/to/Anomaly   or   ANOMALY_DATA_ROOT=/path/to/Anomaly\n";
    help += "  or: defaults write com.andriishafar.anomaly AnomalyDataRoot /path/to/Anomaly\n";
    help += "Default: ~/Documents/Anomaly\n";
#endif
    return help;
}

void Scan(const std::string& root, GameDataStatus& status)
{
    for (const ManifestEntry& entry : kManifest)
    {
        if (!entry.required)
            continue;
        status.files_expected++;
        status.bytes_expected += entry.size;

        uint64_t size = 0;
        const bool exists = StatFile(root + "/" + entry.path, size);
        if (exists && size == entry.size)
        {
            status.files_ok++;
            status.bytes_present += size;
            continue;
        }
        if (status.issues.size() < 64)
        {
            DataIssue issue;
            issue.path = entry.path;
            issue.expected = entry.size;
            issue.actual = exists ? size : 0;
            issue.missing = !exists;
            status.issues.push_back(std::move(issue));
        }
    }
}
} // namespace

GameDataStatus ValidateGameData(const std::string& root)
{
    GameDataStatus status;
    status.root = root;

    status.has_gamedata_dir = IsDirectory(root + "/gamedata");
    uint64_t ignored = 0;
    status.has_original_fsgame = StatFile(root + "/fsgame.ltx", ignored);

    if (!IsDirectory(root + "/db"))
    {
        status.state = DataState::NotFound;
        status.nested_root = FindNestedRoot(root);
        // Count what is expected so the launcher can show the size to copy.
        for (const ManifestEntry& entry : kManifest)
        {
            if (entry.required)
            {
                status.files_expected++;
                status.bytes_expected += entry.size;
            }
        }
        status.summary = IsDirectory(root) ? "Not found: the data folder has no db/ folder" : "Not found: the data folder does not exist";
        status.help = BuildHelp(status);
        return status;
    }

    Scan(root, status);

    if (status.files_ok == status.files_expected)
    {
        status.state = DataState::Ok;
        status.summary = "Ready: all " + std::to_string(status.files_expected) + " archives present (" + FormatGB(status.bytes_present) + ")";
        return status;
    }

    status.state = DataState::Incomplete;
    // The issue list is capped, so derive the count from the totals.
    const uint32_t problems = status.files_expected - status.files_ok;
    status.summary = "Incomplete: " + std::to_string(status.files_ok) + " of " + std::to_string(status.files_expected) +
        " archives OK (" + FormatGB(status.bytes_present) + " of " + FormatGB(status.bytes_expected) + "), " +
        std::to_string(problems) + " missing or wrong size";
    status.help = BuildHelp(status);
    return status;
}

} // namespace AppleShell
