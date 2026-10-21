// Stand-in for xrCPU_Pipe: the processor-specific geometry pipeline (skinning) and its thread pool, bound by Engine.cpp.
#include "../../../src/xrEngine/stdafx.h"

extern "C" void __cdecl xrBind_PSGP(xrDispatchTable* table, _processor_info*)
{
	memset(table, 0, sizeof(*table)); // no skinning: the renderer is not there either
}

extern "C" void __cdecl ttapi_Done() {}
