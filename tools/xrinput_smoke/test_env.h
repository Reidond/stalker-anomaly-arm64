// What the xrEngine sources under test expect from stdafx.h, without the engine: xrCore, the registrator types and a
// stub Device/Engine with just the members the input code reads. Defining STDAFX_3DA is what turns the engine's own
// "stdafx.h" (found next to each source file) into an empty header.
#pragma once

#define STDAFX_3DA
#define XR_INPUT_STANDALONE // xr_input_sdl.cpp peeks the SDL queue itself instead of registering with xr_window
#define ENGINE_API
#define DLL_API

#include "xrCore/xrCore.h"
#include "xrEngine/pure.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

// ---- Device / Engine stand-ins ------------------------------------------------------------------------------------
struct StubStatTimer
{
	void Begin() {}
	void End() {}
};

struct StubStats
{
	StubStatTimer Input;
};

struct CRenderDevice
{
	HWND m_hWnd = nullptr;
	u32 clientWidth = 1280;
	u32 clientHeight = 960;
	u32 dwPrecacheFrame = 0;
	u32 dwFrame = 0;
	u32 dwTimeContinual = 0;
	float fTimeGlobal = 0.f;
	StubStats* Statistic = nullptr;

	u32 TimerAsync_MMT() { return u32(SDL_GetTicks()); }
};

struct StubEvent
{
	std::vector<std::string> deferred;
	void Defer(LPCSTR name, u64 = 0, u64 = 0) { deferred.push_back(name); }
};

struct CEngine
{
	StubEvent Event;
};

extern CRenderDevice Device;
extern CEngine Engine;
#define RDEVICE Device

// ---- tiny test harness --------------------------------------------------------------------------------------------
extern int g_failures;
void check(bool ok, const char* what);
