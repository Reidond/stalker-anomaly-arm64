// Results from the wasm side to the page.
//
// Every test posts one JSON object per update to the page's main thread:
//   {"id": "gpu.mrt", "status": "pass|fail|unverified|info|running", "summary": "...", "data": {...}}
// The page keeps the latest object per id, shows it in the results table and logs it to the console.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lab
{
// A small JSON object writer; enough for flat objects, nested objects and arrays of numbers.
class Json
{
public:
    Json& Str(const char* key, const std::string& value);
    Json& Num(const char* key, double value);
    Json& Int(const char* key, int64_t value);
    Json& Bool(const char* key, bool value);
    Json& Raw(const char* key, const std::string& json); // already-encoded JSON value
    Json& Nums(const char* key, const std::vector<double>& values);
    std::string Done() const { return "{" + body_ + "}"; }

    static std::string Quote(const std::string& s);

private:
    void Key(const char* key);
    std::string body_;
};

// Posts {"id", "status", "summary", "data"} to the page (asynchronously, from any thread).
void Report(const char* id, const char* status, const std::string& summary, const std::string& data_json = "{}");
// printf-style log line that shows in the page's log panel and the console.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Wall-clock milliseconds since the Unix epoch (performance.timeOrigin + performance.now()), the
// same clock on every thread and on the page, so cross-thread latencies can be compared.
double EpochMs();
// Monotonic milliseconds on this thread (emscripten_get_now).
double NowMs();

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

struct Stats
{
    double min = 0, median = 0, p95 = 0, max = 0, mean = 0;
    size_t count = 0;
};
Stats Summarize(std::vector<double> values);
std::string StatsJson(const Stats& s);
} // namespace lab
