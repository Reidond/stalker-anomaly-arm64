// xrdb_unpack: list and extract X-Ray .db archives.
//
//   xrdb_unpack list    <archive.db> [pattern]
//   xrdb_unpack extract <archive.db> -o <outdir> [pattern]
//   xrdb_unpack info    <archive.db>
//
// `pattern` is a substring filter on the entry path. Refuses to write below any directory named
// "game" so the read-only game data cannot be overwritten by accident.
#include "xrdb.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void usage()
{
    fprintf(stderr,
            "usage:\n"
            "  xrdb_unpack list    <archive> [pattern]\n"
            "  xrdb_unpack info    <archive>\n"
            "  xrdb_unpack extract <archive> -o <outdir> [pattern]\n");
}

static bool match(const std::string& name, const char* pattern) { return !pattern || name.find(pattern) != std::string::npos; }

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    const std::string archive_path = argv[2];
    const char* pattern = nullptr;
    std::string outdir;
    for (int i = 3; i < argc; ++i)
    {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            outdir = argv[++i];
        else
            pattern = argv[i];
    }

    xrdb::Archive ar;
    if (!ar.open(archive_path))
    {
        fprintf(stderr, "error: %s\n", ar.error().c_str());
        return 1;
    }

    if (cmd == "info")
    {
        uint64_t real = 0, comp = 0;
        size_t ncomp = 0;
        for (const auto& e : ar.entries())
        {
            real += e.size_real;
            comp += e.size_compressed;
            ncomp += e.compressed();
        }
        printf("%s\n%zu entries (%zu compressed), %llu bytes real, %llu bytes stored, file table %s\n%s\n",
               archive_path.c_str(), ar.entries().size(), ncomp, (unsigned long long)real, (unsigned long long)comp,
               ar.table_compressed() ? "LZ-Huffman" : "raw", ar.header_text().c_str());
        return 0;
    }
    if (cmd == "list")
    {
        for (const auto& e : ar.entries())
            if (match(e.name, pattern))
                printf("%10u %10u %c %s\n", e.size_real, e.size_compressed, e.compressed() ? 'z' : ' ', e.name.c_str());
        return 0;
    }
    if (cmd == "extract")
    {
        if (outdir.empty())
        {
            usage();
            return 2;
        }
        const fs::path out = fs::weakly_canonical(outdir);
        for (const auto& part : out)
            if (part == "game")
            {
                fprintf(stderr, "error: refusing to extract into a 'game' directory (%s)\n", out.c_str());
                return 2;
            }
        size_t n = 0, failed = 0;
        std::vector<uint8_t> data;
        for (const auto& e : ar.entries())
        {
            if (!match(e.name, pattern))
                continue;
            // Entry names come from the archive; keep them inside outdir.
            fs::path rel = fs::path(e.name).lexically_normal();
            if (rel.is_absolute() || rel.empty() || *rel.begin() == "..")
            {
                fprintf(stderr, "skip unsafe path: %s\n", e.name.c_str());
                ++failed;
                continue;
            }
            if (!e.name.empty() && e.name.back() == '/')
            {
                fs::create_directories(out / rel);
                continue;
            }
            if (!ar.read(e, data))
            {
                fprintf(stderr, "FAIL %s\n", ar.error().c_str());
                ++failed;
                continue;
            }
            const fs::path dest = out / rel;
            fs::create_directories(dest.parent_path());
            std::ofstream f(dest, std::ios::binary | std::ios::trunc);
            f.write((const char*)data.data(), (std::streamsize)data.size());
            if (!f)
            {
                fprintf(stderr, "FAIL write %s\n", dest.c_str());
                ++failed;
                continue;
            }
            ++n;
        }
        printf("extracted %zu files, %zu failed\n", n, failed);
        return failed ? 1 : 0;
    }
    usage();
    return 2;
}
