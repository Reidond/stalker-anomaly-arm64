#pragma once

#include "../../build_config_defines.h"

#include "lua.hpp"

#include <cstdlib>
#include <cctype>

typedef unsigned long DWORD;
typedef unsigned char BYTE;

void open_additional_libs(lua_State*);

#ifdef __APPLE__
// Translator for the engine's virtual paths (xr_native_path), used by lfs.* and the io/os wrappers.
void lua_ext_set_native_path_hook(const char *(*hook)(const char *));
// The same for calls that only read (io.open "r", io.lines, loadfile, dofile): may return the real path of a file a mod provides.
void lua_ext_set_read_path_hook(const char *(*hook)(const char *));
#endif
