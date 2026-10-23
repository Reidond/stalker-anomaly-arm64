// xrMemory's allocation front end on Apple platforms.
//
// The Windows build runs with PURE_ALLOC: every allocation goes straight to the CRT (16-byte aligned) and is zero
// filled; the 64 size-class pools in xrMemory_POOL.cpp are never used. malloc() on Apple is already 16-byte
// aligned, so this is the same thing without _aligned_malloc. See xrMemory_subst_msvc.cpp for the original.
#include "../stdafx.h"
#pragma hdrstop

#include "../xrMemory_pure.h"

size_t xr_apple_malloc_size(const void* p);

MEMPOOL mem_pools[mem_pools_count];

#ifndef DEBUG_MEMORY_MANAGER
# define debug_mode 0
#endif

void* xrMemory::mem_alloc(size_t size
#ifdef DEBUG_MEMORY_NAME
                          , const char* _name
#endif
)
{
	__atomic_fetch_add(&stat_calls, 1u, __ATOMIC_RELAXED); // every thread allocates; the count is only for the stats window
	void* result = calloc(1, size ? size : 1);
#ifdef USE_MEMORY_MONITOR
	memory_monitor::monitor_alloc(result, size, _name);
#endif
	return result;
}

void xrMemory::mem_free(void* P)
{
	__atomic_fetch_add(&stat_calls, 1u, __ATOMIC_RELAXED); // every thread allocates; the count is only for the stats window
#ifdef USE_MEMORY_MONITOR
	memory_monitor::monitor_free(P);
#endif
	free(P);
}

void* xrMemory::mem_realloc(void* P, size_t size
#ifdef DEBUG_MEMORY_NAME
                            , const char* _name
#endif
)
{
	__atomic_fetch_add(&stat_calls, 1u, __ATOMIC_RELAXED); // every thread allocates; the count is only for the stats window
	if (!P)
		return mem_alloc(size
#ifdef DEBUG_MEMORY_NAME
		                 , _name
#endif
		);

	const size_t old_size = xr_apple_malloc_size(P);
	void* result = realloc(P, size ? size : 1);
	// MSVC build zero-fills the grown tail
	if (result && size > old_size)
		memset((u8*)result + old_size, 0, size - old_size);
#ifdef USE_MEMORY_MONITOR
	memory_monitor::monitor_free(P);
	memory_monitor::monitor_alloc(result, size, _name);
#endif
	return result;
}
