// jit_lab: LuaJIT benchmark and stability runner. Independent of SDL so it can run headless.
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace jitlab
{
struct Config
{
    double scale = 1.0;            // Workload scale factor (--scale=).
    int stability_seconds = 60;    // Length of the stability run (--stability=).
    size_t region_mb = 32;         // Size of the TXM code region (--region-mb=).
    unsigned wait_ms = 0;          // How long to wait for a debugger (--wait-ms=).
    bool keep_debugger = false;    // Do not detach after authorizing (--keep-debugger).
    bool probe_all = false;        // Execute-test every page of the region (--probe-all).
    bool no_brk = false;           // Skip the brk handshake (--no-brk, non-TXM devices).
    bool verbose = false;          // Log each TXM step to stderr (--verbose).
    bool skip_bench = false;       // Only the stability run (--no-bench).
    bool interp_stability = false; // Also run the stability run with the JIT off (--interp-stability).
    bool require_jit = false;      // Count a run without JIT as failed (--require-jit).
    bool no_jit = false;           // Do not try to enable the JIT at all (--no-jit).
};

struct Result
{
    bool jit_available = false;
    bool passed = false;
    std::string summary;
};

using LogFn = std::function<void(const std::string&)>;

// Parses --key=value / --flag arguments. Unknown arguments are reported through `log` and ignored.
Config ParseArgs(int argc, char** argv, const LogFn& log);

// Runs everything on the calling thread (use a thread with a big stack: the JIT recorder needs it).
Result Run(const Config& config, const LogFn& log);
} // namespace jitlab
