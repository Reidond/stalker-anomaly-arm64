#include "lab_report.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <emscripten.h>
#include <emscripten/threading.h>

namespace lab
{
std::string Json::Quote(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
                out += char(c);
        }
    }
    return out + "\"";
}

void Json::Key(const char* key)
{
    if (!body_.empty())
        body_ += ",";
    body_ += Quote(key) + ":";
}

Json& Json::Str(const char* key, const std::string& value)
{
    Key(key);
    body_ += Quote(value);
    return *this;
}

static std::string NumberText(double value)
{
    if (!std::isfinite(value))
        return "null";
    char buf[64];
    if (value == std::floor(value) && std::fabs(value) < 9007199254740992.0)
        std::snprintf(buf, sizeof(buf), "%.0f", value); // byte counts stay exact
    else
        std::snprintf(buf, sizeof(buf), "%.6g", value);
    return buf;
}

Json& Json::Num(const char* key, double value)
{
    Key(key);
    body_ += NumberText(value);
    return *this;
}

Json& Json::Int(const char* key, int64_t value)
{
    Key(key);
    body_ += std::to_string(value);
    return *this;
}

Json& Json::Bool(const char* key, bool value)
{
    Key(key);
    body_ += value ? "true" : "false";
    return *this;
}

Json& Json::Raw(const char* key, const std::string& json)
{
    Key(key);
    body_ += json;
    return *this;
}

Json& Json::Nums(const char* key, const std::vector<double>& values)
{
    Key(key);
    body_ += "[";
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i)
            body_ += ",";
        body_ += NumberText(values[i]);
    }
    body_ += "]";
    return *this;
}

void Report(const char* id, const char* status, const std::string& summary, const std::string& data_json)
{
    const std::string json = Json().Str("id", id).Str("status", status).Str("summary", summary).Raw("data", data_json).Done();
    char* copy = strdup(json.c_str());
    // Runs on the page's main thread; the page frees the copy.
    MAIN_THREAD_ASYNC_EM_ASM({
        var text = UTF8ToString($0);
        _free($0);
        if (globalThis.labReport) globalThis.labReport(JSON.parse(text));
        else console.log('[lab] early report', text);
    }, copy);
}

void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    char* copy = strdup(buf);
    MAIN_THREAD_ASYNC_EM_ASM({
        var text = UTF8ToString($0);
        _free($0);
        if (globalThis.labLog) globalThis.labLog(text);
        else console.log('[lab]', text);
    }, copy);
}

std::string Fmt(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

EM_JS(double, lab_epoch_ms, (), { return performance.timeOrigin + performance.now(); });

double EpochMs() { return lab_epoch_ms(); }
double NowMs() { return emscripten_get_now(); }

Stats Summarize(std::vector<double> values)
{
    Stats s;
    s.count = values.size();
    if (values.empty())
        return s;
    std::sort(values.begin(), values.end());
    s.min = values.front();
    s.max = values.back();
    s.median = values[values.size() / 2];
    s.p95 = values[std::min(values.size() - 1, size_t(std::ceil(values.size() * 0.95)) - 1)];
    double sum = 0;
    for (double v : values)
        sum += v;
    s.mean = sum / double(values.size());
    return s;
}

std::string StatsJson(const Stats& s)
{
    return Json()
        .Num("min", s.min)
        .Num("median", s.median)
        .Num("p95", s.p95)
        .Num("max", s.max)
        .Num("mean", s.mean)
        .Int("count", int64_t(s.count))
        .Done();
}
} // namespace lab
