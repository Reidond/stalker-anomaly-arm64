// Weak defaults for the few symbols xrCore expects its host program to provide. On Windows xrCore is a DLL whose host
// (xrEngine / xrServerEntities) defines them; here they let xrCore link on its own (tools, tests). The real
// definitions in xrEngine/xrServerEntities are strong and take precedence.
#include "../stdafx.h"
#pragma hdrstop

#ifdef XR_PLATFORM_APPLE

__attribute__((weak)) XRCORE_API u32 build_id = 0;
__attribute__((weak)) XRCORE_API LPCSTR build_date = __DATE__;

// logs the Lua call stack on asserts; the script layer provides the real one
__attribute__((weak)) void printLuaStack() {}

__attribute__((weak)) int get_modded_exes_version() { return 0; }
__attribute__((weak)) LPCSTR get_modded_exes_name() { return "xrCore"; }
__attribute__((weak)) xr_string get_modded_exes_version_string() { return xr_string("0.0.0"); }

#endif // XR_PLATFORM_APPLE
