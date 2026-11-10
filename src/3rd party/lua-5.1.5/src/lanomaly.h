/*
** ANOMALY: additions to Lua 5.1.5 for the Anomaly port (README.anomaly.md).
*/

#ifndef lanomaly_h
#define lanomaly_h

#include "lua.h"

/* What the thread that runs the VM is doing: 'I' Lua bytecode, 'C' a C function
** called from Lua, 'G' the collector, 'N' none of these. A plain global (not
** per thread): one store per C call and per collector step; read by
** lua51_vmstate() for the sampling profiler, from a signal handler on the
** thread that runs Lua. */
LUAI_DATA int lua51_vmstate_;

#define lua51_vmstate_enter(s, prev) \
	{ (prev) = lua51_vmstate_; lua51_vmstate_ = (s); }
#define lua51_vmstate_leave(prev)	{ lua51_vmstate_ = (prev); }

#endif
