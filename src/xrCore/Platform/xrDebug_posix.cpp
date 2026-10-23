// xrDebug for Apple platforms: assertions, fatal errors and crash reporting without SEH, DbgHelp, StackWalker,
// BugTrap or the BlackBox module. Replaces xrDebugNew.cpp (Windows build; unchanged).
//
//  * R_ASSERT / FATAL / VERIFY failures go through xrDebug::backend: logged with the same text layout as on Windows,
//    with a symbolicated backtrace, shown through MessageBox() (a hook the window layer can turn into a real dialog),
//    then the process exits with status 1.
//  * Fatal signals (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) run on an alternate stack, write a crash report next to
//    the log and re-raise with the default disposition, so the OS crash reporter still sees the original fault.
//  * std::terminate and operator new failures are routed to the same path.
#include "../stdafx.h"
#pragma hdrstop

#ifdef XR_PLATFORM_APPLE

#include "../xrDebug.h"
#include "../os_clipboard.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <new>
#include <atomic>
#include <exception>

extern bool shared_str_initialized;
extern LPCSTR log_name();
// Implemented by the script layer (xrEngine/xrGame); xrCore_defaults_apple.cpp has a no-op for programs without it.
extern void printLuaStack();

XRCORE_API xrDebug Debug;

namespace crash_saving
{
void (*save_impl)() = nullptr;
BOOL enabled = TRUE;

void save()
{
	if (enabled && save_impl != nullptr)
		(*save_impl)();
}
} // namespace crash_saving

XRCORE_API string_path g_bug_report_file;
typedef void (*full_memory_stats_callback_type)();
XRCORE_API full_memory_stats_callback_type g_full_memory_stats_callback = 0;

bool ignore_verify = true;

// ---------------------------------------------------------------------------------------------
// Backtraces
// ---------------------------------------------------------------------------------------------
namespace
{
const int MAX_FRAMES = 64;

// "  #3  0x0000000104a3c1f0 xrCore`CLocatorAPI::r_open(char const*, char const*) + 112"
void format_frame(int index, void* address, char* out, size_t size)
{
	Dl_info info;
	if (dladdr(address, &info) && info.dli_sname)
	{
		int status = 0;
		char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
		const char* image = info.dli_fname ? strrchr(info.dli_fname, '/') : nullptr;
		image = image ? image + 1 : (info.dli_fname ? info.dli_fname : "?");
		snprintf(out, size, "  #%-2d %p %s`%s + %td", index, address, image, status == 0 ? demangled : info.dli_sname,
		         (char*)address - (char*)info.dli_saddr);
		free(demangled);
	}
	else
	{
		snprintf(out, size, "  #%-2d %p", index, address);
	}
}

// Writes the backtrace of the calling thread into 'out' (not async-signal-safe: uses dladdr / demangling).
void backtrace_to_string(std::string& out, int skip)
{
	void* frames[MAX_FRAMES];
	int count = backtrace(frames, MAX_FRAMES);
	char line[1024];
	for (int i = skip; i < count; ++i)
	{
		format_frame(i - skip, frames[i], line, sizeof(line));
		out += line;
		out += '\n';
	}
}
} // namespace

void LogStackTrace(LPCSTR header, bool printStack)
{
	if (!shared_str_initialized)
		return;

	if (header)
		Msg("%s", header);

	if (printStack)
	{
		printLuaStack();
		Msg("\n");
		std::string trace;
		backtrace_to_string(trace, 2);
		// one Msg per line so the log keeps its timestamps / marks
		size_t pos = 0;
		while (pos < trace.size())
		{
			size_t eol = trace.find('\n', pos);
			if (eol == std::string::npos) eol = trace.size();
			Msg("%s", trace.substr(pos, eol - pos).c_str());
			pos = eol + 1;
		}
	}
}

void LogStackTrace(LPCSTR header) { LogStackTrace(header, false); }

