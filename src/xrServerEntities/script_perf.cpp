////////////////////////////////////////////////////////////////////////////
//	Module 		: script_perf.cpp
//	Description : Lua cost counters and a sampling profiler for the script engine (Apple port, docs/ios-port/LUA-PERF.md)
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"

#ifdef XR_PLATFORM_APPLE

#include "script_perf.h"
#include "script_bytecode_cache.h"
#include "script_engine.h"
#include "ai_space.h"
#include "../xrEngine/xr_ioconsole.h"
#include "../xrEngine/xr_ioc_cmd.h"
#include <luabind/detail/pcall.hpp>

#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace script_perf
{
bool g_on = false;
thread_local int t_section = Other;

namespace
{
using u64 = unsigned long long;

thread_local int t_depth = 0;      // calls into Lua in progress on this thread
thread_local int t_load_depth = 0; // do_file calls in progress on this thread

pthread_t g_main_thread;
bool g_main_known = false;

inline u64 now_ns()
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// CPU time of this thread: the time a call into Lua really ran, not the time it was preempted (the Mac is shared with builds and other games)
inline u64 thread_cpu_ns()
{
	return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
}

thread_local u64 t_cpu_start = 0;

// True while the main thread is inside a call into Lua. The profiler's timer counts the CPU time of every thread, so most of its ticks
// land while the engine runs C++; LuaJIT would book those to the next Lua function that runs. The gate below drops them.
std::atomic<bool> g_lua_active{false};

inline bool on_main_thread()
{
	return !g_main_known || pthread_equal(pthread_self(), g_main_thread);
}

// What the main thread has spent in Lua since the last frame() ...
u64 g_frame_ns[SectionCount];
u64 g_frame_cpu_ns = 0;
u64 g_frame_calls = 0;
u64 g_frame_nested = 0;
u64 g_frame_gc_ns = 0;
// ... and what other threads have (the parallel garbage collector, A-Life when it runs on a worker)
std::atomic<u64> g_off_ns{0};
std::atomic<u64> g_off_gc_ns{0};
std::atomic<u64> g_off_calls{0};

// ---- reporting periods -----------------------------------------------------------------------------------------------------------

struct Period
{
	std::string label = "run";
	u64 start_ns = 0;
	u64 frames = 0;
	double frame_ms_sum = 0, frame_ms_max = 0;
	std::vector<float> frame_ms, lua_ms;
	double sec_ms[SectionCount] = {};
	double cpu_ms = 0;
	double gc_ms = 0, off_ms = 0, off_gc_ms = 0;
	u64 calls = 0, nested = 0, off_calls = 0;
};

struct Watch
{
	double ms = 0;
	u64 calls = 0;
};

Period g_period;
u64 g_last_frame_ns = 0;
double g_interval_s = 30; // a period report without a mark every so many seconds; 0 = only at marks
std::map<std::string, Watch> g_watch;   // totals of the period, keyed "path" or "path:first argument"
std::vector<std::pair<std::string, bool>> g_watch_paths; // functions to wrap (path, keyed by first argument)
bool g_watch_installed = false;
u64 g_watch_try_ns = 0;

// ---- script loads ----------------------------------------------------------------------------------------------------------------

struct LoadCounters
{
	u64 files = 0, bytes = 0, total_ns = 0;
	double read_ms = 0, compile_ms = 0;
	bool dirty = false;
};
LoadCounters g_load;

double pct(std::vector<float>& v, double p)
{
	if (v.empty())
		return 0;
	const size_t k = std::min(v.size() - 1, size_t(p * double(v.size())));
	std::nth_element(v.begin(), v.begin() + long(k), v.end());
	return v[k];
}

void print_load_report(const char* why)
{
	if (!g_load.files)
		return;
	const script_cache::Stats c = script_cache::stats();
	Msg("* [lua-load] %s files=%llu mb=%.2f total_ms=%.0f read_ms=%.0f compile_ms=%.0f run_ms=%.0f cache=%s hit=%u prefetched=%u miss=%u stale=%u other=%u "
	    "stored=%u hash_ms=%.0f cache_read_ms=%.0f cache_io_ms=%.0f cache_decode_ms=%.0f cache_compile_ms=%.0f cache_store_ms=%.0f",
	    why, g_load.files, double(g_load.bytes) / 1048576.0, double(g_load.total_ns) * 1e-6, g_load.read_ms, g_load.compile_ms,
	    std::max(0.0, double(g_load.total_ns) * 1e-6 - g_load.read_ms - g_load.compile_ms), script_cache::directory()[0] ? "on" : "off", c.hits,
	    c.prefetched, c.misses, c.stale, c.incompatible + c.corrupt, c.stored, c.hash_ms, c.read_ms, c.io_ms, c.decode_ms, c.compile_ms, c.store_ms);
	g_load = LoadCounters();
	script_cache::reset_stats();
}

void print_period(const char* why)
{
	Period& p = g_period;
	const double secs = double(now_ns() - p.start_ns) * 1e-9;
	if (p.frames)
	{
		const double f = double(p.frames);
		double lua_sum = 0;
		for (int i = 0; i < SectionCount; ++i)
			lua_sum += p.sec_ms[i];
		lua_sum += p.gc_ms;
		const double frame_avg = p.frame_ms_sum / f;
		std::vector<float> fm = p.frame_ms, lm = p.lua_ms;
		Msg("* [lua-perf] label=%s why=%s secs=%.1f frames=%llu frame_ms=%.2f frame_p95=%.2f frame_max=%.1f lua_ms=%.3f lua_cpu=%.3f lua_p50=%.3f lua_p95=%.3f "
		    "lua_max=%.2f share=%.1f sched=%.3f binder=%.3f alife=%.3f cb=%.3f other=%.3f gc=%.3f calls=%.0f nested=%.0f off_main=%.3f off_gc=%.3f",
		    p.label.c_str(), why, secs, p.frames, frame_avg, pct(fm, 0.95), p.frame_ms_max, lua_sum / f, p.cpu_ms / f, pct(lm, 0.5), pct(lm, 0.95),
		    lm.empty() ? 0.0 : double(*std::max_element(lm.begin(), lm.end())), frame_avg > 0 ? 100.0 * (lua_sum / f) / frame_avg : 0.0,
		    p.sec_ms[Scheduler] / f, p.sec_ms[Binder] / f, p.sec_ms[ALife] / f, p.sec_ms[Callback] / f, p.sec_ms[Other] / f, p.gc_ms / f,
		    double(p.calls) / f, double(p.nested) / f, p.off_ms / f, p.off_gc_ms / f);
		if (!g_watch.empty())
		{
			std::vector<std::pair<std::string, Watch>> w(g_watch.begin(), g_watch.end());
			std::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
			std::string line;
			for (size_t i = 0; i < w.size() && i < 10; ++i)
			{
				char buf[200];
				snprintf(buf, sizeof(buf), " %s=%.3f/%.2f", w[i].first.c_str(), w[i].second.ms / f, double(w[i].second.calls) / f);
				line += buf;
			}
			Msg("* [lua-perf] label=%s watch (ms/frame / calls/frame):%s", p.label.c_str(), line.c_str());
		}
	}
	else
		Msg("* [lua-perf] label=%s why=%s secs=%.1f frames=0", p.label.c_str(), why, secs);
	g_watch.clear();
}

void start_period(const std::string& label)
{
	g_period = Period();
	g_period.label = label;
	g_period.start_ns = now_ns();
	g_period.frame_ms.reserve(4096);
	g_period.lua_ms.reserve(4096);
	g_off_ns = 0;
	g_off_gc_ns = 0;
	g_off_calls = 0;
}

// ---- the watches: Lua functions wrapped with a timer ---------------------------------------------------------------------------

int l_now(lua_State* L)
{
	lua_pushnumber(L, double(now_ns()));
	return 1;
}

int l_add(lua_State* L)
{
	const char* key = lua_tostring(L, 1);
	if (key && g_on)
	{
		Watch& w = g_watch[key];
		w.ms += (double(now_ns()) - lua_tonumber(L, 2)) * 1e-6;
		++w.calls;
	}
	return 0;
}

const char* const kWatchChunk =
	"local now, add, path, by_arg = ...\n"
	"local t = _G\n"
	"local parts = {}\n"
	"for p in string.gmatch(path, '[^%.]+') do parts[#parts + 1] = p end\n"
	"for i = 1, #parts - 1 do t = t[parts[i]]; if type(t) ~= 'table' then return false end end\n"
	"local name = parts[#parts]\n"
	"local f = t[name]\n"
	"if type(f) ~= 'function' then return false end\n"
	"if by_arg then\n"
	"\tt[name] = function(a, ...) local t0 = now(); local r1, r2, r3, r4 = f(a, ...); add(path .. ':' .. tostring(a), t0); return r1, r2, r3, r4 end\n"
	"else\n"
	"\tt[name] = function(...) local t0 = now(); local r1, r2, r3, r4 = f(...); add(path, t0); return r1, r2, r3, r4 end\n"
	"end\n"
	"return true\n";

bool install_watch(lua_State* L, const std::string& path, bool by_arg)
{
	const int top = lua_gettop(L);
	bool ok = false;
	if (luaL_loadbuffer(L, kWatchChunk, strlen(kWatchChunk), "=lua_perf_watch") == 0)
	{
		lua_pushcfunction(L, l_now);
		lua_pushcfunction(L, l_add);
		lua_pushstring(L, path.c_str());
		lua_pushboolean(L, by_arg);
		if (lua_pcall(L, 4, 1, 0) == 0)
			ok = lua_toboolean(L, -1) != 0;
	}
	lua_settop(L, top);
	return ok;
}

// The functions are defined by scripts that load after the engine starts: retry until they all exist.
void try_install_watches()
{
	if (g_watch_installed || g_watch_paths.empty() || !g_on)
		return;
	const u64 now = now_ns();
	if (now - g_watch_try_ns < 1000000000ull)
		return;
	g_watch_try_ns = now;
	lua_State* L = ai().script_engine().lua();
	if (!L)
		return;
	bool all = true;
	for (auto& w : g_watch_paths)
		if (!install_watch(L, w.first, w.second))
			all = false;
	g_watch_installed = all;
}

// ---- the sampling profiler ---------------------------------------------------------------------------------------------------------

struct FuncStat
{
	std::string source; // chunk name without '@'
	int line = 0;
	std::string name;
	u64 self_i = 0, self_c = 0, self_g = 0, incl = 0;
};

void set_on(bool on);
void prof_stop();

// The sample clock. LuaJIT's own is an ITIMER_PROF: on macOS it ticks every 10 ms whatever interval is asked for, and counts the CPU time of
// every thread, so most ticks land while the engine runs C++ (the VM state then is whatever Lua left it in, and the sample is booked to the next
// Lua function that runs). Instead a thread sends SIGPROF to the main thread every interval, but only while it is inside a call into Lua; the
// signal runs LuaJIT's handler as usual, which has the VM call the callback at its next instruction.
std::atomic<bool> g_sampler_run{false};
pthread_t g_sampler_thread;
pthread_t g_sampler_target;

void* sampler_main(void* arg)
{
	const long ns = long(reinterpret_cast<intptr_t>(arg)) * 1000000L;
	while (g_sampler_run.load(std::memory_order_relaxed))
	{
		struct timespec ts = {0, ns};
		nanosleep(&ts, nullptr);
		if (g_lua_active.load(std::memory_order_relaxed))
			pthread_kill(g_sampler_target, SIGPROF);
	}
	return nullptr;
}

bool g_prof_on = false;
std::string g_prof_label = "run";
int g_prof_ms = 1;
lua_State* g_prof_state = nullptr;
std::unordered_map<std::string, FuncStat> g_funcs;
std::unordered_map<std::string, u64> g_files;
u64 g_state_samples[256];
u64 g_samples = 0;

std::string base_name(const char* src)
{
	const char* s = src;
	if (*s == '@')
		++s;
	const char* b = s;
	for (const char* p = s; *p; ++p)
		if (*p == '/' || *p == '\\')
			b = p + 1;
	return b;
}

void prof_callback(void*, lua_State* L, int samples, int vmstate)
{
	// 'I' interpreter, 'C' a C function called from Lua, 'G' the collector, 'N' a compiled trace, 'J' the JIT compiler
	g_state_samples[vmstate & 255] += u64(samples);
	g_samples += u64(samples);
	lua_Debug ar;
	std::string seen[32];
	int nseen = 0;
	bool leaf_done = false;
	std::string file_seen[8];
	int nfile = 0;
	for (int lvl = 0; lvl < 32 && lua_getstack(L, lvl, &ar); ++lvl)
	{
		if (!lua_getinfo(L, "Sn", &ar))
			continue;
		if (ar.what[0] == 'C')
			continue;
		char tmp[16];
		snprintf(tmp, sizeof(tmp), ":%d", ar.linedefined);
		const std::string key = std::string(ar.source ? ar.source : "?") + tmp;
		FuncStat& f = g_funcs[key];
		if (f.source.empty())
		{
			f.source = ar.source ? (ar.source[0] == '@' ? ar.source + 1 : ar.source) : "?";
			f.line = ar.linedefined;
		}
		if (f.name.empty() && ar.name)
			f.name = ar.name;
		if (!leaf_done)
		{
			leaf_done = true;
			if (vmstate == 'C')
				f.self_c += u64(samples);
			else if (vmstate == 'G')
				f.self_g += u64(samples);
			else
				f.self_i += u64(samples);
		}
		bool dup = false;
		for (int i = 0; i < nseen; ++i)
			if (seen[i] == key)
			{
				dup = true;
				break;
			}
		if (!dup && nseen < 32)
		{
			seen[nseen++] = key;
			f.incl += u64(samples);
		}
		const std::string file = base_name(f.source.c_str());
		bool file_dup = false;
		for (int i = 0; i < nfile; ++i)
			if (file_seen[i] == file)
			{
				file_dup = true;
				break;
			}
		if (!file_dup && nfile < 8)
		{
			file_seen[nfile++] = file;
			g_files[file] += u64(samples);
		}
	}
}

// The text of line `line` of a script file, to say what a profiled function is.
std::string source_line(const std::string& path, int line)
{
	static std::unordered_map<std::string, std::vector<std::string>> cache;
	auto it = cache.find(path);
	if (it == cache.end())
	{
		std::vector<std::string> lines;
		if (IReader* r = FS.r_open(path.c_str()))
		{
			const char* p = static_cast<const char*>(r->pointer());
			const char* end = p + r->length();
			while (p < end)
			{
				const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
				const char* e = nl ? nl : end;
				while (e > p && (e[-1] == '\r'))
					--e;
				lines.emplace_back(p, size_t(e - p));
				p = nl ? nl + 1 : end;
			}
			FS.r_close(r);
		}
		it = cache.emplace(path, std::move(lines)).first;
	}
	if (line < 1 || size_t(line) > it->second.size())
		return std::string();
	std::string s = it->second[size_t(line - 1)];
	size_t a = s.find_first_not_of(" \t");
	s = a == std::string::npos ? std::string() : s.substr(a);
	if (s.size() > 90)
		s.resize(90);
	return s;
}

void prof_report(size_t top)
{
	if (!g_samples)
	{
		Msg("* [lua-prof] label=%s no samples", g_prof_label.c_str());
		return;
	}
	const double total = double(g_samples);
	Msg("* [lua-prof] label=%s samples=%llu interval_ms=%d interp=%.1f%% cfunc=%.1f%% gc=%.1f%% trace=%.1f%% jitc=%.1f%%", g_prof_label.c_str(), g_samples, g_prof_ms,
	    100.0 * double(g_state_samples['I']) / total, 100.0 * double(g_state_samples['C']) / total, 100.0 * double(g_state_samples['G']) / total,
	    100.0 * double(g_state_samples['N']) / total, 100.0 * double(g_state_samples['J']) / total);
	const double lua_total = std::max(1.0, total);
	std::vector<std::pair<const std::string*, const FuncStat*>> v;
	for (const auto& kv : g_funcs)
		v.emplace_back(&kv.first, &kv.second);
	auto self = [](const FuncStat* f) { return f->self_i + f->self_c + f->self_g; };
	std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) { return self(a.second) > self(b.second); });
	for (size_t i = 0; i < v.size() && i < top; ++i)
	{
		const FuncStat* f = v[i].second;
		Msg("* [lua-prof] label=%s self #%zu %.2f%% (interp %.2f c %.2f gc %.2f) incl %.2f%%  %s:%d  %s  | %s", g_prof_label.c_str(), i + 1, 100.0 * double(self(f)) / lua_total,
		    100.0 * double(f->self_i) / lua_total, 100.0 * double(f->self_c) / lua_total, 100.0 * double(f->self_g) / lua_total,
		    100.0 * double(f->incl) / lua_total, base_name(f->source.c_str()).c_str(), f->line, f->name.c_str(), source_line(f->source, f->line).c_str());
	}
	std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second->incl > b.second->incl; });
	for (size_t i = 0; i < v.size() && i < top; ++i)
	{
		const FuncStat* f = v[i].second;
		Msg("* [lua-prof] label=%s incl #%zu %.2f%% self %.2f%%  %s:%d  %s  | %s", g_prof_label.c_str(), i + 1, 100.0 * double(f->incl) / lua_total,
		    100.0 * double(self(f)) / lua_total, base_name(f->source.c_str()).c_str(), f->line, f->name.c_str(), source_line(f->source, f->line).c_str());
	}
	std::vector<std::pair<std::string, u64>> files(g_files.begin(), g_files.end());
	std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
	for (size_t i = 0; i < files.size() && i < 20; ++i)
		Msg("* [lua-prof] label=%s file #%zu %.2f%% %s", g_prof_label.c_str(), i + 1, 100.0 * double(files[i].second) / lua_total, files[i].first.c_str());
}

