// What Emscripten's mmap emulation does under NODERAWFS for the three mapping shapes the engine uses:
//   1. MAP_PRIVATE read window of a large file at an offset (archive entries: LocatorAPI.cpp:1522, stream_reader.cpp:50)
//   2. MAP_PRIVATE read of a whole loose file (CVirtualFileReader, FS.cpp:599)
//   3. MAP_SHARED + PROT_WRITE of a whole loose file, written through the mapping (CVirtualFileRW, FS.cpp:577: save games)
// usage: mmap_probe <scratch dir> <window MB>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <emscripten/heap.h>

static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv)
{
    const std::string dir = argv[1];
    const size_t window = (size_t)atoi(argv[2]) << 20;
    const long page = sysconf(_SC_PAGESIZE);
    printf("page size %ld, heap now %zu MB\n", page, emscripten_get_heap_size() >> 20);

    // a file of 2 windows, filled with a pattern that depends on the offset
    const std::string big = dir + "/big.bin";
    {
        FILE* f = fopen(big.c_str(), "wb");
        std::string chunk(1 << 20, 0);
        for (size_t mb = 0; mb < 2 * (window >> 20); ++mb)
        {
            for (size_t i = 0; i < chunk.size(); i += 4096) memcpy(&chunk[i], &mb, sizeof(mb));
            fwrite(chunk.data(), 1, chunk.size(), f);
        }
        fclose(f);
    }
    int fd = open(big.c_str(), O_RDONLY);
    const off_t start = (off_t)window; // second half, page aligned
    double t0 = now_ms();
    void* v = mmap(nullptr, window, PROT_READ, MAP_PRIVATE, fd, start);
    double t1 = now_ms();
    printf("1. MAP_PRIVATE %zu MB window at offset %lld: %s, %.1f ms, heap now %zu MB\n", window >> 20, (long long)start,
           v == MAP_FAILED ? "FAILED" : "mapped", t1 - t0, emscripten_get_heap_size() >> 20);
    if (v != MAP_FAILED)
    {
        const uint8_t* p = (const uint8_t*)v;
        size_t mb_first = 0, mb_last = 0;
        memcpy(&mb_first, p, sizeof(mb_first));
        memcpy(&mb_last, p + window - 4096, sizeof(mb_last));
        printf("   first MB tag %zu (want %zu), last MB tag %zu (want %zu)\n", mb_first, window >> 20, mb_last, (2 * (window >> 20)) - 1);
        t0 = now_ms();
        munmap(v, window);
        printf("   munmap %.1f ms, heap now %zu MB\n", now_ms() - t0, emscripten_get_heap_size() >> 20);
    }
    close(fd);

    // 2. whole loose file, private
    const std::string small = dir + "/small.bin";
    {
        FILE* f = fopen(small.c_str(), "wb");
        std::string s(300 * 1024 + 17, 'x');
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    }
    fd = open(small.c_str(), O_RDONLY);
    struct stat st; fstat(fd, &st);
    void* r = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    printf("2. MAP_PRIVATE whole file (%lld bytes): %s, content %s\n", (long long)st.st_size, r == MAP_FAILED ? "FAILED" : "mapped",
           r != MAP_FAILED && ((const char*)r)[st.st_size - 1] == 'x' ? "ok" : "BAD");
    if (r != MAP_FAILED) munmap(r, (size_t)st.st_size);
    close(fd);

    // 3. shared read-write mapping, write through it, unmap, read the file back with plain read()
    fd = open(small.c_str(), O_RDWR);
    void* w = mmap(nullptr, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    printf("3. MAP_SHARED PROT_WRITE: %s\n", w == MAP_FAILED ? "FAILED" : "mapped");
    if (w != MAP_FAILED)
    {
        memcpy(w, "SAVEGAME", 8);
        ((char*)w)[st.st_size - 1] = 'Z';
        munmap(w, (size_t)st.st_size);
    }
    close(fd);
    char head[9] = {}, tail = 0;
    FILE* f = fopen(small.c_str(), "rb");
    fread(head, 1, 8, f); fseek(f, -1, SEEK_END); fread(&tail, 1, 1, f); fclose(f);
    printf("   after munmap the file reads back: \"%s\" ... '%c' (%s)\n", head, tail, !strcmp(head, "SAVEGAME") && tail == 'Z' ? "written back" : "NOT written back");
    unlink(big.c_str());
    unlink(small.c_str());
    return 0;
}
