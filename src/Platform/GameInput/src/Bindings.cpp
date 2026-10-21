#include "GameInput/Bindings.h"

#include <algorithm>
#include <cctype>

namespace gameinput
{
namespace
{
// Mirrors actions[] in src/xrGame/xr_level_controller.cpp (same names, same order).
const std::vector<ActionInfo> kStockActions = {
    {"left", "Look left (key)", false},
    {"right", "Look right (key)", false},
    {"up", "Look up (key)", false},
    {"down", "Look down (key)", false},
    {"jump", "Jump", false},
    {"crouch", "Crouch", false},
    {"accel", "Walk (slow)", false},
    {"sprint_toggle", "Sprint", false},
    {"forward", "Move forward", false},
    {"back", "Move back", false},
    {"lstrafe", "Strafe left", false},
    {"rstrafe", "Strafe right", false},
    {"llookout", "Lean left", false},
    {"rlookout", "Lean right", false},
    {"cam_1", "Camera 1", false},
    {"cam_2", "Camera 2", false},
    {"cam_3", "Camera 3", false},
    {"cam_zoom_in", "Camera zoom in", false},
    {"cam_zoom_out", "Camera zoom out", false},
    {"torch", "Flashlight", false},
    {"night_vision", "Night vision", false},
    {"show_detector", "Detector", true},
    {"wpn_1", "Slot 1", false},
    {"wpn_2", "Slot 2", false},
    {"wpn_3", "Slot 3", false},
    {"wpn_4", "Slot 4", false},
    {"wpn_5", "Slot 5", false},
    {"wpn_6", "Slot 6", false},
    {"artefact", "Artefact", false},
    {"wpn_next", "Ammo type", false},
    {"wpn_fire", "Fire", false},
    {"wpn_zoom", "Aim", false},
    {"wpn_zoom_inc", "Zoom +", false},
    {"wpn_zoom_dec", "Zoom -", false},
    {"wpn_reload", "Reload", false},
    {"wpn_func", "Weapon fn", false},
    {"wpn_firemode_prev", "Mode prev", false},
    {"wpn_firemode_next", "Mode next", false},
    {"pause", "Pause", false},
    {"drop", "Drop", false},
    {"use", "Use", false},
    {"scores", "Scores", false},
    {"chat", "Chat", false},
    {"chat_team", "Team chat", false},
    {"screenshot", "Screenshot", false},
    {"quit", "Menu / back", false},
    {"console", "Console", false},
    {"inventory", "Inventory", false},
    {"buy_menu", "Buy menu", false},
    {"skin_menu", "Skin menu", false},
    {"team_menu", "Team menu", false},
    {"active_jobs", "PDA", true},
    {"vote_begin", "Begin vote", false},
    {"show_admin_menu", "Admin menu", false},
    {"vote", "Vote", false},
    {"vote_yes", "Vote yes", false},
    {"vote_no", "Vote no", false},
    {"next_slot", "Next weapon", false},
    {"prev_slot", "Prev weapon", false},
    {"speech_menu_0", "Speech menu 0", false},
    {"speech_menu_1", "Speech menu 1", false},
    {"quick_use_1", "Quick 1", false},
    {"quick_use_2", "Quick 2", false},
    {"quick_use_3", "Quick 3", false},
    {"quick_use_4", "Quick 4", false},
    {"quick_save", "Quick save", true},
    {"quick_load", "Quick load", true},
    {"custom1", "Custom 1", true},
    {"custom2", "Custom 2", true},
    {"custom3", "Custom 3", true},
    {"custom4", "Custom 4", true},
    {"custom5", "Custom 5", true},
    {"custom6", "Custom 6", true},
    {"custom7", "Custom 7", true},
    {"custom8", "Custom 8", true},
    {"custom9", "Custom 9", true},
    {"custom10", "Custom 10", true},
    {"custom11", "Custom 11", true},
    {"custom12", "Custom 12", true},
    {"custom13", "Custom 13", true},
    {"custom14", "Custom 14", true},
    {"custom15", "Custom 15", true},
    {"custom16", "Custom 16", true},
    {"custom17", "Custom 17", true},
    {"custom18", "Custom 18", true},
    {"custom19", "Custom 19", true},
    {"custom20", "Custom 20", true},
    {"custom21", "Custom 21", true},
    {"custom22", "Custom 22", true},
    {"custom23", "Custom 23", true},
    {"custom24", "Custom 24", true},
    {"custom25", "Custom 25", true},
    {"safemode", "Safe mode", false},
    {"freelook", "Free look", false},
    {"cam_autoaim", "Auto aim", true},
    {"editor", "Editor", false},
};

std::string Lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> Split(std::string_view line)
{
    std::vector<std::string_view> out;
    while (!line.empty())
    {
        line = Trim(line);
        if (line.empty())
            break;
        size_t end = 0;
        while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])))
            ++end;
        out.push_back(line.substr(0, end));
        line.remove_prefix(end);
    }
    return out;
}
} // namespace

