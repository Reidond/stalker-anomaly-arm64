////////////////////////////////////////////////////////////////////////////
//	Module 		: script_perf.h
//	Description : Lua cost counters and a sampling profiler for the script engine (Apple port, docs/ios-port/LUA-PERF.md)
////////////////////////////////////////////////////////////////////////////
//
// Two tools, both off until asked for (`-lua_perf` / `-lua_profile` on the command line, or the `lua_perf` and `lua_profile` console
// commands), so the game pays one predictable branch per call into Lua:
//
//   timers    wall time of the calls from C++ into Lua (luabind calls, script threads, the explicit GC steps), per frame and per
//             section (script scheduler, bind_* binders, A-Life, object callbacks, the rest). Only the outermost call is timed: Lua
//             calling the engine calling Lua again is counted once. A report line per period (a label set with `lua_perf mark`)
//             gives the frame time, the Lua time per frame, the share and its distribution.
//   profiler  LuaJIT's sampling hook (luaJIT_profile_start; it samples the interpreter too): self and inclusive samples per Lua
//             function, split by VM state (interpreter / C function / GC), written as `[lua-prof]` lines.
//
// Everything is a no-op on other platforms.

#pragma once

#ifdef XR_PLATFORM_APPLE

struct lua_State;

namespace script_perf
{
enum Section : int
{
	Scheduler, // CScriptProcess / CScriptThread: the script threads (`main` functions)
	Binder,    // CScriptBinder: bind_* objects' update, net_spawn, net_destroy, save/load
	ALife,     // the A-Life simulator update (se_* and smart terrain scripts)
	Callback,  // CScriptCallbackEx: set_callback functions of game objects
	Other,     // everything else (UI, console, level callbacks)
	SectionCount
};

extern bool g_on;                // the timers run
extern thread_local int t_section; // the section the current thread's calls into Lua belong to

// Sets the section for the Lua calls made while it lives.
struct Context
{
	int prev;
	bool active;
	explicit Context(Section s) : prev(0), active(g_on)
	{
		if (active)
		{
			prev = t_section;
			t_section = s;
		}
	}
	~Context()
	{
		if (active)
			t_section = prev;
	}
	Context(const Context&) = delete;
	Context& operator=(const Context&) = delete;
};

// A timed call into Lua. enter() returns 0 when the timers are off, 1 for a call inside another call (counted, not timed), or
// the start stamp of an outermost call.
unsigned long long enter();
void leave(unsigned long long stamp);

struct Call
{
	unsigned long long stamp;
	Call() : stamp(g_on ? enter() : 0) {}
	~Call()
	{
		if (stamp)
			leave(stamp);
	}
	Call(const Call&) = delete;
	Call& operator=(const Call&) = delete;
};

// An explicit garbage-collector step (any thread).
struct GcStep
{
	unsigned long long stamp;
	GcStep();
	~GcStep();
	GcStep(const GcStep&) = delete;
	GcStep& operator=(const GcStep&) = delete;
};

// A script file load (CScriptStorage::do_file): the phases are added to the load counters.
struct LoadScope
{
	unsigned long long start;
	bool outermost;
	LoadScope();
	~LoadScope();
	LoadScope(const LoadScope&) = delete;
	LoadScope& operator=(const LoadScope&) = delete;
};
void load_phase_read(double ms, unsigned long long bytes);
void load_phase_compile(double ms);

void frame();               // CLevel::OnFrame: closes the previous frame
void init(lua_State* L);    // the script engine has a new Lua state: command-line switches, resets the profiler
void detach(lua_State* L);  // that state is about to be closed: final reports, stops the profiler
void register_console();    // lua_perf, lua_profile, lua_cache
double now_ms();
} // namespace script_perf

#else // !XR_PLATFORM_APPLE

struct lua_State;

namespace script_perf
{
enum Section : int { Scheduler, Binder, ALife, Callback, Other, SectionCount };
struct Context { explicit Context(Section) {} };
struct Call {};
struct GcStep {};
struct LoadScope {};
inline void load_phase_read(double, unsigned long long) {}
inline void load_phase_compile(double) {}
inline void frame() {}
inline void init(lua_State*) {}
inline void detach(lua_State*) {}
inline void register_console() {}
inline double now_ms() { return 0; }
} // namespace script_perf

#endif
