#include "GameInput/Profile.h"

namespace gameinput
{
std::string SerializeProfile(const Profile& profile)
{
    return SerializeConfig(profile.config) + profile.bindings.Serialize();
}

void ParseProfile(Profile& profile, std::string_view text, std::vector<std::string>* errors)
{
    ParseConfig(profile.config, text, errors);

    // Only touch the bindings when the blob has any, and then start from a clean slate so removed
    // bindings stay removed.
    if (text.find("bind_pad") != std::string_view::npos)
    {
        // A profile's bind lines are the complete layout: ParseConfig above ignored them, and
        // BindingMap::Load below ignores setting lines the same way (reported as errors, so filter).
        std::string binds;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t nl = text.find('\n', pos);
            if (nl == std::string_view::npos)
                nl = text.size();
            const std::string_view line = text.substr(pos, nl - pos);
            if (line.find("bind_pad") != std::string_view::npos)
            {
                binds.append(line);
                binds += '\n';
            }
            pos = nl + 1;
        }
        profile.bindings.Clear();
        profile.bindings.Load(binds, errors);
    }
}

void ProfileStore::Save(const std::string& stable_key, const Profile& profile)
{
    sections_[stable_key] = SerializeProfile(profile);
}

bool ProfileStore::Load(const std::string& stable_key, Profile& profile, std::vector<std::string>* errors) const
{
    auto it = sections_.find(stable_key);
    if (it == sections_.end())
        return false;
    ParseProfile(profile, it->second, errors);
    return true;
}

std::string ProfileStore::Serialize() const
{
    std::string out;
    for (const auto& [key, text] : sections_)
    {
        out += "[device " + key + "]\n";
        out += text;
        if (out.back() != '\n')
            out += '\n';
        out += '\n';
    }
    return out;
}

void ProfileStore::Deserialize(std::string_view text)
{
    sections_.clear();
    std::string current;
    bool in_section = false;
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos)
            nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);

        if (line.rfind("[device ", 0) == 0 && line.back() == ']')
        {
            current = std::string(line.substr(8, line.size() - 9));
            sections_[current];
            in_section = true;
        }
        else if (in_section && !line.empty())
        {
            sections_[current] += std::string(line) + '\n';
        }
        if (nl == text.size())
            break;
    }
}
} // namespace gameinput