const std::vector<ActionInfo>& StockActions()
{
    return kStockActions;
}

const ActionInfo* FindAction(std::string_view name)
{
    const std::string lower = Lower(name);
    for (const ActionInfo& a : kStockActions)
        if (lower == a.name)
            return &a;
    return nullptr;
}

std::string ActionLabel(std::string_view name)
{
    if (const ActionInfo* a = FindAction(name))
        return a->label;
    return std::string(name);
}

bool BindingMap::Bind(std::string_view action, KeyCode key)
{
    const ActionInfo* info = FindAction(action);
    if (!info || !IsPadKey(key))
        return false;
    Unbind(info->name);
    UnbindKey(key);
    by_action_[info->name] = key;
    by_key_[key] = info->name;
    return true;
}

void BindingMap::Unbind(std::string_view action)
{
    auto it = by_action_.find(action);
    if (it == by_action_.end())
        return;
    by_key_.erase(it->second);
    by_action_.erase(it);
}

void BindingMap::UnbindKey(KeyCode key)
{
    auto it = by_key_.find(key);
    if (it == by_key_.end())
        return;
    by_action_.erase(it->second);
    by_key_.erase(it);
}

void BindingMap::Clear()
{
    by_action_.clear();
    by_key_.clear();
}

const std::string* BindingMap::ActionForKey(KeyCode key) const
{
    auto it = by_key_.find(key);
    return it == by_key_.end() ? nullptr : &it->second;
}

KeyCode BindingMap::KeyForAction(std::string_view action) const
{
    auto it = by_action_.find(action);
    return it == by_action_.end() ? PK_NONE : it->second;
}

std::string BindingMap::Serialize() const
{
    std::string out;
    for (const ActionInfo& a : kStockActions)
    {
        auto it = by_action_.find(std::string_view(a.name));
        if (it == by_action_.end())
            continue;
        out += "bind_pad ";
        out += a.name;
        out += ' ';
        out += PadKeyName(it->second);
        out += '\n';
    }
    return out;
}

int BindingMap::Load(std::string_view text, std::vector<std::string>* errors)
{
    int applied = 0;
    int line_no = 0;
    auto fail = [&](const std::string& why) {
        if (errors)
            errors->push_back("line " + std::to_string(line_no) + ": " + why);
    };

    while (!text.empty())
    {
        ++line_no;
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);

        const size_t comment = line.find_first_of("#;");
        if (comment != std::string_view::npos)
            line = line.substr(0, comment);
        const auto words = Split(line);
        if (words.empty())
            continue;

        const std::string cmd = Lower(words[0]);
        if (cmd == "bind_pad" && words.size() == 3)
        {
            const KeyCode key = PadKeyFromName(words[2]);
            if (!FindAction(words[1]))
                fail("unknown action '" + std::string(words[1]) + "'");
            else if (key == PK_NONE)
                fail("unknown pad key '" + std::string(words[2]) + "'");
            else if (Bind(words[1], key))
                ++applied;
        }
        else if (cmd == "unbind_pad" && words.size() == 2)
        {
            if (!FindAction(words[1]))
                fail("unknown action '" + std::string(words[1]) + "'");
            else
            {
                Unbind(FindAction(words[1])->name);
                ++applied;
            }
        }
        else if (cmd == "unbindall_pad" && words.size() == 1)
        {
            Clear();
            ++applied;
        }
        else
        {
            fail("cannot parse '" + std::string(Trim(line)) + "'");
        }
    }
    return applied;
}

