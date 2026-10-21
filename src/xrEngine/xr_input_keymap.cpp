#include "xr_input_keymap.h"

#include <SDL3/SDL_keyboard.h>

#include <cstddef>
#include <cstring>

#include "../Common/dinput_compat.h"

namespace xr_input
{
namespace
{
struct ScanDik
{
    SDL_Scancode scancode;
    int dik;
};

// One row per key the engine can name. SDL scancodes are physical positions (USB HID usage), like the DIK
// codes, so no layout is involved.
const ScanDik kTable[] = {
    {SDL_SCANCODE_ESCAPE, DIK_ESCAPE},
    {SDL_SCANCODE_1, DIK_1},
    {SDL_SCANCODE_2, DIK_2},
    {SDL_SCANCODE_3, DIK_3},
    {SDL_SCANCODE_4, DIK_4},
    {SDL_SCANCODE_5, DIK_5},
    {SDL_SCANCODE_6, DIK_6},
    {SDL_SCANCODE_7, DIK_7},
    {SDL_SCANCODE_8, DIK_8},
    {SDL_SCANCODE_9, DIK_9},
    {SDL_SCANCODE_0, DIK_0},
    {SDL_SCANCODE_MINUS, DIK_MINUS},
    {SDL_SCANCODE_EQUALS, DIK_EQUALS},
    {SDL_SCANCODE_BACKSPACE, DIK_BACK},
    {SDL_SCANCODE_TAB, DIK_TAB},
    {SDL_SCANCODE_Q, DIK_Q},
    {SDL_SCANCODE_W, DIK_W},
    {SDL_SCANCODE_E, DIK_E},
    {SDL_SCANCODE_R, DIK_R},
    {SDL_SCANCODE_T, DIK_T},
    {SDL_SCANCODE_Y, DIK_Y},
    {SDL_SCANCODE_U, DIK_U},
    {SDL_SCANCODE_I, DIK_I},
    {SDL_SCANCODE_O, DIK_O},
    {SDL_SCANCODE_P, DIK_P},
    {SDL_SCANCODE_LEFTBRACKET, DIK_LBRACKET},
    {SDL_SCANCODE_RIGHTBRACKET, DIK_RBRACKET},
    {SDL_SCANCODE_RETURN, DIK_RETURN},
    {SDL_SCANCODE_LCTRL, DIK_LCONTROL},
    {SDL_SCANCODE_A, DIK_A},
    {SDL_SCANCODE_S, DIK_S},
    {SDL_SCANCODE_D, DIK_D},
    {SDL_SCANCODE_F, DIK_F},
    {SDL_SCANCODE_G, DIK_G},
    {SDL_SCANCODE_H, DIK_H},
    {SDL_SCANCODE_J, DIK_J},
    {SDL_SCANCODE_K, DIK_K},
    {SDL_SCANCODE_L, DIK_L},
    {SDL_SCANCODE_SEMICOLON, DIK_SEMICOLON},
    {SDL_SCANCODE_APOSTROPHE, DIK_APOSTROPHE},
    {SDL_SCANCODE_GRAVE, DIK_GRAVE},
    {SDL_SCANCODE_LSHIFT, DIK_LSHIFT},
    {SDL_SCANCODE_BACKSLASH, DIK_BACKSLASH},
    {SDL_SCANCODE_NONUSHASH, DIK_BACKSLASH}, // ISO keyboards: the key next to Enter is the same position
    {SDL_SCANCODE_Z, DIK_Z},
    {SDL_SCANCODE_X, DIK_X},
    {SDL_SCANCODE_C, DIK_C},
    {SDL_SCANCODE_V, DIK_V},
    {SDL_SCANCODE_B, DIK_B},
    {SDL_SCANCODE_N, DIK_N},
    {SDL_SCANCODE_M, DIK_M},
    {SDL_SCANCODE_COMMA, DIK_COMMA},
    {SDL_SCANCODE_PERIOD, DIK_PERIOD},
    {SDL_SCANCODE_SLASH, DIK_SLASH},
    {SDL_SCANCODE_RSHIFT, DIK_RSHIFT},
    {SDL_SCANCODE_KP_MULTIPLY, DIK_MULTIPLY},
    {SDL_SCANCODE_LALT, DIK_LMENU},
    {SDL_SCANCODE_SPACE, DIK_SPACE},
    {SDL_SCANCODE_CAPSLOCK, DIK_CAPITAL},
    {SDL_SCANCODE_F1, DIK_F1},
    {SDL_SCANCODE_F2, DIK_F2},
    {SDL_SCANCODE_F3, DIK_F3},
    {SDL_SCANCODE_F4, DIK_F4},
    {SDL_SCANCODE_F5, DIK_F5},
    {SDL_SCANCODE_F6, DIK_F6},
    {SDL_SCANCODE_F7, DIK_F7},
    {SDL_SCANCODE_F8, DIK_F8},
    {SDL_SCANCODE_F9, DIK_F9},
    {SDL_SCANCODE_F10, DIK_F10},
    {SDL_SCANCODE_NUMLOCKCLEAR, DIK_NUMLOCK},
    {SDL_SCANCODE_SCROLLLOCK, DIK_SCROLL},
    {SDL_SCANCODE_KP_7, DIK_NUMPAD7},
    {SDL_SCANCODE_KP_8, DIK_NUMPAD8},
    {SDL_SCANCODE_KP_9, DIK_NUMPAD9},
    {SDL_SCANCODE_KP_MINUS, DIK_SUBTRACT},
    {SDL_SCANCODE_KP_4, DIK_NUMPAD4},
    {SDL_SCANCODE_KP_5, DIK_NUMPAD5},
    {SDL_SCANCODE_KP_6, DIK_NUMPAD6},
    {SDL_SCANCODE_KP_PLUS, DIK_ADD},
    {SDL_SCANCODE_KP_1, DIK_NUMPAD1},
    {SDL_SCANCODE_KP_2, DIK_NUMPAD2},
    {SDL_SCANCODE_KP_3, DIK_NUMPAD3},
    {SDL_SCANCODE_KP_0, DIK_NUMPAD0},
    {SDL_SCANCODE_KP_PERIOD, DIK_DECIMAL},
    {SDL_SCANCODE_NONUSBACKSLASH, DIK_OEM_102},
    {SDL_SCANCODE_F11, DIK_F11},
    {SDL_SCANCODE_F12, DIK_F12},
    {SDL_SCANCODE_F13, DIK_F13},
    {SDL_SCANCODE_F14, DIK_F14},
    {SDL_SCANCODE_F15, DIK_F15},
    {SDL_SCANCODE_INTERNATIONAL2, DIK_KANA},
    {SDL_SCANCODE_INTERNATIONAL1, DIK_ABNT_C1},
    {SDL_SCANCODE_INTERNATIONAL4, DIK_CONVERT},
    {SDL_SCANCODE_INTERNATIONAL5, DIK_NOCONVERT},
    {SDL_SCANCODE_INTERNATIONAL3, DIK_YEN},
    {SDL_SCANCODE_KP_EQUALS, DIK_NUMPADEQUALS},
    {SDL_SCANCODE_MEDIA_PREVIOUS_TRACK, DIK_PREVTRACK},
    {SDL_SCANCODE_MEDIA_NEXT_TRACK, DIK_NEXTTRACK},
    {SDL_SCANCODE_KP_ENTER, DIK_NUMPADENTER},
    {SDL_SCANCODE_RCTRL, DIK_RCONTROL},
    {SDL_SCANCODE_MUTE, DIK_MUTE},
    {SDL_SCANCODE_MEDIA_PLAY_PAUSE, DIK_PLAYPAUSE},
    {SDL_SCANCODE_MEDIA_STOP, DIK_MEDIASTOP},
    {SDL_SCANCODE_VOLUMEDOWN, DIK_VOLUMEDOWN},
    {SDL_SCANCODE_VOLUMEUP, DIK_VOLUMEUP},
    {SDL_SCANCODE_AC_HOME, DIK_WEBHOME},
    {SDL_SCANCODE_KP_COMMA, DIK_NUMPADCOMMA},
    {SDL_SCANCODE_KP_DIVIDE, DIK_DIVIDE},
    {SDL_SCANCODE_PRINTSCREEN, DIK_SYSRQ},
    {SDL_SCANCODE_RALT, DIK_RMENU},
    {SDL_SCANCODE_PAUSE, DIK_PAUSE},
    {SDL_SCANCODE_HOME, DIK_HOME},
    {SDL_SCANCODE_UP, DIK_UP},
    {SDL_SCANCODE_PAGEUP, DIK_PRIOR},
    {SDL_SCANCODE_LEFT, DIK_LEFT},
    {SDL_SCANCODE_RIGHT, DIK_RIGHT},
    {SDL_SCANCODE_END, DIK_END},
    {SDL_SCANCODE_DOWN, DIK_DOWN},
    {SDL_SCANCODE_PAGEDOWN, DIK_NEXT},
    {SDL_SCANCODE_INSERT, DIK_INSERT},
    {SDL_SCANCODE_DELETE, DIK_DELETE},
    {SDL_SCANCODE_LGUI, DIK_LWIN},
    {SDL_SCANCODE_RGUI, DIK_RWIN},
    {SDL_SCANCODE_APPLICATION, DIK_APPS},
    {SDL_SCANCODE_POWER, DIK_POWER},
    {SDL_SCANCODE_SLEEP, DIK_SLEEP},
    {SDL_SCANCODE_WAKE, DIK_WAKE},
    {SDL_SCANCODE_AC_SEARCH, DIK_WEBSEARCH},
    {SDL_SCANCODE_AC_BOOKMARKS, DIK_WEBFAVORITES},
    {SDL_SCANCODE_AC_REFRESH, DIK_WEBREFRESH},
    {SDL_SCANCODE_AC_STOP, DIK_WEBSTOP},
    {SDL_SCANCODE_AC_FORWARD, DIK_WEBFORWARD},
    {SDL_SCANCODE_AC_BACK, DIK_WEBBACK},
    {SDL_SCANCODE_MEDIA_SELECT, DIK_MEDIASELECT},
};

struct Tables
{
    unsigned char to_dik[SDL_SCANCODE_COUNT] = {};
    // Several scancodes may share a DIK (NONUSHASH and BACKSLASH); the first row wins for the inverse.
    SDL_Scancode to_scancode[256] = {};

