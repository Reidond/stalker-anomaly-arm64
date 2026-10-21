// SDL3 keyboard vocabulary -> the engine's DirectInput key numbering (Apple platforms).
//
// Anomaly's Lua scripts (DIK_keys, MCM, bind_stalker), user.ltx and the engine's key tables all use the
// DirectInput scan code values (DIK_*, see Common/dinput_compat.h), so SDL scancodes are translated to them
// here and never the other way round (docs/ios-port/M1-AUDIT.md 2.7). Nothing in this file depends on the
// engine; xrinput_smoke tests it on its own.
#pragma once

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

namespace xr_input
{
// DIK_* code for an SDL scancode, or 0 for a key the engine has no number for.
int ScancodeToDik(SDL_Scancode scancode);
// Inverse of ScancodeToDik, SDL_SCANCODE_UNKNOWN when the DIK code is not in the table.
SDL_Scancode DikToScancode(int dik);

// A key's label for the Controls menu, in Windows-1251 (the engine's text encoding). Printable keys use the
// active layout ("A", ";", Cyrillic letters), the rest SDL's name ("Left Shift", "Escape"). False if the key
// has no name.
bool DikName(int dik, char* dest, int dest_size);

// What a key press types with the given modifiers, in Windows-1251. False when the key types nothing
// (modifier, function key, Ctrl/Alt combination) or the character has no Windows-1251 form.
bool DikToText(int dik, bool shift, bool caps, bool ctrl, bool alt, bool altgr, char* dest, int dest_size);

// UTF-8 to Windows-1251; characters outside the code page become '?'. Returns the length written, the
// terminator excluded. `dest_size` includes the terminator.
int Utf8ToCp1251(const char* utf8, char* dest, int dest_size);
// One code point to Windows-1251; false if it has no form there.
bool CodepointToCp1251(unsigned codepoint, char& out);
} // namespace xr_input
