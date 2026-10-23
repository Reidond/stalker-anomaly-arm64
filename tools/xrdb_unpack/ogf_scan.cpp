// ogf_scan: finds skinned vertices with non-finite floats (P, N, T, B, weights, texture coordinates) in the .ogf files of .db archives.
//
//   ogf_scan <archive.db> [more archives]
//
// One line per model chunk and attribute: `<archive>: <model>/childN <kind>: <n> of <count> vertices non-finite (first <i>)`; the totals
// go to stderr. The renderer quantises these vertices into bytes (FSkinned.cpp, `q_N`): a NaN or an infinity there is undefined behaviour
// of the float to int conversion (docs/ios-port/SANITIZERS.md, open item 4).
#include "xrdb.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
struct Hit
{
    size_t vertices = 0, total = 0;
    int first = -1;
};
size_t g_chunks = 0, g_vertices = 0;

uint32_t rd32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void note(std::map<std::string, Hit>& out, const std::string& key, uint32_t index, uint32_t count)
{
    Hit& h = out[key];
    ++h.vertices;
    if (h.first < 0)
        h.first = int(index);
    h.total = count;
}

// OGF_VERTICES: u32 type, u32 count, then the vertices. Types 1 to 4 (and the FVF_1L..4L values) are the skinned layouts of vertBoned1W..4W
// (xrEngine/bone.h, packed to 2 bytes): [bone indices] P N T B [weights] u v [matrix].
void scan_vertices(const std::string& model, const uint8_t* d, size_t n, std::map<std::string, Hit>& out, const std::string& child)
{
    if (n < 8)
        return;
    const uint32_t type = rd32(d), count = rd32(d + 4), fvf = 0x12071980;
    size_t stride = 0, pos_off = 0;
    if (type == 1 || type == fvf) { stride = 60; pos_off = 0; }
    else if (type == 2 || type == 2 * fvf) { stride = 64; pos_off = 4; }
    else if (type == 3 || type == 4 * fvf) { stride = 70; pos_off = 6; }
    else if (type == 4 || type == 5 * fvf) { stride = 76; pos_off = 8; }
    else
        return;
    if (8 + stride * count > n)
        return;
    ++g_chunks;
    g_vertices += count;
    const int floats = int((stride - pos_off - (stride == 60 ? 4 : 0)) / 4); // P N T B, the weights and u v
    static const char* groups[] = {"P", "N", "T", "B", "weights/uv"};
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t* v = d + 8 + stride * i;
        bool bad[5] = {};
        for (int k = 0; k < floats; ++k)
        {
            float f;
            memcpy(&f, v + pos_off + 4 * k, 4);
            if (!std::isfinite(f))
                bad[k < 12 ? k / 3 : 4] = true;
        }
        for (int g = 0; g < 5; ++g)
            if (bad[g]) // each vertex counts once per attribute
                note(out, model + child + " " + groups[g], i, count);
    }
}

void walk(const std::string& model, const uint8_t* d, size_t n, std::map<std::string, Hit>& out, int depth, const std::string& child)
{
    size_t pos = 0;
    while (pos + 8 <= n)
    {
        const uint32_t id = rd32(d + pos), size = rd32(d + pos + 4);
        if (pos + 8 + size > n)
            return;
        const uint8_t* c = d + pos + 8;
        const uint32_t cid = id & 0x7fffffff;
        if (!(id & 0x80000000))
        {
            if (depth == 0 && cid == 9) // OGF_CHILDREN
                walk(model, c, size, out, 1, child);
            else if (depth == 1) // one OGF per child
                walk(model, c, size, out, 2, child + "/child" + std::to_string(cid));
            else if (cid == 3) // OGF_VERTICES
                scan_vertices(model, c, size, out, child);
        }
        pos += 8 + size;
    }
}
} // namespace

int main(int argc, char** argv)
{
    size_t files = 0;
    for (int i = 1; i < argc; ++i)
    {
        xrdb::Archive ar;
        if (!ar.open(argv[i]))
        {
            fprintf(stderr, "%s: %s\n", argv[i], ar.error().c_str());
            continue;
        }
        std::map<std::string, Hit> out;
        for (const auto& e : ar.entries())
        {
            if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".ogf") != 0)
                continue;
            std::vector<uint8_t> buf;
            if (!ar.read(e, buf))
                continue;
            ++files;
            walk(e.name, buf.data(), buf.size(), out, 0, "");
        }
        for (const auto& kv : out)
            printf("%s: %s: %zu of %zu vertices non-finite (first %d)\n", argv[i], kv.first.c_str(), kv.second.vertices, kv.second.total,
                   kv.second.first);
    }
    fprintf(stderr, "scanned %zu .ogf files, %zu skinned vertex chunks, %zu vertices\n", files, g_chunks, g_vertices);
    return 0;
}
