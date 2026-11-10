// Implementation of the Win32/CRT compatibility layer declared in src/Common/PlatformApple.inl (macOS and iOS).
//
// Deliberately independent of the rest of xrCore: no stdafx.h, so it can be used (and tested) on its own and does not
// pull in xrCore's `thread_t` next to the Mach `thread_t`.
#include "Common/Platform.h"
#undef interface // Platform.h defines it for the engine sources; Apple system headers below may use the name

#ifdef XR_PLATFORM_APPLE

#include <fnmatch.h>
#include <pwd.h>
#ifdef XR_PLATFORM_WEB
// Emscripten: POSIX libc. The Apple-only calls below (mach, sysctl, malloc zones, dyld) have web replacements in
// Platform/PlatformWeb.cpp, or an XR_PLATFORM_WEB branch where they are one line.
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
void xr_apple_vm_info(size_t* free_bytes, size_t* reserved, size_t* committed); // PlatformWeb.cpp
#else
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "XrMemStats.h"


// ---------------------------------------------------------------------------------------------
// Paths: virtual ('\\', any case) <-> native
// ---------------------------------------------------------------------------------------------
namespace
{
std::mutex g_dir_cache_lock;
// native directory path -> (lower-cased entry name -> every real spelling; a case-sensitive volume can hold "Scripts" and "scripts" side by side)
std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> g_dir_cache;

std::string lower(const std::string& s)
{
	std::string r(s);
	for (char& c : r) c = (char)tolower((unsigned char)c);
	return r;
}

bool exists_exact(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}

// Looks 'name' up in directory 'dir', ignoring case: every spelling it has there. 'rescan' forces a fresh listing.
bool find_entries(const std::string& dir, const std::string& name, bool rescan, std::vector<std::string>& real)
{
	const std::string key = lower(name);
	std::lock_guard<std::mutex> guard(g_dir_cache_lock);
	auto it = g_dir_cache.find(dir);
	if (it == g_dir_cache.end() || rescan)
	{
		auto& entries = g_dir_cache[dir];
		entries.clear();
		if (DIR* d = opendir(dir.empty() ? "." : dir.c_str()))
		{
			while (dirent* e = readdir(d))
				entries[lower(e->d_name)].push_back(e->d_name);
			closedir(d);
		}
		it = g_dir_cache.find(dir);
	}
	auto f = it->second.find(key);
	if (f == it->second.end()) return false;
	real = f->second;
	return true;
}

// The virtual paths are lower case, so an app container path ("/private/var/mobile/Containers/Data/Application/<UUID>")
// arrives without its capitals, and from inside the iOS sandbox its parents cannot be listed to find them again.
// $HOME is the container root on iOS (the home directory on macOS): put its real spelling back first.
std::string restore_home_spelling(const std::string& native)
{
	// $HOME says "/private/var/mobile/Containers/Data/Application/<UUID>" while NSSearchPathForDirectoriesInDomains (and so the
	// roots of the generated fsgame.ltx) say "/var/mobile/...": the same directory through the /var symlink. Both spellings
	// are restored.
	static const std::vector<std::string> homes = []
	{
		const char* h = getenv("HOME");
		std::string r(h ? h : "");
		while (r.size() > 1 && r.back() == '/') r.pop_back();
		std::vector<std::string> v;
		if (r.size() >= 2)
		{
			v.push_back(r);
			static const std::string priv = "/private";
			if (r.compare(0, priv.size() + 1, priv + "/") == 0)
				v.push_back(r.substr(priv.size()));
			else
				v.push_back(priv + r);
		}
		return v;
	}();
	for (const std::string& home : homes)
	{
		if (native.size() < home.size() || native.compare(0, home.size(), home) == 0) continue;
		if (strncasecmp(native.c_str(), home.c_str(), home.size()) != 0) continue;
		if (native.size() != home.size() && native[home.size()] != '/') continue;
		return home + native.substr(home.size());
	}
	return native;
}

std::string join_path(const std::string& base, const std::string& name)
{
	if (base.empty()) return name;
	return base.back() == '/' ? base + name : base + "/" + name;
}

// The spellings of 'comp' that exist in 'base': the exact one first, then the other case variants ("Scripts" next to "scripts").
void spellings(const std::string& base, const std::string& comp, std::vector<std::string>& out)
{
	out.clear();
	if (comp == "." || comp == "..")
	{
		out.push_back(comp);
		return;
	}
	if (exists_exact(join_path(base, comp))) out.push_back(comp);
	std::vector<std::string> real;
	if (!find_entries(base, comp, false, real)) find_entries(base, comp, true, real);
	for (const std::string& r : real)
		if (r != comp) out.push_back(r);
	if (out.size() < 2) return;
	// A case-insensitive volume lists the entry under its stored spelling while the spelling asked for exists as well: one
	// entry, two names. Keep one name per inode.
	std::vector<std::string> unique;
	std::vector<std::pair<dev_t, ino_t>> ids;
	for (const std::string& n : out)
	{
		struct stat st;
		if (lstat(join_path(base, n).c_str(), &st) != 0) continue;
		const std::pair<dev_t, ino_t> id(st.st_dev, st.st_ino);
		if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
		ids.push_back(id);
		unique.push_back(n);
	}
	out.swap(unique);
}

// Every existing path that spells 'comps[i..]' below 'base' (up to 'limit' of them, the exact spelling first).
void walk(const std::vector<std::string>& comps, size_t i, const std::string& base, size_t limit, std::vector<std::string>& found)
{
	if (found.size() >= limit) return;
	if (i == comps.size())
	{
		found.push_back(base);
		return;
	}
	std::vector<std::string> names;
	spellings(base, comps[i], names);
	for (const std::string& n : names)
	{
		walk(comps, i + 1, join_path(base, n), limit, found);
		if (found.size() >= limit) return;
	}
}

// Every existing spelling of 'native' ('/'-separated), at most 'limit'. A path that exists as spelled is the only one asked for.
void resolve_all(const std::string& native_in, size_t limit, std::vector<std::string>& found)
{
	const std::string native = restore_home_spelling(native_in);
	found.clear();
	if (native.empty()) return;
	std::vector<std::string> comps;
	size_t pos = native[0] == '/' ? 1 : 0;
	while (pos <= native.size())
	{
		size_t next = native.find('/', pos);
		if (next == std::string::npos) next = native.size();
		if (next > pos) comps.push_back(native.substr(pos, next - pos));
		pos = next + 1;
	}
	if (limit == 1 && exists_exact(native))
	{
		found.push_back(native);
		return;
	}
	walk(comps, 0, native[0] == '/' ? "/" : "", limit, found);
}

// Resolves 'native' (already '/'-separated) component by component, ignoring case where the exact spelling does not
// exist. If a folder exists in several spellings ("Scripts" and "scripts", which only a case-sensitive volume allows) the
// one that holds the file is taken. Components that do not exist at all (files about to be created) are kept as given.
std::string resolve_case(const std::string& native_in)
{
	const std::string native = restore_home_spelling(native_in);
	if (native.empty() || exists_exact(native)) return native;

	std::vector<std::string> found;
	resolve_all(native, 1, found);
	if (!found.empty()) return found[0];

	std::string out;
	size_t pos = 0;
	if (native[0] == '/')
	{
		out = "/";
		pos = 1;
	}
	bool missing = false;
	while (pos <= native.size())
	{
		size_t next = native.find('/', pos);
		if (next == std::string::npos) next = native.size();
		std::string comp = native.substr(pos, next - pos);
		pos = next + 1;
		if (comp.empty()) continue;

		std::string candidate = out;
		if (!candidate.empty() && candidate.back() != '/') candidate += '/';
		if (!missing && comp != "." && comp != "..")
		{
			std::vector<std::string> names;
			spellings(out, comp, names);
			if (names.empty())
				missing = true;
			else
				comp = names[0];
		}
		out = candidate + comp;
	}
	return out;
}

std::string to_native(const char* virtual_path)
{
	std::string s(virtual_path ? virtual_path : "");
	for (char& c : s)
		if (c == '\\') c = '/';
	return resolve_case(s);
}

// _findfirst state. A folder that exists in several spellings is listed as one: the entries of all of them, with a
// subfolder (which the engine finds again by its lower-case path, and so by all spellings) returned once.
struct FindState
{
	struct Dir
	{
		DIR* dir;
		int fd;
	};
	std::vector<Dir> dirs;
	size_t current = 0;
	std::string pattern;
	std::unordered_set<std::string> subfolders;
};

bool match_all(const std::string& pattern) { return pattern == "*.*" || pattern == "*"; }
} // namespace