// ---------------------------------------------------------------------------------------------
// xrDebug
// ---------------------------------------------------------------------------------------------
void xrDebug::gather_info(const char* expression, const char* description, const char* argument0, const char* argument1,
                          const char* file, int line, const char* function, LPSTR assertion_info,
                          u32 const assertion_info_size)
{
	if (!expression)
		expression = "<no expression>";
	LPSTR buffer_base = assertion_info;
	LPSTR buffer = assertion_info;
	int assertion_size = (int)assertion_info_size;
	LPCSTR endline = "\n";
	LPCSTR prefix = "[error]";
	bool extended_description = (description && !argument0 && strchr(description, '\n'));
	for (int i = 0; i < 2; ++i)
	{
		if (!i)
			buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sFATAL ERROR%s%s", endline,
			                     endline, endline);
		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sExpression    : %s%s", prefix,
		                     expression, endline);
		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sFunction      : %s%s", prefix,
		                     function, endline);
		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sFile          : %s%s", prefix, file,
		                     endline);
		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sLine          : %d%s", prefix, line,
		                     endline);

		if (extended_description)
		{
			buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s%s%s", endline, description,
			                     endline);
			if (argument0)
			{
				if (argument1)
				{
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s%s", argument0,
					                     endline);
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s%s", argument1,
					                     endline);
				}
				else
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s%s", argument0,
					                     endline);
			}
		}
		else
		{
			buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sDescription   : %s%s", prefix,
			                     description, endline);
			if (argument0)
			{
				if (argument1)
				{
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sArgument 0    : %s%s",
					                     prefix, argument0, endline);
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sArgument 1    : %s%s",
					                     prefix, argument1, endline);
				}
				else
					buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%sArguments     : %s%s",
					                     prefix, argument0, endline);
			}
		}

		buffer += xr_sprintf(buffer, assertion_size - u32(buffer - buffer_base), "%s", endline);
		if (!i)
		{
			if (shared_str_initialized)
			{
				Msg("%s", assertion_info);
				FlushLog();
			}
			buffer = assertion_info;
			endline = "\r\n";
			prefix = "";
		}
	}

	if (!IsDebuggerPresent() && !strstr(GetCommandLine(), "-no_call_stack_assert"))
	{
		if (shared_str_initialized)
			Msg("stack trace:\n");
		LogStackTrace(nullptr, true);
		if (shared_str_initialized)
			FlushLog();
		os_clipboard::copy_to_clipboard(assertion_info);
	}
}

