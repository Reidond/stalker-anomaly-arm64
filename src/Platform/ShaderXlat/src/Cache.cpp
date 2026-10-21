// Translation cache and the small helpers that sit next to it.
#include "Internal.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

namespace shaderxlat
{
namespace
{
namespace fs = std::filesystem;

// Bump when Result's serialised form, the hooks or anything that changes output changes.
constexpr uint32_t kFormatVersion = 3;
constexpr uint32_t kMagic = 0x4c58534du; // 'MSXL'

struct Writer
{
    std::string buf;
    void u32(uint32_t v) { buf.append((const char*)&v, 4); }
    void u64(uint64_t v) { buf.append((const char*)&v, 8); }
    void str(const std::string& s)
    {
        u32((uint32_t)s.size());
        buf.append(s);
    }
};

struct Reader
{
    const char* p;
    const char* end;
    bool bad = false;
    uint32_t u32()
    {
        if (end - p < 4) { bad = true; return 0; }
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    uint64_t u64()
    {
        if (end - p < 8) { bad = true; return 0; }
        uint64_t v;
        std::memcpy(&v, p, 8);
        p += 8;
        return v;
    }
    std::string str()
    {
        const uint32_t n = u32();
        if (bad || (size_t)(end - p) < n) { bad = true; return {}; }
        std::string s(p, n);
        p += n;
        return s;
    }
    // Counts are checked against the bytes left so a corrupt file cannot make us allocate wildly.
    uint32_t count()
    {
        const uint32_t n = u32();
        if (n > (size_t)(end - p)) { bad = true; return 0; }
        return n;
    }
};

void put(Writer& w, const ConstantMember& m)
{
    w.str(m.name);
    w.u32(m.offset); w.u32(m.size); w.u32(m.used); w.u32((uint32_t)m.type); w.u32(m.rows); w.u32(m.columns);
    w.u32(m.array_elements); w.u32(m.array_stride); w.u32(m.row_major); w.str(m.type_name);
    w.u32((uint32_t)m.members.size());
    for (const auto& c : m.members) put(w, c);
}

ConstantMember getMember(Reader& r, int depth = 0)
{
    ConstantMember m;
    m.name = r.str();
    m.offset = r.u32(); m.size = r.u32(); m.used = r.u32(); m.type = (ScalarType)r.u32(); m.rows = r.u32(); m.columns = r.u32();
    m.array_elements = r.u32(); m.array_stride = r.u32(); m.row_major = r.u32(); m.type_name = r.str();
    const uint32_t n = r.count();
    if (depth > 16) { r.bad = true; return m; }
    for (uint32_t i = 0; i < n && !r.bad; ++i) m.members.push_back(getMember(r, depth + 1));
    return m;
}

void put(Writer& w, const StageVariable& v)
{
    w.str(v.semantic); w.u32(v.semantic_index); w.str(v.name); w.u32(v.location); w.u32(v.reg); w.u32(v.mask);
    w.u32((uint32_t)v.type); w.u32(v.components); w.u32(v.system_value); w.u32(v.active);
}

StageVariable getVar(Reader& r)
{
    StageVariable v;
    v.semantic = r.str(); v.semantic_index = r.u32(); v.name = r.str(); v.location = r.u32(); v.reg = r.u32(); v.mask = r.u32();
    v.type = (ScalarType)r.u32(); v.components = r.u32(); v.system_value = r.u32(); v.active = r.u32();
    return v;
}

std::string serialise(const Result& res, uint64_t key)
{
    Writer w;
    w.u32(kMagic); w.u32(kFormatVersion); w.u64(key);
    w.str(res.msl); w.str(res.entry_point);
    w.u32((uint32_t)res.applied_hooks.size());
    for (const auto& h : res.applied_hooks) w.str(h);
    w.u32((uint32_t)res.dependencies.size());
    for (const auto& d : res.dependencies) { w.str(d.name); w.str(d.resolved); w.u64(d.hash); }

    const Reflection& r = res.reflection;
    w.u32((uint32_t)r.constant_buffers.size());
    for (const auto& c : r.constant_buffers)
    {
        w.str(c.name); w.u32(c.slot); w.u32(c.msl_index); w.u32(c.size);
        w.u32((uint32_t)c.members.size());
        for (const auto& m : c.members) put(w, m);
    }
    w.u32((uint32_t)r.textures.size());
    for (const auto& t : r.textures)
    {
        w.str(t.name); w.u32(t.slot); w.u32(t.msl_index); w.u32((uint32_t)t.dim); w.u32((uint32_t)t.sampled_type);
        w.u32(t.depth); w.u32(t.multisampled); w.u32(t.is_uav); w.u32(t.uav_write_only); w.u32(t.uav_read_only);
    }
    w.u32((uint32_t)r.samplers.size());
    for (const auto& s : r.samplers) { w.str(s.name); w.u32(s.slot); w.u32(s.msl_index); w.u32(s.comparison); }
    w.u32((uint32_t)r.storage_buffers.size());
    for (const auto& s : r.storage_buffers) { w.str(s.name); w.u32(s.slot); w.u32(s.msl_index); w.u32(s.is_uav); }
    w.u32((uint32_t)r.sampler_texture_pairs.size());
    for (const auto& p : r.sampler_texture_pairs) { w.str(p.sampler); w.str(p.texture); }
    w.u32((uint32_t)r.inputs.size());
    for (const auto& v : r.inputs) put(w, v);
    w.u32((uint32_t)r.outputs.size());
    for (const auto& v : r.outputs) put(w, v);
    for (uint32_t t : r.threads) w.u32(t);
    w.u32(r.writes_depth); w.u32(r.uses_discard); w.u32(r.uses_sample_index);
    return w.buf;
}

bool deserialise(const std::string& data, uint64_t key, Result& res)
{
    Reader r{data.data(), data.data() + data.size()};
    if (r.u32() != kMagic || r.u32() != kFormatVersion || r.u64() != key)
        return false;
    res.msl = r.str();
    res.entry_point = r.str();
    for (uint32_t i = 0, n = r.count(); i < n && !r.bad; ++i) res.applied_hooks.push_back(r.str());
    for (uint32_t i = 0, n = r.count(); i < n && !r.bad; ++i)
    {
        IncludeDependency d;
        d.name = r.str(); d.resolved = r.str(); d.hash = r.u64();
        res.dependencies.push_back(std::move(d));
    }
    Reflection& f = res.reflection;
    for (uint32_t i = 0, n = r.count(); i < n && !r.bad; ++i)
    {
        ConstantBuffer c;
        c.name = r.str(); c.slot = r.u32(); c.msl_index = r.u32(); c.size = r.u32();
        for (uint32_t k = 0, m = r.count(); k < m && !r.bad; ++k) c.members.push_back(getMember(r));
        f.constant_buffers.push_back(std::move(c));
    }
    for (uint32_t i = 0, n = r.count(); i < n && !r.bad; ++i)
    {
        TextureBinding t;
        t.name = r.str(); t.slot = r.u32(); t.msl_index = r.u32(); t.dim = (TextureDim)r.u32(); t.sampled_type = (ScalarType)r.u32();
        t.depth = r.u32(); t.multisampled = r.u32(); t.is_uav = r.u32(); t.uav_write_only = r.u32(); t.uav_read_only = r.u32();
        f.textures.push_back(std::move(t));
    }
    for (uint32_t i = 0, n = r.count(); i < n && !r.bad; ++i)
    {
        SamplerBinding s;
        s.name = r.str(); s.slot = r.u32(); s.msl_index = r.u32(); s.comparison = r.u32();
        f.samplers.push_back(std::move(s));
    }
    for (uint32_t i = 0, n = r.count(); i < n && !r.bad; ++i)
    {
        StorageBuffer s;
        s.name = r.str(); s.slot = r.u32(); s.msl_index = r.u32(); s.is_uav = r.u32();
        f.storage_buffers.push_back(std::move(s));
    }
    for (uint32_t i = 0, n = r.count(); i < n && !r.bad; ++i)
    {
        SamplerTexturePair p;
        p.sampler = r.str(); p.texture = r.str();
        f.sampler_texture_pairs.push_back(std::move(p));
    }
    for (uint32_t i = 0, n = r.count(); i < n && !r.bad; ++i) f.inputs.push_back(getVar(r));
    for (uint32_t i = 0, n = r.count(); i < n && !r.bad; ++i) f.outputs.push_back(getVar(r));
    for (uint32_t& t : f.threads) t = r.u32();
    f.writes_depth = r.u32(); f.uses_discard = r.u32(); f.uses_sample_index = r.u32();
    return !r.bad && r.p == r.end;
}

uint64_t requestKey(const std::string& source, const std::string& name, const std::vector<Macro>& macros, const Options& o)
{
    uint64_t h = detail::fnv1a64(version(), std::strlen(version()));
    auto mix = [&](const std::string& s) {
        const uint64_t n = s.size();
        h = detail::fnv1a64(&n, sizeof n, h);
        h = detail::fnv1a64(s.data(), s.size(), h);
    };
    mix(frontend_name());
    mix(source);
    mix(name);
    for (const Macro& m : macros) { mix(m.name); mix(m.value); }
    const uint32_t opts[] = {(uint32_t)o.stage, (uint32_t)o.platform, o.msl_version, o.row_major_matrices, o.fxc_compat,
                             o.cbuffer_base, o.storage_buffer_base, o.vertex_stream_base, o.uav_texture_base, kFormatVersion};
    h = detail::fnv1a64(opts, sizeof opts, h);
    mix(o.entry);
    mix(o.profile);
    return h;
}

std::string entryPath(const std::string& dir, uint64_t key)
{
    char name[40];
    snprintf(name, sizeof name, "%016llx.xlat", (unsigned long long)key);
    return (fs::path(dir) / name).string();
}

bool readWhole(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
} // namespace

TranslationCache::TranslationCache(std::string directory) : directory_(std::move(directory)) {}

Result TranslationCache::translate(const std::string& source, const std::string& source_name, const std::vector<Macro>& macros,
                                   const Options& options, IncludeProvider* includes, bool* cache_hit)
{
    if (cache_hit)
        *cache_hit = false;
    const uint64_t key = requestKey(source, source_name, macros, options);
    const std::string path = entryPath(directory_, key);

    std::string blob;
    Result cached;
    if (readWhole(path, blob) && deserialise(blob, key, cached))
    {
        bool fresh = true;
        for (const IncludeDependency& d : cached.dependencies)
        {
            std::string resolved, contents;
            std::string name = d.name;
            if (!includes || !includes->open(name, "", resolved, contents) ||
                detail::fnv1a64(contents.data(), contents.size()) != d.hash)
            {
                fresh = false;
                break;
            }
        }
        if (fresh)
        {
            cached.ok = true;
            if (cache_hit)
                *cache_hit = true;
            return cached;
        }
    }

    Result res = shaderxlat::translate(source, source_name, macros, options, includes);
    if (res.ok)
    {
        std::error_code ec;
        fs::create_directories(directory_, ec);
        const std::string tmp = path + ".tmp" + std::to_string((unsigned long long)std::hash<std::thread::id>()(std::this_thread::get_id()));
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            const std::string data = serialise(res, key);
            f.write(data.data(), (std::streamsize)data.size());
            if (!f)
            {
                fs::remove(tmp, ec);
                return res;
            }
        }
        fs::rename(tmp, path, ec);
        if (ec)
            fs::remove(tmp, ec);
    }
    return res;
}

void TranslationCache::clear()
{
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(directory_, ec))
        if (de.path().extension() == ".xlat")
            fs::remove(de.path(), ec);
}

std::vector<std::string> checkStageInterface(const Reflection& vertex, const Reflection& pixel)
{
    std::vector<std::string> problems;
    for (const StageVariable& in : pixel.inputs)
    {
        if (in.system_value || !in.active)
            continue;
        const StageVariable* match = nullptr;
        for (const StageVariable& out : vertex.outputs)
            if (!out.system_value && out.active && out.semantic == in.semantic && out.semantic_index == in.semantic_index)
                match = &out;
        const std::string what = in.semantic + std::to_string(in.semantic_index);
        if (!match)
            problems.push_back(what + ": read by the pixel shader but not written by the vertex shader");
        else if (match->components != in.components || match->type != in.type)
            problems.push_back(what + ": vertex shader writes " + std::to_string(match->components) + " components, pixel shader reads " +
                               std::to_string(in.components));
    }
    return problems;
}
} // namespace shaderxlat
