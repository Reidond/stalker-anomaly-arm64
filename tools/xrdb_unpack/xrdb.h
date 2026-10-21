// Reader for X-Ray engine .db / .xdb archives.
//
// Layout (see CLocatorAPI::LoadArchive in src/xrCore/LocatorAPI.cpp):
//   chunk 666  optional INI header ([header] entry_point, auto_load, ...)
//   chunk 1    file table, possibly LZ-Huffman compressed:
//              repeated { u16 size; u32 size_real; u32 size_compr; u32 crc; char name[size-16]; u32 offset; }
//   payloads   stored raw, or LZO1X-compressed when size_real != size_compr
// Chunks are { u32 id (bit 31 = LZ-Huffman compressed); u32 size; u8 data[size]; }.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xrdb
{
struct Entry
{
    std::string name;        // forward slashes, relative to the archive's entry point
    uint32_t size_real = 0;
    uint32_t size_compressed = 0;
    uint32_t crc = 0;
    uint32_t offset = 0;
    bool compressed() const { return size_real != size_compressed; }
};

class Archive
{
public:
    Archive() = default;
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    // Maps the archive and reads its file table. Returns false and fills error() on failure.
    bool open(const std::string& path);
    void close();

    const std::string& error() const { return error_; }
    const std::string& header_text() const { return header_; }
    const std::vector<Entry>& entries() const { return entries_; }
    bool table_compressed() const { return table_compressed_; }

    // Decompresses one entry. Returns false and fills error() on failure.
    bool read(const Entry& entry, std::vector<uint8_t>& out);

private:
    bool find_chunk(uint32_t id, std::vector<uint8_t>& out, bool* found);

    int fd_ = -1;
    const uint8_t* map_ = nullptr;
    uint64_t size_ = 0;
    std::string error_;
    std::string header_;
    bool table_compressed_ = false;
    bool last_chunk_compressed_ = false;
    std::vector<Entry> entries_;
};

// Codecs, exposed for tests.
// LZ-Huffman as used for chunk data: u32 decoded size, then the bit stream.
bool lzhuf_decompress(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out);
// LZO1X-1 raw block. dst_size must be the exact decoded size.
bool lzo1x_decompress(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size);
} // namespace xrdb
