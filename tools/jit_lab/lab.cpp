#include "lab.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/sysctl.h>
#include <sys/utsname.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <luajit.h>
}

#if __has_include(<luajit_txm.h>)
#include <luajit_txm.h>
#define JITLAB_HAVE_TXM 1
#else
#define JITLAB_HAVE_TXM 0
#endif

#include "bench_lua.h"

namespace jitlab
{
namespace
{
using Clock = std::chrono::steady_clock;

double NowSeconds()
{
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

std::string Sysctl(const char* name)
{
    char buffer[128] = {};
    size_t size = sizeof(buffer) - 1;
    if (sysctlbyname(name, buffer, &size, nullptr, 0) != 0)
        return "?";
    return buffer;
}

// -- Helpers exported to the Lua workloads ---------------------------------------------------

int LabNow(lua_State* L)
{
    lua_pushnumber(L, NowSeconds());
    return 1;
}

int LabAdd(lua_State* L)
{
    lua_pushnumber(L, lua_tonumber(L, 1) + lua_tonumber(L, 2));
    return 1;
}

// lab.call_n(f, n): the engine calling a script callback n times.
int LabCallN(lua_State* L)
{
    const int n = int(luaL_checkinteger(L, 2));
    luaL_checktype(L, 1, LUA_TFUNCTION);
    double sum = 0;
    for (int i = 1; i <= n; ++i)
    {
        lua_pushvalue(L, 1);
        lua_pushinteger(L, i);
        if (lua_pcall(L, 1, 1, 0) != 0)
            return lua_error(L);
        sum += lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    lua_pushnumber(L, std::fmod(sum, 1000003.0));
    return 1;
}

// lab.nest(f, n): Lua -> C -> Lua f -> (C add) chains.
int LabNest(lua_State* L)
{
    const int n = int(luaL_checkinteger(L, 2));
    luaL_checktype(L, 1, LUA_TFUNCTION);
    double sum = 0;
    for (int i = 1; i <= n; ++i)
    {
        lua_pushvalue(L, 1);
        lua_pushinteger(L, i);
        lua_call(L, 1, 1);
        sum += lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    lua_pushnumber(L, std::fmod(sum, 1000003.0));
    return 1;
}

void RegisterLab(lua_State* L)
{
    lua_newtable(L);
    lua_pushcfunction(L, LabNow);
    lua_setfield(L, -2, "now");
    lua_pushcfunction(L, LabAdd);
    lua_setfield(L, -2, "add");
    lua_pushcfunction(L, LabCallN);
    lua_setfield(L, -2, "call_n");
    lua_pushcfunction(L, LabNest);
    lua_setfield(L, -2, "nest");
}

// -- JIT control -----------------------------------------------------------------------------

bool JitEnabled(lua_State* L)
{
    lua_getglobal(L, "jit");
    lua_getfield(L, -1, "status");
    lua_call(L, 0, 1);
    const bool on = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    return on;
}

std::string JitStatusLine(lua_State* L)
{
    const char* script =
        "local t = {} "
        "local function f(...) for i = 1, select('#', ...) do t[#t + 1] = tostring((select(i, ...))) end end "
        "f(jit.status()) return table.concat(t, ' ')";
    std::string line = "?";
    if (luaL_dostring(L, script) == 0 && lua_isstring(L, -1))
        line = lua_tostring(L, -1);
    lua_pop(L, 1);
    return line;
}

void SetJit(lua_State* L, bool on)
{
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | (on ? LUAJIT_MODE_ON : LUAJIT_MODE_OFF));
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_FLUSH);
}

// -- Benchmarks ------------------------------------------------------------------------------

struct Timing
{
    double cold = 0;     // First run: includes trace compilation under the JIT.
    double best = 0;     // Best of the following runs.
    double checksum = 0;
    bool ok = true;
    std::string error;
};

// Runs benchmark `index` (1-based in module.benches) three times; stack-neutral.
Timing TimeBench(lua_State* L, int module, int index, double scale)
{
    Timing t;
    lua_getfield(L, module, "benches");
    lua_rawgeti(L, -1, index);
    lua_getfield(L, -1, "fn");
    const int fn = lua_gettop(L);
    for (int run = 0; run < 3; ++run)
    {
        lua_pushvalue(L, fn);
        lua_pushnumber(L, scale);
        const double start = NowSeconds();
        if (lua_pcall(L, 1, 1, 0) != 0)
        {
            t.ok = false;
            t.error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
            lua_pop(L, 1);
            break;
        }
        const double elapsed = NowSeconds() - start;
        t.checksum = lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (run == 0)
            t.cold = elapsed;
        if (run == 1 || (run == 2 && elapsed < t.best))
            t.best = elapsed;
    }
    lua_settop(L, module);
    return t;
}

bool ChecksumsMatch(double a, double b, bool is_float)
{
    if (a == b)
        return true;
    if (!is_float)
        return false;
    return std::fabs(a - b) <= 1e-8 * std::fmax(1.0, std::fmax(std::fabs(a), std::fabs(b)));
}

struct BenchInfo
{
    std::string group;
    std::string name;
    double scale = 1;
    bool is_float = false;
};

std::vector<BenchInfo> ListBenches(lua_State* L, int module)
{
    std::vector<BenchInfo> list;
    lua_getfield(L, module, "benches");
    const int count = int(lua_objlen(L, -1));
    for (int i = 1; i <= count; ++i)
    {
        lua_rawgeti(L, -1, i);
        BenchInfo info;
        lua_getfield(L, -1, "group");
        info.group = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "name");
        info.name = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "scale");
        info.scale = lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "float");
        info.is_float = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
        list.push_back(info);
    }
    lua_pop(L, 1);
    return list;
}

} // namespace

