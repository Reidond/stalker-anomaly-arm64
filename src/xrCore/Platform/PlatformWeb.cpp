// WebAssembly (Emscripten) replacements for the few Apple-only calls in PlatformApple.cpp: the allocator queries behind
// xrMemory (malloc_size, malloc_zone_statistics, task_info). Spike S1b, docs/web-port/WASM-BUILD.md. The names keep their
// xr_apple_ prefix because xrMemory_align.cpp and memory_usage.cpp call them by it; W1 renames them.
#include "../stdafx.h"
#pragma hdrstop

#ifdef XR_PLATFORM_WEB

#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#include <malloc.h>

// CPU::GetCLK(): the "cycle counter" of the web build is the monotonic clock in nanoseconds (_math.h). CPU::clk_per_second is
// calibrated against QueryPerformanceCounter in CPU::Detect(), which here is the same clock, so it comes out at 1e9.
namespace CPU
{
XRCORE_API u64 GetCLK(void)
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
} // namespace CPU

// The size of the block malloc handed out (not the size that was asked for), as malloc_size() reports it: xrMemory::mem_realloc
// zero-fills the grown tail from it. Emscripten's dlmalloc, emmalloc and mimalloc all provide malloc_usable_size().
size_t xr_apple_malloc_size(const void* p) { return p ? malloc_usable_size(const_cast<void*>(p)) : 0; }

// Bytes handed out by malloc and not yet freed. dlmalloc and emmalloc have mallinfo(); mimalloc (-sMALLOC=mimalloc) does not,
// and the link would fail on a strong reference, so it is weak: the heap's current size stands in when it is absent.
extern "C" struct mallinfo mallinfo(void) __attribute__((weak));

size_t xr_apple_heap_bytes_in_use()
{
	if (mallinfo)
		return (size_t)(unsigned)mallinfo().uordblks;
	return emscripten_get_heap_size();
}

// free: heap that can still be had before the ceiling; committed: the wasm memory as it stands now; reserved: the ceiling.
void xr_apple_vm_info(size_t* free_bytes, size_t* reserved, size_t* committed)
{
	const size_t now = emscripten_get_heap_size();
	const size_t max = emscripten_get_heap_max();
	*committed = now;
	*reserved = max;
	*free_bytes = max > now ? max - now : 0;
}

#endif // XR_PLATFORM_WEB
