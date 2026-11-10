// shader_batch: compiles every shader entry the X-Ray R4 renderer can request, across a set of
// macro permutations, with ShaderXlat, validates the produced MSL with `xcrun metal` for macOS and
// iOS, and reports pass/fail per file and stage together with timings.
//
//   shader_batch --root <dir> [--root <dir> ...] --out <dir> [options]
//
// Each --root is a "shaders" directory (containing r3/); earlier roots override later ones, like
// loose gamedata\shaders overriding shaders.db0. Nothing under the roots is ever written.
#include "profiles.h"

#include "ShaderXlat/ShaderXlat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace fs = std::filesystem;
using shaderxlat::Stage;

namespace
{
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ---------------------------------------------------------------- file access

std::vector<std::string> g_roots;

bool readFile(const fs::path& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// The engine's includer: "<shader dir>/<name>" (shader dir = "r3/") first, then "<name>"; loose
// overlay roots shadow archive roots.
class RootIncludes final : public shaderxlat::IncludeProvider
{
public:
    bool open(const std::string& name, const std::string&, std::string& resolved, std::string& contents) override
    {
        for (const char* prefix : {"r3/", ""})
            for (const std::string& root : g_roots)
            {
                fs::path p = fs::path(root) / (std::string(prefix) + name);
                std::error_code ec;
                if (fs::is_regular_file(p, ec) && readFile(p, contents))
                {
                    resolved = (fs::path(prefix) / name).lexically_normal().string();
                    return true;
                }
            }
        return false;
    }
};

// ---------------------------------------------------------------- jobs

struct Entry
{
    std::string rel;   // path relative to r3/, e.g. "accum_sun.ps" or "dx11/tess.hs"
    Stage stage;
    std::string name;  // without extension
};

const char* stageName(Stage s)
{
    switch (s)
    {
    case Stage::Vertex: return "vs";
    case Stage::Pixel: return "ps";
    case Stage::Geometry: return "gs";
    case Stage::Hull: return "hs";
    case Stage::Domain: return "ds";
    case Stage::Compute: return "cs";
    }
    return "?";
}

bool stageFromExt(const std::string& ext, Stage& s)
{
    if (ext == ".vs") s = Stage::Vertex;
    else if (ext == ".ps") s = Stage::Pixel;
    else if (ext == ".gs") s = Stage::Geometry;
    else if (ext == ".hs") s = Stage::Hull;
    else if (ext == ".ds") s = Stage::Domain;
    else if (ext == ".cs") s = Stage::Compute;
    else return false;
    return true;
}

struct Job
{
    Entry entry;
    std::string profile;
    int skin = -1;
    std::vector<shaderxlat::Macro> macros;
    // --requests: the request as r4_shader_warmup --export-requests wrote it
    std::string entryPoint = "main", target, request; // request: "<name><ext>[ skin N]"
};

// A request exported by r4_shader_warmup that is not a job: missing from the data, or skipped like the Metal path skips it.
struct OtherRequest
{
    std::string label, status, detail;
};

enum class MetalState : uint8_t { NotRun, Ok, Fail };

// What tools/shader_batch/wgsl_main.cpp (shader_wgsl) reports for one job.
struct WgslReport
{
    bool run = false, ok = false, crashed = false;
    std::string failure, firstError, log;
    bool derivativeUniformity = false, discard = false, depthNonComparison = false, textureLoadDepth = false;
    uint32_t clipDistances = 0;
    uint32_t workgroup[4] = {0, 0, 0, 0}; // x, y, z, storage bytes
    uint32_t isNanRewrites = 0, nonFiniteConstants = 0;
    std::vector<std::string> enables, limits;
    struct Binding
    {
        uint32_t group = 0, binding = 0;
        std::string kind, dim, sampled, format;
        char cls = '?';
        uint32_t reg = 0;
        std::string name;
    };
    std::vector<Binding> bindings;
    struct Pair { uint32_t sg, sb, tg, tb; };
    std::vector<Pair> pairs;
    struct Var { uint32_t location; std::string type, interpolation; };
    std::vector<Var> inputs, outputs;
    double ms = 0;     // in the helper (patch + Tint + validation)
    double wallMs = 0; // including the process spawn
};

struct JobResult
{
    Job job;
    bool ok = false;
    shaderxlat::Category category = shaderxlat::Category::None;
    std::string errorLine;      // first error, for grouping
    std::string fullLog;
    shaderxlat::Timings timings;
    size_t mslBytes = 0;
    MetalState metalMac = MetalState::NotRun, metalIos = MetalState::NotRun;
    std::string metalLogMac, metalLogIos;
    double metalMacMs = 0, metalIosMs = 0;
    std::vector<std::string> hooks;
    size_t cbCount = 0, texCount = 0, samplerCount = 0;
    shaderxlat::Reflection refl;
    bool notAnEntry = false; // include-only file with no `main`
    bool cacheHit = false;
    double iosCrossMs = 0;
    WgslReport wgsl;
};

std::string firstErrorLine(const std::string& log)
{
    std::istringstream is(log);
    std::string line;
    while (std::getline(is, line))
        if (line.find("error") != std::string::npos || line.find("Error") != std::string::npos || line.find("crashed") != std::string::npos ||
            std::regex_search(line, std::regex(R"re(:\s*E\d+:)re")))
            return line;
    std::istringstream again(log);
    std::getline(again, line);
    return line;
}

// ---------------------------------------------------------------- metal validation

struct Proc
{
    int status = -1;
    std::string output;
};

Proc run(const std::vector<std::string>& args)
{
    Proc r;
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return r;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    std::vector<char*> argv;
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ) != 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        posix_spawn_file_actions_destroy(&fa);
        return r;
    }
    close(pipefd[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0)
        r.output.append(buf, (size_t)n);
    close(pipefd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    r.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    posix_spawn_file_actions_destroy(&fa);
    return r;
}

bool validateMsl(const fs::path& file, bool ios, std::string& log, double& ms)
{
    const auto t0 = Clock::now();
    std::vector<std::string> a = {"xcrun", "-sdk", ios ? "iphoneos" : "macosx", "metal", "-c", "-std=metal3.0",
                                  ios ? "-mios-version-min=17.0" : "-mmacosx-version-min=14.0",
                                  "-Wno-unused-variable", file.string(), "-o", "/dev/null"};
    Proc p = run(a);
    ms = msSince(t0);
    log = p.output;
    return p.status == 0;
}

// ---------------------------------------------------------------- WGSL (Tint) validation

// WebGPU @location of an interstage varying. D3D links vertex outputs to pixel inputs by semantic; WebGPU by location,
// and only maxInterStageShaderVariables (16 by default) locations exist, so the stock semantics get a fixed compact
// table. Anything outside it lands at 16 and above, where the limit check reports it.
uint32_t wgslInterstageLocation(const std::string& sem, uint32_t index)
{
    struct Row { const char* name; uint32_t base; uint32_t count; };
    // The stock shaders link TEXCOORD0..7, COLOR0..1, FOG, POSITION and POSITIONT between stages (the sweep's
    // "Interstage semantics" section); TEXCOORD8/9 and NORMAL take the spare slots.
    static const Row rows[] = {{"TEXCOORD", 0, 10}, {"COLOR", 10, 2}, {"FOG", 12, 1}, {"POSITION", 13, 1}, {"POSITIONT", 14, 1}, {"NORMAL", 15, 1}};
    for (const Row& r : rows)
        if (sem == r.name && index < r.count)
            return r.base + index;
    uint32_t h = 2166136261u;
    for (char c : sem)
        h = (h ^ (uint8_t)c) * 16777619u;
    return 16 + (h + index) % 16;
}

// Register -> location for the interstage side of a stage (vertex outputs, pixel inputs). Where two semantics share a
// register (D3D packing), the first one names it, as in the MSL path.
std::vector<std::pair<uint32_t, uint32_t>> interstageMap(const shaderxlat::Reflection& r, Stage stage)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (stage != Stage::Vertex && stage != Stage::Pixel)
        return out;
    std::set<uint32_t> seen;
    for (const auto& v : stage == Stage::Vertex ? r.outputs : r.inputs)
        if (!v.system_value && seen.insert(v.reg).second)
            out.push_back({v.reg, wgslInterstageLocation(v.semantic, v.semantic_index)});
    return out;
}

void parseWgslReport(const std::string& text, WgslReport& w)
{
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line))
    {
        const size_t sp = line.find(' ');
        const std::string key = line.substr(0, sp), val = sp == std::string::npos ? "" : line.substr(sp + 1);
        std::istringstream vs(val);
        if (key == "ok") w.ok = val == "1";
        else if (key == "failure") w.failure = val;
        else if (key == "ms") w.ms = std::atof(val.c_str());
        else if (key == "derivative_uniformity") w.derivativeUniformity = val == "1";
        else if (key == "discard") w.discard = val == "1";
        else if (key == "clip_distances") w.clipDistances = (uint32_t)std::atoi(val.c_str());
        else if (key == "depth_non_comparison_sampler") w.depthNonComparison = val == "1";
        else if (key == "texture_load_depth") w.textureLoadDepth = val == "1";
        else if (key == "workgroup") vs >> w.workgroup[0] >> w.workgroup[1] >> w.workgroup[2] >> w.workgroup[3];
        else if (key == "rewrites") vs >> w.isNanRewrites >> w.nonFiniteConstants;
        else if (key == "enable") w.enables.push_back(val);
        else if (key == "limit") w.limits.push_back(val);
        else if (key == "binding")
        {
            WgslReport::Binding b;
            vs >> b.group >> b.binding >> b.kind >> b.dim >> b.sampled >> b.format >> b.cls >> b.reg >> b.name;
            w.bindings.push_back(b);
        }
        else if (key == "pair")
        {
            WgslReport::Pair p{};
            vs >> p.sg >> p.sb >> p.tg >> p.tb;
            w.pairs.push_back(p);
        }
        else if (key == "in" || key == "out")
        {
            WgslReport::Var v;
            vs >> v.location >> v.type >> v.interpolation;
            (key == "in" ? w.inputs : w.outputs).push_back(v);
        }
        else if (key == "log")
        {
            w.log += val + "\n";
            if (w.firstError.empty() && val.find("error") != std::string::npos)
                w.firstError = val;
        }
    }
    if (w.firstError.empty() && !w.ok)
    {
        std::istringstream again(w.log);
        std::getline(again, w.firstError);
    }
}

