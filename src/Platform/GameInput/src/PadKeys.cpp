#include "GameInput/PadKeys.h"

#include <algorithm>
#include <cctype>

namespace gameinput
{
namespace
{
struct KeyInfo
{
    const char* name;
    const char* ps;
    const char* xbox;
};

const KeyInfo kKeyInfo[] = {
#define GAMEINPUT_X(id, name, ps, xb) {name, ps, xb},
    GAMEINPUT_PAD_KEYS(GAMEINPUT_X)
#undef GAMEINPUT_X
};
constexpr int kButtonKeyCount = int(sizeof(kKeyInfo) / sizeof(kKeyInfo[0]));
static_assert(kPadKeyBase + kButtonKeyCount == kPadButtonKeyEnd, "key table out of sync with PadKey enum");

constexpr const char kRadialPrefix[] = "gp_radial_";

std::string Lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}
} // namespace

std::string PadKeyName(KeyCode key)
{
    if (!IsPadKey(key))
        return {};
    if (IsRadialKey(key))
        return kRadialPrefix + std::to_string(key - kRadialKeyBase + 1);
    return kKeyInfo[key - kPadKeyBase].name;
}

KeyCode PadKeyFromName(std::string_view name)
{
    const std::string lower = Lower(name);
    if (lower.rfind(kRadialPrefix, 0) == 0)
    {
        const std::string num = lower.substr(sizeof(kRadialPrefix) - 1);
        if (!num.empty() && num.size() <= 2 && std::all_of(num.begin(), num.end(), [](unsigned char c) { return std::isdigit(c); }))
        {
            const int n = std::stoi(num);
            if (n >= 1 && n <= kRadialKeyCount)
                return kRadialKeyBase + n - 1;
        }
        return PK_NONE;
    }
    for (int i = 0; i < kButtonKeyCount; ++i)
        if (lower == kKeyInfo[i].name)
            return kPadKeyBase + i;
    return PK_NONE;
}

std::string PadKeyLabel(KeyCode key, ControllerFamily family)
{
    if (!IsPadKey(key))
        return {};
    if (IsRadialKey(key))
        return "Radial " + std::to_string(key - kRadialKeyBase + 1);
    const KeyInfo& info = kKeyInfo[key - kPadKeyBase];
    return family == ControllerFamily::PlayStation ? info.ps : info.xbox;
}
} // namespace gameinput