Config ParseArgs(int argc, char** argv, const LogFn& log)
{
    Config config;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto value = [&](const char* key) -> const char* {
            const size_t len = std::strlen(key);
            return arg.compare(0, len, key) == 0 ? arg.c_str() + len : nullptr;
        };
        if (const char* v = value("--scale="))
            config.scale = std::atof(v);
        else if (const char* v2 = value("--stability="))
            config.stability_seconds = std::atoi(v2);
        else if (const char* v3 = value("--region-mb="))
            config.region_mb = size_t(std::atoi(v3));
        else if (const char* v4 = value("--wait-ms="))
            config.wait_ms = unsigned(std::atoi(v4));
        else if (arg == "--keep-debugger")
            config.keep_debugger = true;
        else if (arg == "--probe-all")
            config.probe_all = true;
        else if (arg == "--no-brk")
            config.no_brk = true;
        else if (arg == "--verbose")
            config.verbose = true;
        else if (arg == "--no-bench")
            config.skip_bench = true;
        else if (arg == "--interp-stability")
            config.interp_stability = true;
        else if (arg == "--require-jit")
            config.require_jit = true;
        else if (arg == "--no-jit")
            config.no_jit = true;
        else if (arg == "--headless" || arg == "--exit" || arg == "--stikdebug")
            continue; // Handled by main.cpp.
        else
            log("ignoring argument: " + arg);
    }
    return config;
}