// Groups Tint messages: drop the "file:line:col" prefix, numbers and quoted identifiers.
std::string wgslErrorKey(const std::string& failure, const std::string& line)
{
    std::string s = std::regex_replace(line, std::regex(R"(^[^ ]*:\d+:\d+(-\d+)?:?\s*)"), "");
    s = std::regex_replace(s, std::regex(R"('[^']*')"), "'…'");
    s = std::regex_replace(s, std::regex(R"(\b\d+\b)"), "N");
    s = std::regex_replace(s, std::regex(R"(%[A-Za-z0-9_]+)"), "%x");
    if (s.size() > 220)
        s = s.substr(0, 220) + "…";
    return failure + ": " + s;
}

// ---------------------------------------------------------------- engine request scan

struct EngineUse
{
    std::set<std::string> requested; // "<name>.<ext>" literals from blenders and .s scripts
    std::vector<std::string> gsUse, tessUse, csUse; // "file:line  text"
    std::set<std::pair<std::string, std::string>> pairs; // (vs, ps) names requested together
};

EngineUse scanEngine(const std::vector<std::string>& srcDirs, const std::vector<std::string>& roots)
{
    EngineUse eu;
    // 1. .s scripts: shader:begin("vs","ps")
    std::regex begin(R"re(begin\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)")re");
    for (const std::string& root : roots)
    {
        std::error_code ec;
        for (auto& de : fs::recursive_directory_iterator(fs::path(root) / "r3", ec))
        {
            if (de.path().extension() != ".s")
                continue;
            std::string txt;
            readFile(de.path(), txt);
            for (std::sregex_iterator it(txt.begin(), txt.end(), begin), e; it != e; ++it)
            {
                eu.requested.insert((*it)[1].str() + ".vs");
                eu.requested.insert((*it)[2].str() + ".ps");
                eu.pairs.insert({(*it)[1].str(), (*it)[2].str()});
            }
        }
    }
    // 2. C++ blenders: r_Pass("vs","ps") / r_Pass("vs","gs","ps") / r_TessPass(...)
    std::regex pass2(R"re(r_Pass\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)")re");
    std::regex pass3(R"re(r_Pass\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)")re");
    std::regex tess(R"re(r_TessPass\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]*)"\s*,\s*"([^"]+)")re");
    for (const std::string& dir : srcDirs)
    {
        std::error_code ec;
        for (auto& de : fs::recursive_directory_iterator(dir, ec))
        {
            const auto ext = de.path().extension();
            if (ext != ".cpp" && ext != ".h")
                continue;
            std::ifstream f(de.path());
            std::string line;
            int ln = 0;
            while (std::getline(f, line))
            {
                ++ln;
                std::smatch m;
                const std::string where = de.path().filename().string() + ":" + std::to_string(ln) + "  ";
                if (std::regex_search(line, m, tess))
                {
                    eu.requested.insert(m[1].str() + ".vs");
                    eu.requested.insert(m[2].str() + ".hs");
                    eu.requested.insert(m[3].str() + ".ds");
                    if (!m[4].str().empty())
                        eu.requested.insert(m[4].str() + ".gs");
                    eu.requested.insert(m[5].str() + ".ps");
                    eu.tessUse.push_back(where + line);
                }
                else if (std::regex_search(line, m, pass3) && line.find("D3DBLEND") == std::string::npos)
                {
                    eu.pairs.insert({m[1].str(), m[3].str()});
                    // r_Pass(vs, gs, ps, ...) is the only 3-string overload
                    eu.requested.insert(m[1].str() + ".vs");
                    eu.requested.insert(m[2].str() + ".gs");
                    eu.requested.insert(m[3].str() + ".ps");
                    eu.gsUse.push_back(where + line);
                }
                else if (std::regex_search(line, m, pass2))
                {
                    eu.requested.insert(m[1].str() + ".vs");
                    eu.requested.insert(m[2].str() + ".ps");
                    eu.pairs.insert({m[1].str(), m[2].str()});
                }
                if (line.find("r_ComputePass") != std::string::npos)
                    eu.csUse.push_back(where + line);
            }
        }
    }
    return eu;
}

// ---------------------------------------------------------------- json

std::string jesc(const std::string& s)
{
    std::string o;
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) o += ' ';
        else o += (char)c;
    }
    return o;
}

const char* catName(shaderxlat::Category c)
{
    switch (c)
    {
    case shaderxlat::Category::None: return "none";
    case shaderxlat::Category::UnsupportedStage: return "unsupported_stage";
    case shaderxlat::Category::FrontendError: return "frontend_error";
    case shaderxlat::Category::CrossError: return "cross_error";
    case shaderxlat::Category::InternalError: return "internal_error";
    }
    return "?";
}

const char* scalarName(shaderxlat::ScalarType t)
{
    using T = shaderxlat::ScalarType;
    switch (t)
    {
    case T::Bool: return "bool";
    case T::Int: return "int";
    case T::UInt: return "uint";
    case T::Half: return "half";
    case T::Float: return "float";
    case T::Double: return "double";
    case T::Struct: return "struct";
    default: return "?";
    }
}

const char* dimName(shaderxlat::TextureDim d)
{
    using D = shaderxlat::TextureDim;
    static const char* n[] = {"1d", "1d_array", "2d", "2d_array", "2dms", "2dms_array", "3d", "cube", "cube_array", "buffer"};
    return n[(int)d];
}

void memberJson(std::ostream& o, const shaderxlat::ConstantMember& m)
{
    o << "{\"name\":\"" << jesc(m.name) << "\",\"offset\":" << m.offset << ",\"size\":" << m.size << ",\"type\":\"" << scalarName(m.type)
      << "\",\"rows\":" << m.rows << ",\"cols\":" << m.columns << ",\"array\":" << m.array_elements << ",\"array_stride\":" << m.array_stride
      << ",\"used\":" << (m.used ? "true" : "false") << ",\"type_name\":\"" << jesc(m.type_name) << "\",\"row_major\":" << (m.row_major ? "true" : "false");
    if (!m.members.empty())
    {
        o << ",\"members\":[";
        for (size_t i = 0; i < m.members.size(); ++i) { if (i) o << ","; memberJson(o, m.members[i]); }
        o << "]";
    }
    o << "}";
}

