// Host test of the `delete <path>` script step (src/Platform/Apple/DeleteStep.h): what it removes and, more to the point, what it never touches.
#include <Platform/Apple/DeleteStep.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using AppleShell::DeleteBelowGameFolder;
using AppleShell::DeleteResult;

static int g_failures = 0;
static void check(bool ok, const std::string& what)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

static void touch(const fs::path& p)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}

int main()
{
    char templ[] = "/tmp/delete_step_test.XXXXXX";
    const fs::path base = mkdtemp(templ);
    const fs::path root = base / "game";
    const fs::path outside = base / "outside";
    std::string why;

    // a game folder: stock archives, stock folders, addon files in several spellings
    for (const char* f : {"db/files.db0", "db/shaders.db0", "db/levels/level_x.db0", "db/textures/textures.db0", "db/meshes/meshes.db0",
                          "db/sounds/s.db0", "db/configs/scripts.db0", "db/mods/m.db0", "db/addons/pack.db0", "DB/Patches/p.DB0",
                          "gamedata/Scripts/x.script", "GameData/Scripts/y.script", "gamedata/configs/axr_options.ltx", "gamedata/keep/file.txt"})
        touch(root / f);
    touch(outside / "keep.txt");
    fs::create_symlink(outside, root / "gamedata" / "linked");
    fs::create_symlink(root / "db" / "files.db0", root / "gamedata" / "linkfile");
    fs::create_symlink(outside, root / "db" / "addons" / "linked_dir");
    auto exists = [&](const char* p) { return fs::exists(fs::symlink_status(root / p)); };
    auto run = [&](const char* p) { return DeleteBelowGameFolder(root.string(), p, why); };

    printf("removed:\n");
    check(run("gamedata/Scripts/x.script") == DeleteResult::Removed && !exists("gamedata/Scripts/x.script"), "a file below gamedata");
    check(run("gamedata/Scripts") == DeleteResult::Kept && exists("gamedata/Scripts"), "a folder that still holds a file (a spelling next to it) is kept");
    check(run("GameData/Scripts/y.script") == DeleteResult::Removed, "a file below GameData (any case)");
    check(run("gamedata/keep") == DeleteResult::Kept && exists("gamedata/keep/file.txt"), "a folder with something in it is kept (never recursive)");
    check(run("db/addons/pack.db0") == DeleteResult::Removed && !exists("db/addons/pack.db0"), "an archive in db/addons");
    check(run("DB/Patches/p.DB0") == DeleteResult::Removed, "an archive in DB/Patches (any case)");
    check(run("db/addons") == DeleteResult::Kept, "db/addons itself is only removed when empty (a link inside keeps it here)");
    check(run("DB/Patches") == DeleteResult::Removed && !exists("DB/Patches"), "an empty db/patches folder");
    check(run("gamedata/nothing.txt") == DeleteResult::NotThere, "a file that is not there");

    printf("refused, and the file stays:\n");
    for (const char* p : {"db/files.db0", "db/shaders.db0", "db/levels/level_x.db0", "db/textures/textures.db0", "db/meshes/meshes.db0", "db/sounds/s.db0",
                          "db/configs/scripts.db0", "db/mods/m.db0", "db/levels", "db/textures", "db/configs", "db/mods", "DB/FILES.DB0"})
    {
        const DeleteResult r = run(p);
        check(r == DeleteResult::Refused && (exists(p) || std::string(p) == "DB/FILES.DB0"), std::string("delete ") + p + " refused");
    }
    check(exists("db/files.db0") && exists("db/shaders.db0") && exists("db/levels/level_x.db0") && exists("db/textures/textures.db0"), "every stock archive is still there");
    for (const char* p : {"", "/etc/hosts", "../outside/keep.txt", "gamedata/../db/files.db0", "gamedata/./Scripts", "bin/x.exe", "mods/x", "fsgame.ltx", "default-44100.mhr"})
        check(run(p) == DeleteResult::Refused, std::string("delete '") + p + "' refused");
    check(fs::exists(outside / "keep.txt"), "the file outside the game folder is still there");

    printf("links:\n");
    check(run("gamedata/linked/keep.txt") == DeleteResult::Refused && fs::exists(outside / "keep.txt"), "a link in the middle of the path (a folder pointing outside) is refused");
    check(run("gamedata/linked") == DeleteResult::Refused && fs::exists(outside), "a link as the target is refused");
    check(run("gamedata/linkfile") == DeleteResult::Refused && exists("db/files.db0"), "a link to a stock archive is refused and the archive stays");
    check(run("db/addons/linked_dir/keep.txt") == DeleteResult::Refused && fs::exists(outside / "keep.txt"), "a link inside db/addons is refused too");

    std::error_code ec;
    fs::remove_all(base, ec);
    printf("\n%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