    Tables()
    {
        for (const ScanDik& row : kTable)
        {
            to_dik[row.scancode] = static_cast<unsigned char>(row.dik);
            if (to_scancode[row.dik] == SDL_SCANCODE_UNKNOWN)
                to_scancode[row.dik] = row.scancode;
        }
    }
};

const Tables& GetTables()
{
    static const Tables tables;
    return tables;
}

// Decodes one code point at `s`, advancing it. Malformed bytes decode as U+FFFD.
unsigned DecodeUtf8(const char*& s)
{
    const unsigned char c = static_cast<unsigned char>(*s);
    int extra = 0;
    unsigned cp = 0;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { ++s; return 0xFFFD; }
    ++s;
    for (int i = 0; i < extra; ++i)
    {
        if ((static_cast<unsigned char>(*s) & 0xC0) != 0x80)
            return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(*s) & 0x3F);
        ++s;
    }
    return cp;
}

bool CopyString(const char* src, char* dest, int dest_size)
{
    if (!dest || dest_size <= 0)
        return false;
    dest[0] = 0;
    if (!src || !*src)
        return false;
    const int n = static_cast<int>(strlen(src));
    const int copy = n < dest_size - 1 ? n : dest_size - 1;
    memcpy(dest, src, static_cast<size_t>(copy));
    dest[copy] = 0;
    return true;
}
} // namespace