const char* xr_native_path(const char* virtual_path)
{
	thread_local std::string ring[8];
	thread_local unsigned next = 0;
	std::string& slot = ring[next++ & 7];
	slot = to_native(virtual_path);
	return slot.c_str();
}

void xr_virtual_path(const char* native_path, char* out, size_t out_size)
{
	if (!out || !out_size) return;
	size_t i = 0;
	for (; native_path && native_path[i] && i + 1 < out_size; ++i)
		out[i] = (native_path[i] == '/') ? '\\' : native_path[i];
	out[i] = 0;
}

int xr_mkdir_virtual(const char* virtual_path)
{
	const std::string native = to_native(virtual_path);
	int r = mkdir(native.c_str(), 0755);
	if (r != 0 && errno == EEXIST) return 0;
	return r;
}

int xr_stat(const char* virtual_path, struct stat* st) { return stat(xr_native_path(virtual_path), st); }

int xr_rename(const char* from, const char* to)
{
	const std::string a = to_native(from);
	const std::string b = to_native(to);
	return rename(a.c_str(), b.c_str());
}

FILE* xr_fopen(const char* virtual_path, const char* mode) { return fopen(xr_native_path(virtual_path), mode); }

int _open(const char* path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT)
	{
		va_list args;
		va_start(args, flags);
		mode = (mode_t)va_arg(args, int);
		va_end(args);
		if (!mode) mode = 0644;
	}
	return open(xr_native_path(path), flags, mode);
}

int _sopen_s(int* fd, const char* path, int flags, int /*shflag*/, int mode)
{
	*fd = open(xr_native_path(path), flags, (mode_t)(mode ? mode : 0644));
	return (*fd < 0) ? errno : 0;
}

int _read(int fd, void* buf, unsigned n)
{
	// MSVC returns the full count for regular files; POSIX read() may return less.
	unsigned total = 0;
	while (total < n)
	{
		ssize_t r = read(fd, (char*)buf + total, n - total);
		if (r < 0)
		{
			if (errno == EINTR) continue;
			return -1;
		}
		if (r == 0) break;
		total += (unsigned)r;
	}
	return (int)total;
}

int _utime(const char* path, const _utimbuf* times)
{
	struct utimbuf t;
	t.actime = times->actime;
	t.modtime = times->modtime;
	return utime(xr_native_path(path), &t);
}

DWORD GetFileAttributes(const char* path)
{
	struct stat st;
	if (stat(xr_native_path(path), &st) != 0) return INVALID_FILE_ATTRIBUTES;
	DWORD attr = 0;
	if (S_ISDIR(st.st_mode)) attr |= FILE_ATTRIBUTE_DIRECTORY;
	if (!(st.st_mode & S_IWUSR)) attr |= FILE_ATTRIBUTE_READONLY;
	return attr ? attr : FILE_ATTRIBUTE_NORMAL;
}

BOOL SetFileAttributes(const char* path, DWORD attr)
{
	const char* native = xr_native_path(path);
	struct stat st;
	if (stat(native, &st) != 0) return FALSE;
	mode_t mode = st.st_mode;
	if (attr & FILE_ATTRIBUTE_READONLY) mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
	else mode |= S_IWUSR;
	return chmod(native, mode) == 0;
}

