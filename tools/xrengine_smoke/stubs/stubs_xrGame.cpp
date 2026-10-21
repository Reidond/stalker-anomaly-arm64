// Stand-in for xrGame: the game "DLL" entry points (EngineAPI.cpp), and the few game globals the engine's console
// commands and settings loader name. The factory creates nothing, so the smoke test cannot go past the renderer load.
#include "../../../src/xrEngine/stdafx.h"

BOOL DllMainXrGame(HANDLE, u32, LPVOID) { return TRUE; }

extern "C" DLL_Pure* __cdecl xrFactory_Create(CLASS_ID) { return nullptr; }
extern "C" void __cdecl xrFactory_Destroy(DLL_Pure*) {}

void updateCurrentScope() {}

int g_ai_unlimited_ammo = 0;
u32 g_crosshair_color = 0xffffffff;
float g_dispersion_base = 1.f;
float g_dispersion_factor = 1.f;
float g_fTimeFactor = 1.f;