void prof_reset()
{
	g_funcs.clear();
	g_files.clear();
	memset(g_state_samples, 0, sizeof(g_state_samples));
	g_samples = 0;
}

void prof_start(int ms)
{
	lua_State* L = ai().script_engine().lua();
	if (!L)
		return;
	prof_stop();
	g_prof_ms = std::max(1, std::min(100, ms));
	char mode[16];
	snprintf(mode, sizeof(mode), "i%d", g_prof_ms);
	prof_reset();
	g_prof_state = L;
	luaJIT_profile_start(L, mode, prof_callback, nullptr);
	// LuaJIT armed its ITIMER_PROF: disarm it, the sampler thread is the clock (see above)
	struct itimerval off = {};
	setitimer(ITIMER_PROF, &off, nullptr);
	set_on(true);
	g_sampler_target = pthread_self();
	g_sampler_run = true;
	pthread_create(&g_sampler_thread, nullptr, sampler_main, reinterpret_cast<void*>(intptr_t(g_prof_ms)));
	g_prof_on = true;
	Msg("* [lua-prof] started, %d ms samples", g_prof_ms);
}

void prof_stop()
{
	if (!g_prof_on)
		return;
	if (g_sampler_run.exchange(false))
		pthread_join(g_sampler_thread, nullptr);
	luaJIT_profile_stop(g_prof_state);
	g_prof_on = false;
	g_prof_state = nullptr;
}

