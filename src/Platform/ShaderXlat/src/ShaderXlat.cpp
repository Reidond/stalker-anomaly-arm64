#include "ShaderXlat/ShaderXlat.h"

#include "Internal.h"

#include <chrono>
#include <map>
#include <set>

namespace shaderxlat
{
namespace
{
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Sits between the frontend and the caller's provider: normalises directive paths, applies the
// FXC-compat hooks to every header and records what was read.
class TrackingIncludes final : public IncludeProvider
{
public:
    TrackingIncludes(IncludeProvider* inner, bool compat, Result& result) : inner_(inner), compat_(compat), result_(result) {}

    bool open(const std::string& rawName, const std::string& from, std::string& resolved, std::string& contents) override
    {
        if (!inner_)
            return false;
        std::string name = rawName;
        for (char& c : name)
            if (c == '\\')
                c = '/';
        while (name.compare(0, 2, "./") == 0)
            name.erase(0, 2);
        if (!inner_->open(name, from, resolved, contents))
            return false;
        const uint64_t contentHash = detail::fnv1a64(contents.data(), contents.size());
        // The shipped shader set contains a file that includes itself; FXC and DXC stop at a fixed
        // nesting depth, vkd3d-shader would recurse forever. A file is legitimately opened many times
        // (once per directive), so cap opens per file rather than tracking nesting.
        if (++openCount_[resolved] > kMaxOpensPerFile)
            return false;
        if (seen_.insert(resolved).second)
            result_.dependencies.push_back({name, resolved, contentHash});
        if (compat_)
            detail::applyFxcCompat(contents, resolved, result_.applied_hooks);
        return true;
    }

private:
    IncludeProvider* inner_;
    bool compat_;
    Result& result_;
    std::set<std::string> seen_;
    std::map<std::string, int> openCount_;
    static constexpr int kMaxOpensPerFile = 256;
};
} // namespace

namespace detail
{
std::string defaultProfile(Stage stage)
{
    switch (stage)
    {
    case Stage::Vertex: return "vs_5_0";
    case Stage::Pixel: return "ps_5_0";
    case Stage::Geometry: return "gs_5_0";
    case Stage::Hull: return "hs_5_0";
    case Stage::Domain: return "ds_5_0";
    case Stage::Compute: return "cs_5_0";
    }
    return "ps_5_0";
}
} // namespace detail

Result translate(const std::string& source, const std::string& sourceName, const std::vector<Macro>& macros,
                 const Options& options, IncludeProvider* includes)
{
    const auto tStart = Clock::now();
    Result result;

    if (options.stage == Stage::Geometry || options.stage == Stage::Hull || options.stage == Stage::Domain)
    {
        result.category = Category::UnsupportedStage;
        result.log = "Metal has no geometry/tessellation shader stages; this shader needs a renderer-side replacement";
        result.timings.total_ms = msSince(tStart);
        return result;
    }

    // Preprocess: FXC-compat hooks on the root source. Headers go through TrackingIncludes.
    const auto tPre = Clock::now();
    std::string text = source;
    if (options.fxc_compat)
        detail::applyFxcCompat(text, sourceName, result.applied_hooks);
    TrackingIncludes tracker(includes, options.fxc_compat, result);
    result.timings.preprocess_ms = msSince(tPre);

    detail::FrontendInput in;
    in.source = &text;
    in.name = &sourceName;
    in.macros = &macros;
    in.options = &options;
    in.includes = &tracker;
    in.profile = options.profile.empty() ? detail::defaultProfile(options.stage) : options.profile;

    const auto tFront = Clock::now();
    detail::FrontendOutput fo;
    const bool frontOk = detail::frontendCompile(in, fo);
    result.timings.frontend_ms = msSince(tFront);
    result.log = fo.log;
    if (!frontOk)
    {
        result.category = Category::FrontendError;
        result.timings.total_ms = msSince(tStart);
        return result;
    }

    result.reflection = std::move(fo.d3d);
    const auto tCross = Clock::now();
    const bool crossOk = detail::crossCompile(fo.spirv, options, result);
    result.timings.cross_ms = msSince(tCross);
    if (!crossOk)
    {
        if (result.category == Category::None)
            result.category = Category::CrossError;
        result.timings.total_ms = msSince(tStart);
        return result;
    }
    if (options.keep_spirv)
        result.spirv = std::move(fo.spirv);
    result.ok = true;
    result.timings.total_ms = msSince(tStart);
    return result;
}

Result retarget(const Result& translated, const Options& options)
{
    const auto t0 = Clock::now();
    Result result;
    result.dependencies = translated.dependencies;
    result.applied_hooks = translated.applied_hooks;
    result.reflection = translated.reflection;
    if (translated.spirv.empty())
    {
        result.category = Category::InternalError;
        result.log = "retarget() needs Result::spirv; translate with Options::keep_spirv";
        return result;
    }
    result.ok = detail::crossCompile(translated.spirv, options, result);
    if (!result.ok && result.category == Category::None)
        result.category = Category::CrossError;
    result.timings.cross_ms = result.timings.total_ms = msSince(t0);
    return result;
}

const char* version() { return "ShaderXlat 0.1 (vkd3d-shader 2.1, SPIRV-Cross vulkan-sdk-1.4.363.0)"; }
} // namespace shaderxlat