int ScancodeToDik(SDL_Scancode scancode)
{
    if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT)
        return 0;
    return GetTables().to_dik[scancode];
}

SDL_Scancode DikToScancode(int dik)
{
    if (dik < 0 || dik >= 256)
        return SDL_SCANCODE_UNKNOWN;
    return GetTables().to_scancode[dik];
}

bool CodepointToCp1251(unsigned cp, char& out)
{
    if (cp < 0x80)
    {
        out = static_cast<char>(cp);
        return true;
    }
    if (cp >= 0x410 && cp <= 0x44F) // А..я
    {
        out = static_cast<char>(0xC0 + (cp - 0x410));
        return true;
    }
    struct Pair
    {
        unsigned cp;
        unsigned char byte;
    };
    static const Pair kExtra[] = {
        {0x0401, 0xA8}, {0x0451, 0xB8}, {0x0490, 0xA5}, {0x0491, 0xB4}, {0x0404, 0xAA}, {0x0454, 0xBA},
        {0x0406, 0xB2}, {0x0456, 0xB3}, {0x0407, 0xAF}, {0x0457, 0xBF}, {0x0408, 0xA3}, {0x0458, 0xBC},
        {0x0405, 0xBD}, {0x0455, 0xBE}, {0x0402, 0x80}, {0x0403, 0x81}, {0x0409, 0x8A}, {0x040A, 0x8C},
        {0x040B, 0x8E}, {0x040C, 0x8D}, {0x040E, 0xA1}, {0x040F, 0x8F}, {0x0452, 0x90}, {0x0453, 0x83},
        {0x0459, 0x9A}, {0x045A, 0x9C}, {0x045B, 0x9E}, {0x045C, 0x9D}, {0x045E, 0xA2}, {0x045F, 0x9F},
        {0x2116, 0xB9}, {0x00A0, 0xA0}, {0x00A4, 0xA4}, {0x00A6, 0xA6}, {0x00A7, 0xA7}, {0x00A9, 0xA9},
        {0x00AB, 0xAB}, {0x00AC, 0xAC}, {0x00AD, 0xAD}, {0x00AE, 0xAE}, {0x00B0, 0xB0}, {0x00B1, 0xB1},
        {0x00B5, 0xB5}, {0x00B6, 0xB6}, {0x00B7, 0xB7}, {0x00BB, 0xBB}, {0x2013, 0x96}, {0x2014, 0x97},
        {0x2018, 0x91}, {0x2019, 0x92}, {0x201A, 0x82}, {0x201C, 0x93}, {0x201D, 0x94}, {0x201E, 0x84},
        {0x2020, 0x86}, {0x2021, 0x87}, {0x2022, 0x95}, {0x2026, 0x85}, {0x2030, 0x89}, {0x2039, 0x8B},
        {0x203A, 0x9B}, {0x20AC, 0x88}, {0x2122, 0x99},
    };
    for (const Pair& p : kExtra)
    {
        if (p.cp == cp)
        {
            out = static_cast<char>(p.byte);
            return true;
        }
    }
    return false;
}