// ---------------------------------------------------------------------------------------------
// Directory enumeration
// ---------------------------------------------------------------------------------------------
static bool next_entry(FindState* state, _finddata_t* data)
{
	while (state->current < state->dirs.size())
	{
		const FindState::Dir& dir = state->dirs[state->current];
		dirent* e = readdir(dir.dir);
		if (!e)
		{
			++state->current;
			continue;
		}
		if (!match_all(state->pattern) && fnmatch(state->pattern.c_str(), e->d_name, FNM_CASEFOLD) != 0) continue;

		struct stat st;
		if (fstatat(dir.fd, e->d_name, &st, 0) != 0)
		{
			// dangling symlink or a race with a delete: skip it
			continue;
		}
		const bool is_dot = strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0;
		if (S_ISDIR(st.st_mode) && !is_dot && !state->subfolders.insert(lower(e->d_name)).second) continue;
		memset(data, 0, sizeof(*data));
		strncpy(data->name, e->d_name, sizeof(data->name) - 1);
		data->size = (_fsize_t)st.st_size;
		data->time_write = st.st_mtime;
		data->time_create = st.st_ctime;
		data->time_access = st.st_atime;
		if (S_ISDIR(st.st_mode)) data->attrib |= _A_SUBDIR;
		if (!(st.st_mode & S_IWUSR)) data->attrib |= _A_RDONLY;
		// dot files (.DS_Store, AppleDouble "._x" files) play the role of Windows' hidden attribute
		if (e->d_name[0] == '.' && !is_dot)
			data->attrib |= _A_HIDDEN;
		return true;
	}
	return false;
}

intptr_t _findfirst(const char* pattern, _finddata_t* data)
{
	std::string p(pattern);
	size_t slash = p.find_last_of("\\/");
	std::string dir = (slash == std::string::npos) ? "." : p.substr(0, slash + 1);
	std::string mask = (slash == std::string::npos) ? p : p.substr(slash + 1);

	for (char& c : dir)
		if (c == '\\') c = '/';
	std::vector<std::string> paths;
	resolve_all(dir, 16, paths);
	if (paths.empty()) paths.push_back(to_native(dir.c_str()));

	FindState* state = new FindState;
	state->pattern = mask;
	for (const std::string& path : paths)
		if (DIR* d = opendir(path.c_str()))
			state->dirs.push_back({d, dirfd(d)});
	if (state->dirs.empty() || !next_entry(state, data))
	{
		for (const FindState::Dir& d : state->dirs)
			closedir(d.dir);
		delete state;
		errno = ENOENT;
		return -1;
	}
	return (intptr_t)state;
}

int _findnext(intptr_t handle, _finddata_t* data)
{
	return next_entry(reinterpret_cast<FindState*>(handle), data) ? 0 : -1;
}

int _findclose(intptr_t handle)
{
	FindState* state = reinterpret_cast<FindState*>(handle);
	for (const FindState::Dir& d : state->dirs)
		closedir(d.dir);
	delete state;
	return 0;
}

// ---------------------------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------------------------
void Sleep(DWORD ms)
{
	if (ms == 0)
	{
		sched_yield();
		return;
	}
	timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
	{
	}
}

#ifdef XR_PLATFORM_WEB
// clock_gettime_nsec_np does not exist; CLOCK_MONOTONIC is performance.now() (or process.hrtime) in Emscripten
static unsigned long long xr_monotonic_nsec()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
#else
static unsigned long long xr_monotonic_nsec() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
#endif

unsigned long long GetTickCount64() { return xr_monotonic_nsec() / 1000000ull; }
DWORD GetTickCount() { return (DWORD)GetTickCount64(); }

BOOL QueryPerformanceCounter(LARGE_INTEGER* counter)
{
	// nanoseconds; same clock as mach_absolute_time but already scaled
	counter->QuadPart = (LONGLONG)xr_monotonic_nsec();
	return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* freq)
{
	freq->QuadPart = 1000000000LL;
	return TRUE;
}

// ---------------------------------------------------------------------------------------------
// Process / thread / environment
// ---------------------------------------------------------------------------------------------
static std::string g_command_line_override;
static bool g_command_line_overridden = false;

void xr_set_command_line(const char* line)
{
	g_command_line_override = line ? line : "";
	g_command_line_overridden = true;
}

const char* GetCommandLine()
{
	if (g_command_line_overridden)
		return g_command_line_override.c_str();
	static std::string cmdline = []
	{
		std::string r;
#ifdef XR_PLATFORM_WEB
		// no process argv outside main(): the browser shell passes the line with xr_set_command_line(); under Node the
		// pre-js cmake/web/node_argv.js puts process.argv in $XR_COMMAND_LINE
		const char* env = getenv("XR_COMMAND_LINE");
		int argc = 0;
		char** argv = nullptr;
		if (env) r = env;
#else
		int argc = *_NSGetArgc();
		char** argv = *_NSGetArgv();
#endif
		for (int i = 0; i < argc; ++i)
		{
			if (i) r += ' ';
			const bool quote = strchr(argv[i], ' ') != nullptr;
			if (quote) r += '"';
			r += argv[i];
			if (quote) r += '"';
		}
		return r;
	}();
	return cmdline.c_str();
}

DWORD GetCurrentThreadId()
{
#ifdef XR_PLATFORM_WEB
	return (DWORD)(uintptr_t)pthread_self(); // the thread's control block address in the shared heap: unique per live thread
#else
	uint64_t tid = 0;
	pthread_threadid_np(nullptr, &tid);
	return (DWORD)tid;
#endif
}

#ifdef XR_PLATFORM_WEB
BOOL IsDebuggerPresent() { return FALSE; }
#else
BOOL IsDebuggerPresent()
{
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
	struct kinfo_proc info;
	size_t size = sizeof(info);
	memset(&info, 0, sizeof(info));
	if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return FALSE;
	return (info.kp_proc.p_flag & P_TRACED) != 0;
}
#endif

void DebugBreak()
{
	if (IsDebuggerPresent()) raise(SIGTRAP);
}

void OutputDebugString(const char* str)
{
	if (str) fputs(str, stderr);
}

DWORD GetModuleFileName(HMODULE /*module*/, char* buffer, DWORD size)
{
	char exe[1024];
#ifdef XR_PLATFORM_WEB
	// there is no executable on disk to ask for: <cwd>/anomaly, which keeps the "bin directory" next to the data the way the
	// desktop builds have it
	if (!getcwd(exe, sizeof(exe) - sizeof("/anomaly"))) return 0;
	strcat(exe, "/anomaly");
#else
	uint32_t exe_size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &exe_size) != 0) return 0;
#endif
	char real[1024];
	if (!realpath(exe, real)) strncpy(real, exe, sizeof(real) - 1);
	xr_virtual_path(real, buffer, size);
	return (DWORD)strlen(buffer);
}

DWORD GetCurrentDirectory(DWORD size, char* buffer)
{
	char cwd[1024];
	if (!getcwd(cwd, sizeof(cwd))) return 0;
	xr_virtual_path(cwd, buffer, size);
	return (DWORD)strlen(buffer);
}

