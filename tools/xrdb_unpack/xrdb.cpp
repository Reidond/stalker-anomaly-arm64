#include "xrdb.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xrdb
{
namespace
{
constexpr uint32_t kCompressMark = 1u << 31;
constexpr uint32_t kHeaderChunk = 666;
constexpr uint32_t kFileTableChunk = 1;

uint32_t rd32(const uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
} // namespace

Archive::~Archive() { close(); }

void Archive::close()
{
    if (map_)
        munmap(const_cast<uint8_t*>(map_), size_);
    if (fd_ >= 0)
        ::close(fd_);
    map_ = nullptr;
    fd_ = -1;
    size_ = 0;
}

bool Archive::find_chunk(uint32_t id, std::vector<uint8_t>& out, bool* found)
{
    *found = false;
    last_chunk_compressed_ = false;
    uint64_t pos = 0;
    while (pos + 8 <= size_)
    {
        const uint32_t type = rd32(map_ + pos);
        const uint32_t sz = rd32(map_ + pos + 4);
        pos += 8;
        if (pos + sz > size_)
        {
            error_ = "truncated chunk";
            return false;
        }
        if ((type & ~kCompressMark) == id)
        {
            *found = true;
            last_chunk_compressed_ = (type & kCompressMark) != 0;
            if (type & kCompressMark)
            {
                if (!lzhuf_decompress(map_ + pos, sz, out))
                {
                    error_ = "chunk LZ-Huffman decode failed";
                    return false;
                }
            }
            else
                out.assign(map_ + pos, map_ + pos + sz);
            return true;
        }
        pos += sz;
    }
    return true;
}

bool Archive::open(const std::string& path)
{
    close();
    entries_.clear();
    header_.clear();
    error_.clear();

    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0)
    {
        error_ = "cannot open " + path;
        return false;
    }
    struct stat st;
    if (fstat(fd_, &st) != 0 || st.st_size <= 0)
    {
        error_ = "cannot stat " + path;
        return false;
    }
    size_ = (uint64_t)st.st_size;
    void* m = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (m == MAP_FAILED)
    {
        error_ = "mmap failed";
        return false;
    }
    map_ = (const uint8_t*)m;

    std::vector<uint8_t> chunk;
    bool found = false;
    if (!find_chunk(kHeaderChunk, chunk, &found))
        return false;
    if (found)
        header_.assign(chunk.begin(), chunk.end());

    if (!find_chunk(kFileTableChunk, chunk, &found))
        return false;
    table_compressed_ = last_chunk_compressed_;
    if (!found)
    {
        error_ = "no file table chunk";
        return false;
    }

    size_t pos = 0;
    while (pos + 2 <= chunk.size())
    {
        const uint32_t len = chunk[pos] | (chunk[pos + 1] << 8);
        pos += 2;
        if (len < 16 || pos + len > chunk.size())
        {
            error_ = "corrupt file table";
            return false;
        }
        const uint8_t* b = chunk.data() + pos;
        Entry e;
        e.size_real = rd32(b);
        e.size_compressed = rd32(b + 4);
        e.crc = rd32(b + 8);
        e.name.assign((const char*)b + 12, len - 16);
        e.offset = rd32(b + len - 4);
        for (char& c : e.name)
            if (c == '\\')
                c = '/';
        entries_.push_back(std::move(e));
        pos += len;
    }
    return true;
}

bool Archive::read(const Entry& e, std::vector<uint8_t>& out)
{
    if ((uint64_t)e.offset + e.size_compressed > size_)
    {
        error_ = "entry out of range: " + e.name;
        return false;
    }
    const uint8_t* src = map_ + e.offset;
    out.resize(e.size_real);
    if (!e.compressed())
    {
        memcpy(out.data(), src, e.size_real);
        return true;
    }
    if (!lzo1x_decompress(src, e.size_compressed, out.data(), e.size_real))
    {
        error_ = "LZO decode failed: " + e.name;
        return false;
    }
    return true;
}
} // namespace xrdb