Result Run(const Config& config, const LogFn& log)
{
    Result result;
    log("== jit_lab ==");
    {
        struct utsname uts;
        uname(&uts);
        uint64_t memsize = 0;
        size_t memsize_len = sizeof(memsize);
        sysctlbyname("hw.memsize", &memsize, &memsize_len, nullptr, 0);
        log(Format("device    %s  %s %s  ram %.1f GB", Sysctl("hw.machine").c_str(),
            Sysctl("kern.osproductversion").c_str(), Sysctl("kern.osversion").c_str(),
            double(memsize) / 1073741824.0));
    }

#if JITLAB_HAVE_TXM
    if (config.no_jit)
    {
        log("txm       skipped (--no-jit)");
    }
    else
    {
        unsigned flags = 0;
        if (config.keep_debugger)
            flags |= LUAJIT_TXM_KEEP_DEBUGGER;
        if (config.no_brk)
            flags |= LUAJIT_TXM_NO_BRK;
        if (config.verbose)
            flags |= LUAJIT_TXM_VERBOSE;
        if (config.probe_all)
            flags |= LUAJIT_TXM_PROBE_ALL;
        log(Format("txm       debugger attached: %s; requesting %zu MiB region (wait up to %u ms)",
            luajit_txm_debugger_attached() ? "yes" : "no", config.region_mb, config.wait_ms));
        const double t0 = NowSeconds();
        const int rc = luajit_txm_init(config.region_mb << 20, flags, config.wait_ms);
        luajit_txm_info info;
        luajit_txm_get_info(&info);
        log(Format("txm       init rc=%d state=%d in %.0f ms: %s", rc, info.state, (NowSeconds() - t0) * 1000.0,
            info.msg));
        if (rc == 0)
        {
            log(Format("txm       rx=%p rw=%p size=%zu KiB near-vm=%ld auth=%.0f ms detached=%d", info.rx, info.rw,
                info.size >> 10, info.near_vm, info.auth_ms, info.detached));
        }
    }
#else
    log("txm       this LuaJIT build has no TXM support (stock build)");
#endif

    lua_State* L = luaL_newstate();
    if (!L)
    {
        log("luaL_newstate failed");
        result.summary = "FAIL: no Lua state";
        return result;
    }
    luaL_openlibs(L);
    log(Format("luajit    %s (%s build)  jit.status: %s", LUAJIT_VERSION, JITLAB_LUAJIT_VARIANT,
        JitStatusLine(L).c_str()));
    result.jit_available = JitEnabled(L);

    RegisterLab(L);
    const int lab_table = lua_gettop(L);
    if (luaL_loadbuffer(L, kBenchLua, std::strlen(kBenchLua), "=bench.lua") != 0)
    {
        log(std::string("bench.lua: ") + lua_tostring(L, -1));
        lua_close(L);
        result.summary = "FAIL: bench.lua does not load";
        return result;
    }
    lua_pushvalue(L, lab_table);
    if (lua_pcall(L, 1, 1, 0) != 0)
    {
        log(std::string("bench.lua: ") + lua_tostring(L, -1));
        lua_close(L);
        result.summary = "FAIL: bench.lua raised an error";
        return result;
    }
    const int module = lua_gettop(L);

    bool all_ok = true;
    int bench_count = 0;
    double sum_speedup_log = 0;
    int speedups = 0;

    if (!config.skip_bench)
    {
        log("");
        log(Format("%-14s %-18s %10s %10s %10s %8s  %s", "group", "benchmark", "interp ms", "jit cold", "jit warm",
            "speedup", "check"));
        const std::vector<BenchInfo> benches = ListBenches(L, module);
        for (size_t i = 0; i < benches.size(); ++i)
        {
            const BenchInfo& b = benches[i];
            const double scale = b.scale * config.scale;
            SetJit(L, false);
            Timing interp = TimeBench(L, module, int(i + 1), scale);
            Timing jit;
            bool have_jit = false;
            if (result.jit_available)
            {
                SetJit(L, true);
                jit = TimeBench(L, module, int(i + 1), scale);
                have_jit = true;
            }
            std::string check = "ok";
            if (!interp.ok)
            {
                check = "interp error: " + interp.error;
                all_ok = false;
            }
            else if (have_jit && !jit.ok)
            {
                check = "jit error: " + jit.error;
                all_ok = false;
            }
            else if (have_jit && !ChecksumsMatch(interp.checksum, jit.checksum, b.is_float))
            {
                check = Format("MISMATCH jit=%.12g interp=%.12g", jit.checksum, interp.checksum);
                all_ok = false;
            }
            if (have_jit && jit.ok && interp.ok && jit.best > 0)
            {
                const double speedup = interp.best / jit.best;
                sum_speedup_log += std::log(speedup);
                ++speedups;
                log(Format("%-14s %-18s %10.1f %10.1f %10.1f %7.1fx  %s", b.group.c_str(), b.name.c_str(),
                    interp.best * 1000, jit.cold * 1000, jit.best * 1000, speedup, check.c_str()));
            }
            else
            {
                log(Format("%-14s %-18s %10.1f %10s %10s %8s  %s", b.group.c_str(), b.name.c_str(), interp.best * 1000,
                    "-", "-", "-", check.c_str()));
            }
            ++bench_count;
        }
        if (speedups > 0)
            log(Format("geometric mean speedup of the JIT over the interpreter: %.2fx", std::exp(sum_speedup_log / speedups)));
    }

    // Stability run: JIT on if available, mixed workloads plus generated code, results checked.
    auto stability = [&](bool jit_on, int seconds) {
        if (seconds <= 0)
            return;
        log("");
        log(Format("stability run: %d s, JIT %s", seconds, jit_on ? "ON" : "OFF"));
        SetJit(L, jit_on);
        lua_getfield(L, module, "stability");
        lua_pushnumber(L, seconds);
        lua_pushcfunction(L, [](lua_State* S) -> int {
            // Progress lines go straight to the log through an upvalue-free trampoline.
            lua_getfield(S, LUA_REGISTRYINDEX, "jitlab.log");
            auto* fn = static_cast<const LogFn*>(lua_touserdata(S, -1));
            lua_pop(S, 1);
            if (fn)
                (*fn)(lua_tostring(S, 1));
            return 0;
        });
        lua_pushlightuserdata(L, const_cast<LogFn*>(&log));
        lua_setfield(L, LUA_REGISTRYINDEX, "jitlab.log");
        lua_pushboolean(L, jit_on);
        if (lua_pcall(L, 3, 1, 0) != 0)
        {
            log(std::string("stability run raised: ") + lua_tostring(L, -1));
            lua_pop(L, 1);
            all_ok = false;
            return;
        }
        auto field = [&](const char* name) {
            lua_getfield(L, -1, name);
            const double v = lua_tonumber(L, -1);
            lua_pop(L, 1);
            return v;
        };
        const double mism = field("mismatches"), errs = field("errors");
        log(Format("stability done: %.1f s, %d iterations, %d generated functions, %d traces, %d aborts, %d flushes, "
                   "%d mismatches, %d errors",
            field("seconds"), int(field("iters")), int(field("generated")), int(field("traces")), int(field("aborts")),
            int(field("flushes")), int(mism), int(errs)));
        lua_getfield(L, -1, "first_error");
        if (lua_isstring(L, -1))
            log(std::string("first problem: ") + lua_tostring(L, -1));
        lua_pop(L, 2);
        if (mism != 0 || errs != 0)
            all_ok = false;
    };

    stability(result.jit_available, config.stability_seconds);
    if (config.interp_stability && result.jit_available)
        stability(false, config.stability_seconds);

#if JITLAB_HAVE_TXM
    {
        luajit_txm_info info;
        luajit_txm_get_info(&info);
        log(Format("txm       region use: %zu KiB of %zu KiB", info.used >> 10, info.size >> 10));
    }
#endif

    lua_close(L);

    // A second state after the first one closed: its machine code areas went back to the region
    // and the new state must be able to use them.
    if (result.jit_available)
    {
        lua_State* L2 = luaL_newstate();
        luaL_openlibs(L2);
        const char* script =
            "local s = 0 for i = 1, 3000000 do s = s + i % 7 end "
            "return jit.status() and require('jit.util').traceinfo(1) ~= nil and s == 8999997";
        const bool ok = luaL_dostring(L2, script) == 0 && lua_toboolean(L2, -1) && lua_isboolean(L2, -1);
        log(Format("second state after lua_close: %s", ok ? "JIT ON, compiled and correct" : "FAILED"));
        if (!ok)
            all_ok = false;
        lua_close(L2);
    }

    if (config.require_jit && !result.jit_available)
        all_ok = false;
    result.passed = all_ok;
    result.summary = Format("RESULT jit=%s benchmarks=%d %s", result.jit_available ? "ON" : "OFF", bench_count,
        all_ok ? "PASS" : "FAIL");
    log("");
    log(result.summary);
    return result;
}
} // namespace jitlab
