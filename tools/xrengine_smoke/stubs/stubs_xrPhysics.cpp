// Stand-in for xrPhysics: the ODE allocator hook the Windows build runs from the DLL's DllMain.
#include "../../../src/xrEngine/stdafx.h"

BOOL DllMainXrPhysics(HANDLE, DWORD, LPVOID) { return TRUE; }