BOOL GetUserName(char* buffer, DWORD* size)
{
	const char* name = nullptr;
	if (passwd* pw = getpwuid(getuid())) name = pw->pw_name;
	if (!name) name = "user";
	strncpy(buffer, name, *size - 1);
	buffer[*size - 1] = 0;
	*size = (DWORD)strlen(buffer) + 1;
	return TRUE;
}

BOOL GetComputerName(char* buffer, DWORD* size)
{
	if (gethostname(buffer, *size) != 0) strncpy(buffer, "localhost", *size - 1);
	buffer[*size - 1] = 0;
	// "Mac.local" -> "Mac"
	if (char* dot = strchr(buffer, '.')) *dot = 0;
	*size = (DWORD)strlen(buffer) + 1;
	return TRUE;
}

static xr_message_box_hook_t* g_message_box_hook = nullptr;

void xr_set_message_box_hook(xr_message_box_hook_t* hook) { g_message_box_hook = hook; }

int MessageBox(HWND, const char* text, const char* caption, unsigned type)
{
	fprintf(stderr, "[%s] %s\n", caption ? caption : "", text ? text : "");
	fflush(stderr);
	if (g_message_box_hook) g_message_box_hook(caption, text, type);
	return IDOK;
}

// ---------------------------------------------------------------------------------------------
// Memory queries (used by xrMemory, see memory_usage.cpp / Platform/xrMemory_apple.cpp)
// ---------------------------------------------------------------------------------------------
#ifndef XR_PLATFORM_WEB // the web versions of these three are in Platform/PlatformWeb.cpp
size_t xr_apple_malloc_size(const void* p) { return p ? malloc_size(p) : 0; }

size_t xr_apple_heap_bytes_in_use()
{
	malloc_statistics_t stats = {};
	malloc_zone_statistics(nullptr, &stats); // all zones
	return stats.size_in_use;
}

void xr_apple_vm_info(size_t* free_bytes, size_t* reserved, size_t* committed)
{
	// free: what the OS reports as available; committed: the process footprint; reserved: virtual size
	*free_bytes = *reserved = *committed = 0;
	task_vm_info_data_t info;
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
	{
		*committed = (size_t)info.phys_footprint;
		*reserved = (size_t)info.virtual_size;
	}
	int64_t physical = 0;
	size_t len = sizeof(physical);
	if (sysctlbyname("hw.memsize", &physical, &len, nullptr, 0) == 0 && (size_t)physical > *committed)
		*free_bytes = (size_t)physical - *committed;
}
#endif // !XR_PLATFORM_WEB

#ifndef XR_PLATFORM_WEB // mach, malloc zones and task_info: the web build gets no-op inline versions from XrMemStats.h
// ---------------------------------------------------------------------------------------------
// Memory statistics for docs/web-port/MEMORY.md: what a 32-bit WebAssembly heap would have to hold. Counters and a sampler
// thread only; nothing here changes what the engine does.
// ---------------------------------------------------------------------------------------------
namespace
{
std::atomic<long long> g_map_live[XR_MAP_SITE_COUNT];
std::atomic<long long> g_map_live_total{0};
std::atomic<long long> g_map_peak{0};
std::atomic<unsigned long long> g_map_count{0};
} // namespace

void xr_apple_map_note(int site, long long delta_bytes)
{
	if (site < 0 || site >= XR_MAP_SITE_COUNT)
		return;
	g_map_live[site].fetch_add(delta_bytes, std::memory_order_relaxed);
	const long long total = g_map_live_total.fetch_add(delta_bytes, std::memory_order_relaxed) + delta_bytes;
	if (delta_bytes > 0)
	{
		g_map_count.fetch_add(1, std::memory_order_relaxed);
		long long peak = g_map_peak.load(std::memory_order_relaxed);
		while (total > peak && !g_map_peak.compare_exchange_weak(peak, total, std::memory_order_relaxed))
		{
		}
	}
}

namespace
{
unsigned count_threads()
{
	thread_act_array_t list = nullptr;
	mach_msg_type_number_t n = 0;
	if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS)
		return 0;
	for (mach_msg_type_number_t i = 0; i < n; ++i)
		mach_port_deallocate(mach_task_self(), list[i]);
	vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_act_t));
	return n;
}

long long ll_max0(long long v) { return v > 0 ? v : 0; }

void read_mem_values(XrMemValues* v, bool with_threads)
{
	malloc_statistics_t ms = {};
	malloc_zone_statistics(nullptr, &ms); // all zones
	v->malloc_in_use = ms.size_in_use;
	v->malloc_allocated = ms.size_allocated;
	v->malloc_blocks = ms.blocks_in_use;
	v->malloc_max_in_use = ms.max_size_in_use;
	task_vm_info_data_t info;
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
	{
		v->footprint = info.phys_footprint;
		v->internal = info.internal;
		v->compressed = info.compressed;
		v->resident = info.resident_size;
	}
	v->map_total = 0;
	for (int i = 0; i < XR_MAP_SITE_COUNT; ++i)
	{
		v->map_live[i] = (uint64_t)ll_max0(g_map_live[i].load(std::memory_order_relaxed));
		v->map_total += v->map_live[i];
	}
	v->map_peak = (uint64_t)ll_max0(g_map_peak.load(std::memory_order_relaxed));
	v->map_count = g_map_count.load(std::memory_order_relaxed);
	if (with_threads)
		v->threads = count_threads();
}

// One row of the CSV: the fixed columns, then the probe's.
struct MemSampler
{
	std::thread thread;
	std::mutex mutex; // csv, columns and peaks
	std::condition_variable cv;
	bool stop = false;
	bool running = false;
	FILE* csv = nullptr;
	XrMemProbe probe = nullptr;
	XrMemLog log = nullptr;
	unsigned interval_ms = 100;
	std::chrono::steady_clock::time_point t0;
	std::chrono::steady_clock::time_point last_log;
	std::chrono::steady_clock::time_point last_flush;
	std::vector<std::string> names;
	std::vector<uint64_t> cur, peak;
	std::vector<double> peak_t;
	bool header_written = false;
	unsigned threads = 0;

	double now_s() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

	static std::string mb(uint64_t v)
	{
		char b[32];
		snprintf(b, sizeof b, "%.0f", double(v) / 1048576.0);
		return b;
	}