void xrDebug::do_exit(const std::string& message)
{
	FlushLog();
	MessageBox(NULL, message.c_str(), "Error", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
	_exit(1);
}

void xrDebug::backend(const char* expression, const char* description, const char* argument0, const char* argument1,
                      const char* file, int line, const char* function, bool& ignore_always)
{
	// we save first
	crash_saving::save();

	static xrCriticalSection CS;
	CS.Enter();

	string4096 assertion_info;
	gather_info(expression, description, argument0, argument1, file, line, function, assertion_info,
	            sizeof(assertion_info));

	LPCSTR endline = "\r\n";
	LPSTR buffer = assertion_info + xr_strlen(assertion_info);
	buffer += xr_sprintf(buffer, sizeof(assertion_info) - u32(buffer - &assertion_info[0]),
	                     "%sPress OK to abort execution%s", endline, endline);

	if (handler)
		handler();

	if (get_on_dialog())
		get_on_dialog()(true);

	FlushLog();

	if (IsDebuggerPresent())
		DebugBreak();

	MessageBox(NULL, assertion_info, "Fatal Error", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);

	if (get_on_dialog())
		get_on_dialog()(false);

	CS.Leave();

	_exit(1);
}

LPCSTR xrDebug::error2string(long code)
{
	// HRESULTs from Direct3D (and the Metal shim, which keeps the same codes) have no system text here; small values
	// are errno / GetLastError() results.
	static string1024 desc_storage;
	if (code > 0 && code < 200)
		xr_strcpy(desc_storage, strerror((int)code));
	else
		xr_sprintf(desc_storage, "HRESULT 0x%08lX", (unsigned long)code);
	return desc_storage;
}

void xrDebug::error(long hr, const char* expr, const char* file, int line, const char* function, bool& ignore_always)
{
	backend(expr, error2string(hr), 0, 0, file, line, function, ignore_always);
}

void xrDebug::error(long hr, const char* expr, const char* e2, const char* file, int line, const char* function,
                    bool& ignore_always)
{
	backend(expr, error2string(hr), e2, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* file, int line, const char* function, bool& ignore_always)
{
	backend(e1, "assertion failed", 0, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const std::string& e2, const char* file, int line, const char* function,
                   bool& ignore_always)
{
	backend(e1, e2.c_str(), 0, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* e2, const char* file, int line, const char* function,
                   bool& ignore_always)
{
	backend(e1, e2, 0, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* e2, const char* e3, const char* file, int line, const char* function,
                   bool& ignore_always)
{
	backend(e1, e2, e3, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* e2, const char* e3, const char* e4, const char* file, int line,
                   const char* function, bool& ignore_always)
{
	backend(e1, e2, e3, e4, file, line, function, ignore_always);
}

// AVO: print, don't crash
void xrDebug::soft_fail(LPCSTR e1, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
		Msg("! VERIFY_FAILED: %s[%d] {%s}  %s", file, line, function, e1);
}

void xrDebug::soft_fail(LPCSTR e1, const std::string& e2, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
		Msg("! VERIFY_FAILED: %s[%d] {%s}  %s %s", file, line, function, e1, e2.c_str());
}

void xrDebug::soft_fail(LPCSTR e1, LPCSTR e2, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
		Msg("! VERIFY_FAILED: %s[%d] {%s}  %s %s", file, line, function, e1, e2);
}

void xrDebug::soft_fail(LPCSTR e1, LPCSTR e2, LPCSTR e3, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
		Msg("! VERIFY_FAILED: %s[%d] {%s}  %s %s %s", file, line, function, e1, e2, e3);
}

void xrDebug::soft_fail(LPCSTR e1, LPCSTR e2, LPCSTR e3, LPCSTR e4, LPCSTR file, int line, LPCSTR function)
{
	if (!ignore_verify)
		Msg("! VERIFY_FAILED: %s[%d] {%s}  %s %s %s %s", file, line, function, e1, e2, e3, e4);
}

void xrDebug::soft_fail(LPCSTR e1, LPCSTR e2, LPCSTR e3, LPCSTR e4, LPCSTR e5, LPCSTR file, int line,
                        LPCSTR function)
{
	if (!ignore_verify)
		Msg("! VERIFY_FAILED: %s[%d] {%s}  %s %s %s %s %s", file, line, function, e1, e2, e3, e4, e5);
}

void __cdecl xrDebug::fatal(const char* file, int line, const char* function, const char* F, ...)
{
	string1024 buffer;

	va_list p;
	va_start(p, F);
	vsnprintf(buffer, sizeof(buffer), F, p);
	va_end(p);

	bool ignore_always = true;

	backend(nullptr, "fatal error", buffer, 0, file, line, function, ignore_always);
}

// ---------------------------------------------------------------------------------------------
// Out of memory, terminate, signals
// ---------------------------------------------------------------------------------------------
static void out_of_memory_handler()
{
	Msg("* [x-ray]: OOM");

	if (g_full_memory_stats_callback)
		g_full_memory_stats_callback();
	else
	{
		Memory.mem_compact();
		size_t process_heap = Memory.mem_usage();
		u32 eco_strings_count = 0;
		int eco_strings = (int)g_pStringContainer->stat_economy(eco_strings_count);
		int eco_smem = (int)g_pSharedMemoryContainer->stat_economy();
		Msg("* [x-ray]: process heap[%llu K]", (unsigned long long)(process_heap / 1024));
		Msg("* [x-ray]: strings: memory[%d K], count[%u]", eco_strings / 1024, eco_strings_count);
		Msg("* [x-ray]: shared: memory[%d]", eco_smem);
	}

	Debug.fatal(DEBUG_INFO, "Out of memory");
}

static void handler_base(LPCSTR reason_string)
{
	bool ignore_always = false;
	Debug.backend(nullptr, reason_string, 0, 0, DEBUG_INFO, ignore_always);
}

static void terminate_handler()
{
	// The report logs, and logging can itself fail with an exception (the log's mutex throws once it is destroyed, which is
	// the case after the static destructors have run): that is another std::terminate() and this handler again, for ever. One
	// level only; when the report cannot be made the process ends.
	static std::atomic_flag in_terminate = ATOMIC_FLAG_INIT;
	if (in_terminate.test_and_set())
		_exit(128 + SIGABRT);
	handler_base("Unexpected application termination");
}

static std::atomic_flag g_in_crash = ATOMIC_FLAG_INIT;

// Everything the fatal handler does after the first line is best effort and runs on a process whose heap, log or level may be
// the thing that broke (a crash while the level is being destroyed made the game-over save below loop for minutes, and the log,
// which is buffered, was never written). The alarm ends the process if it takes too long.
static void crash_handler_watchdog(int)
{
	static const char msg[] = "\n*** the crash handler did not finish in time, exiting\n";
	(void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
	_exit(128 + SIGSEGV);
}

// Fatal signal: report, then let the default action (and the OS crash reporter) run.
static void fatal_signal_handler(int sig, siginfo_t* info, void*)
{
	if (g_in_crash.test_and_set())
	{
		signal(sig, SIG_DFL);
		raise(sig);
		return;
	}

	const char* name = (sig == SIGSEGV) ? "SIGSEGV (invalid memory access)" :
		(sig == SIGBUS) ? "SIGBUS (bus error / unreadable mapped file)" :
		(sig == SIGILL) ? "SIGILL (illegal instruction)" :
		(sig == SIGFPE) ? "SIGFPE (floating point exception)" :
		(sig == SIGABRT) ? "SIGABRT (abort)" : "signal";

	char head[256];
	int n = snprintf(head, sizeof(head), "\n*** Fatal %s at address %p\n", name, info ? info->si_addr : nullptr);
	if (n > 0) (void)!write(STDERR_FILENO, head, (size_t)n);

	signal(SIGALRM, crash_handler_watchdog);
	alarm(20);

	// the log first: it is buffered, and what comes after can hang
	if (shared_str_initialized)
	{
		Msg("*** Fatal %s at address %p", name, info ? info->si_addr : nullptr);
		FlushLog();
	}

	crash_saving::save();

	// Everything below is best effort: the heap or the log may be what crashed.
	std::string trace;
	backtrace_to_string(trace, 2); // drop this handler and _sigtramp
	(void)!write(STDERR_FILENO, trace.data(), trace.size());

	if (shared_str_initialized)
	{
		size_t pos = 0;
		while (pos < trace.size())
		{
			size_t eol = trace.find('\n', pos);
			if (eol == std::string::npos) eol = trace.size();
			Msg("%s", trace.substr(pos, eol - pos).c_str());
			pos = eol + 1;
		}
		FlushLog();
	}

	// crash report file next to the log
	char path[2 * _MAX_PATH] = {};
	if (xr_FS && FS.m_Flags.test(CLocatorAPI::flReady) && FS.path_exist("$logs$"))
	{
		string64 stamp;
		timestamp(stamp);
		string_path name_only;
		xr_sprintf(name_only, "%s_%s_%s.crash.txt", Core.ApplicationName, Core.UserName, stamp);
		string_path full;
		FS.update_path(full, "$logs$", name_only);
		xr_strcpy(path, xr_native_path(full));
	}
	if (path[0])
	{
		if (FILE* f = fopen(path, "w"))
		{
			fprintf(f, "Fatal %s at address %p\n\n%s", name, info ? info->si_addr : nullptr, trace.c_str());
			fclose(f);
		}
	}

	signal(sig, SIG_DFL);
	raise(sig);
}

// Every thread that may fault needs its own alternate stack, or a stack overflow cannot be reported.
static void install_alt_stack()
{
	static thread_local bool done = false;
	if (done) return;
	done = true;
	// A sanitizer runtime installs (and at thread exit unmaps) its own alternate stack; replacing it with a malloc'ed block
	// makes ASan's thread teardown munmap that block and die ("failed to deallocate", CHECK in UnsetAlternateSignalStack).
	stack_t old = {};
	if (sigaltstack(nullptr, &old) == 0 && !(old.ss_flags & SS_DISABLE) && old.ss_sp) return;
	const size_t size = 128 * 1024;
	stack_t ss;
	ss.ss_sp = malloc(size);
	ss.ss_size = size;
	ss.ss_flags = 0;
	if (ss.ss_sp) sigaltstack(&ss, nullptr);
}

void debug_on_thread_spawn()
{
	install_alt_stack();
	std::set_terminate(terminate_handler);
	std::set_new_handler(out_of_memory_handler);
}

void xrDebug::_initialize(const bool& dedicated)
{
	static bool installed = false; // thread_spawn() calls this for every new thread
	if (!installed) *g_bug_report_file = 0;

	debug_on_thread_spawn();
	if (installed) return;
	installed = true;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = fatal_signal_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
	for (int sig : signals)
		sigaction(sig, &sa, nullptr);
}

void xrDebug::_destroy() {}

#endif // XR_PLATFORM_APPLE
