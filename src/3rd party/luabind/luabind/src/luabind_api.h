#pragma once

#ifdef LUABIND_BUILDING
#	define LUABIND_API 		/*__declspec(dllexport)*/
#else 
#	define LUABIND_API		/*__declspec(dllimport)*/
#endif // #ifdef XR_SCRIPTS

// Calling convention of the allocator hook; MSVC-only keyword.
#ifndef LUABIND_CDECL
#	ifdef _MSC_VER
#		define LUABIND_CDECL __cdecl
#	else
#		define LUABIND_CDECL
#	endif
#endif