	// Takes one sample; the caller holds the mutex.
	void sample_locked()
	{
		XrMemValues v;
		read_mem_values(&v, true);
		threads = v.threads;
		XrMemExtra extra[XR_MEM_EXTRA_MAX];
		const int ne = probe ? std::min(probe(extra), int(XR_MEM_EXTRA_MAX)) : 0;
		const size_t ncols = 15 + (size_t)ne;
		if (names.size() != ncols)
		{
			names = {"malloc_in_use", "malloc_allocated", "malloc_blocks", "malloc_max_in_use", "footprint", "internal", "compressed",
			         "resident", "map_archive", "map_stream", "map_loose_read", "map_loose_rw", "map_total", "map_peak", "threads"};
			for (int i = 0; i < ne; ++i)
				names.push_back(extra[i].name);
			cur.assign(ncols, 0);
			if (peak.size() < ncols)
			{
				peak.resize(ncols, 0);
				peak_t.resize(ncols, 0.0);
			}
		}
		uint64_t* c = cur.data();
		c[0] = v.malloc_in_use; c[1] = v.malloc_allocated; c[2] = v.malloc_blocks; c[3] = v.malloc_max_in_use;
		c[4] = v.footprint; c[5] = v.internal; c[6] = v.compressed; c[7] = v.resident;
		c[8] = v.map_live[XR_MAP_ARCHIVE]; c[9] = v.map_live[XR_MAP_STREAM]; c[10] = v.map_live[XR_MAP_LOOSE_READ];
		c[11] = v.map_live[XR_MAP_LOOSE_RW]; c[12] = v.map_total; c[13] = v.map_peak; c[14] = v.threads;
		for (int i = 0; i < ne; ++i)
			c[15 + i] = extra[i].value;
		const double t = now_s();
		for (size_t i = 0; i < ncols; ++i)
			if (c[i] > peak[i])
			{
				peak[i] = c[i];
				peak_t[i] = t;
			}
		if (csv)
		{
			if (!header_written)
			{
				fprintf(csv, "t_s");
				for (const std::string& n : names)
					fprintf(csv, ",%s", n.c_str());
				fprintf(csv, "\n");
				header_written = true;
			}
			fprintf(csv, "%.3f", t);
			for (size_t i = 0; i < ncols; ++i)
				fprintf(csv, ",%llu", (unsigned long long)c[i]);
			fprintf(csv, "\n");
			const auto now = std::chrono::steady_clock::now();
			if (now - last_flush >= std::chrono::seconds(1))
			{
				fflush(csv);
				last_flush = now;
			}
		}
	}

	// A line of the current values with the peaks in brackets; the caller holds the mutex.
	void log_locked(const char* tag)
	{
		if (!log || cur.empty())
			return;
		std::string s = std::string("anomaly: mem: ") + tag + " t=" + std::to_string((long long)now_s()) + "s";
		static const char* const shown[] = {"malloc_in_use", "footprint", "map_total", "map_peak"};
		auto add = [&](size_t i) {
			s += std::string(" ") + names[i] + " " + mb(cur[i]) + " (peak " + mb(peak[i]) + ")";
		};
		for (size_t i = 0; i < names.size(); ++i)
		{
			bool fixed_shown = false;
			for (const char* n : shown)
				fixed_shown |= names[i] == n;
			if (fixed_shown || i >= 15)
				add(i);
		}
		s += " MB, threads " + std::to_string(cur[14]);
		log(s.c_str());
	}

	void run()
	{
		std::unique_lock<std::mutex> lk(mutex);
		while (!stop)
		{
			sample_locked();
			const auto now = std::chrono::steady_clock::now();
			if (now - last_log >= std::chrono::seconds(10))
			{
				last_log = now;
				log_locked("sample");
			}
			cv.wait_for(lk, std::chrono::milliseconds(interval_ms), [&] { return stop; });
		}
		sample_locked();
		log_locked("final");
		if (csv)
		{
			fclose(csv);
			csv = nullptr;
		}
	}
};

MemSampler* g_mem_sampler = nullptr;
} // namespace

void xr_apple_memstat_start(const char* csv_path, unsigned interval_ms, XrMemProbe probe, XrMemLog log)
{
	if (g_mem_sampler)
		return;
	MemSampler* s = new MemSampler();
	s->probe = probe;
	s->log = log;
	s->interval_ms = interval_ms ? interval_ms : 100;
	s->t0 = s->last_log = s->last_flush = std::chrono::steady_clock::now();
	if (csv_path && *csv_path)
		s->csv = fopen(csv_path, "w");
	s->running = true;
	g_mem_sampler = s;
	s->thread = std::thread([s] { s->run(); });
}

void xr_apple_memstat_stop()
{
	MemSampler* s = g_mem_sampler;
	if (!s || !s->running)
		return;
	{
		std::lock_guard<std::mutex> lk(s->mutex);
		s->stop = true;
	}
	s->cv.notify_all();
	if (s->thread.joinable())
		s->thread.join();
	s->running = false;
	// the sampler object stays (marks after stop are ignored); a few hundred bytes
}

bool xr_apple_memstat_running() { return g_mem_sampler && g_mem_sampler->running; }

void xr_apple_memstat_now(XrMemValues* out) { read_mem_values(out, true); }

void xr_apple_memstat_mark(const char* label)
{
	MemSampler* s = g_mem_sampler;
	if (!s || !s->running)
		return;
	std::lock_guard<std::mutex> lk(s->mutex);
	s->sample_locked();
	if (s->csv)
		fprintf(s->csv, "#mark,%.3f,%s\n", s->now_s(), label ? label : "");
	if (s->log)
	{
		char b[256];
		snprintf(b, sizeof b, "anomaly: mem: mark %s", label ? label : "");
		s->log(b);
	}
	s->log_locked("mark");
}
#endif // !XR_PLATFORM_WEB

