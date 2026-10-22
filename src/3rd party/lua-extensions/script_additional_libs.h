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
#endif
