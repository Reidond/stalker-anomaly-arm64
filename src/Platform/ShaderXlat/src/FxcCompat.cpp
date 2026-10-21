// Minimal, in-memory source hooks that make FXC-accepted HLSL acceptable to vkd3d-shader. The game's
// shaders are never modified on disk; hooks run on the text handed to the frontend and only fire
// when their pattern is present. Every hook that fires is reported in Result::applied_hooks.
//
// Exactly one is needed for the stock shader set (the shadow-map header shadow.h, included by the
// lighting shaders); everything else
// FXC accepts that DXC does not (non-immediate texture offsets, vector ternary conditions,
// duplicate input semantics) is handled by vkd3d's own FXC-like front end.
#include "Internal.h"

#include <cctype>
#include <cstring>

namespace shaderxlat::detail
{
namespace
{
// register(ps, t0) -> register(t0). FXC accepts a shader-stage qualifier; vkd3d-shader reports
// "not yet implemented: Reservation shader target". The qualifier is redundant
// here because the resource is only referenced from the stage it is compiled for.
bool stripRegisterStage(std::string& s)
{
    bool changed = false;
    size_t pos = 0;
    while ((pos = s.find("register", pos)) != std::string::npos)
    {
        size_t p = pos + 8;
        while (p < s.size() && std::isspace((unsigned char)s[p])) ++p;
        if (p >= s.size() || s[p] != '(') { pos = p; continue; }
        ++p;
        while (p < s.size() && std::isspace((unsigned char)s[p])) ++p;
        const size_t kwStart = p;
        while (p < s.size() && std::isalpha((unsigned char)s[p])) ++p;
        const std::string kw = s.substr(kwStart, p - kwStart);
        if (kw == "vs" || kw == "ps" || kw == "gs" || kw == "hs" || kw == "ds" || kw == "cs")
        {
            size_t q = p;
            while (q < s.size() && std::isspace((unsigned char)s[q])) ++q;
            if (q < s.size() && s[q] == ',')
            {
                ++q;
                while (q < s.size() && std::isspace((unsigned char)s[q])) ++q;
                s.erase(kwStart, q - kwStart);
                changed = true;
                pos = kwStart;
                continue;
            }
        }
        pos = p;
    }
    return changed;
}
} // namespace

void applyFxcCompat(std::string& text, const std::string& path, std::vector<std::string>& applied)
{
    (void)path;
    auto note = [&](const char* name) {
        for (const auto& a : applied)
            if (a == name)
                return;
        applied.push_back(name);
    };

    if (text.find("register") != std::string::npos && stripRegisterStage(text))
        note("register-stage-qualifier");
}
} // namespace shaderxlat::detail
