/*
** ANOMALY: additions to Lua 5.1.5 for the Anomaly port (README.anomaly.md).
*/

#define lanomaly_c
#define LUA_CORE

#include "lua.h"
#include "lualib.h"

#include "lanomaly.h"


int lua51_vmstate_ = 'N';

LUA_API int lua51_vmstate (void) {
  return lua51_vmstate_;
}
