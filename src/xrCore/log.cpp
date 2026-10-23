#include "stdafx.h"
#pragma hdrstop

#include <time.h>
#include "resource.h"
#include "log.h"
#include "log_lines.h"
#ifdef _EDITOR
#include "malloc.h"
#endif

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include "profiler.h"

extern BOOL LogExecCB = TRUE;
static string_path logFName = "engine.log";
static string_path log_file_name = "engine.log";
static BOOL no_log = TRUE;
#ifdef PROFILE_CRITICAL_SECTIONS
static xrCriticalSection logCS(MUTEX_PROFILE_ID(log));
#else // PROFILE_CRITICAL_SECTIONS
static xrCriticalSection logCS;
#endif // PROFILE_CRITICAL_SECTIONS
xr_vector<xr_string> LogFile;
static LogCallback LogCB = 0;
// Everything below that touches LogFile, last_line and log_closed holds logCS: Log() is called from every thread the engine has
// (the ALife/secondary thread, the freeze watchdog, sound, GCD workers), the console reads the lines to draw them, and CloseLog() runs
// while any of them may still be logging.
static xr_string last_line; // the last line as it was logged, before the " [n]" counter: a repeat only bumps the counter
static int items_count = 0;
static bool log_closed = false; // CloseLog() ran: the file system is going away, nothing is written to the log file any more

void FlushLog()
{
	PROF_EVENT();

	// The freeze watchdog thread calls this too, also while the main thread tears the file system down: the lock is taken before
	// anything is looked at, and CloseLog() (same lock) is what ends the right to write.
	logCS.Enter();
	if (!no_log && !log_closed && xr_FS)
	{
		PROF_EVENT("Flushing");
		IWriter* f = FS.w_open(logFName);
		if (f)
		{
			for (const auto& i : LogFile)
			{
				LPCSTR s = i.c_str();
				f->w_string(s ? s : "");
			}
			// not FS.w_close(): it registers the file in the file system's table, which only the thread that owns the file system
			// may change (the log file is registered by CreateLog already)
			xr_delete(f);
		}
	}
	logCS.Leave();
}

std::string getCurrentTimeStamp(LPCSTR format = "%d.%m.%Y %H:%M:%S") {
	using namespace std::chrono;

	// get current time
	auto now = system_clock::now();

	// get number of milliseconds for the current second
	// (remainder after division into seconds)
	auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

	// convert to std::time_t in order to convert to std::tm (broken time)
	auto timer = system_clock::to_time_t(now);

	// convert to broken time
	std::tm bt = *std::localtime(&timer);

	std::ostringstream oss;

	oss << std::put_time(&bt, format); // HH:MM:SS
	oss << '.' << std::setfill('0') << std::setw(3) << ms.count();

	return oss.str();
}

std::string timeInDMYHMSMMM()
{
	return getCurrentTimeStamp("%d.%m.%Y %H:%M:%S");
}

std::string timeInHMSMMM()
{
	return getCurrentTimeStamp("%H:%M:%S");
}

BOOL logTimestamps = FALSE;
#ifdef XR_PLATFORM_APPLE
enum Console_mark : int;
#else
enum Console_mark;
#endif
extern bool is_console_mark(Console_mark type);
#ifdef XR_PLATFORM_APPLE
// Default for programs that link xrCore without xrEngine (tools, tests); the real one in XR_IOConsole.cpp overrides it.
__attribute__((weak)) bool is_console_mark(Console_mark) { return false; }
#endif

void AddOne(const char* split)
{

	logCS.Enter();

#ifdef DEBUG
    OutputDebugString(split);
    OutputDebugString("\n");
#endif

	// DUMP_PHASE;
	{
		// demonized: add timestamps to log
		std::string t = split;
		if (logTimestamps) {
			std::string c = "";
			if (t.length() > 0 && is_console_mark((Console_mark)t[0])) {
				c += t[0];
				c += " ";
				t.erase(0, 1);
			}
			t = c + "[" + timeInHMSMMM() + "] " + t;
		}
		// (LogFile is empty after CloseLog() / ClearLog(): erase(end() - 1) on it moved the end below the begin and the next
		// push_back crashed, which is how a quit that logged the same line twice died)
		if (!LogFile.empty() && last_line == t.c_str())
		{
			xr_string tmp = t.c_str();

			if (items_count == 0)
				items_count = 2;
			else
				items_count++;

			tmp += " [";
			tmp += std::to_string(items_count).c_str();
			tmp += "]";

			LogFile.back() = tmp;
		}
		else
		{
			// DUMP_PHASE;
			LogFile.push_back(xr_string(t.c_str()));
			last_line = t.c_str();
			items_count = 0;
		}
	}

	//exec CallBack
	if (LogExecCB && LogCB)LogCB(split);

	logCS.Leave();
}

void Log(const char* s)
{
	int i, j;

	u32 length = xr_strlen(s);
#ifndef _EDITOR
	PSTR split = (PSTR)_alloca((length + 1) * sizeof(char));
#else
    PSTR split = (PSTR)alloca((length + 1) * sizeof(char));
#endif
	for (i = 0, j = 0; s[i] != 0; i++)
	{
		if (s[i] == '\n')
		{
			split[j] = 0; // end of line
			if (split[0] == 0)
			{
				split[0] = ' ';
				split[1] = 0;
			}
			AddOne(split);
			j = 0;
		}
		else
		{
			split[j++] = s[i];
		}
	}
	split[j] = 0;
	AddOne(split);
}