// ---- switching the timers ----------------------------------------------------------------------------------------------------------

void set_on(bool on)
{
	if (on == g_on)
		return;
	if (on)
	{
		g_on = true;
		start_period("run");
		g_last_frame_ns = 0;
		luabind::set_call_hooks(enter, leave);
		if (g_watch_paths.empty())
			g_watch_paths.emplace_back("SendScriptCallback", true);
		g_watch_installed = false;
		Msg("* [lua-perf] timers on (periods of %.0f s, `lua_perf mark <label>` starts one)", g_interval_s);
	}
	else
	{
		print_period("off");
		g_on = false;
		luabind::set_call_hooks(nullptr, nullptr);
		Msg("* [lua-perf] timers off");
	}
}

// ---- console -----------------------------------------------------------------------------------------------------------------------

class CCC_LuaPerf : public IConsole_Command
{
public:
	CCC_LuaPerf(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
	virtual void Execute(LPCSTR args)
	{
		char cmd[32] = {};
		char rest[200] = {};
		sscanf(args ? args : "", "%31s %199[^\n]", cmd, rest);
		if (!cmd[0])
			Msg("* [lua-perf] timers %s, period '%s', %llu frames so far; usage: lua_perf on|off|mark <label>|report|reset|interval <s>|watch <path> [arg]|unwatch",
			    g_on ? "on" : "off", g_period.label.c_str(), g_period.frames);
		else if (!strcmp(cmd, "on"))
			set_on(true);
		else if (!strcmp(cmd, "off"))
			set_on(false);
		else if (!g_on)
			Msg("! lua_perf: the timers are off (lua_perf on)");
		else if (!strcmp(cmd, "mark"))
		{
			g_interval_s = 0; // marks are in charge of the periods now
			print_period("mark");
			start_period(rest[0] ? rest : "run");
		}
		else if (!strcmp(cmd, "report"))
		{
			print_period("report");
			print_load_report("report");
		}
		else if (!strcmp(cmd, "reset"))
			start_period(g_period.label);
		else if (!strcmp(cmd, "interval"))
			g_interval_s = atof(rest);
		else if (!strcmp(cmd, "watch"))
		{
			char path[160] = {}, arg[16] = {};
			sscanf(rest, "%159s %15s", path, arg);
			g_watch_paths.emplace_back(path, !strcmp(arg, "arg"));
			g_watch_installed = false;
		}
		else if (!strcmp(cmd, "unwatch"))
			g_watch_paths.clear();
		else
			InvalidSyntax();
	}
	virtual void Info(TInfo& I) { xr_strcpy(I, "on|off|mark <label>|report|reset|interval <s>|watch <path> [arg]|unwatch"); }
};

class CCC_LuaProfile : public IConsole_Command
{
public:
	CCC_LuaProfile(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
	virtual void Execute(LPCSTR args)
	{
		char cmd[32] = {};
		char word[64] = {};
		int n = 0;
		sscanf(args ? args : "", "%31s %63s", cmd, word);
		n = atoi(word);
		if (!strcmp(cmd, "on"))
			prof_start(n > 0 ? n : g_prof_ms);
		else if (!strcmp(cmd, "off"))
		{
			prof_stop();
			Msg("* [lua-prof] stopped");
		}
		else if (!strcmp(cmd, "report"))
			prof_report(n > 0 ? size_t(n) : 30);
		else if (!strcmp(cmd, "reset"))
		{
			prof_reset();
			g_prof_label = word[0] ? word : "run";
		}
		else
			Msg("* [lua-prof] %s, %llu samples; usage: lua_profile on [ms]|off|report [top]|reset [label]", g_prof_on ? "running" : "stopped", g_samples);
	}
	virtual void Info(TInfo& I) { xr_strcpy(I, "on [ms]|off|report [top]|reset [label]"); }
};

class CCC_LuaCache : public IConsole_Command
{
public:
	CCC_LuaCache(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
	virtual void Execute(LPCSTR args)
	{
		char cmd[32] = {};
		sscanf(args ? args : "", "%31s", cmd);
		if (!strcmp(cmd, "clear"))
			Msg("* [lua-cache] %zu entries removed from %s", script_cache::purge(), script_cache::directory());
		else if (!strcmp(cmd, "warm"))
		{
			const double t0 = now_ms();
			const size_t n = ai().script_engine().warm_bytecode_cache();
			Msg("* [lua-cache] warmed %zu scripts in %.0f ms", n, now_ms() - t0);
		}
		else if (!strcmp(cmd, "off"))
		{
			ai().script_engine().set_bytecode_cache(false);
			Msg("* [lua-cache] off");
		}
		else if (!strcmp(cmd, "on"))
		{
			ai().script_engine().set_bytecode_cache(true);
			Msg("* [lua-cache] on, %s", script_cache::directory());
		}
		else
		{
			const script_cache::Stats c = script_cache::stats();
			Msg("* [lua-cache] %s %s: %u hits, %u misses, %u stale, %u incompatible, %u corrupt, %u stored, %u store failures; "
			    "usage: lua_cache info|on|off|clear|warm",
			    script_cache::directory()[0] ? "on" : "off", script_cache::directory(), c.hits, c.misses, c.stale, c.incompatible, c.corrupt,
			    c.stored, c.store_failed);
		}
	}
	virtual void Info(TInfo& I) { xr_strcpy(I, "info|on|off|clear|warm"); }
};
} // namespace

unsigned long long enter()
{
	if (!g_on)
		return 0;
	if (t_depth++)
	{
		if (on_main_thread())
			++g_frame_nested;
		return 1;
	}
	t_cpu_start = thread_cpu_ns();
	if (on_main_thread())
		g_lua_active.store(true, std::memory_order_relaxed);
	return now_ns();
}

void leave(unsigned long long stamp)
{
	if (!stamp)
		return;
	--t_depth;
	if (stamp == 1)
		return;
	if (on_main_thread())
		g_lua_active.store(false, std::memory_order_relaxed);
	const u64 ns = now_ns() - stamp;
	if (on_main_thread())
	{
		g_frame_ns[t_section] += ns;
		g_frame_cpu_ns += thread_cpu_ns() - t_cpu_start;
		++g_frame_calls;
	}
	else
	{
		g_off_ns += ns;
		++g_off_calls;
	}
}

GcStep::GcStep() : stamp(g_on ? now_ns() : 0) {}

GcStep::~GcStep()
{
	if (!stamp)
		return;
	const u64 ns = now_ns() - stamp;
	if (on_main_thread())
		g_frame_gc_ns += ns;
	else
		g_off_gc_ns += ns;
}

LoadScope::LoadScope() : start(g_on ? now_ns() : 0), outermost(false)
{
	if (start)
		outermost = (t_load_depth++ == 0);
}

LoadScope::~LoadScope()
{
	if (!start)
		return;
	--t_load_depth;
	++g_load.files;
	g_load.dirty = true;
	if (outermost)
		g_load.total_ns += now_ns() - start;
}

void load_phase_read(double ms, unsigned long long bytes)
{
	if (!g_on)
		return;
	g_load.read_ms += ms;
	g_load.bytes += bytes;
}

void load_phase_compile(double ms)
{
	if (g_on)
		g_load.compile_ms += ms;
}

double now_ms()
{
	return double(now_ns()) * 1e-6;
}

void frame()
{
	if (!g_on)
		return;
	if (!g_main_known)
	{
		g_main_thread = pthread_self();
		g_main_known = true;
	}
	try_install_watches();
	const u64 now = now_ns();
	if (g_load.dirty && t_load_depth == 0 && g_load.files)
		print_load_report("level");
	if (g_last_frame_ns)
	{
		Period& p = g_period;
		const double dt = double(now - g_last_frame_ns) * 1e-6;
		double lua = 0;
		for (int i = 0; i < SectionCount; ++i)
		{
			const double ms = double(g_frame_ns[i]) * 1e-6;
			p.sec_ms[i] += ms;
			lua += ms;
		}
		p.cpu_ms += double(g_frame_cpu_ns) * 1e-6;
		const double gc = double(g_frame_gc_ns) * 1e-6;
		p.gc_ms += gc;
		lua += gc;
		p.off_ms += double(g_off_ns.exchange(0)) * 1e-6;
		p.off_gc_ms += double(g_off_gc_ns.exchange(0)) * 1e-6;
		p.off_calls += g_off_calls.exchange(0);
		p.calls += g_frame_calls;
		p.nested += g_frame_nested;
		++p.frames;
		p.frame_ms_sum += dt;
		p.frame_ms_max = std::max(p.frame_ms_max, dt);
		p.frame_ms.push_back(float(dt));
		p.lua_ms.push_back(float(lua));
		if (g_interval_s > 0 && double(now - p.start_ns) * 1e-9 >= g_interval_s)
		{
			print_period("interval");
			start_period(g_period.label);
		}
	}
	memset(g_frame_ns, 0, sizeof(g_frame_ns));
	g_frame_calls = g_frame_nested = g_frame_gc_ns = g_frame_cpu_ns = 0;
	g_last_frame_ns = now;
}

void init(lua_State* L)
{
	// a new Lua state: the profiler of the old one is gone
	g_prof_on = false;
	g_prof_state = nullptr;
	g_watch_installed = false;
	const char* params = Core.Params ? Core.Params : "";
	if (strstr(params, "-lua_perf"))
		set_on(true);
	if (strstr(params, "-lua_profile") && L)
	{
		int ms = 1;
		if (const char* p = strstr(params, "-lua_profile_ms "))
			ms = atoi(p + strlen("-lua_profile_ms "));
		prof_start(ms);
	}
}

void detach(lua_State* L)
{
	if (g_prof_on && g_prof_state == L)
	{
		prof_report(30);
		prof_stop();
	}
	if (g_on)
	{
		print_period("shutdown");
		print_load_report("shutdown");
		start_period(g_period.label);
		g_last_frame_ns = 0;
	}
}

void register_console()
{
	CMD1(CCC_LuaPerf, "lua_perf");
	CMD1(CCC_LuaProfile, "lua_profile");
	CMD1(CCC_LuaCache, "lua_cache");
}
} // namespace script_perf

#endif // XR_PLATFORM_APPLE
