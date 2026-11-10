#include "storage_lab.h"

#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/threading.h>
#include <pthread.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "lab_report.h"

// JS side: lab_lib.js.
extern "C" {
void lab_io_start(lab::storage::IoShared* io);
int lab_opfs_bench_open();
double lab_opfs_write(const void* ptr, int len, double offset);
double lab_opfs_read(void* ptr, int len, double offset);
void lab_opfs_flush();
void lab_opfs_bench_close();
}

namespace lab::storage
{
// lab_lib.js addresses these fields by byte offset.
static_assert(offsetof(IoShared, ready) == 28 && offsetof(IoShared, shared_view_ok) == 40);
static_assert(offsetof(IoShared, crc) == 48 && offsetof(IoShared, fetch_ms) == 304);
static_assert(offsetof(IoShared, read_ms) == 320 && offsetof(IoShared, error) == 336);

namespace
{
alignas(8) IoShared g_io;

uint32_t Crc32(const uint8_t* p, size_t n)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

int32_t Load(const int32_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void Store(int32_t* p, int32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

// Reader side: post a request and block until the I/O thread finishes it.
void Request(int32_t op, int32_t chunk, void* dest)
{
    g_io.op = op;
    g_io.chunk = chunk;
    g_io.dest = int32_t(reinterpret_cast<intptr_t>(dest));
    const int32_t seq = Load(&g_io.req_seq) + 1;
    Store(&g_io.req_seq, seq);
    emscripten_futex_wake(&g_io.req_seq, 1 << 30);
    for (;;)
    {
        const int32_t done = Load(&g_io.done_seq);
        if (done == seq)
            break;
        emscripten_futex_wait(&g_io.done_seq, uint32_t(done), INFINITY);
    }
}

void* ReaderMain(void*)
{
    // Wait for the I/O thread to open OPFS and read the chunk manifest.
    while (Load(&g_io.ready) == 0)
        emscripten_futex_wait(&g_io.ready, 0, INFINITY);
    if (g_io.ready < 0)
    {
        Report("storage.blocking", "fail", std::string("I/O thread failed to start: ") + g_io.error);
        return nullptr;
    }
    const int count = g_io.chunk_count;
    const size_t size = size_t(g_io.chunk_size);
    uint8_t* buf = static_cast<uint8_t*>(std::aligned_alloc(16, size));
    std::vector<double> cold_ms, warm_ms, fetch_ms, store_ms, read_ms;
    int bad = 0, net = 0, opfs = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < count; ++i)
        {
            std::memset(buf, 0, size);
            const double t0 = NowMs();
            Request(kOpChunk, i, buf); // this thread is blocked until the chunk is in buf
            const double t1 = NowMs();
            const bool ok = g_io.result == int32_t(size) && Crc32(buf, size) == g_io.crc[i];
            bad += ok ? 0 : 1;
            (g_io.source == 1 ? net : opfs) += 1;
            if (pass == 0)
            {
                cold_ms.push_back(t1 - t0);
                fetch_ms.push_back(g_io.fetch_ms);
                store_ms.push_back(g_io.store_ms);
            }
            else
            {
                warm_ms.push_back(t1 - t0);
                read_ms.push_back(g_io.read_ms);
            }
        }
    const Stats c = Summarize(cold_ms), w = Summarize(warm_ms), f = Summarize(fetch_ms), s = Summarize(store_ms), r = Summarize(read_ms);
    const bool pass_ok = bad == 0 && net == count && opfs == count;
    Report("storage.blocking", pass_ok ? "pass" : "fail",
           Fmt("reader thread blocked while the I/O thread fetched and stored %d x %zu MiB chunks: %.1f ms per chunk median "
               "(fetch %.1f, OPFS write %.1f); second pass from OPFS %.1f ms (read %.1f); %d checksum or size errors",
               count, size >> 20, c.median, f.median, s.median, w.median, r.median, bad),
           Json()
               .Int("chunks", count)
               .Int("chunk_bytes", int64_t(size))
               .Int("from_network", net)
               .Int("from_opfs", opfs)
               .Int("errors", bad)
               .Raw("cold_ms", StatsJson(c))
               .Raw("fetch_ms", StatsJson(f))
               .Raw("opfs_write_ms", StatsJson(s))
               .Raw("warm_ms", StatsJson(w))
               .Raw("opfs_read_ms", StatsJson(r))
               .Bool("sync_handle_takes_shared_views", g_io.shared_view_ok == 1)
               .Done());
    std::free(buf);

    Request(kOpBench, 0, nullptr); // the I/O thread runs the 1 GiB benchmark and reports it
    Request(kOpStop, 0, nullptr);
    return nullptr;
}

void* IoMain(void*)
{
    // The thread stays alive after this returns: JS keeps it busy from its event loop.
    emscripten_runtime_keepalive_push();
    lab_io_start(&g_io);
    return nullptr;
}
} // namespace

void Start()
{
    std::memset(&g_io, 0, sizeof(g_io));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20);
    pthread_t io, reader;
    pthread_create(&io, &attr, IoMain, nullptr);
    pthread_create(&reader, &attr, ReaderMain, nullptr);
    pthread_detach(io);
    pthread_detach(reader);
}
} // namespace lab::storage