int Utf8ToCp1251(const char* utf8, char* dest, int dest_size)
{
    if (!dest || dest_size <= 0)
        return 0;
    int n = 0;
    if (utf8)
    {
        const char* s = utf8;
        while (*s && n < dest_size - 1)
        {
            char c = '?';
            if (!CodepointToCp1251(DecodeUtf8(s), c))
                c = '?';
            dest[n++] = c;
        }
    }
    dest[n] = 0;
    return n;
}

bool DikName(int dik, char* dest, int dest_size)
{
    if (!dest || dest_size <= 0)
        return false;
    dest[0] = 0;
    const SDL_Scancode scancode = DikToScancode(dik);
    if (scancode == SDL_SCANCODE_UNKNOWN)
        return false;

    // A key that types something is named after what the active layout prints on it.
    const SDL_Keycode key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
    const char* name = nullptr;
    if (key > 0x20 && key < SDLK_EXTENDED_MASK && key != SDLK_DELETE)
        name = SDL_GetKeyName(key);
    if (!name || !*name)
        name = SDL_GetScancodeName(scancode);
    if (!name || !*name)
        return false;

    Utf8ToCp1251(name, dest, dest_size);
    return dest[0] != 0;
}

bool DikToText(int dik, bool shift, bool caps, bool ctrl, bool alt, bool altgr, char* dest, int dest_size)
{
    if (!dest || dest_size <= 1)
        return false;
    dest[0] = 0;

    // Ctrl and Alt chords are commands for the editor, not text. AltGr produces characters on layouts that have it.
    if (ctrl || (alt && !altgr))
        return false;

    const SDL_Scancode scancode = DikToScancode(dik);
    if (scancode == SDL_SCANCODE_UNKNOWN)
        return false;

    SDL_Keymod mods = SDL_KMOD_NONE;
    if (shift)
        mods |= SDL_KMOD_SHIFT;
    if (caps)
        mods |= SDL_KMOD_CAPS;
    if (altgr)
        mods |= SDL_KMOD_RALT;

    const SDL_Keycode key = SDL_GetKeyFromScancode(scancode, mods, false);
    if (key < 0x20 || key == SDLK_DELETE || key >= SDLK_EXTENDED_MASK)
        return false; // control character, or a key without a character (arrows, function keys, modifiers)

    char byte = 0;
    if (!CodepointToCp1251(static_cast<unsigned>(key), byte))
        return false;
    dest[0] = byte;
    dest[1] = 0;
    return true;
}
} // namespace xr_input