BindingMap BindingMap::Default()
{
    BindingMap m;
    struct Pair { const char* action; KeyCode key; };
    static const Pair kDefaults[] = {
        // Movement. The left stick fires the lstick_* keys; gp_walk is held while the stick is
        // pushed gently, which the engine treats as "accel" (walk).
        {"forward", PK_LStickUp}, {"back", PK_LStickDown}, {"lstrafe", PK_LStickLeft}, {"rstrafe", PK_LStickRight},
        {"accel", PK_Walk}, {"sprint_toggle", PK_L3},
        // Face buttons.
        {"jump", PK_Cross}, {"crouch", PK_Circle}, {"wpn_reload", PK_Square}, {"use", PK_Triangle},
        // Triggers and shoulders. L1/R1 are also the radial-menu buttons: a tap fires these.
        {"wpn_fire", PK_R2}, {"wpn_zoom", PK_L2}, {"prev_slot", PK_L1}, {"next_slot", PK_R1},
        // D-pad and right stick click.
        {"torch", PK_DpadUp}, {"night_vision", PK_DpadDown}, {"llookout", PK_DpadLeft}, {"rlookout", PK_DpadRight},
        {"freelook", PK_R3},
        // System buttons. PS is not bound: iPadOS takes it for Home (verified on the iPad), so the
        // game never sees a press; "pause" stays reachable from the menu.
        {"inventory", PK_Create}, {"quit", PK_Options}, {"active_jobs", PK_Touchpad},
        // Items wheel (hold L1), page 1.
        {"quick_use_1", PK_RADIAL_1 + 0}, {"quick_use_2", PK_RADIAL_1 + 1}, {"quick_use_3", PK_RADIAL_1 + 2},
        {"quick_use_4", PK_RADIAL_1 + 3}, {"artefact", PK_RADIAL_1 + 4}, {"show_detector", PK_RADIAL_1 + 5},
        {"quick_save", PK_RADIAL_1 + 6}, {"drop", PK_RADIAL_1 + 7},
        // Items wheel, page 2: addon-defined actions.
        {"custom1", PK_RADIAL_1 + 8}, {"custom2", PK_RADIAL_1 + 9}, {"custom3", PK_RADIAL_1 + 10},
        {"custom4", PK_RADIAL_1 + 11}, {"custom5", PK_RADIAL_1 + 12}, {"custom6", PK_RADIAL_1 + 13},
        {"custom7", PK_RADIAL_1 + 14}, {"custom8", PK_RADIAL_1 + 15},
        // Combat wheel (hold R1), page 1.
        {"wpn_1", PK_RADIAL_1 + 16}, {"wpn_2", PK_RADIAL_1 + 17}, {"wpn_3", PK_RADIAL_1 + 18},
        {"wpn_4", PK_RADIAL_1 + 19}, {"wpn_5", PK_RADIAL_1 + 20}, {"wpn_6", PK_RADIAL_1 + 21},
        {"safemode", PK_RADIAL_1 + 22}, {"wpn_func", PK_RADIAL_1 + 23},
        // Combat wheel, page 2.
        {"wpn_next", PK_RADIAL_1 + 24}, {"wpn_firemode_next", PK_RADIAL_1 + 25}, {"wpn_firemode_prev", PK_RADIAL_1 + 26},
        {"wpn_zoom_inc", PK_RADIAL_1 + 27}, {"wpn_zoom_dec", PK_RADIAL_1 + 28}, {"custom9", PK_RADIAL_1 + 29},
        {"custom10", PK_RADIAL_1 + 30}, {"custom11", PK_RADIAL_1 + 31},
    };
    for (const Pair& p : kDefaults)
        m.Bind(p.action, p.key);
    return m;
}
} // namespace gameinput
