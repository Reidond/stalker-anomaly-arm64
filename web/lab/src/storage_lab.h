// Item 3: OPFS from wasm threads.
//
// An I/O pthread owns the OPFS handles and the network: it never blocks, it waits for requests with
// Atomics.waitAsync, fetches missing chunks, stores them with a synchronous access handle and copies
// them into the requester's buffer. A reader pthread stands in for the engine thread: it blocks on a
// futex until each chunk it asked for is there. Afterwards the I/O thread writes and reads 1 GiB in
// 4 MiB chunks through a synchronous access handle, called from C++.
#pragma once

#include <cstdint>

namespace lab::storage
{
// Shared between the reader thread (C++), the I/O thread (C++ and JS) and nobody else.
struct IoShared
{
    int32_t req_seq;  // bumped by the reader for each request; the I/O thread waits on it
    int32_t done_seq; // set to req_seq by the I/O thread when the request is finished
    int32_t op;       // kOpChunk, kOpBench, kOpStop
    int32_t chunk;
    int32_t dest;     // wasm pointer the chunk is copied to
    int32_t result;   // bytes delivered, or < 0 on error
    int32_t source;   // 1 network (then stored to OPFS), 2 OPFS
    int32_t ready;    // 0 starting, 1 ready, -1 failed
    int32_t chunk_count;
    int32_t chunk_size;
    int32_t shared_view_ok; // 1 if the sync handle accepted views of the shared wasm memory directly
    int32_t pad;
    uint32_t crc[64];
    double fetch_ms, store_ms, read_ms, copy_ms;
    char error[256];
};

constexpr int32_t kOpChunk = 1;
constexpr int32_t kOpBench = 2;
constexpr int32_t kOpStop = 3;

void Start();
} // namespace lab::storage
