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
#include <set>

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

bool isIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string upper(std::string s)
{
    for (char& c : s)
        c = (char)std::toupper((unsigned char)c);
    return s;
}

// index of the bracket that closes the one at `open`, or npos
size_t matching(const std::string& s, size_t open, char o, char c)
{
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i)
    {
        if (s[i] == o) ++depth;
        else if (s[i] == c && --depth == 0) return i;
    }
    return std::string::npos;
}

std::vector<std::string> splitTop(const std::string& s, char sep)
{
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '(' || c == '[' || c == '{' || c == '<') ++depth;
        else if (c == ')' || c == ']' || c == '}' || c == '>') --depth;
        else if (c == sep && depth == 0)
        {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(s.substr(start));
    return out;
}

struct Decl
{
    std::string type, name, semantic; // semantic upper-cased, empty if none
};

// "linear float2 tc : TEXCOORD0" -> {float2, tc, TEXCOORD0}
bool parseDecl(const std::string& text, Decl& d)
{
    std::string t = trim(text), sem;
    const size_t colon = t.rfind(':');
    if (colon != std::string::npos && t.find('(', colon) == std::string::npos)
    {
        sem = upper(trim(t.substr(colon + 1)));
        t = trim(t.substr(0, colon));
    }
    std::vector<std::string> words;
    size_t i = 0;
    while (i < t.size())
    {
        while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
        size_t j = i;
        while (j < t.size() && !std::isspace((unsigned char)t[j])) ++j;
        if (j > i) words.push_back(t.substr(i, j - i));
        i = j;
    }
    static const std::set<std::string> qualifiers = {"in", "out", "inout", "uniform", "linear", "nointerpolation",
                                                     "centroid", "noperspective", "sample", "precise", "const", "static"};
    std::string name;
    std::vector<std::string> typeWords;
    for (size_t k = 0; k < words.size(); ++k)
    {
        if (k + 1 == words.size())
            name = words[k];
        else if (!qualifiers.count(words[k]))
            typeWords.push_back(words[k]);
    }
    const size_t br = name.find('[');
    if (br != std::string::npos)
        name = name.substr(0, br);
    if (name.empty() || typeWords.empty())
        return false;
    d.type = typeWords.front();
    for (size_t k = 1; k < typeWords.size(); ++k)
        d.type += " " + typeWords[k];
    d.name = name;
    d.semantic = sem;
    return true;
}

// members of `struct <type> { ... }` that carry a semantic
std::vector<Decl> structMembers(const std::string& s, const std::string& type)
{
    std::vector<Decl> out;
    size_t pos = 0;
    while ((pos = s.find("struct", pos)) != std::string::npos)
    {
        size_t p = pos + 6;
        const bool left = pos == 0 || !isIdentChar(s[pos - 1]);
        pos = p;
        if (!left || p >= s.size() || !std::isspace((unsigned char)s[p]))
            continue;
        while (p < s.size() && std::isspace((unsigned char)s[p])) ++p;
        size_t q = p;
        while (q < s.size() && isIdentChar(s[q])) ++q;
        if (s.substr(p, q - p) != type)
            continue;
        while (q < s.size() && std::isspace((unsigned char)s[q])) ++q;
        if (q >= s.size() || s[q] != '{')
            continue;
        const size_t close = matching(s, q, '{', '}');
        if (close == std::string::npos)
            return out;
        for (const std::string& m : splitTop(s.substr(q + 1, close - q - 1), ';'))
        {
            Decl d;
            if (parseDecl(m, d) && !d.semantic.empty())
                out.push_back(d);
        }
        return out;
    }
    return out;
}

// float4 can be narrowed to float2 by a cast; nothing can be widened
int vectorWidth(const std::string& t)
{
    static const char* bases[] = {"float", "half", "int", "uint", "min16float"};
    for (const char* b : bases)
    {
        const size_t n = std::strlen(b);
        if (t.compare(0, n, b) == 0)
        {
            if (t.size() == n) return 1;
            if (t.size() == n + 1 && t[n] >= '1' && t[n] <= '4') return t[n] - '0';
        }
    }
    return 0;
}
} // namespace

// An entry-point parameter whose semantic is also carried by a member of a struct parameter, with a different type
// (`p_flat I, float2 tc0 : TEXCOORD0` where p_flat.tcdh is float4): FXC lets both read the same interpolant, vkd3d-shader
// reports E5013. `text` is the preprocessed source. The duplicate parameter is dropped and defined from the struct member.
bool aliasDuplicateInputSemantics(std::string& text, const std::string& entry)
{
    // the definition of the entry point: `<ret> entry ( ... ) [: SEM] {`
    size_t open = std::string::npos, bodyOpen = std::string::npos, pos = 0;
    while ((pos = text.find(entry, pos)) != std::string::npos)
    {
        const size_t e = pos + entry.size();
        const bool word = (pos == 0 || !isIdentChar(text[pos - 1])) && (e >= text.size() || !isIdentChar(text[e]));
        pos = e;
        if (!word)
            continue;
        size_t p = e;
        while (p < text.size() && std::isspace((unsigned char)text[p])) ++p;
        if (p >= text.size() || text[p] != '(')
            continue;
        const size_t close = matching(text, p, '(', ')');
        if (close == std::string::npos)
            continue;
        size_t q = close + 1;
        while (q < text.size() && text[q] != '{' && text[q] != ';') ++q;
        if (q < text.size() && text[q] == '{')
        {
            open = p;
            bodyOpen = q;
        }
    }
    if (open == std::string::npos)
        return false;
    const size_t close = matching(text, open, '(', ')');
    std::vector<std::string> params = splitTop(text.substr(open + 1, close - open - 1), ',');
    std::vector<Decl> decls(params.size());
    std::vector<bool> parsed(params.size());
    for (size_t i = 0; i < params.size(); ++i)
        parsed[i] = parseDecl(params[i], decls[i]);

    std::vector<bool> drop(params.size(), false);
    std::string defs;
    for (size_t i = 0; i < params.size(); ++i)
    {
        if (!parsed[i] || decls[i].semantic.empty() || decls[i].semantic.compare(0, 3, "SV_") == 0)
            continue;
        const int want = vectorWidth(decls[i].type);
        if (!want)
            continue;
        for (size_t k = 0; k < params.size() && !drop[i]; ++k)
        {
            if (k == i || !parsed[k] || !decls[k].semantic.empty())
                continue;
            for (const Decl& m : structMembers(text, decls[k].type))
            {
                const int have = vectorWidth(m.type);
                if (m.semantic == decls[i].semantic && have > want && want > 0)
                {
                    defs += "\n    " + decls[i].type + " " + decls[i].name + " = (" + decls[i].type + ")" + decls[k].name + "." +
                            m.name + ";";
                    drop[i] = true;
                    break;
                }
            }
        }
    }
    if (defs.empty())
        return false;
    std::string list;
    for (size_t i = 0; i < params.size(); ++i)
        if (!drop[i])
            list += (list.empty() ? "" : ",") + params[i];
    // body first (positions after it do not move), then the parameter list
    text.insert(bodyOpen + 1, defs);
    text.replace(open + 1, close - open - 1, list);
    return true;
}

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
