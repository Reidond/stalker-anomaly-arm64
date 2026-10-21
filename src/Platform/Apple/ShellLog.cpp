// Shell log file, SDL log capture and crash-signal reporting.

#include "ShellInternal.h"

#include <SDL3/SDL_log.h>

#include <TargetConditionals.h>
#include <cxxabi.h>
#include <execinfo.h>
#include <fcntl.h>
#include <os/log.h>
#include <os/proc.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <exception>
#include <mutex>
#include <typeinfo>

namespace AppleShell
{
namespace
{
std::mutex g_log_mutex;
int g_log_fd = -1;
os_log_t g_os_log = nullptr;

// The last few KB of log text, kept in static memory so the crash handler can dump it without allocating.
constexpr size_t kRingSize = 8192;
char g_ring[kRingSize];
std::atomic<size_t> g_ring_pos{0};

void RingAppend(const char* text, size_t length)
{
    size_t pos = g_ring_pos.load(std::memory_order_relaxed);
    for (size_t i = 0; i < length; ++i)
        g_ring[(pos + i) % kRingSize] = text[i];
    g_ring_pos.store(pos + length, std::memory_order_release);
}

void Emit(const char* line)
{
    // Timestamp: seconds since Init, like the event history.
    char stamped[1280];
    int n = snprintf(stamped, sizeof(stamped), "[%9.3f] %s\n", Internal::SecondsSinceInit(), line);
    if (n < 0)
        return;
    size_t length = size_t(n) < sizeof(stamped) ? size_t(n) : sizeof(stamped) - 1;

    std::lock_guard<std::mutex> lock(g_log_mutex);
    RingAppend(stamped, length);
    if (g_log_fd >= 0)
        (void)!write(g_log_fd, stamped, length);
    if (g_os_log)
        os_log(g_os_log, "%{public}s", line);
    fputs(stamped, stderr);
}

void SDLLogSink(void* /*userdata*/, int category, SDL_LogPriority priority, const char* message)
{
    static const char* const kNames[] = {"", "TRACE", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"};
    const char* name = (priority > 0 && priority < SDL_LOG_PRIORITY_COUNT) ? kNames[priority] : "";
    char line[1100];
    snprintf(line, sizeof(line), "SDL[%d] %s %s", category, name, message);
    Emit(line);
}

// ---- crash handler -------------------------------------------------------------------------

char g_crash_path[1024];
char g_crash_context[256];
constexpr int kCrashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS};
alignas(16) char g_alt_stack[64 * 1024];
std::terminate_handler g_previous_terminate = nullptr;

// Async-signal-safe helpers: no allocation, no stdio.
void WriteAll(int fd, const char* text)
{
    (void)!write(fd, text, strlen(text));
}

void WriteNumber(int fd, unsigned long long value, int base)
{
    char buffer[24];
    int i = sizeof(buffer);
    buffer[--i] = 0;
    do
    {
        buffer[--i] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value && i > 0);
    if (base == 16)
        WriteAll(fd, "0x");
    WriteAll(fd, buffer + i);
}

const char* SignalName(int sig)
{
    switch (sig)
    {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    case SIGSYS: return "SIGSYS";
    default: return "signal";
    }
}

void WriteRecentLog(int fd)
{
    const size_t end = g_ring_pos.load(std::memory_order_acquire);
    const size_t begin = end > kRingSize ? end - kRingSize : 0;
    WriteAll(fd, "--- recent log ---\n");
    for (size_t i = begin; i < end; ++i)
        (void)!write(fd, &g_ring[i % kRingSize], 1);
    WriteAll(fd, "--- end of log ---\n");
}

void CrashHandler(int sig, siginfo_t* info, void* /*context*/)
{
    int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0)
    {
        WriteAll(fd, "==== Anomaly shell crash report ====\n");
        if (g_crash_context[0])
        {
            WriteAll(fd, g_crash_context);
            WriteAll(fd, "\n");
        }
        WriteAll(fd, "time (unix): ");
        WriteNumber(fd, (unsigned long long)time(nullptr), 10);
        WriteAll(fd, "\nsignal: ");
        WriteAll(fd, SignalName(sig));
        WriteAll(fd, " (");
        WriteNumber(fd, (unsigned long long)sig, 10);
        WriteAll(fd, ")  code: ");
        WriteNumber(fd, (unsigned long long)(info ? info->si_code : 0), 10);
        WriteAll(fd, "\nfault address: ");
        WriteNumber(fd, (unsigned long long)(info ? (uintptr_t)info->si_addr : 0), 16);
#if TARGET_OS_IOS
        WriteAll(fd, "\navailable memory: ");
        WriteNumber(fd, (unsigned long long)os_proc_available_memory(), 10);
        WriteAll(fd, " bytes");
#endif
        WriteAll(fd, "\n--- backtrace ---\n");
        void* frames[64];
        int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, fd);
        WriteRecentLog(fd);
        WriteAll(fd, "\n");
        close(fd);
    }
    // SA_RESETHAND restored the default action: returning re-executes the fault (or abort re-raises),
    // so the system still produces its own crash report.
}

[[noreturn]] void TerminateHandler()
{
    char text[512];
    const std::type_info* type = abi::__cxa_current_exception_type();
    if (type)
        snprintf(text, sizeof(text), "std::terminate: uncaught C++ exception of type %s\n", type->name());
    else
        snprintf(text, sizeof(text), "std::terminate called without an active exception\n");
    Internal::WriteCrashText(text);
    if (g_previous_terminate)
        g_previous_terminate();
    abort();
}
} // namespace

namespace Internal
{
void OpenLog(const std::string& path, const std::string& previous_path)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_fd >= 0)
        return;
    if (!g_os_log)
        g_os_log = os_log_create("com.andriishafar.anomaly", "shell");
    // Keep the previous run's log: Documents/anomaly-shell.prev.log.
    rename(path.c_str(), previous_path.c_str());
    g_log_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
}

void CloseLog()
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_fd >= 0)
    {
        close(g_log_fd);
        g_log_fd = -1;
    }
}

void RedirectSDLLog()
{
    SDL_SetLogOutputFunction(SDLLogSink, nullptr);
}

void RestoreSDLLog()
{
    SDL_SetLogOutputFunction(SDL_GetDefaultLogOutputFunction(), nullptr);
}

void SetCrashContext(const std::string& context)
{
    snprintf(g_crash_context, sizeof(g_crash_context), "%s", context.c_str());
}

void InstallCrashHandler(const std::string& crash_path)
{
    snprintf(g_crash_path, sizeof(g_crash_path), "%s", crash_path.c_str());

    stack_t stack{};
    stack.ss_sp = g_alt_stack;
    stack.ss_size = sizeof(g_alt_stack);
    sigaltstack(&stack, nullptr);

    struct sigaction action{};
    action.sa_sigaction = CrashHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (int sig : kCrashSignals)
        sigaction(sig, &action, nullptr);

    g_previous_terminate = std::set_terminate(TerminateHandler);
}

void WriteCrashText(const char* text)
{
    if (!g_crash_path[0])
        return;
    int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    WriteAll(fd, "==== Anomaly shell crash report ====\n");
    if (g_crash_context[0])
    {
        WriteAll(fd, g_crash_context);
        WriteAll(fd, "\n");
    }
    WriteAll(fd, text);
    WriteAll(fd, "\n--- backtrace ---\n");
    void* frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, fd);
    WriteRecentLog(fd);
    WriteAll(fd, "\n");
    close(fd);
}
} // namespace Internal

void Log(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    Emit(line);
}

} // namespace AppleShell
