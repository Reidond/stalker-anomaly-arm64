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
};

enum class MetalState : uint8_t { NotRun, Ok, Fail };

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
} // namespace

int main(int argc, char** argv)
{
    std::string outDir = "shader_batch_out";
    std::vector<std::string> engineSrc;
    std::vector<std::string> profileFilter;
    std::string nameFilter, cacheDir;
    bool validate = true, keepMsl = false, dumpReflection = false, compat = true;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
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
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (g_roots.empty())
    {
        fprintf(stderr, "usage: shader_batch --root <shaders dir> [--root ...] --out <dir> [--engine-src <dir>] [--profile name]\n"
                        "                    [--filter substr] [--no-validate] [--keep-msl] [--reflect] [--no-compat] [--cache dir] [--jobs N]\n");
        return 2;
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
    for (const auto& p : profiles)
    {
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
    fs::create_directories(out / "msl");

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
            js << "]}\n";
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
        }
    }
    std::error_code ec;
    fs::remove_all(out / "msl_tmp", ec);
    printf("%s", md.str().substr(0, md.str().find("## Failure groups")).c_str());
    printf("details: %s/summary.md  %s/results.jsonl  %s/failures.log\n", outDir.c_str(), outDir.c_str(), outDir.c_str());
    return 0;
}
