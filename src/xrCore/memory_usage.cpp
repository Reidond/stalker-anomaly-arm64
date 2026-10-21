#include "stdafx.h"
#ifndef XR_PLATFORM_APPLE
#include <malloc.h>
#endif
#include <errno.h>

#ifdef XR_PLATFORM_APPLE
// Mach / malloc-zone queries live in Platform/PlatformApple.cpp (their headers clash with xrCore's thread_t)
size_t xr_apple_heap_bytes_in_use();
void xr_apple_vm_info(size_t* free_bytes, size_t* reserved, size_t* committed);

XRCORE_API void vminfo(size_t* _free, size_t* reserved, size_t* committed)
{
	xr_apple_vm_info(_free, reserved, committed);
}

XRCORE_API void log_vminfo()
{
	size_t w_free, w_reserved, w_committed;
	vminfo(&w_free, &w_reserved, &w_committed);
	Msg("* [apple]: free[%lld K], reserved[%lld K], committed[%lld K]", (long long)(w_free / 1024),
	    (long long)(w_reserved / 1024), (long long)(w_committed / 1024));
}

size_t xrMemory::mem_usage() { return xr_apple_heap_bytes_in_use(); }
#else // XR_PLATFORM_APPLE

XRCORE_API void vminfo(size_t* _free, size_t* reserved, size_t* committed)
{
	MEMORY_BASIC_INFORMATION memory_info;
	memory_info.BaseAddress = 0;
	*_free = *reserved = *committed = 0;
	while (VirtualQuery(memory_info.BaseAddress, &memory_info, sizeof(memory_info)))
	{
		switch (memory_info.State)
		{
		case MEM_FREE:
			*_free += memory_info.RegionSize;
			break;
		case MEM_RESERVE:
			*reserved += memory_info.RegionSize;
			break;
		case MEM_COMMIT:
			*committed += memory_info.RegionSize;
			break;
		}
		memory_info.BaseAddress = (char*)memory_info.BaseAddress + memory_info.RegionSize;
	}
}

XRCORE_API void log_vminfo()
{
	size_t w_free, w_reserved, w_committed;
	vminfo(&w_free, &w_reserved, &w_committed);
	Msg(
		"* [win32]: free[%lld K], reserved[%lld K], committed[%lld K]",
		w_free / 1024,
		w_reserved / 1024,
		w_committed / 1024
	);
}

size_t xrMemory::mem_usage()
{
	_HEAPINFO hinfo = {};
	int status;
	size_t bytesUsed = 0;
	while ((status = _heapwalk(&hinfo)) == _HEAPOK)
	{
		if (hinfo._useflag == _USEDENTRY)
			bytesUsed += hinfo._size;
	}
	switch (status)
	{
	case _HEAPEMPTY:
		break;
	case _HEAPEND:
		break;
	case _HEAPBADPTR:
		FATAL("bad pointer to heap");
		break;
	case _HEAPBADBEGIN:
		FATAL("bad start of heap");
		break;
	case _HEAPBADNODE:
		FATAL("bad node in heap");
		break;
	}
	return bytesUsed;
}
#endif // XR_PLATFORM_APPLE