std::string reflectionJson(const shaderxlat::Reflection& r, const std::string& entry)
{
    std::ostringstream o;
    o << "{\"entry\":\"" << jesc(entry) << "\",\"constant_buffers\":[";
    for (size_t i = 0; i < r.constant_buffers.size(); ++i)
    {
        const auto& c = r.constant_buffers[i];
        o << (i ? "," : "") << "{\"name\":\"" << jesc(c.name) << "\",\"slot\":" << c.slot << ",\"msl\":" << c.msl_index << ",\"size\":" << c.size << ",\"members\":[";
        for (size_t k = 0; k < c.members.size(); ++k) { if (k) o << ","; memberJson(o, c.members[k]); }
        o << "]}";
    }
    o << "],\"textures\":[";
    for (size_t i = 0; i < r.textures.size(); ++i)
    {
        const auto& t = r.textures[i];
        o << (i ? "," : "") << "{\"name\":\"" << jesc(t.name) << "\",\"slot\":" << t.slot << ",\"msl\":" << t.msl_index << ",\"dim\":\"" << dimName(t.dim)
          << "\",\"type\":\"" << scalarName(t.sampled_type) << "\",\"depth\":" << (t.depth ? "true" : "false") << ",\"uav\":" << (t.is_uav ? "true" : "false") << "}";
    }
    o << "],\"samplers\":[";
    for (size_t i = 0; i < r.samplers.size(); ++i)
        o << (i ? "," : "") << "{\"name\":\"" << jesc(r.samplers[i].name) << "\",\"slot\":" << r.samplers[i].slot << ",\"msl\":" << r.samplers[i].msl_index
          << ",\"comparison\":" << (r.samplers[i].comparison ? "true" : "false") << "}";
    o << "],\"pairs\":[";
    for (size_t i = 0; i < r.sampler_texture_pairs.size(); ++i)
        o << (i ? "," : "") << "[\"" << jesc(r.sampler_texture_pairs[i].sampler) << "\",\"" << jesc(r.sampler_texture_pairs[i].texture) << "\"]";
    auto vars = [&](const char* key, const std::vector<shaderxlat::StageVariable>& v) {
        o << "],\"" << key << "\":[";
        for (size_t i = 0; i < v.size(); ++i)
            o << (i ? "," : "") << "{\"semantic\":\"" << jesc(v[i].semantic) << "\",\"index\":" << v[i].semantic_index << ",\"active\":" << (v[i].active ? "true" : "false") << ",\"reg\":" << v[i].reg << ",\"mask\":" << v[i].mask << ",\"location\":" << v[i].location
              << ",\"type\":\"" << scalarName(v[i].type) << "\",\"components\":" << v[i].components << ",\"sv\":" << (v[i].system_value ? "true" : "false") << "}";
    };
    vars("inputs", r.inputs);
    vars("outputs", r.outputs);
    o << "],\"threads\":[" << r.threads[0] << "," << r.threads[1] << "," << r.threads[2] << "],\"writes_depth\":" << (r.writes_depth ? "true" : "false")
      << ",\"uses_discard\":" << (r.uses_discard ? "true" : "false") << "}";
    return o.str();
}

// ---------------------------------------------------------------- WGSL summary

std::string jobLabel(const JobResult& r)
{
    if (!r.job.request.empty())
        return r.job.request;
    return r.job.entry.rel + "@" + r.job.profile + (r.job.skin >= 0 ? "/skin" + std::to_string(r.job.skin) : "");
}

// The D3D name of a WGSL binding (via the register the binding number encodes).
std::string d3dName(const JobResult& r, const WgslReport::Binding& b)
{
    if (b.cls == 's')
        for (const auto& s : r.refl.samplers)
            if (s.slot == b.reg)
                return s.name;
    if (b.cls == 't' || b.cls == 'u')
        for (const auto& t : r.refl.textures)
            if (t.slot == b.reg && t.is_uav == (b.cls == 'u'))
                return t.name;
    if (b.cls == 'b')
        for (const auto& c : r.refl.constant_buffers)
            if (c.slot == b.reg)
                return c.name;
    return b.name;
}

// Sampler states by name, as CBlender_Compile::r_dx10Sampler sets them (Blender_Recorder_R3.cpp).
const char* samplerFilter(const std::string& name)
{
    if (name == "smp_smap")
        return "comparison";
    if (name == "smp_nofilter" || name == "smp_jitter" || name == "smp_point")
        return "nearest";
    return "linear";
}

template <class C>
void listSome(std::ostringstream& md, const C& items, size_t max = 12)
{
    size_t shown = 0;
    for (const auto& l : items)
    {
        if (shown++ == max)
        {
            md << "- ... (" << items.size() - max << " more)\n";
            break;
        }
        md << "- " << l << "\n";
    }
}

