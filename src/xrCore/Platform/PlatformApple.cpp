// Implementation of the Win32/CRT compatibility layer declared in src/Common/PlatformApple.inl (macOS and iOS).
//
// Deliberately independent of the rest of xrCore: no stdafx.h, so it can be used (and tested) on its own and does not
// pull in xrCore's `thread_t` next to the Mach `thread_t`.
#include "Common/Platform.h"

#ifdef XR_PLATFORM_APPLE

#include <crt_externs.h>
#include <fnmatch.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <pwd.h>
#include <sys/sysctl.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>


// ---------------------------------------------------------------------------------------------
// Paths: virtual ('\\', any case) <-> native
// ---------------------------------------------------------------------------------------------
namespace
{
std::mutex g_dir_cache_lock;
// native directory path -> (lower-cased entry name -> real entry name)
std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_dir_cache;

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

// Looks 'name' up in directory 'dir', ignoring case. 'rescan' forces a fresh listing.
bool find_entry(const std::string& dir, const std::string& name, bool rescan, std::string& real)
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
				entries.emplace(lower(e->d_name), e->d_name);
			closedir(d);
		}
		it = g_dir_cache.find(dir);
	}
	auto f = it->second.find(key);
	if (f == it->second.end()) return false;
	real = f->second;
	return true;
}

// Resolves 'native' (already '/'-separated) component by component, ignoring case where the exact spelling does not
// exist. Components that do not exist at all (files about to be created) are kept as given.
std::string resolve_case(const std::string& native)
{
	if (native.empty() || exists_exact(native)) return native;

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
			std::string real;
			if (exists_exact(candidate + comp))
				real = comp;
			else if (!find_entry(out, comp, false, real) && !find_entry(out, comp, true, real))
				missing = true;
			if (!missing) comp = real;
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

// _findfirst state
struct FindState
{
	DIR* dir;
	std::string pattern;
	int dir_fd;
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
	while (dirent* e = readdir(state->dir))
	{
		if (!match_all(state->pattern) && fnmatch(state->pattern.c_str(), e->d_name, FNM_CASEFOLD) != 0) continue;

		struct stat st;
		if (fstatat(state->dir_fd, e->d_name, &st, 0) != 0)
		{
			// dangling symlink or a race with a delete: skip it
			continue;
		}
		memset(data, 0, sizeof(*data));
		strncpy(data->name, e->d_name, sizeof(data->name) - 1);
		data->size = (_fsize_t)st.st_size;
		data->time_write = st.st_mtime;
		data->time_create = st.st_ctime;
		data->time_access = st.st_atime;
		if (S_ISDIR(st.st_mode)) data->attrib |= _A_SUBDIR;
		if (!(st.st_mode & S_IWUSR)) data->attrib |= _A_RDONLY;
		// dot files (.DS_Store, AppleDouble "._x" files) play the role of Windows' hidden attribute
		if (e->d_name[0] == '.' && strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
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

	DIR* d = opendir(to_native(dir.c_str()).c_str());
	if (!d)
	{
		errno = ENOENT;
		return -1;
	}
	FindState* state = new FindState{d, mask, dirfd(d)};
	if (!next_entry(state, data))
	{
		closedir(d);
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
	closedir(state->dir);
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

unsigned long long GetTickCount64() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull; }
DWORD GetTickCount() { return (DWORD)GetTickCount64(); }

BOOL QueryPerformanceCounter(LARGE_INTEGER* counter)
{
	// nanoseconds; same clock as mach_absolute_time but already scaled
	counter->QuadPart = (LONGLONG)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
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
const char* GetCommandLine()
{
	static std::string cmdline = []
	{
		std::string r;
		int argc = *_NSGetArgc();
		char** argv = *_NSGetArgv();
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
	uint64_t tid = 0;
	pthread_threadid_np(nullptr, &tid);
	return (DWORD)tid;
}

BOOL IsDebuggerPresent()
{
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
	struct kinfo_proc info;
	size_t size = sizeof(info);
	memset(&info, 0, sizeof(info));
	if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return FALSE;
	return (info.kp_proc.p_flag & P_TRACED) != 0;
}

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
	uint32_t exe_size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &exe_size) != 0) return 0;
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

#endif // XR_PLATFORM_APPLE
