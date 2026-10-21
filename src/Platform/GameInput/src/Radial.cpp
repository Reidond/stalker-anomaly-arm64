#include "GameInput/Radial.h"

#include <algorithm>
#include <cmath>

namespace gameinput
{
namespace
{
constexpr float kTwoPi = 6.28318530718f;

RadialPage MakePage(const char* title, int first_key_index, std::initializer_list<const char*> labels)
{
    RadialPage page;
    page.title = title;
    int i = 0;
    for (const char* label : labels)
        page.slots.push_back({PK_RADIAL_1 + first_key_index + i++, label});
    return page;
}
} // namespace

int RadialPickSlot(Vec2 stick, int count, int current, const RadialConfig& cfg)
{
    if (count <= 0 || stick.Length() < cfg.deadzone)
        return -1;

    // 0 at the top, increasing clockwise (stick +y is down, so "up" is -y).
    float angle = std::atan2(stick.x, -stick.y);
    if (angle < 0.0f)
        angle += kTwoPi;

    const float sector = kTwoPi / float(count);
    int picked = int(std::floor((angle + sector * 0.5f) / sector)) % count;

    if (current >= 0 && current < count && picked != current)
    {
        // Stay on the current slot until the stick is clearly inside the neighbour.
        float centre = float(current) * sector;
        float diff = std::fabs(angle - centre);
        diff = std::min(diff, kTwoPi - diff);
        if (diff < sector * (0.5f + cfg.sticky))
            picked = current;
    }
    return picked;
}

RadialMenu::RadialMenu() : wheels_(DefaultWheels()) {}

std::vector<RadialWheel> RadialMenu::DefaultWheels()
{
    std::vector<RadialWheel> wheels;

    RadialWheel items;
    items.name = "Items";
    items.open_key = PK_L1;
    items.pages.push_back(MakePage("Quick use", 0,
        {"Quick 1", "Quick 2", "Quick 3", "Quick 4", "Artefact", "Detector", "Quick save", "Drop"}));
    items.pages.push_back(MakePage("Custom", 8,
        {"Custom 1", "Custom 2", "Custom 3", "Custom 4", "Custom 5", "Custom 6", "Custom 7", "Custom 8"}));
    wheels.push_back(std::move(items));

    RadialWheel combat;
    combat.name = "Combat";
    combat.open_key = PK_R1;
    combat.pages.push_back(MakePage("Weapons", 16,
        {"Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Safe mode", "Weapon fn"}));
    combat.pages.push_back(MakePage("Weapon tools", 24,
        {"Ammo type", "Mode next", "Mode prev", "Zoom +", "Zoom -", "Custom 9", "Custom 10",
            "Custom 11"}));
    wheels.push_back(std::move(combat));
    return wheels;
}

void RadialMenu::SetWheels(std::vector<RadialWheel> wheels)
{
    Cancel();
    wheels_ = std::move(wheels);
}

void RadialMenu::Cancel()
{
    state_ = State::Idle;
    wheel_ = -1;
    page_ = 0;
    selected_ = -1;
    held_ = 0.0f;
    stick_ = {};
}

RadialView RadialMenu::View() const
{
    RadialView v;
    v.pending = state_ == State::Pending;
    v.open = state_ == State::Open;
    v.wheel = wheel_;
    v.page = page_;
    v.stick = stick_;
    if (wheel_ >= 0 && wheel_ < int(wheels_.size()))
    {
        const RadialWheel& w = wheels_[wheel_];
        v.page_count = int(w.pages.size());
        if (page_ < v.page_count)
            v.slot_count = int(w.pages[page_].slots.size());
    }
    v.selected = selected_;
    return v;
}

std::vector<RadialEvent> RadialMenu::Update(float dt, const std::vector<bool>& wheel_down, Vec2 stick, int page_step,
    const RadialConfig& cfg)
{
    std::vector<RadialEvent> events;
    stick_ = stick;

    auto down = [&](int i) { return i >= 0 && i < int(wheel_down.size()) && wheel_down[i]; };

    if (state_ == State::Idle)
    {
        for (int i = 0; i < int(wheels_.size()); ++i)
        {
            if (down(i) && !wheels_[i].pages.empty())
            {
                state_ = State::Pending;
                wheel_ = i;
                page_ = 0;
                held_ = 0.0f;
                selected_ = -1;
                break;
            }
        }
        return events;
    }

    const RadialWheel& wheel = wheels_[wheel_];
    if (state_ == State::Pending)
    {
        if (!down(wheel_))
        {
            events.push_back({RadialEvent::Type::Tap, wheel_, 0, wheel.open_key});
            Cancel();
            return events;
        }
        held_ += dt;
        if (held_ >= cfg.hold_seconds)
        {
            state_ = State::Open;
            events.push_back({RadialEvent::Type::Opened, wheel_, page_, 0});
        }
        return events;
    }

    // Open.
    const int pages = int(wheel.pages.size());
    if (page_step != 0 && pages > 1)
    {
        page_ = (page_ + page_step + pages) % pages;
        selected_ = -1;
        events.push_back({RadialEvent::Type::PageChanged, wheel_, page_, 0});
    }

    const RadialPage& page = wheel.pages[page_];
    selected_ = RadialPickSlot(stick, int(page.slots.size()), selected_, cfg);

    if (!down(wheel_))
    {
        if (selected_ >= 0)
            events.push_back({RadialEvent::Type::Confirmed, wheel_, page_, page.slots[selected_].key});
        else
            events.push_back({RadialEvent::Type::Cancelled, wheel_, page_, 0});
        Cancel();
    }
    return events;
}
} // namespace gameinput
