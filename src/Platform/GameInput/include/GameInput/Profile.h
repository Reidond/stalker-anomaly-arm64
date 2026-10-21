// Per-device profiles: tuning plus pad bindings, keyed by DeviceIdentity::stable_key.
//
// The store is a plain text blob so the host can keep it wherever suits it (the engine's user
// directory, a file in Documents):
//
//     [device dualsense:030000004c05...]
//     look.exponent = 1.9
//     bind_pad jump gp_cross
#pragma once

#include <map>
#include <string>
#include <string_view>

#include "GameInput/Bindings.h"
#include "GameInput/Config.h"

namespace gameinput
{
struct Profile
{
    Config config;
    BindingMap bindings = BindingMap::Default();
};

std::string SerializeProfile(const Profile& profile);
// Settings and bind_pad lines; anything unparsable is reported in `errors` and skipped. A bind_pad
// line replaces the defaults only if the text contains at least one (so a settings-only blob keeps
// the default layout).
void ParseProfile(Profile& profile, std::string_view text, std::vector<std::string>* errors = nullptr);

class ProfileStore
{
public:
    void Save(const std::string& stable_key, const Profile& profile);
    // Returns false (and leaves `profile` untouched) if the device has no saved profile.
    bool Load(const std::string& stable_key, Profile& profile, std::vector<std::string>* errors = nullptr) const;
    bool Has(const std::string& stable_key) const { return sections_.count(stable_key) != 0; }

    std::string Serialize() const;
    void Deserialize(std::string_view text);

private:
    std::map<std::string, std::string> sections_; // stable key -> profile text
};
} // namespace gameinput