// Called by lab_lib.js on the I/O thread for kOpBench: 1 GiB written and read back in 4 MiB chunks.
extern "C" EMSCRIPTEN_KEEPALIVE void lab_io_run_bench()
{
    using namespace lab;
    constexpr size_t kChunk = 4u << 20;
    constexpr int kCount = 256; // 1 GiB
    if (!lab_opfs_bench_open())
    {
        Report("storage.opfs_bench", "fail", "could not open a synchronous access handle for the benchmark file");
        return;
    }
    uint8_t* buf = static_cast<uint8_t*>(std::aligned_alloc(16, kChunk));
    for (size_t i = 0; i < kChunk; ++i)
        buf[i] = uint8_t(i * 131 + 7);
    double written = 0, read = 0;
    int bad = 0;
    const double t0 = NowMs();
    for (int i = 0; i < kCount; ++i)
    {
        std::memcpy(buf, &i, sizeof(i)); // each chunk differs
        written += lab_opfs_write(buf, int(kChunk), double(i) * kChunk);
    }
    lab_opfs_flush();
    const double t1 = NowMs();
    for (int i = 0; i < kCount; ++i)
    {
        read += lab_opfs_read(buf, int(kChunk), double(i) * kChunk);
        int tag;
        std::memcpy(&tag, buf, sizeof(tag));
        bad += tag == i ? 0 : 1;
    }
    const double t2 = NowMs();
    lab_opfs_bench_close();
    std::free(buf);
    const double mib = double(kChunk) * kCount / (1024.0 * 1024.0);
    const double wmb = mib / ((t1 - t0) / 1000.0), rmb = mib / ((t2 - t1) / 1000.0);
    const bool ok = written == double(kChunk) * kCount && read == double(kChunk) * kCount && bad == 0;
    Report("storage.opfs_bench", ok ? "pass" : "fail",
           Fmt("1 GiB in 4 MiB chunks from a wasm thread: write+flush %.0f MiB/s (%.0f ms), read %.0f MiB/s (%.0f ms); %d bad chunks",
               wmb, t1 - t0, rmb, t2 - t1, bad),
           Json()
               .Num("write_mib_s", wmb)
               .Num("read_mib_s", rmb)
               .Num("write_ms", t1 - t0)
               .Num("read_ms", t2 - t1)
               .Num("bytes_written", written)
               .Num("bytes_read", read)
               .Int("bad_chunks", bad)
               .Bool("on_main_browser_thread", emscripten_is_main_browser_thread())
               .Done());
}
