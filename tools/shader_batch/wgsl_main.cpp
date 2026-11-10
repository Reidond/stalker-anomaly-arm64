// shader_wgsl: one SPIR-V module -> WGSL through ShaderXlat's Tint back end, validated like Chrome validates it.
// shader_batch runs it once per job in a child process because Tint aborts the process on an internal compiler error.
//
//   shader_wgsl --spv <in.spv> --stage vs|ps|cs [--out <out.wgsl>] [--loc REG=LOC]... [--uav-format U=FMT]...
//               [--no-remap] [--strict-uniformity] [--no-polyfill]
//
// Prints a line-oriented report on stdout (`key value...`, one fact per line; see printReport) and exits 0 when the
// WGSL is valid, 1 when it is not, 2 on usage errors.
#include "ShaderXlat/Wgsl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace shaderxlat;

namespace
{
bool readSpirv(const std::string& path, std::vector<uint32_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    if (s.size() % 4)
        return false;
    out.resize(s.size() / 4);
    std::memcpy(out.data(), s.data(), s.size());
    return true;
}

bool parsePair(const std::string& s, uint32_t& a, std::string& b)
{
    const size_t eq = s.find('=');
    if (eq == std::string::npos)
        return false;
    a = (uint32_t)std::strtoul(s.substr(0, eq).c_str(), nullptr, 10);
    b = s.substr(eq + 1);
    return true;
}

void printReport(const wgsl::Result& r)
{
    printf("ok %d\nfailure %s\nentry %s\nms %.3f\nderivative_uniformity %d\ndiscard %d\nclip_distances %u\n", r.ok ? 1 : 0,
           wgsl::failureName(r.failure), r.entry_point.c_str(), r.ms, r.derivative_uniformity_needed ? 1 : 0, r.uses_discard ? 1 : 0,
           r.clip_distances);
    printf("rewrites %u %u\n", r.float_classification_rewrites, r.nonfinite_constants);
    printf("depth_non_comparison_sampler %d\ntexture_load_depth %d\nworkgroup %u %u %u %u\n", r.depth_texture_with_non_comparison_sampler ? 1 : 0,
           r.texture_load_with_depth_texture ? 1 : 0, r.workgroup[0], r.workgroup[1], r.workgroup[2], r.workgroup_storage_bytes);
    for (const auto& e : r.enables)
        printf("enable %s\n", e.c_str());
    for (const auto& b : r.bindings)
        printf("binding %u %u %s %s %s %s %c %u %s\n", b.group, b.binding, b.kind.c_str(), b.dim.empty() ? "-" : b.dim.c_str(),
               b.sampled.empty() ? "-" : b.sampled.c_str(), b.format.empty() ? "-" : b.format.c_str(), b.d3d_class, b.d3d_register,
               b.name.c_str());
    for (const auto& p : r.sampler_uses)
        printf("pair %u %u %u %u\n", p.sampler_group, p.sampler_binding, p.texture_group, p.texture_binding);
    for (const auto& v : r.inputs)
        printf("in %u %s %s\n", v.location, v.type.c_str(), v.interpolation.empty() ? "-" : v.interpolation.c_str());
    for (const auto& v : r.outputs)
        printf("out %u %s %s\n", v.location, v.type.c_str(), v.interpolation.empty() ? "-" : v.interpolation.c_str());
    for (const auto& l : r.limit_errors)
        printf("limit %s\n", l.c_str());
    std::istringstream log(r.log);
    for (std::string line; std::getline(log, line);)
        if (!line.empty())
            printf("log %s\n", line.c_str());
}
} // namespace

int main(int argc, char** argv)
{
    std::string spvPath, outPath, stage;
    wgsl::Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--spv") spvPath = next();
        else if (a == "--out") outPath = next();
        else if (a == "--stage") stage = next();
        else if (a == "--no-remap") o.remap_bindings = false;
        else if (a == "--strict-uniformity") o.allow_non_uniform_derivatives = false;
        else if (a == "--no-polyfill") o.polyfill_float_classification = false;
        else if (a == "--loc" || a == "--uav-format")
        {
            uint32_t k;
            std::string v;
            if (!parsePair(next(), k, v))
            {
                fprintf(stderr, "%s expects A=B\n", a.c_str());
                return 2;
            }
            if (a == "--loc")
                o.locations.push_back({k, (uint32_t)std::strtoul(v.c_str(), nullptr, 10)});
            else
                o.uav_formats.push_back({k, v});
        }
        else
        {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (stage == "vs") o.stage = Stage::Vertex;
    else if (stage == "ps") o.stage = Stage::Pixel;
    else if (stage == "cs") o.stage = Stage::Compute;
    else
    {
        fprintf(stderr, "usage: shader_wgsl --spv <file> --stage vs|ps|cs [--out file.wgsl] [--loc R=L]... [--uav-format U=FMT]...\n");
        return 2;
    }
    std::vector<uint32_t> spirv;
    if (!readSpirv(spvPath, spirv))
    {
        fprintf(stderr, "cannot read %s\n", spvPath.c_str());
        return 2;
    }
    const wgsl::Result r = wgsl::fromSpirv(spirv, o);
    if (!outPath.empty() && !r.wgsl.empty())
        std::ofstream(outPath) << r.wgsl;
    printReport(r);
    return r.ok ? 0 : 1;
}