std::string wgslSummary(const std::vector<JobResult>& results, const std::vector<OtherRequest>& other, const EngineUse& eu,
                        const std::vector<batch::Profile>& profiles, bool runtimeSet)
{
    std::ostringstream md;
    md << "\n## WGSL (Tint)\n\n";
    struct Agg { size_t jobs = 0, translated = 0, wgsl = 0; double ms = 0; };
    std::map<std::string, Agg> byProfile, byStage;
    Agg all;
    std::map<std::string, std::vector<std::string>> groups, limitGroups;
    std::map<std::string, std::set<std::string>> feature; // feature -> files
    std::map<std::string, size_t> featureJobs, enables;
    std::map<char, uint32_t> maxReg;
    size_t maxBindingsPerStage = 0, cmpAgree = 0, cmpDisagree = 0;
    std::vector<std::string> cmpExamples;
    std::map<std::string, std::set<std::string>> depthFilter; // "texture via sampler (filter)" -> files
    std::map<std::string, size_t> semantics;
    std::map<std::string, bool> fileOk; // file -> WGSL ok in every job it has
    std::vector<double> times;
    for (const JobResult& r : results)
    {
        if (r.notAnEntry || r.category == shaderxlat::Category::UnsupportedStage)
            continue;
        const std::string stage = stageName(r.job.entry.stage);
        for (Agg* a : {&all, &byProfile[r.job.profile], &byStage[stage]})
        {
            ++a->jobs;
            a->translated += r.ok;
            a->wgsl += r.ok && r.wgsl.ok;
            a->ms += r.wgsl.ms;
        }
        auto fo = fileOk.find(r.job.entry.rel);
        const bool ok = r.ok && r.wgsl.ok;
        if (fo == fileOk.end()) fileOk[r.job.entry.rel] = ok; else fo->second = fo->second && ok;
        const std::string label = jobLabel(r);
        if (!r.ok)
            continue;
        if (!r.wgsl.ok)
        {
            groups[wgslErrorKey(r.wgsl.failure, r.wgsl.firstError)].push_back(label);
            continue;
        }
        times.push_back(r.wgsl.ms);
        const WgslReport& w = r.wgsl;
        auto mark = [&](const std::string& f) { feature[f].insert(r.job.entry.rel); ++featureJobs[f]; };
        if (w.derivativeUniformity) mark("needs `diagnostic(off, derivative_uniformity)`");
        if (w.isNanRewrites) mark("SPIR-V rewrite: `OpIsNan`/`OpIsInf` as bit tests");
        if (w.nonFiniteConstants) mark("SPIR-V rewrite: Inf/NaN constants as run-time values");
        if (w.workgroup[0]) mark("compute workgroup " + std::to_string(w.workgroup[0]) + "x" + std::to_string(w.workgroup[1]) + "x" +
                                 std::to_string(w.workgroup[2]) + ", " + std::to_string(w.workgroup[3]) + " bytes shared");
        if (w.discard) mark("`discard`");
        if (w.clipDistances) mark("`clip_distances` (" + std::to_string(w.clipDistances) + ")");
        if (w.depthNonComparison) mark("depth texture sampled with a non-comparison sampler (Tint inspector)");
        if (w.textureLoadDepth) mark("`textureLoad` on a depth texture");
        for (const auto& e : w.enables) ++enables[e];
        for (const auto& l : w.limits)
            limitGroups[std::regex_replace(l, std::regex(R"(\b\d+\b)"), "N")].push_back(label);
        maxBindingsPerStage = std::max(maxBindingsPerStage, w.bindings.size());
        bool hasCmp = false, hasDepth = false;
        for (const auto& b : w.bindings)
        {
            maxReg[b.cls] = std::max(maxReg[b.cls], b.reg);
            hasCmp |= b.kind == "sampler_comparison";
            hasDepth |= b.kind.rfind("depth", 0) == 0;
        }
        if (hasCmp) mark("comparison sampler (`sampler_comparison`)");
        if (hasDepth) mark("depth texture (`texture_depth_*`)");
        // agreement with the SPIRV-Cross analysis the MSL path uses (SamplerBinding::comparison, TextureBinding::depth)
        bool agree = true;
        std::string why;
        for (const auto& b : w.bindings)
        {
            if (b.cls == 's')
                for (const auto& s : r.refl.samplers)
                    if (s.slot == b.reg && s.comparison != (b.kind == "sampler_comparison"))
                    {
                        agree = false;
                        why = s.name + (s.comparison ? " comparison in MSL path, " : " plain in MSL path, ") + b.kind + " in WGSL";
                    }
            if (b.cls == 't')
                for (const auto& t : r.refl.textures)
                    if (!t.is_uav && t.slot == b.reg && t.depth != (b.kind.rfind("depth", 0) == 0))
                    {
                        agree = false;
                        why = t.name + (t.depth ? " depth in MSL path, " : " colour in MSL path, ") + b.kind + " in WGSL";
                    }
        }
        if (agree) ++cmpAgree;
        else
        {
            ++cmpDisagree;
            if (cmpExamples.size() < 12)
                cmpExamples.push_back(label + ": " + why);
        }
        // depth resources (the shadow map) read through a filtering sampler
        for (const auto& p : w.pairs)
        {
            const WgslReport::Binding *sb = nullptr, *tb = nullptr;
            for (const auto& b : w.bindings)
            {
                if (b.group == p.sg && b.binding == p.sb) sb = &b;
                if (b.group == p.tg && b.binding == p.tb) tb = &b;
            }
            if (!sb || !tb)
                continue;
            const std::string tex = d3dName(r, *tb), smp = d3dName(r, *sb);
            // s_smap: the shadow map, a depth format. s_smap_minmax (R32F) and s_half_depth (R32F on NVIDIA, R16F
            // elsewhere): 32-bit float targets, not filterable in WebGPU without the float32-filterable feature.
            if (tex != "s_smap" && tex != "s_smap_minmax" && tex != "s_half_depth" && tb->kind.rfind("depth", 0) != 0)
                continue;
            depthFilter[tex + " (" + tb->kind + ") with " + smp + " (" + samplerFilter(smp) + ")"].insert(r.job.entry.rel);
        }
        // interstage semantics actually linked
        const auto& sig = r.job.entry.stage == Stage::Vertex ? r.refl.outputs : r.job.entry.stage == Stage::Pixel ? r.refl.inputs : r.refl.outputs;
        if (r.job.entry.stage != Stage::Compute)
            for (const auto& v : sig)
                if (!v.system_value)
                    ++semantics[v.semantic + std::to_string(v.semantic_index)];
    }
    auto pct = [](size_t a, size_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
    char buf[64];
    snprintf(buf, sizeof buf, "%.2f", pct(all.wgsl, all.jobs));
    md << "- **valid WGSL: " << all.wgsl << " / " << all.jobs << " jobs (" << buf << " %)**";
    snprintf(buf, sizeof buf, "%.2f", pct(all.wgsl, all.translated));
    md << "; of the " << all.translated << " jobs that reach SPIR-V: " << all.wgsl << " (" << buf << " %)\n";
    size_t filesOk = 0;
    for (auto& [f, ok] : fileOk) filesOk += ok;
    md << "- files valid in every job: " << filesOk << " / " << fileOk.size() << "\n";
    std::sort(times.begin(), times.end());
    double sum = 0;
    for (double t : times) sum += t;
    if (!times.empty())
        md << "- Tint time per shader (patch + SPIR-V reader + WGSL writer + re-parse + lowering), ms: mean " << sum / times.size() << ", p50 "
           << times[times.size() / 2] << ", p99 " << times[std::min(times.size() - 1, size_t(0.99 * times.size()))] << ", max " << times.back() << "\n";
    md << "\n| profile | jobs | reach SPIR-V | valid WGSL | % of jobs |\n|---|---|---|---|---|\n";
    for (auto& [k, a] : byProfile)
    {
        snprintf(buf, sizeof buf, "%.2f", pct(a.wgsl, a.jobs));
        md << "| " << k << " | " << a.jobs << " | " << a.translated << " | " << a.wgsl << " | " << buf << " |\n";
    }
    md << "\n| stage | jobs | reach SPIR-V | valid WGSL |\n|---|---|---|---|\n";
    for (auto& [k, a] : byStage)
        md << "| " << k << " | " << a.jobs << " | " << a.translated << " | " << a.wgsl << " |\n";

    if (runtimeSet)
    {
        std::map<std::string, std::vector<std::string>> cls;
        for (const auto& o : other)
            cls[o.status == "missing" ? "missing from the data" : "skipped: " + o.detail].push_back(o.label);
        for (const JobResult& r : results)
        {
            std::string c;
            if (r.category == shaderxlat::Category::UnsupportedStage) c = "unsupported stage (GS/HS/DS)";
            else if (!r.ok) c = "HLSL front end fails: " + std::regex_replace(r.errorLine, std::regex(R"(^[^ ]*:\d+:\d+:\s*)"), "");
            else if (r.wgsl.ok) c = "valid WGSL";
            else c = "WGSL " + wgslErrorKey(r.wgsl.failure, r.wgsl.firstError);
            if (r.job.request.find("(overlay only)") != std::string::npos)
                c = "repo overlay only (missing from the game data): " + c;
            cls[c].push_back(jobLabel(r));
        }
        md << "\n### Runtime set (r4_shader_warmup requests at default settings)\n\n" << results.size() + other.size()
           << " requests; every one is classified below.\n\n| class | requests |\n|---|---|\n";
        for (auto& [k, v] : cls)
            md << "| " << k << " | " << v.size() << " |\n";
        md << "\n";
        for (auto& [k, v] : cls)
            if (k != "valid WGSL")
            {
                md << "**" << k << "** (" << v.size() << ")\n";
                listSome(md, v, 40);
                md << "\n";
            }
    }

    md << "\n### WGSL failure groups\n\n";
    std::vector<std::pair<std::string, std::vector<std::string>>> gl(groups.begin(), groups.end());
    std::sort(gl.begin(), gl.end(), [](auto& a, auto& b) { return a.second.size() > b.second.size(); });
    for (auto& [msg, labels] : gl)
    {
        std::set<std::string> files;
        for (const auto& l : labels) files.insert(l.substr(0, l.find('@')));
        md << "#### " << labels.size() << " jobs, " << files.size() << " files: " << msg << "\n";
        listSome(md, std::set<std::string>(labels.begin(), labels.end()));
        md << "\n";
    }

    md << "### Features in the valid WGSL\n\n| feature | jobs | files |\n|---|---|---|\n";
    for (auto& [f, files] : feature)
        md << "| " << f << " | " << featureJobs[f] << " | " << files.size() << " |\n";
    for (auto& [e, n] : enables)
        md << "| `enable " << e << ";` | " << n << " | |\n";
    md << "\nComparison/depth classification, Tint vs the SPIRV-Cross analysis of the MSL path: " << cmpAgree << " jobs agree, " << cmpDisagree
       << " differ.\n\n";
    listSome(md, cmpExamples);
    md << "\n#### Depth and 32-bit float resources and the samplers that read them\n\n";
    for (auto& [k, files] : depthFilter)
    {
        md << "- " << k << ": " << files.size() << " files\n";
        std::vector<std::string> fl(files.begin(), files.end());
        std::ostringstream inner;
        listSome(inner, fl, 8);
        std::string s = inner.str();
        s = std::regex_replace(s, std::regex("(^|\n)- "), "$1  - ");
        md << s;
    }
    md << "\n#### Entry-point limits (WebGPU defaults)\n\n";
    if (limitGroups.empty())
        md << "None exceeded.\n";
    for (auto& [k, labels] : limitGroups)
    {
        md << "- " << labels.size() << " x " << k << "\n";
        std::ostringstream inner;
        listSome(inner, std::set<std::string>(labels.begin(), labels.end()), 6);
        md << std::regex_replace(inner.str(), std::regex("(^|\n)- "), "$1  - ");
    }
    md << "\n#### Bindings\n\nHighest register per class: ";
    for (auto& [c, reg] : maxReg) md << c << reg << " ";
    md << "; most bindings in one shader: " << maxBindingsPerStage << "\n";
    md << "\n#### Interstage semantics (vertex outputs and pixel inputs, jobs using each)\n\n";
    for (auto& [s, n] : semantics) md << s << " (" << n << ", @location " << wgslInterstageLocation(s.substr(0, s.find_first_of("0123456789")), (uint32_t)std::atoi(s.c_str() + s.find_first_of("0123456789"))) << ") ";
    md << "\n";

    // vertex/pixel linking under WebGPU rules: every pixel input needs a vertex output at the same location with the same
    // component count, base type and interpolation (RenderPipeline.cpp).
    if (!eu.pairs.empty())
    {
        std::map<std::string, const JobResult*> idx;
        for (const JobResult& r : results)
            if (r.ok && r.wgsl.ok && r.job.skin < 0)
                idx[r.job.entry.rel + "@" + r.job.profile] = &r;
        size_t checked = 0, clean = 0;
        std::map<std::string, size_t> kinds;
        std::map<std::string, std::set<std::string>> ex;
        for (const auto& [vsName, psName] : eu.pairs)
            for (const auto& prof : profiles)
            {
                auto vi = idx.find(vsName + ".vs@" + prof.name), pi = idx.find(psName + ".ps@" + prof.name);
                if (vi == idx.end() || pi == idx.end())
                    continue;
                ++checked;
                bool bad = false;
                for (const auto& in : pi->second->wgsl.inputs)
                {
                    const WgslReport::Var* outv = nullptr;
                    for (const auto& o : vi->second->wgsl.outputs)
                        if (o.location == in.location)
                            outv = &o;
                    std::string kind;
                    if (!outv) kind = "pixel input with no vertex output";
                    else
                    {
                        auto comps = [](const std::string& t) { return t.rfind("vec", 0) == 0 ? t[3] - '0' : 1; };
                        auto base = [](const std::string& t) { auto lt = t.find('<'); return lt == std::string::npos ? t : t.substr(lt + 1, 3); };
                        if (comps(outv->type) != comps(in.type)) kind = "component count differs";
                        else if (base(outv->type) != base(in.type)) kind = "base type differs";
                        else if (outv->interpolation != in.interpolation) kind = "interpolation differs";
                    }
                    if (kind.empty())
                        continue;
                    bad = true;
                    ++kinds[kind];
                    ex[kind].insert(vsName + " -> " + psName + " @location " + std::to_string(in.location) + " (" + in.type +
                                    (outv ? " from " + outv->type : "") + ")");
                }
                clean += !bad;
            }
        md << "\n#### Vertex/pixel linking (pairs from blenders and .s scripts, WebGPU matching rules)\n\n" << checked
           << " (vs, ps, profile) combinations, " << clean << " link as they are.\n\n";
        for (auto& [k, n] : kinds)
        {
            md << "- " << n << " x " << k << " (" << ex[k].size() << " distinct)\n";
            std::ostringstream inner;
            listSome(inner, ex[k], 10);
            md << std::regex_replace(inner.str(), std::regex("(^|\n)- "), "$1  - ");
        }
    }
    return md.str();
}
} // namespace