// ---------------------------------------------------------------------------------------------
// Code pages
// ---------------------------------------------------------------------------------------------
namespace
{
// Windows-1251 0x80..0xBF -> Unicode (0xC0..0xFF map to U+0410..U+044F)
const unsigned short cp1251_high[64] = {
	0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
	0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
	0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
	0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};

wchar_t cp1251_to_unicode(unsigned char c)
{
	if (c < 0x80) return c;
	if (c < 0xC0) return cp1251_high[c - 0x80];
	return (wchar_t)(0x0410 + (c - 0xC0));
}

bool unicode_to_cp1251(unsigned cp, unsigned char& out)
{
	if (cp < 0x80)
	{
		out = (unsigned char)cp;
		return true;
	}
	if (cp >= 0x0410 && cp <= 0x044F)
	{
		out = (unsigned char)(0xC0 + (cp - 0x0410));
		return true;
	}
	for (int i = 0; i < 64; ++i)
		if (cp1251_high[i] == cp)
		{
			out = (unsigned char)(0x80 + i);
			return true;
		}
	return false;
}

// Decodes one code point; returns the number of bytes consumed (0 on invalid input).
int utf8_decode(const unsigned char* s, size_t avail, unsigned& cp)
{
	if (avail < 1) return 0;
	if (s[0] < 0x80)
	{
		cp = s[0];
		return 1;
	}
	int n = ((s[0] & 0xE0) == 0xC0) ? 2 : ((s[0] & 0xF0) == 0xE0) ? 3 : ((s[0] & 0xF8) == 0xF0) ? 4 : 0;
	if (!n || (size_t)n > avail) return 0;
	cp = s[0] & (0xFF >> (n + 1));
	for (int i = 1; i < n; ++i)
	{
		if ((s[i] & 0xC0) != 0x80) return 0;
		cp = (cp << 6) | (s[i] & 0x3F);
	}
	return n;
}

int utf8_encode(unsigned cp, char* out)
{
	if (cp < 0x80)
	{
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800)
	{
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000)
	{
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}
} // namespace

int MultiByteToWideChar(UINT code_page, DWORD, const char* src, int src_len, wchar_t* dst, int dst_len)
{
	if (src_len < 0) src_len = (int)strlen(src) + 1;
	int count = 0;
	const unsigned char* s = (const unsigned char*)src;
	for (int i = 0; i < src_len;)
	{
		unsigned cp;
		if (code_page == CP_UTF8)
		{
			int n = utf8_decode(s + i, (size_t)(src_len - i), cp);
			if (!n) return 0;
			i += n;
		}
		else
		{
			cp = (unsigned)cp1251_to_unicode(s[i]); // CP_ACP is treated as 1251, the game's code page
			++i;
		}
		if (dst)
		{
			if (count >= dst_len) return 0;
			dst[count] = (wchar_t)cp;
		}
		++count;
	}
	return count;
}

int WideCharToMultiByte(UINT code_page, DWORD, const wchar_t* src, int src_len, char* dst, int dst_len,
                        const char* default_char, BOOL* used_default)
{
	if (src_len < 0) src_len = (int)wcslen(src) + 1;
	if (used_default) *used_default = FALSE;
	int count = 0;
	for (int i = 0; i < src_len; ++i)
	{
		char tmp[4];
		int n;
		unsigned cp = (unsigned)src[i];
		if (code_page == CP_UTF8)
			n = utf8_encode(cp, tmp);
		else
		{
			unsigned char c;
			if (unicode_to_cp1251(cp, c)) tmp[0] = (char)c;
			else
			{
				tmp[0] = default_char ? default_char[0] : '?';
				if (used_default) *used_default = TRUE;
			}
			n = 1;
		}
		if (dst)
		{
			if (count + n > dst_len) return 0;
			memcpy(dst + count, tmp, (size_t)n);
		}
		count += n;
	}
	return count;
}

// ---------------------------------------------------------------------------------------------
// Handle objects: events, mutexes, semaphores and threads, with CloseHandle / WaitForSingleObject semantics
// ---------------------------------------------------------------------------------------------
namespace
{
enum class HandleKind : int
{
	Event,
	Mutex,
	Semaphore,
	Thread
};

struct HandleObject
{
	HandleKind kind;
	std::mutex lock;
	std::condition_variable cond;
	// event
	bool manual_reset = false;
	bool signaled = false;
	// mutex (recursive, owner tracked by thread id)
	pthread_t owner = pthread_t(); // (an opaque pointer on Apple, an integer on Emscripten)
	int recursion = 0;
	// semaphore
	LONG count = 0;
	LONG maximum = 0;
	// thread
	pthread_t thread = pthread_t();
	bool finished = false;
	bool detached = false;
	DWORD exit_code = STILL_ACTIVE;
	DWORD thread_id = 0;
	LPTHREAD_START_ROUTINE start = nullptr;
	void* parameter = nullptr;
	void (*cdecl_start)(void*) = nullptr;
	unsigned (*ex_start)(void*) = nullptr;
	int priority = 0;
	std::atomic<int> refs{1}; // handle + running thread
};

HandleObject* as_object(HANDLE h)
{
	if (!h || h == INVALID_HANDLE_VALUE) return nullptr;
	return static_cast<HandleObject*>(h);
}

void release(HandleObject* o)
{
	if (o->refs.fetch_sub(1) == 1) delete o;
}

// Waits on one object until `ready()` accepts it (under its lock); takes ownership semantics in `acquire`.
bool try_acquire(HandleObject* o)
{
	switch (o->kind)
	{
	case HandleKind::Event:
		if (!o->signaled) return false;
		if (!o->manual_reset) o->signaled = false;
		return true;
	case HandleKind::Mutex:
		if (o->recursion == 0 || pthread_equal(o->owner, pthread_self()))
		{
			o->owner = pthread_self();
			++o->recursion;
			return true;
		}
		return false;
	case HandleKind::Semaphore:
		if (o->count <= 0) return false;
		--o->count;
		return true;
	case HandleKind::Thread:
		return o->finished;
	}
	return false;
}

DWORD wait_one(HandleObject* o, DWORD ms)
{
	std::unique_lock<std::mutex> guard(o->lock);
	if (ms == INFINITE)
	{
		o->cond.wait(guard, [&] { return try_acquire(o); });
		return WAIT_OBJECT_0;
	}
	return o->cond.wait_for(guard, std::chrono::milliseconds(ms), [&] { return try_acquire(o); }) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

void* thread_main(void* arg)
{
	HandleObject* o = static_cast<HandleObject*>(arg);
	DWORD code = 0;
	if (o->start) code = o->start(o->parameter);
	else if (o->ex_start) code = (DWORD)o->ex_start(o->parameter);
	else if (o->cdecl_start) o->cdecl_start(o->parameter);
	{
		std::lock_guard<std::mutex> guard(o->lock);
		o->exit_code = code;
		o->finished = true;
	}
	o->cond.notify_all();
	release(o);
	return nullptr;
}

HandleObject* spawn_thread(LPTHREAD_START_ROUTINE start, unsigned (*ex_start)(void*), void (*cdecl_start)(void*), void* arg,
                           size_t stack_size, bool detached)
{
	HandleObject* o = new HandleObject;
	o->kind = HandleKind::Thread;
	o->start = start;
	o->ex_start = ex_start;
	o->cdecl_start = cdecl_start;
	o->parameter = arg;
	o->detached = detached;
	o->refs = 2; // the returned handle and the thread itself
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	if (stack_size) pthread_attr_setstacksize(&attr, stack_size < 16384 ? 16384 : stack_size);
	if (pthread_create(&o->thread, &attr, thread_main, o) != 0)
	{
		pthread_attr_destroy(&attr);
		delete o;
		return nullptr;
	}
	pthread_attr_destroy(&attr);
	return o;
}
} // namespace

HANDLE CreateEvent(void*, BOOL manual_reset, BOOL initial_state, const char*)
{
	HandleObject* o = new HandleObject;
	o->kind = HandleKind::Event;
	o->manual_reset = manual_reset != 0;
	o->signaled = initial_state != 0;
	return o;
}

BOOL SetEvent(HANDLE event)
{
	HandleObject* o = as_object(event);
	if (!o || o->kind != HandleKind::Event) return FALSE;
	{
		std::lock_guard<std::mutex> guard(o->lock);
		o->signaled = true;
	}
	o->cond.notify_all();
	return TRUE;
}

BOOL ResetEvent(HANDLE event)
{
	HandleObject* o = as_object(event);
	if (!o || o->kind != HandleKind::Event) return FALSE;
	std::lock_guard<std::mutex> guard(o->lock);
	o->signaled = false;
	return TRUE;
}

HANDLE CreateMutex(void*, BOOL initial_owner, const char*)
{
	HandleObject* o = new HandleObject;
	o->kind = HandleKind::Mutex;
	if (initial_owner)
	{
		o->owner = pthread_self();
		o->recursion = 1;
	}
	return o;
}

BOOL ReleaseMutex(HANDLE mutex)
{
	HandleObject* o = as_object(mutex);
	if (!o || o->kind != HandleKind::Mutex) return FALSE;
	{
		std::lock_guard<std::mutex> guard(o->lock);
		if (o->recursion == 0 || !pthread_equal(o->owner, pthread_self())) return FALSE;
		--o->recursion;
	}
	o->cond.notify_one();
	return TRUE;
}

HANDLE CreateSemaphore(void*, LONG initial_count, LONG maximum_count, const char*)
{
	HandleObject* o = new HandleObject;
	o->kind = HandleKind::Semaphore;
	o->count = initial_count;
	o->maximum = maximum_count;
	return o;
}

BOOL ReleaseSemaphore(HANDLE semaphore, LONG release_count, LONG* previous_count)
{
	HandleObject* o = as_object(semaphore);
	if (!o || o->kind != HandleKind::Semaphore) return FALSE;
	{
		std::lock_guard<std::mutex> guard(o->lock);
		if (previous_count) *previous_count = o->count;
		if (o->count + release_count > o->maximum) return FALSE;
		o->count += release_count;
	}
	o->cond.notify_all();
	return TRUE;
}

DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds)
{
	HandleObject* o = as_object(handle);
	if (!o) return WAIT_FAILED;
	return wait_one(o, milliseconds);
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD milliseconds)
{
	// Polling is good enough for the handful of call sites (multiplayer sync, tools).
	const uint64_t start = xr_monotonic_nsec();
	for (;;)
	{
		DWORD ready = 0;
		for (DWORD i = 0; i < count; ++i)
		{
			HandleObject* o = as_object(handles[i]);
			if (!o) return WAIT_FAILED;
			if (!wait_all)
			{
				if (wait_one(o, 0) == WAIT_OBJECT_0) return WAIT_OBJECT_0 + i;
			}
			else
			{
				std::lock_guard<std::mutex> guard(o->lock);
				bool signaled = (o->kind == HandleKind::Event) ? o->signaled : (o->kind == HandleKind::Thread) ? o->finished : o->count > 0;
				if (signaled) ++ready;
			}
		}
		if (wait_all && ready == count)
		{
			for (DWORD i = 0; i < count; ++i) wait_one(as_object(handles[i]), 0);
			return WAIT_OBJECT_0;
		}
		if (milliseconds != INFINITE && (xr_monotonic_nsec() - start) / 1000000ull >= milliseconds)
			return WAIT_TIMEOUT;
		usleep(500);
	}
}

BOOL CloseHandle(HANDLE handle)
{
	HandleObject* o = as_object(handle);
	if (!o) return FALSE;
	if (o->kind == HandleKind::Thread && !o->detached)
	{
		std::unique_lock<std::mutex> guard(o->lock);
		if (!o->finished) pthread_detach(o->thread);
	}
	release(o);
	return TRUE;
}

HANDLE CreateThread(void*, size_t stack_size, LPTHREAD_START_ROUTINE start, void* parameter, DWORD, DWORD* thread_id)
{
	HandleObject* o = spawn_thread(start, nullptr, nullptr, parameter, stack_size, false);
	if (o && thread_id) *thread_id = (DWORD)(uintptr_t)o->thread;
	return o;
}

uintptr_t _beginthread(void (*start)(void*), unsigned stack_size, void* arg)
{
	HandleObject* o = spawn_thread(nullptr, nullptr, start, arg, stack_size, true);
	if (!o) return (uintptr_t)-1;
	pthread_detach(o->thread);
	return (uintptr_t)o;
}

uintptr_t _beginthreadex(void*, unsigned stack_size, unsigned (*start)(void*), void* arg, unsigned, unsigned* thread_id)
{
	HandleObject* o = spawn_thread(nullptr, start, nullptr, arg, stack_size, false);
	if (!o) return 0;
	if (thread_id) *thread_id = (unsigned)(uintptr_t)o->thread;
	return (uintptr_t)o;
}

HANDLE GetCurrentThread() { return (HANDLE)(intptr_t)-2; }

BOOL GetExitCodeThread(HANDLE thread, DWORD* exit_code)
{
	HandleObject* o = as_object(thread);
	if (!o || o->kind != HandleKind::Thread) return FALSE;
	std::lock_guard<std::mutex> guard(o->lock);
	*exit_code = o->finished ? o->exit_code : STILL_ACTIVE;
	return TRUE;
}

BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
int GetThreadPriority(HANDLE) { return THREAD_PRIORITY_NORMAL; }
DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR mask) { return mask; } // no affinity on Apple platforms
DWORD SetThreadIdealProcessor(HANDLE, DWORD) { return 0; }

