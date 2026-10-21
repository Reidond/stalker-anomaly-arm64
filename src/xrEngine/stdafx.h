#ifndef STDAFX_3DA
#define STDAFX_3DA
#pragma once

#ifdef _EDITOR
#include "..\editors\ECore\stdafx.h"
#else

#include "../Common/Platform.h"

// The in-game editor is a Windows DLL (editor.dll); not built on Apple.
#if !defined(NDEBUG) && !defined(XR_PLATFORM_APPLE)
# ifndef INGAME_EDITOR
# define INGAME_EDITOR
# endif // #ifndef INGAME_EDITOR
#endif // #if !defined(NDEBUG) && !defined(XR_PLATFORM_APPLE)

#ifdef INGAME_EDITOR
# define _WIN32_WINNT 0x0550
#endif // #ifdef INGAME_EDITOR

#include "../xrCore/xrCore.h"
#include "../Include/xrAPI/xrAPI.h"

// Discord Rich Presence (Discord Game SDK + ICU) is a Windows feature. The Apple build leaves it out unless
// cmake/targets/xrEngine.cmake is given -DANOMALY_XRENGINE_DISCORD=ON; the game still links against the stubs in x_ray.cpp.
#if !defined(XR_PLATFORM_APPLE) || defined(ANOMALY_XRENGINE_DISCORD)
# define XR_USE_DISCORD 1
#endif

// The debug-build mailslot console (mailSlot.cpp, msParse in EventAPI.cpp) is Windows IPC.
#if defined(DEBUG) && !defined(XR_PLATFORM_APPLE)
# define XR_USE_MAILSLOT 1
#endif

#ifdef _DEBUG
# define D3D_DEBUG_INFO
#endif

#pragma warning(push)
#pragma warning(disable:4995)
#include <d3d9.h>
//#include <dplay8.h>
#pragma warning(pop)

// you must define ENGINE_BUILD then building the engine itself
// and not define it if you are about to build DLL
#ifndef NO_ENGINE_API
#ifdef ENGINE_BUILD
#define DLL_API
//__declspec(dllimport)
#define ENGINE_API
//__declspec(dllexport)
#else
#undef DLL_API
#define DLL_API
//__declspec(dllexport)
#define ENGINE_API
//__declspec(dllimport)
#endif
#else
#define ENGINE_API
#define DLL_API
#endif // NO_ENGINE_API

#define ECORE_API

// Our headers
#include "engine.h"
#include "defines.h"
#ifndef NO_XRLOG
#include "../xrcore/log.h"
#endif
#include "device.h"
#include "../xrcore/fs.h"

#include "../xrcdb/xrXRC.h"

#include "../xrSound/sound.h"

extern ENGINE_API CInifile* pGameIni;

#pragma comment( lib, "xrCore.lib" )
#pragma comment( lib, "xrCDB.lib" )
#pragma comment( lib, "xrSound.lib" )

//AVO: lua re-org
#ifdef USE_LUAJIT_ONE //defined in project props
#pragma comment(lib, "LuaJIT-1.1.8.lib")
#else
#pragma comment(lib, "lua51.lib" )
#endif
//#include "lua/library_linkage.h"
//-AVO

#pragma comment( lib, "xrAPI.lib" )

#pragma comment( lib, "winmm.lib" )

#pragma comment( lib, "d3d9.lib" )
#pragma comment( lib, "dinput8.lib" )
#pragma comment( lib, "dxguid.lib" )

#ifndef DEBUG
# define LUABIND_NO_ERROR_CHECKING
#endif

#if !defined(DEBUG) || defined(FORCE_NO_EXCEPTIONS)
# define LUABIND_NO_EXCEPTIONS
# define BOOST_NO_EXCEPTIONS
#endif

#define LUABIND_DONT_COPY_STRINGS

#define READ_IF_EXISTS(ltx,method,section,name,default_value)\
 (((ltx)->line_exist(section, name)) ? ((ltx)->method(section, name)) : (default_value))

#endif // !M_BORLAND
#endif // !defined STDAFX_3DA
