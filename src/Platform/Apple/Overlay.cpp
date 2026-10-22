// The Monolith gamedata overlay: the files the engine build needs next to the Anomaly game data (scripts, configs and
// shaders of src/../gamedata in the repository). On Windows they are copied into the game's gamedata folder; the game
// data here is read-only, so the app bundle carries them and the shell packs them into an X-Ray archive that the engine
// mounts like any db0 file (entry_point $fs_root$\gamedata\). Files the player puts into gamedata still win over it.
#include "ShellInternal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace AppleShell::Internal
{
namespace
{
constexpr const char* kArchiveName = "monolith_overlay.db0";
constexpr const char* kSignatureName = "monolith_overlay.sig";

struct Source
{
    std::string relative; // forward slashes
    fs::path path;
    uint64_t size = 0;
};

uint64_t Fnv1a(uint64_t h, const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint32_t Crc32(const std::vector<uint8_t>& data)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data)
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void Put32(std::string& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(char((v >> (8 * i)) & 0xFF));
}

std::vector<Source> Collect(const fs::path& root)
{
    std::vector<Source> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end;
         it.increment(ec))
    {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        const std::string name = it->path().filename().string();
        if (name == ".DS_Store" || name[0] == '.')
            continue;
        Source s;
        s.relative = fs::relative(it->path(), root, ec).generic_string();
        s.path = it->path();
        s.size = it->file_size(ec);
        files.push_back(std::move(s));
    }
    std::sort(files.begin(), files.end(), [](const Source& a, const Source& b) { return a.relative < b.relative; });
    return files;
}

std::string Signature(const std::vector<Source>& files)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const Source& s : files)
    {
        std::error_code ec;
        const auto mtime = fs::last_write_time(s.path, ec).time_since_epoch().count();
        h = Fnv1a(h, s.relative.data(), s.relative.size());
        h = Fnv1a(h, &s.size, sizeof(s.size));
        h = Fnv1a(h, &mtime, sizeof(mtime));
    }
    char text[64];
    snprintf(text, sizeof(text), "v2 %zu %016llx", files.size(), (unsigned long long)h);
    return text;
}

std::string ReadText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// [chunk 0: file data][chunk 666: header ini][chunk 1: file table], nothing compressed (size_real == size_compressed).
bool WriteArchive(const fs::path& out_path, const std::vector<Source>& files)
{
    std::string data;
    std::string table;
    std::vector<uint8_t> bytes;
    constexpr uint32_t kDataStart = 8; // chunk 0 payload starts after its 8 byte header
    for (const Source& s : files)
    {
        std::ifstream in(s.path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (!in && !bytes.empty())
            return false;
        const uint32_t offset = kDataStart + uint32_t(data.size());
        data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());

        std::string name = s.relative;
        std::replace(name.begin(), name.end(), '/', '\\');
        std::string entry;
        Put32(entry, uint32_t(bytes.size()));
        Put32(entry, uint32_t(bytes.size()));
        Put32(entry, Crc32(bytes));
        entry += name;
        Put32(entry, offset);
        table.push_back(char(entry.size() & 0xFF));
        table.push_back(char((entry.size() >> 8) & 0xFF));
        table += entry;
    }
    // level_name and level_ver: CApplication::GetArchiveHeader reads them from the header of every archive
    const std::string header = "[header]\nauto_load = true\ncreator = \"anomaly app shell\"\nentry_point = $fs_root$\\gamedata\\\n"
                               "level_name = monolith_overlay\nlevel_ver = 1.0\n";

    std::string file;
    auto chunk = [&file](uint32_t id, const std::string& payload)
    {
        Put32(file, id);
        Put32(file, uint32_t(payload.size()));
        file += payload;
    };
    chunk(0, data);
    chunk(666, header);
    chunk(1, table);

    const fs::path tmp = out_path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(file.data(), std::streamsize(file.size()));
        if (!out)
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, out_path, ec);
    return !ec;
}
} // namespace

bool BuildOverlayArchive(const std::string& source_dir, const std::string& archive_dir)
{
    std::error_code ec;
    if (source_dir.empty() || !fs::is_directory(source_dir, ec))
        return false;
    fs::create_directories(archive_dir, ec);

    const std::vector<Source> files = Collect(source_dir);
    if (files.empty())
        return false;
    const fs::path archive = fs::path(archive_dir) / kArchiveName;
    const fs::path signature = fs::path(archive_dir) / kSignatureName;
    const std::string wanted = Signature(files);
    if (fs::exists(archive, ec) && ReadText(signature) == wanted)
        return true;

    if (!WriteArchive(archive, files))
    {
        Log("overlay: cannot write %s", archive.c_str());
        return false;
    }
    std::ofstream(signature, std::ios::binary | std::ios::trunc) << wanted;
    Log("overlay: packed %zu files from %s into %s", files.size(), source_dir.c_str(), archive.c_str());
    return true;
}
} // namespace AppleShell::Internal