int main(int argc, char** argv)
{
    std::string outDir = "shader_batch_out";
    std::vector<std::string> engineSrc;
    std::vector<std::string> profileFilter;
    std::string nameFilter, cacheDir;
    bool validate = true, keepMsl = false, dumpReflection = false, compat = true;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    // --target wgsl: SPIR-V -> WGSL with Tint through shader_wgsl (built with -DANOMALY_SHADERXLAT_WGSL=ON), instead of
    // validating MSL with xcrun metal.
    std::string target = "msl", wgslTool, requestsFile, wgslLocations = "semantic";
    bool keepWgsl = false, wgslRemap = true, wgslStrictUniformity = false, wgslPolyfill = true;
    std::vector<std::string> uavFormats;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--root") g_roots.push_back(next());
        else if (a == "--out") outDir = next();
        else if (a == "--engine-src") engineSrc.push_back(next());
        else if (a == "--profile") profileFilter.push_back(next());
        else if (a == "--filter") nameFilter = next();
        else if (a == "--no-validate") validate = false;
        else if (a == "--keep-msl") keepMsl = true;
        else if (a == "--reflect") dumpReflection = true;
        else if (a == "--cache") cacheDir = next();
        else if (a == "--no-compat") compat = false;
        else if (a == "--jobs") jobs = (unsigned)std::atoi(next().c_str());
        else if (a == "--target") target = next();
        else if (a == "--wgsl-tool") wgslTool = next();
        else if (a == "--keep-wgsl") keepWgsl = true;
        else if (a == "--requests") requestsFile = next();
        else if (a == "--wgsl-locations") wgslLocations = next();
        else if (a == "--wgsl-no-remap") wgslRemap = false;
        else if (a == "--wgsl-strict-uniformity") wgslStrictUniformity = true;
        else if (a == "--wgsl-no-polyfill") wgslPolyfill = false;
        else if (a == "--uav-format") uavFormats.push_back(next());
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (g_roots.empty() || (target != "msl" && target != "wgsl") || (wgslLocations != "semantic" && wgslLocations != "register"))
    {
        fprintf(stderr, "usage: shader_batch --root <shaders dir> [--root ...] --out <dir> [--engine-src <dir>] [--profile name]\n"
                        "                    [--filter substr] [--no-validate] [--keep-msl] [--reflect] [--no-compat] [--cache dir] [--jobs N]\n"
                        "                    [--target msl|wgsl] [--wgsl-tool path] [--keep-wgsl] [--requests file]\n"
                        "                    [--wgsl-locations semantic|register] [--wgsl-no-remap] [--wgsl-strict-uniformity]\n"
                        "                    [--wgsl-no-polyfill]\n"
                        "                    [--uav-format U=FMT]\n");
        return 2;
    }
    const bool wgslMode = target == "wgsl";
    if (wgslMode)
    {
        validate = false; // no xcrun metal; the WGSL is validated by Tint instead
        if (wgslTool.empty())
            wgslTool = (fs::path(argv[0]).parent_path() / "shader_wgsl").string();
        std::error_code ec;
        if (!fs::is_regular_file(wgslTool, ec))
        {
            fprintf(stderr, "--target wgsl needs %s (configure with -DANOMALY_SHADERXLAT_WGSL=ON and build shader_wgsl)\n",
                    wgslTool.c_str());
            return 2;
        }
    }
    for (const std::string& r : g_roots)
        for (const auto& part : fs::path(fs::weakly_canonical(outDir)))
            if (part == "game") { fprintf(stderr, "refusing to write below a 'game' directory\n"); return 2; }

    // ---- enumerate entries (union over roots, relative to r3/)
    std::map<std::string, Entry> entries;
    for (const std::string& root : g_roots)
    {
        std::error_code ec;
        const fs::path r3 = fs::path(root) / "r3";
        for (auto& de : fs::recursive_directory_iterator(r3, ec))
        {
            if (!de.is_regular_file())
                continue;
            Stage st;
            if (!stageFromExt(de.path().extension().string(), st))
                continue;
            const std::string rel = fs::relative(de.path(), r3).generic_string();
            if (nameFilter.size() && rel.find(nameFilter) == std::string::npos)
                continue;
            entries.emplace(rel, Entry{rel, st, fs::path(rel).replace_extension().generic_string()});
        }
    }

    // ---- which VS files take SKIN_* variants
    std::set<std::string> skinned;
    for (auto& [rel, e] : entries)
    {
        if (e.stage != Stage::Vertex)
            continue;
        std::string txt;
        RootIncludes inc;
        std::string resolved;
        if (inc.open(rel, "", resolved, txt) && (txt.find("skin.h") != std::string::npos || txt.find("SKIN_") != std::string::npos ||
                                                  e.name.find("model") != std::string::npos))
            skinned.insert(rel);
    }

    // ---- build the job list
    std::vector<Job> joblist;
    std::vector<batch::Profile> profiles = batch::allProfiles();
    std::vector<OtherRequest> otherRequests;
    if (!requestsFile.empty())
    {
        // The runtime set: what r4_shader_warmup --export-requests wrote, one request per line:
        //   name \t ext \t skin \t status \t entry \t target \t origin [\t NAME=VALUE]...
        // with the exact macro list CRender::shader_compile builds at default settings.
        std::ifstream rf(requestsFile);
        if (!rf)
        {
            fprintf(stderr, "cannot read %s\n", requestsFile.c_str());
            return 2;
        }
        profiles = {batch::Profile{"runtime", {}}};
        std::string line;
        while (std::getline(rf, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            std::vector<std::string> f;
            std::istringstream ls(line);
            for (std::string field; std::getline(ls, field, '\t');)
                f.push_back(field);
            if (f.size() < 7)
                continue;
            const std::string name = f[0], ext = f[1], status = f[3];
            const int skin = std::atoi(f[2].c_str());
            const std::string label = name + ext + (skin >= 0 ? " skin " + std::to_string(skin) : "");
            if (nameFilter.size() && label.find(nameFilter) == std::string::npos)
                continue;
            std::string file = name;
            if (const size_t p = file.find('('); p != std::string::npos)
                file = file.substr(0, p);
            bool overlayOnly = false;
            if (status == "missing")
            {
                // missing from the game data the exporter mounted, but maybe present in one of our roots (the repo's
                // gamedata/shaders overlay ships files the archive lacks)
                RootIncludes inc;
                std::string resolved, text;
                overlayOnly = inc.open(file + ext, "", resolved, text) && f.size() > 7;
            }
            if (status != "ok" && !overlayOnly)
            {
                otherRequests.push_back({label, status, f[6]});
                continue;
            }
            Stage st;
            if (!stageFromExt(ext, st))
                continue;
            Job j;
            j.entry = Entry{file + ext, st, file};
            j.profile = "runtime";
            j.skin = skin;
            j.entryPoint = f[4];
            j.target = f[5];
            j.request = label + (overlayOnly ? " (overlay only)" : "");
            for (size_t k = 7; k < f.size(); ++k)
            {
                const size_t eq = f[k].find('=');
                if (eq != std::string::npos)
                    j.macros.push_back({f[k].substr(0, eq), f[k].substr(eq + 1)});
            }
            // the engine compiles a file once per distinct name; requests that differ only in parameters share it
            joblist.push_back(j);
        }
    }
    for (const auto& p : profiles)
    {
        if (!requestsFile.empty())
            break;
        const bool selected = profileFilter.empty() ? p.name != "msaa4" && p.name != "ssao4_hdao"
                                                    : std::find(profileFilter.begin(), profileFilter.end(), p.name) != profileFilter.end();
        if (!selected)
            continue;
        for (auto& [rel, e] : entries)
        {
            Job j;
            j.entry = e;
            j.profile = p.name;
            j.macros = batch::withSkin(p.macros, -1);
            joblist.push_back(j);
            // Skinned model vertex shaders are compiled once per skinning level in the first profile only.
            if (skinned.count(rel) && (p.name == profiles.front().name || profileFilter.size()))
                for (int s = 0; s <= 4; ++s)
                {
                    Job k = j;
                    k.skin = s;
                    k.macros = batch::withSkin(p.macros, s);
                    joblist.push_back(k);
                }
        }
    }
    printf("%zu entry files, %zu jobs, %u threads\n", entries.size(), joblist.size(), jobs);

    EngineUse eu;
    if (!engineSrc.empty())
        eu = scanEngine(engineSrc, g_roots);

    const fs::path out(outDir);
    fs::create_directories(out / (wgslMode ? "wgsl" : "msl"));

    // ---- run
    std::vector<JobResult> results(joblist.size());
    std::atomic<size_t> cursor{0}, done{0};
    std::mutex logMutex;
    std::unique_ptr<shaderxlat::TranslationCache> cache;
    if (!cacheDir.empty())
        cache = std::make_unique<shaderxlat::TranslationCache>(cacheDir);
    const auto t0 = Clock::now();
    auto worker = [&]() {
        RootIncludes inc;
        for (;;)
        {
            const size_t idx = cursor.fetch_add(1);
            if (idx >= joblist.size())
                break;
            const Job& job = joblist[idx];
            JobResult& r = results[idx];
            r.job = job;

            std::string src, resolved;
            if (!inc.open(job.entry.rel, "", resolved, src))
            {
                r.errorLine = "cannot read source";
                continue;
            }
            shaderxlat::Options o;
            o.stage = job.entry.stage;
            o.platform = shaderxlat::Platform::MacOS;
            o.keep_spirv = true;
            o.fxc_compat = compat;
            o.entry = job.entryPoint;
            o.profile = job.target;
            const std::string label = job.entry.name + "." + stageName(job.entry.stage) + "@" + job.profile +
                                      (job.skin >= 0 ? "/skin" + std::to_string(job.skin) : "");
            bool hit = false;
            shaderxlat::Result res = cache ? cache->translate(src, job.entry.rel, job.macros, o, &inc, &hit)
                                           : shaderxlat::translate(src, job.entry.rel, job.macros, o, &inc);
            r.cacheHit = hit;
            r.ok = res.ok;
            r.category = res.category;
            r.fullLog = res.log;
            r.errorLine = res.ok ? "" : firstErrorLine(res.log);
            r.timings = res.timings;
            r.hooks = res.applied_hooks;
            if (!res.ok && (res.log.find("missing entry point") != std::string::npos || res.log.find("Entry point") != std::string::npos))
                r.notAnEntry = true;
            if (res.ok)
            {
                r.mslBytes = res.msl.size();
                r.refl = res.reflection;
                r.cbCount = res.reflection.constant_buffers.size();
                r.texCount = res.reflection.textures.size();
                r.samplerCount = res.reflection.samplers.size();
                std::string safe = job.entry.name;
                std::replace(safe.begin(), safe.end(), '/', '_');
                const std::string base = safe + "." + stageName(job.entry.stage) + "." + job.profile +
                                         (job.skin >= 0 ? ".skin" + std::to_string(job.skin) : "");
                if (dumpReflection)
                {
                    std::lock_guard<std::mutex> lk(logMutex);
                    fs::create_directories(out / "reflection" / job.profile);
                    std::ofstream(out / "reflection" / job.profile / (base + ".json")) << reflectionJson(res.reflection, res.entry_point);
                }
                if (wgslMode && (job.entry.stage == Stage::Vertex || job.entry.stage == Stage::Pixel || job.entry.stage == Stage::Compute))
                {
                    const fs::path dir = out / "wgsl_tmp";
                    {
                        std::lock_guard<std::mutex> lk(logMutex);
                        fs::create_directories(dir);
                    }
                    const fs::path spv = dir / (base + ".spv"), wgslFile = dir / (base + ".wgsl");
                    std::ofstream(spv, std::ios::binary)
                        .write(reinterpret_cast<const char*>(res.spirv.data()), std::streamsize(res.spirv.size() * 4));
                    std::vector<std::string> args = {wgslTool, "--spv", spv.string(), "--stage", stageName(job.entry.stage), "--out",
                                                     wgslFile.string()};
                    if (wgslLocations == "semantic")
                        for (const auto& [reg, loc] : interstageMap(res.reflection, job.entry.stage))
                        {
                            args.push_back("--loc");
                            args.push_back(std::to_string(reg) + "=" + std::to_string(loc));
                        }
                    for (const std::string& u : uavFormats)
                    {
                        args.push_back("--uav-format");
                        args.push_back(u);
                    }
                    if (!wgslRemap)
                        args.push_back("--no-remap");
                    if (wgslStrictUniformity)
                        args.push_back("--strict-uniformity");
                    if (!wgslPolyfill)
                        args.push_back("--no-polyfill");
                    const auto tw = Clock::now();
                    Proc p = run(args);
                    r.wgsl.run = true;
                    r.wgsl.wallMs = msSince(tw);
                    parseWgslReport(p.output, r.wgsl);
                    if (p.status != 0 && p.status != 1)
                    {
                        // Tint aborted (internal compiler error) or the helper failed to start
                        r.wgsl.ok = false;
                        r.wgsl.crashed = true;
                        r.wgsl.failure = "crash";
                        std::string last;
                        std::istringstream os(p.output);
                        for (std::string l; std::getline(os, l);)
                            if (!l.empty())
                                last = l;
                        r.wgsl.firstError = last.empty() ? "exit status " + std::to_string(p.status) : last;
                        r.wgsl.log = p.output;
                    }
                    {
                        std::lock_guard<std::mutex> lk(logMutex);
                        std::error_code ec;
                        if (keepWgsl || !r.wgsl.ok)
                        {
                            fs::create_directories(out / "wgsl");
                            if (fs::exists(wgslFile, ec))
                                fs::copy_file(wgslFile, out / "wgsl" / wgslFile.filename(), fs::copy_options::overwrite_existing, ec);
                        }
                        if (!r.wgsl.ok)
                        {
                            // the failing SPIR-V, for other front ends (Naga) and for bug reports
                            fs::create_directories(out / "wgsl_failed_spirv");
                            fs::copy_file(spv, out / "wgsl_failed_spirv" / spv.filename(), fs::copy_options::overwrite_existing, ec);
                        }
                        fs::remove(spv, ec);
                        fs::remove(wgslFile, ec);
                    }
                }
                if (validate)
                {
                    fs::path dir = out / "msl_tmp";
                    {
                        std::lock_guard<std::mutex> lk(logMutex);
                        fs::create_directories(dir);
                    }
                    const fs::path f = dir / (base + ".metal");
                    { std::ofstream(f) << res.msl; }
                    r.metalMac = validateMsl(f, false, r.metalLogMac, r.metalMacMs) ? MetalState::Ok : MetalState::Fail;
                    // iOS: same SPIR-V, iOS flavoured MSL
                    shaderxlat::Options oi = o;
                    oi.platform = shaderxlat::Platform::IOS;
                    const auto ti = Clock::now();
                    shaderxlat::Result ri = shaderxlat::retarget(res, oi);
                    r.iosCrossMs = msSince(ti);
                    if (!ri.ok)
                    {
                        r.metalIos = MetalState::Fail;
                        r.metalLogIos = "ios cross-compile: " + ri.log;
                    }
                    else
                    {
                        const fs::path fi = dir / (base + ".ios.metal");
                        { std::ofstream(fi) << ri.msl; }
                        r.metalIos = validateMsl(fi, true, r.metalLogIos, r.metalIosMs) ? MetalState::Ok : MetalState::Fail;
                        if (keepMsl || r.metalIos == MetalState::Fail)
                        {
                            std::lock_guard<std::mutex> lk(logMutex);
                            fs::create_directories(out / "msl");
                            fs::copy_file(fi, out / "msl" / (base + ".ios.metal"), fs::copy_options::overwrite_existing);
                        }
                        std::error_code ec;
                        fs::remove(fi, ec);
                    }
                    if (keepMsl || r.metalMac == MetalState::Fail)
                    {
                        std::lock_guard<std::mutex> lk(logMutex);
                        fs::copy_file(f, out / "msl" / (base + ".metal"), fs::copy_options::overwrite_existing);
                    }
                    std::error_code ec;
                    fs::remove(f, ec);
                }
                else if (keepMsl)
                {
                    std::lock_guard<std::mutex> lk(logMutex);
                    std::ofstream(out / "msl" / (base + ".metal")) << res.msl;
                }
            }
            const size_t d = ++done;
            if (d % 100 == 0)
            {
                std::lock_guard<std::mutex> lk(logMutex);
                fprintf(stderr, "  %zu/%zu  (%.0fs)\n", d, joblist.size(), msSince(t0) / 1000);
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < jobs; ++i)
        pool.emplace_back(worker);
    for (auto& t : pool)
        t.join();
    const double wall = msSince(t0) / 1000;

    // ---- report
    struct Agg { size_t n = 0, ok = 0, metalOkMac = 0, metalOkIos = 0, metalRun = 0; double ms = 0, front = 0, cross = 0; };
    std::map<std::string, Agg> byProfile, byStage;
    size_t total = 0, ok = 0, notEntry = 0, unsupported = 0, metalMacOk = 0, metalIosOk = 0, metalRun = 0;
    std::map<std::string, std::vector<std::string>> failures; // grouped message -> labels
    std::vector<double> times;
    for (const JobResult& r : results)
    {
        if (r.notAnEntry)
        {
            ++notEntry;
            continue;
        }
        if (r.category == shaderxlat::Category::UnsupportedStage)
        {
            ++unsupported;
            continue;
        }
        ++total;
        Agg& ap = byProfile[r.job.profile];
        Agg& as = byStage[stageName(r.job.entry.stage)];
        for (Agg* a : {&ap, &as})
        {
            ++a->n;
            a->ok += r.ok;
            a->ms += r.timings.total_ms;
            a->front += r.timings.frontend_ms;
            a->cross += r.timings.cross_ms;
            if (r.metalMac != MetalState::NotRun)
            {
                ++a->metalRun;
                a->metalOkMac += r.metalMac == MetalState::Ok;
                a->metalOkIos += r.metalIos == MetalState::Ok;
            }
        }
        if (r.ok)
        {
            ++ok;
            times.push_back(r.timings.total_ms);
            if (r.metalMac != MetalState::NotRun)
            {
                ++metalRun;
                metalMacOk += r.metalMac == MetalState::Ok;
                metalIosOk += r.metalIos == MetalState::Ok;
            }
        }
        const std::string label = r.job.entry.rel + "@" + r.job.profile + (r.job.skin >= 0 ? "/skin" + std::to_string(r.job.skin) : "");
        if (!r.ok)
        {
            // Group by the message with file paths and numbers stripped.
            std::string key = std::regex_replace(r.errorLine, std::regex(R"(^[^ ]*:\d+:\d+:\s*)"), "");
            failures[key].push_back(label);
        }
        else
        {
            if (r.metalMac == MetalState::Fail)
                failures["metal(macOS): " + firstErrorLine(r.metalLogMac)].push_back(label);
            if (r.metalIos == MetalState::Fail)
                failures["metal(iOS): " + firstErrorLine(r.metalLogIos)].push_back(label);
        }
    }

    std::sort(times.begin(), times.end());
    auto pct = [&](double p) { return times.empty() ? 0.0 : times[std::min(times.size() - 1, (size_t)(p * times.size()))]; };
    double sum = 0;
    for (double t : times) sum += t;

    std::ostringstream md;
    md << "# shader_batch summary\n\n";
    md << "frontend: " << shaderxlat::frontend_name() << ", jobs run: " << results.size() << ", wall: " << wall << " s on " << jobs << " threads\n\n";
    md << "- translate pass: **" << ok << " / " << total << "** (" << (total ? 100.0 * ok / total : 0) << " %)  "
       << "[excluded: " << notEntry << " include-only files with no entry point, " << unsupported << " GS/HS/DS jobs]\n";
    if (validate)
        md << "- MSL accepted by `xcrun metal`: macOS **" << metalMacOk << " / " << metalRun << "**, iOS **" << metalIosOk << " / " << metalRun << "**\n";
    if (cache)
    {
        size_t hits = 0;
        for (const JobResult& r : results) hits += r.cacheHit;
        md << "- cache: " << hits << " hits / " << results.size() << " jobs\n";
    }
    md << "- translate time per shader (ms): mean " << (times.empty() ? 0 : sum / times.size()) << ", p50 " << pct(0.5)
       << ", p90 " << pct(0.9) << ", p99 " << pct(0.99) << ", max " << pct(1.0) << "\n\n";
    md << "| profile | jobs | translated | mean ms | frontend ms | cross ms | metal macOS | metal iOS |\n|---|---|---|---|---|---|---|---|\n";
    for (auto& [k, a] : byProfile)
        md << "| " << k << " | " << a.n << " | " << a.ok << " | " << (a.n ? a.ms / a.n : 0) << " | " << (a.n ? a.front / a.n : 0) << " | "
           << (a.n ? a.cross / a.n : 0) << " | " << a.metalOkMac << "/" << a.metalRun << " | " << a.metalOkIos << "/" << a.metalRun << " |\n";
    md << "\n| stage | jobs | translated | mean ms |\n|---|---|---|---|\n";
    for (auto& [k, a] : byStage)
        md << "| " << k << " | " << a.n << " | " << a.ok << " | " << (a.n ? a.ms / a.n : 0) << " |\n";

    md << "\n## Failure groups\n\n";
    std::vector<std::pair<std::string, std::vector<std::string>>> fl(failures.begin(), failures.end());
    std::sort(fl.begin(), fl.end(), [](auto& a, auto& b) { return a.second.size() > b.second.size(); });
    for (auto& [msg, labels] : fl)
    {
        md << "### " << labels.size() << " x " << msg << "\n";
        std::set<std::string> uniq(labels.begin(), labels.end());
        size_t shown = 0;
        for (const auto& l : uniq)
        {
            if (shown++ == 12) { md << "- ... (" << uniq.size() - 12 << " more)\n"; break; }
            md << "- " << l << "\n";
        }
        md << "\n";
    }
    if (notEntry)
    {
        md << "## Include-only files (no `main`)\n\n";
        std::set<std::string> names;
        for (const JobResult& r : results) if (r.notAnEntry) names.insert(r.job.entry.rel);
        for (const auto& n : names) md << "- " << n << "\n";
        md << "\n";
    }
    md << "## Stages Metal cannot run directly\n\n";
    {
        std::set<std::string> gs, tess, cs;
        for (auto& [rel, e] : entries)
        {
            if (e.stage == Stage::Geometry) gs.insert(rel);
            if (e.stage == Stage::Hull || e.stage == Stage::Domain) tess.insert(rel);
            if (e.stage == Stage::Compute) cs.insert(rel);
        }
        auto dump = [&](const char* title, const std::set<std::string>& files, const std::vector<std::string>& uses) {
            md << "**" << title << "** (" << files.size() << " files)\n";
            for (const auto& f : files) md << "- `" << f << "`" << (eu.requested.count(f) ? " (requested by engine/blender)" : "") << "\n";
            for (const auto& u : uses) md << "  - use: `" << u << "`\n";
            md << "\n";
        };
        dump("Geometry shaders", gs, eu.gsUse);
        dump("Tessellation (hull/domain)", tess, eu.tessUse);
        md << "**Compute** (" << cs.size() << " files)\n";
        for (const auto& f : cs) md << "- `" << f << "`\n";
        for (const auto& u : eu.csUse) md << "  - use: `" << u << "`\n";
        md << "\n";
    }
    if (!eu.requested.empty())
    {
        size_t reqOk = 0, reqTotal = 0;
        std::set<std::string> reqFail, reqMissing;
        std::map<std::string, bool> okByFile;
        for (const JobResult& r : results)
            if (!r.notAnEntry && r.category != shaderxlat::Category::UnsupportedStage)
            {
                auto it = okByFile.find(r.job.entry.rel);
                if (it == okByFile.end()) okByFile[r.job.entry.rel] = r.ok; else it->second = it->second && r.ok;
            }
        for (const auto& name : eu.requested)
        {
            auto it = okByFile.find(name);
            if (it == okByFile.end()) { reqMissing.insert(name); continue; }
            ++reqTotal;
            if (it->second) ++reqOk; else reqFail.insert(name);
        }
        md << "## Engine-requested names (blenders + .s scripts)\n\n";
        std::set<std::string> noFile, wrongStage;
        for (const auto& n : reqMissing)
            (entries.count(n) ? wrongStage : noFile).insert(n);
        md << reqOk << " / " << reqTotal << " requested entry files translate in every profile.\n\n";
        md << wrongStage.size() << " requested files are GS/HS/DS stages that Metal cannot run (listed below), and " << noFile.size()
           << " requested names have no file in the shader tree. A missing vertex shader makes the engine fall back to "
              "`stub_default.vs`; a missing pixel shader is a hard error in the original engine too.\n\n";
        for (const auto& n : noFile) md << "- no file: `" << n << "`\n";
        for (const auto& n : wrongStage) md << "- unsupported stage: `" << n << "`\n";
        for (const auto& n : reqFail) md << "- fails: `" << n << "`\n";
        md << "\n";
    }
    if (!eu.pairs.empty())
    {
        // Index results by (file, profile) for non-skinned jobs.
        std::map<std::string, const JobResult*> idx;
        for (const JobResult& r : results)
            if (r.ok && r.job.skin < 0)
                idx[r.job.entry.rel + "@" + r.job.profile] = &r;
        size_t checked = 0, clean = 0, missingOut = 0, wider = 0;
        std::map<std::string, size_t> examples;
        for (const auto& [vsName, psName] : eu.pairs)
            for (const auto& prof : profiles)
            {
                auto vi = idx.find(vsName + ".vs@" + prof.name), pi = idx.find(psName + ".ps@" + prof.name);
                if (vi == idx.end() || pi == idx.end())
                    continue;
                ++checked;
                bool bad = false;
                for (const std::string& problem : shaderxlat::checkStageInterface(vi->second->refl, pi->second->refl))
                {
                    bad = true;
                    const bool missing = problem.find("not written") != std::string::npos;
                    ++(missing ? missingOut : wider);
                    if (examples.size() < 40)
                        examples[vsName + " -> " + psName + " : " + problem] += 1;
                }
                clean += !bad;
            }
        md << "\n## Vertex/pixel interface check (pairs requested by blenders and .s scripts)\n\n";
        md << checked << " (vs, ps, profile) combinations checked, " << clean << " clean. Pixel inputs without a vertex output: " << missingOut
           << ", width/type mismatches: " << wider << ".\n\n";
        for (const auto& [k, n] : examples) md << "- " << k << (n > 1 ? " (x" + std::to_string(n) + ")" : "") << "\n";
        md << "\n";
    }
    if (wgslMode)
        md << wgslSummary(results, otherRequests, eu, profiles, !requestsFile.empty());
    { std::ofstream(out / "summary.md") << md.str(); }

    // per-job JSON lines
    {
        std::ofstream js(out / "results.jsonl");
        for (const JobResult& r : results)
        {
            js << "{\"file\":\"" << jesc(r.job.entry.rel) << "\",\"stage\":\"" << stageName(r.job.entry.stage) << "\",\"profile\":\""
               << r.job.profile << "\",\"skin\":" << r.job.skin << ",\"ok\":" << (r.ok ? "true" : "false") << ",\"category\":\"" << catName(r.category)
               << "\",\"not_an_entry\":" << (r.notAnEntry ? "true" : "false") << ",\"error\":\"" << jesc(r.errorLine)
               << "\",\"total_ms\":" << r.timings.total_ms << ",\"frontend_ms\":" << r.timings.frontend_ms << ",\"cross_ms\":" << r.timings.cross_ms
               << ",\"msl_bytes\":" << r.mslBytes << ",\"metal_macos\":\"" << (r.metalMac == MetalState::Ok ? "ok" : r.metalMac == MetalState::Fail ? "fail" : "-")
               << "\",\"metal_ios\":\"" << (r.metalIos == MetalState::Ok ? "ok" : r.metalIos == MetalState::Fail ? "fail" : "-")
               << "\",\"hooks\":[";
            for (size_t i = 0; i < r.hooks.size(); ++i) js << (i ? "," : "") << "\"" << jesc(r.hooks[i]) << "\"";
            js << "]";
            if (!r.job.request.empty())
                js << ",\"request\":\"" << jesc(r.job.request) << "\"";
            if (r.wgsl.run)
            {
                const WgslReport& w = r.wgsl;
                js << ",\"wgsl\":{\"ok\":" << (w.ok ? "true" : "false") << ",\"failure\":\"" << jesc(w.failure) << "\",\"error\":\""
                   << jesc(w.firstError) << "\",\"ms\":" << w.ms << ",\"wall_ms\":" << w.wallMs
                   << ",\"derivative_uniformity\":" << (w.derivativeUniformity ? "true" : "false")
                   << ",\"discard\":" << (w.discard ? "true" : "false") << ",\"clip_distances\":" << w.clipDistances
                   << ",\"depth_non_comparison_sampler\":" << (w.depthNonComparison ? "true" : "false")
                   << ",\"isnan_rewrites\":" << w.isNanRewrites << ",\"nonfinite_constants\":" << w.nonFiniteConstants
                   << ",\"workgroup\":[" << w.workgroup[0] << "," << w.workgroup[1] << "," << w.workgroup[2] << "],\"workgroup_storage\":"
                   << w.workgroup[3] << ",\"enables\":[";
                for (size_t i = 0; i < w.enables.size(); ++i) js << (i ? "," : "") << "\"" << jesc(w.enables[i]) << "\"";
                js << "],\"bindings\":[";
                for (size_t i = 0; i < w.bindings.size(); ++i)
                {
                    const auto& b = w.bindings[i];
                    js << (i ? "," : "") << "{\"group\":" << b.group << ",\"binding\":" << b.binding << ",\"kind\":\"" << b.kind
                       << "\",\"dim\":\"" << b.dim << "\",\"sampled\":\"" << b.sampled << "\",\"format\":\"" << b.format << "\",\"d3d\":\""
                       << b.cls << b.reg << "\"}";
                }
                js << "],\"limits\":[";
                for (size_t i = 0; i < w.limits.size(); ++i) js << (i ? "," : "") << "\"" << jesc(w.limits[i]) << "\"";
                js << "]}";
            }
            js << "}\n";
        }
    }
    // full logs of failures
    {
        std::ofstream fl2(out / "failures.log");
        for (const JobResult& r : results)
        {
            if (r.notAnEntry) continue;
            if (!r.ok && r.category != shaderxlat::Category::UnsupportedStage)
                fl2 << "==== " << r.job.entry.rel << " @" << r.job.profile << (r.job.skin >= 0 ? " skin" + std::to_string(r.job.skin) : "") << "\n" << r.fullLog << "\n";
            else if (r.ok && r.metalMac == MetalState::Fail)
                fl2 << "==== METAL macOS " << r.job.entry.rel << " @" << r.job.profile << "\n" << r.metalLogMac << "\n";
            else if (r.ok && r.metalIos == MetalState::Fail)
                fl2 << "==== METAL iOS " << r.job.entry.rel << " @" << r.job.profile << "\n" << r.metalLogIos << "\n";
            else if (r.ok && r.wgsl.run && !r.wgsl.ok)
                fl2 << "==== WGSL " << r.wgsl.failure << " " << r.job.entry.rel << " @" << r.job.profile
                    << (r.job.skin >= 0 ? " skin" + std::to_string(r.job.skin) : "") << "\n" << r.wgsl.log << "\n";
        }
    }
    std::error_code ec;
    fs::remove_all(out / "msl_tmp", ec);
    fs::remove(out / "wgsl_tmp", ec); // empty by now: every job removes its own files
    if (wgslMode)
    {
        const std::string s = md.str();
        const size_t at = s.find("## WGSL");
        printf("%s", s.substr(at, s.find("### WGSL failure groups", at) - at).c_str());
    }
    else
        printf("%s", md.str().substr(0, md.str().find("## Failure groups")).c_str());
    printf("details: %s/summary.md  %s/results.jsonl  %s/failures.log\n", outDir.c_str(), outDir.c_str(), outDir.c_str());
    return 0;
}