// ---------------------------------------------------------------------------------------------
// Time of day, system information, environment, dynamic libraries
// ---------------------------------------------------------------------------------------------
static void fill_systemtime(SYSTEMTIME* st, const tm& t, int milliseconds)
{
	st->wYear = (WORD)(t.tm_year + 1900);
	st->wMonth = (WORD)(t.tm_mon + 1);
	st->wDayOfWeek = (WORD)t.tm_wday;
	st->wDay = (WORD)t.tm_mday;
	st->wHour = (WORD)t.tm_hour;
	st->wMinute = (WORD)t.tm_min;
	st->wSecond = (WORD)t.tm_sec;
	st->wMilliseconds = (WORD)milliseconds;
}

void GetLocalTime(SYSTEMTIME* st)
{
	timeval tv;
	gettimeofday(&tv, nullptr);
	time_t seconds = tv.tv_sec;
	tm t;
	localtime_r(&seconds, &t);
	fill_systemtime(st, t, (int)(tv.tv_usec / 1000));
}

void GetSystemTime(SYSTEMTIME* st)
{
	timeval tv;
	gettimeofday(&tv, nullptr);
	time_t seconds = tv.tv_sec;
	tm t;
	gmtime_r(&seconds, &t);
	fill_systemtime(st, t, (int)(tv.tv_usec / 1000));
}

