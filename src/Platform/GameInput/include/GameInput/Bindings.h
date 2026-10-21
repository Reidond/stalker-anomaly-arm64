// Rebindable map from pad keys to Anomaly's named actions.
//
// Action names are exactly the strings in the engine's actions[] table (xr_level_controller.cpp),
// so the adapter resolves them with action_name_to_id(). The text format is the engine's own bind
// syntax with a pad-specific command, so a profile can be fed straight to the console:
//
//     bind_pad jump gp_cross
//
// One pad key per action: the engine side adds a third slot to _binding for it. Binding a key that
// is already used moves it, like CCC_Bind does for keyboard keys.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "GameInput/PadKeys.h"

namespace gameinput
{
struct ActionInfo
{
    const char* name;  // engine action name, e.g. "wpn_fire"
    const char* label; // short UI label
    bool single_player_only;
};

// All stock Anomaly actions, in the engine's EGameActions order.
const std::vector<ActionInfo>& StockActions();
const ActionInfo* FindAction(std::string_view name);
// UI label for an action; falls back to the raw name.
std::string ActionLabel(std::string_view name);

class BindingMap
{
public:
    BindingMap() = default;

    // The default DualSense layout; see docs/ios-port/CONTROLS.md.
    static BindingMap Default();

    // Binds `key` to `action`, removing both the action's previous key and any other action that
    // held `key`. Returns false for unknown actions or non-pad keys.
    bool Bind(std::string_view action, KeyCode key);
    void Unbind(std::string_view action);
    void UnbindKey(KeyCode key);
    void Clear();

    // nullptr if unbound.
    const std::string* ActionForKey(KeyCode key) const;
    KeyCode KeyForAction(std::string_view action) const; // PK_NONE if unbound
    size_t Size() const { return by_action_.size(); }

    // "bind_pad <action> <key>" lines in stock-action order, one per binding.
    std::string Serialize() const;
    // Applies bind_pad / unbind_pad lines ('#' and ';' start comments). Returns the number of
    // lines that changed something; problems are appended to `errors` when given.
    int Load(std::string_view text, std::vector<std::string>* errors = nullptr);

private:
    std::map<std::string, KeyCode, std::less<>> by_action_;
    std::map<KeyCode, std::string> by_key_;
};
} // namespace gameinput