void __cdecl Msg(const char* format, ...)
{
	va_list mark;
	string2048 buf;
	va_start(mark, format);
	int sz = _vsnprintf(buf, sizeof(buf) - 1, format, mark);
	buf[sizeof(buf) - 1] = 0;
	va_end(mark);
	if (sz) Log(buf);
}

void Log(const char* msg, const char* dop)
{
	if (!dop)
	{
		Log(msg);
		return;
	}

	u32 buffer_size = (xr_strlen(msg) + 1 + xr_strlen(dop) + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);
	strconcat(buffer_size, buf, msg, " ", dop);
	Log(buf);
}

void Log(const char* msg, u32 dop)
{
	u32 buffer_size = (xr_strlen(msg) + 1 + 10 + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);

	xr_sprintf(buf, buffer_size, "%s %d", msg, dop);
	Log(buf);
}

void Log(const char* msg, int dop)
{
	u32 buffer_size = (xr_strlen(msg) + 1 + 11 + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);

	xr_sprintf(buf, buffer_size, "%s %i", msg, dop);
	Log(buf);
}

void Log(const char* msg, float dop)
{
	// actually, float string representation should be no more, than 40 characters,
	// but we will count with slight overhead
	u32 buffer_size = (xr_strlen(msg) + 1 + 64 + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);

	xr_sprintf(buf, buffer_size, "%s %f", msg, dop);
	Log(buf);
}

void Log(const char* msg, const Fvector& dop)
{
	u32 buffer_size = (xr_strlen(msg) + 2 + 3 * (64 + 1) + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);

	xr_sprintf(buf, buffer_size, "%s (%f,%f,%f)", msg, VPUSH(dop));
	Log(buf);
}

void Log(const char* msg, const Fmatrix& dop)
{
	u32 buffer_size = (xr_strlen(msg) + 2 + 4 * (4 * (64 + 1) + 1) + 1) * sizeof(char);
	PSTR buf = (PSTR)_alloca(buffer_size);

	xr_sprintf(buf, buffer_size, "%s:\n%f,%f,%f,%f\n%f,%f,%f,%f\n%f,%f,%f,%f\n%f,%f,%f,%f\n",
	           msg,
	           dop.i.x, dop.i.y, dop.i.z, dop._14_,
	           dop.j.x, dop.j.y, dop.j.z, dop._24_,
	           dop.k.x, dop.k.y, dop.k.z, dop._34_,
	           dop.c.x, dop.c.y, dop.c.z, dop._44_
	);
	Log(buf);
}

void LogWinErr(const char* msg, long err_code)
{
	Msg("%s: %s", msg, Debug.error2string(err_code));
}

LogCallback SetLogCB(LogCallback cb)
{
	LogCallback result = LogCB;
	LogCB = cb;
	return (result);
}

LPCSTR log_name()
{
	return (log_file_name);
}

void InitLog()
{
	LogFile.reserve(10000);
}

void CreateLog(BOOL nl)
{
	no_log = nl;
	strconcat(sizeof(log_file_name), log_file_name, Core.ApplicationName, "_", Core.UserName, ".log");
	if (FS.path_exist("$logs$"))
		FS.update_path(logFName, "$logs$", log_file_name);
	if (!no_log)
	{
		//Alun: Backup existing log
		xr_string backup_logFName = EFS.ChangeFileExt(logFName, ".bkp");
		FS.file_rename(logFName, backup_logFName.c_str(), true);
		//-Alun
		IWriter* f = FS.w_open(logFName);
		if (f == NULL)
		{
			MessageBox(NULL, "Can't create log file.", "Error", MB_ICONERROR);
			abort();
		}
		FS.w_close(f);
	}
}

void CloseLog(void)
{
	logCS.Enter();
	FlushLog();
	log_closed = true;
	LogFile.clear();
	last_line.clear();
	items_count = 0;
	logCS.Leave();
}

void ClearLog()
{
	logCS.Enter();
	LogFile.clear_not_free();
	last_line.clear();
	items_count = 0;
	logCS.Leave();
	FlushLog();
}

u32 LogLineCount()
{
	logCS.Enter();
	const u32 count = (u32)LogFile.size();
	logCS.Leave();
	return count;
}

u32 LogCopyLines(u32 last, u32 count, xr_vector<xr_string>& out)
{
	out.clear();
	logCS.Enter();
	u32 first = 0;
	if (!LogFile.empty())
	{
		if (last >= LogFile.size())
			last = (u32)LogFile.size() - 1;
		first = last + 1 > count ? last + 1 - count : 0;
		out.assign(LogFile.begin() + first, LogFile.begin() + last + 1);
	}
	logCS.Leave();
	return first;
}

xr_string FormatString(LPCSTR fmt, ...)
{
	va_list mark;
	string2048 buf;
	va_start(mark, fmt);
	int sz = _vsnprintf(buf, sizeof(buf) - 1, fmt, mark);
	buf[sizeof(buf) - 1] = 0;
	va_end(mark);
	if (sz) return xr_string(buf);
	return xr_string("");
}