void GetSystemInfo(SYSTEM_INFO* info)
{
	memset(info, 0, sizeof(*info));
	int cpus = 1;
#ifdef XR_PLATFORM_WEB
	cpus = (int)sysconf(_SC_NPROCESSORS_ONLN); // navigator.hardwareConcurrency / os.cpus().length
	if (cpus < 1) cpus = 1;
	info->wProcessorArchitecture = 0xFFFF; // PROCESSOR_ARCHITECTURE_UNKNOWN: WebAssembly is none of the Windows ones
#else
	size_t size = sizeof(cpus);
	sysctlbyname("hw.logicalcpu", &cpus, &size, nullptr, 0);
	info->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
#endif
	info->dwPageSize = (DWORD)getpagesize();
	info->dwAllocationGranularity = info->dwPageSize;
	info->dwNumberOfProcessors = (DWORD)cpus;
	info->dwActiveProcessorMask = cpus >= 64 ? ~0ull : ((1ull << cpus) - 1);
}

BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* status)
{
	uint64_t total = 0;
#ifdef XR_PLATFORM_WEB
	total = (uint64_t)emscripten_get_heap_max(); // the heap ceiling (-sMAXIMUM_MEMORY): all the memory the module can ever have
#else
	size_t size = sizeof(total);
	sysctlbyname("hw.memsize", &total, &size, nullptr, 0);
#endif
	size_t free_bytes = 0, reserved = 0, committed = 0;
	xr_apple_vm_info(&free_bytes, &reserved, &committed);
	memset(status, 0, sizeof(*status));
	status->dwLength = sizeof(*status);
	status->ullTotalPhys = total;
	status->ullAvailPhys = free_bytes;
	status->ullTotalVirtual = total;
	status->ullAvailVirtual = free_bytes;
	status->ullTotalPageFile = total;
	status->ullAvailPageFile = free_bytes;
	status->dwMemoryLoad = total ? (DWORD)(100 - (free_bytes * 100 / total)) : 0;
	return TRUE;
}

DWORD GetEnvironmentVariable(const char* name, char* buffer, DWORD size)
{
	const char* value = getenv(name);
	if (!value) return 0;
	DWORD needed = (DWORD)strlen(value);
	if (needed + 1 > size) return needed + 1;
	memcpy(buffer, value, needed + 1);
	return needed;
}

HMODULE LoadLibrary(const char*) { return nullptr; } // Windows module names; nothing loads on Apple platforms
BOOL FreeLibrary(HMODULE) { return TRUE; }
FARPROC GetProcAddress(HMODULE, const char*) { return nullptr; }

HINSTANCE ShellExecute(HWND, const char*, const char* file, const char*, const char*, int)
{
	fprintf(stderr, "ShellExecute(%s) is not supported on this platform\n", file ? file : "");
	return (HINSTANCE)(intptr_t)2; // ERROR_FILE_NOT_FOUND: values <= 32 are failures
}

// ---------------------------------------------------------------------------------------------
// sscanf_s: drops the size argument that follows %s, %c and %[ conversions
// ---------------------------------------------------------------------------------------------
int xr_sscanf_s(const char* buffer, const char* format, ...)
{
	void* args[16] = {};
	int n = 0;
	va_list va;
	va_start(va, format);
	for (const char* p = format; *p && n < 16; ++p)
	{
		if (*p != '%') continue;
		++p;
		if (*p == '%') continue;
		bool suppressed = false;
		if (*p == '*')
		{
			suppressed = true;
			++p;
		}
		while (*p >= '0' && *p <= '9') ++p;
		while (*p == 'h' || *p == 'l' || *p == 'L' || *p == 'q' || *p == 'j' || *p == 'z' || *p == 't') ++p;
		if (!*p) break;
		const char conv = *p;
		if (conv == '[')
		{
			++p;
			if (*p == '^') ++p;
			if (*p == ']') ++p;
			while (*p && *p != ']') ++p;
		}
		if (suppressed) continue;
		args[n++] = va_arg(va, void*);
		if (conv == 's' || conv == 'c' || conv == '[') (void)va_arg(va, unsigned int);
	}
	va_end(va);
	return sscanf(buffer, format, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8],
	              args[9], args[10], args[11], args[12], args[13], args[14], args[15]);
}

#endif // XR_PLATFORM_APPLE
