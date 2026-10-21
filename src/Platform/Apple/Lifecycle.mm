// Lifecycle, memory pressure, thermal and power observers. Everything runs on the main thread.

#include "ShellInternal.h"

#include <SDL3/SDL.h>

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <os/proc.h>
#endif

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>

namespace AppleShell
{
namespace
{
// An app memory warning keeps the reported level at Critical for this long, then the level falls
// back to whatever the pressure notifications and the budget watchdog say.
constexpr uint64_t kAppWarningHoldMs = 5000;

struct LifecycleState
{
    std::map<ListenerId, Listener> listeners;
    ListenerId next_id = 1;
    bool paused = false;
    bool in_background = false;
    bool keep_awake_requested = true;

    std::deque<EventRecord> history;
    std::mutex history_mutex;

    MemoryLevel memory_level = MemoryLevel::Normal; // what listeners were last told
    MemoryLevel pressure_level = MemoryLevel::Normal;
    MemoryLevel budget_level = MemoryLevel::Normal;
    uint64_t app_warning_until = 0; // SDL ticks (ms), 0 = none
    uint64_t last_budget_poll = 0;

    ThermalState thermal = ThermalState::Nominal;
    bool low_power = false;

    dispatch_source_t pressure_source = nullptr;
    id thermal_observer = nil;
    id power_observer = nil;
    bool watch_installed = false;
};

LifecycleState& State()
{
    static LifecycleState state;
    return state;
}

ThermalState FromNS(NSProcessInfoThermalState s)
{
    switch (s)
    {
    case NSProcessInfoThermalStateFair: return ThermalState::Fair;
    case NSProcessInfoThermalStateSerious: return ThermalState::Serious;
    case NSProcessInfoThermalStateCritical: return ThermalState::Critical;
    default: return ThermalState::Nominal;
    }
}

uint64_t AvailableMemoryBytes()
{
#if TARGET_OS_IOS
    return uint64_t(os_proc_available_memory());
#else
    return 0;
#endif
}

// Iterates a copy so a listener may add or remove listeners from inside its callback.
template <typename Fn>
void ForEachListener(Fn&& fn)
{
    auto copy = State().listeners;
    for (auto& entry : copy)
        fn(entry.second);
}

void ApplyKeepAwake()
{
    auto& s = State();
    if (s.keep_awake_requested && !s.paused)
        SDL_DisableScreenSaver();
    else
        SDL_EnableScreenSaver();
}

void DispatchLifecycle(LifecycleEvent event)
{
    auto& s = State();
    Internal::RecordEvent("lifecycle", ToString(event));

    switch (event)
    {
    case LifecycleEvent::WillEnterBackground:
        s.paused = true;
        ApplyKeepAwake();
        break;
    case LifecycleEvent::DidEnterBackground:
        s.in_background = true;
        Internal::WriteSessionState("background");
        break;
    case LifecycleEvent::WillEnterForeground:
        break;
    case LifecycleEvent::DidEnterForeground:
        s.paused = false;
        s.in_background = false;
        Internal::WriteSessionState("running");
        ApplyKeepAwake();
        break;
    case LifecycleEvent::Terminating:
        Internal::WriteSessionState("clean");
        break;
    }

    ForEachListener([&](Listener& l) {
        if (l.on_lifecycle)
            l.on_lifecycle(event);
        if (event == LifecycleEvent::WillEnterBackground && l.on_pause)
            l.on_pause();
        if (event == LifecycleEvent::DidEnterForeground && l.on_resume)
            l.on_resume();
    });
}

// Reports the worst of the system-pressure level, the budget watchdog level and a pending app warning.
void ReconcileMemory(MemorySource source)
{
    auto& s = State();
    MemoryLevel worst = std::max(s.pressure_level, s.budget_level);
    if (s.app_warning_until)
        worst = MemoryLevel::Critical;
    if (worst == s.memory_level)
        return;

    MemoryEvent event;
    event.level = worst;
    event.previous = s.memory_level;
    event.source = source;
    event.available_bytes = AvailableMemoryBytes();
    s.memory_level = worst;

    char text[160];
    snprintf(text, sizeof(text), "%s -> %s via %s, %s available", ToString(event.previous), ToString(worst),
        ToString(source), event.available_bytes ? FormatBytes(event.available_bytes).c_str() : "n/a");
    Internal::RecordEvent("memory", text);

    ForEachListener([&](Listener& l) {
        if (l.on_memory)
            l.on_memory(event);
    });
}

bool WatchLifecycle(void* /*userdata*/, SDL_Event* event)
{
    switch (event->type)
    {
    case SDL_EVENT_WILL_ENTER_BACKGROUND: DispatchLifecycle(LifecycleEvent::WillEnterBackground); break;
    case SDL_EVENT_DID_ENTER_BACKGROUND: DispatchLifecycle(LifecycleEvent::DidEnterBackground); break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND: DispatchLifecycle(LifecycleEvent::WillEnterForeground); break;
    case SDL_EVENT_DID_ENTER_FOREGROUND: DispatchLifecycle(LifecycleEvent::DidEnterForeground); break;
    case SDL_EVENT_TERMINATING: DispatchLifecycle(LifecycleEvent::Terminating); break;
    case SDL_EVENT_LOW_MEMORY:
        // UIKit's memory warning means the OS wants memory back now: report it as critical.
        Internal::RecordEvent("memory", "UIKit memory warning received");
        State().app_warning_until = SDL_GetTicks() + kAppWarningHoldMs;
        ReconcileMemory(MemorySource::AppWarning);
        break;
    default: break;
    }
    return true;
}
} // namespace

const char* ToString(ThermalState state)
{
    switch (state)
    {
    case ThermalState::Nominal: return "Nominal";
    case ThermalState::Fair: return "Fair";
    case ThermalState::Serious: return "Serious";
    case ThermalState::Critical: return "Critical";
    }
    return "?";
}

const char* ToString(LifecycleEvent event)
{
    switch (event)
    {
    case LifecycleEvent::WillEnterBackground: return "WillEnterBackground";
    case LifecycleEvent::DidEnterBackground: return "DidEnterBackground";
    case LifecycleEvent::WillEnterForeground: return "WillEnterForeground";
    case LifecycleEvent::DidEnterForeground: return "DidEnterForeground";
    case LifecycleEvent::Terminating: return "Terminating";
    }
    return "?";
}

const char* ToString(MemoryLevel level)
{
    switch (level)
    {
    case MemoryLevel::Normal: return "Normal";
    case MemoryLevel::Warning: return "Warning";
    case MemoryLevel::Critical: return "Critical";
    }
    return "?";
}

const char* ToString(MemorySource source)
{
    switch (source)
    {
    case MemorySource::SystemPressure: return "system pressure";
    case MemorySource::AppWarning: return "app memory warning";
    case MemorySource::Budget: return "budget watchdog";
    }
    return "?";
}

ListenerId AddListener(Listener listener)
{
    auto& s = State();
    ListenerId id = s.next_id++;
    s.listeners[id] = std::move(listener);
    return id;
}

void RemoveListener(ListenerId id)
{
    State().listeners.erase(id);
}

bool IsPaused()
{
    return State().paused;
}

void SetKeepScreenAwake(bool keep)
{
    State().keep_awake_requested = keep;
    ApplyKeepAwake();
}

std::vector<EventRecord> RecentEvents(size_t max_count)
{
    auto& s = State();
    std::lock_guard<std::mutex> lock(s.history_mutex);
    size_t count = std::min(max_count, s.history.size());
    return std::vector<EventRecord>(s.history.end() - count, s.history.end());
}

namespace Internal
{
void RecordEvent(const char* category, const std::string& text)
{
    auto& s = State();
    {
        std::lock_guard<std::mutex> lock(s.history_mutex);
        EventRecord record;
        record.time_s = SecondsSinceInit();
        record.category = category;
        record.text = text;
        s.history.push_back(std::move(record));
        while (s.history.size() > 128)
            s.history.pop_front();
    }
    Log("event %s: %s", category, text.c_str());
}

bool IsInBackground()
{
    return State().in_background;
}

void StartObservers()
{
    auto& s = State();
    if (!s.watch_installed)
    {
        SDL_AddEventWatch(WatchLifecycle, nullptr);
        s.watch_installed = true;
    }

    NSProcessInfo* info = NSProcessInfo.processInfo;
    s.thermal = FromNS(info.thermalState);
    s.low_power = info.lowPowerModeEnabled;

    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    NSOperationQueue* main = NSOperationQueue.mainQueue;

    s.thermal_observer = [center addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:main
        usingBlock:^(NSNotification*) {
            auto& st = State();
            ThermalState previous = st.thermal;
            st.thermal = FromNS(NSProcessInfo.processInfo.thermalState);
            if (st.thermal == previous)
                return;
            Internal::RecordEvent("thermal", std::string(ToString(previous)) + " -> " + ToString(st.thermal));
            ForEachListener([&](Listener& l) {
                if (l.on_thermal)
                    l.on_thermal(st.thermal, previous);
            });
        }];

    s.power_observer = [center addObserverForName:NSProcessInfoPowerStateDidChangeNotification object:nil queue:main
        usingBlock:^(NSNotification*) {
            auto& st = State();
            bool low = NSProcessInfo.processInfo.lowPowerModeEnabled;
            if (low == st.low_power)
                return;
            st.low_power = low;
            Internal::RecordEvent("power", low ? "Low Power Mode on" : "Low Power Mode off");
            ForEachListener([&](Listener& l) {
                if (l.on_low_power)
                    l.on_low_power(low);
            });
        }];

    // Kernel memory-pressure notifications, delivered on the main queue so callbacks stay on the main thread.
    s.pressure_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
        DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, dispatch_get_main_queue());
    dispatch_source_set_event_handler(s.pressure_source, ^{
        auto& st = State();
        unsigned long flags = dispatch_source_get_data(st.pressure_source);
        if (flags & DISPATCH_MEMORYPRESSURE_CRITICAL)
            st.pressure_level = MemoryLevel::Critical;
        else if (flags & DISPATCH_MEMORYPRESSURE_WARN)
            st.pressure_level = MemoryLevel::Warning;
        else
            st.pressure_level = MemoryLevel::Normal;
        ReconcileMemory(MemorySource::SystemPressure);
    });
    dispatch_resume(s.pressure_source);

    ApplyKeepAwake();
}

void StopObservers()
{
    auto& s = State();
    if (s.watch_installed)
    {
        SDL_RemoveEventWatch(WatchLifecycle, nullptr);
        s.watch_installed = false;
    }
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    if (s.thermal_observer)
        [center removeObserver:s.thermal_observer];
    if (s.power_observer)
        [center removeObserver:s.power_observer];
    s.thermal_observer = nil;
    s.power_observer = nil;
    if (s.pressure_source)
    {
        dispatch_source_cancel(s.pressure_source);
        s.pressure_source = nullptr;
    }
    SDL_EnableScreenSaver();
}

// Budget watchdog (iOS only). os_proc_available_memory is the number that decides jetsam, and the
// kernel pressure notifications can lag behind a fast allocation spike. Levels step down only with
// a margin so they do not flap around a threshold.
void PollMemoryBudget()
{
    auto& s = State();
    const uint64_t now = SDL_GetTicks();

    if (s.app_warning_until && now >= s.app_warning_until)
    {
        s.app_warning_until = 0;
        ReconcileMemory(MemorySource::AppWarning);
    }

#if TARGET_OS_IOS
    if (now - s.last_budget_poll < 250)
        return;
    s.last_budget_poll = now;

    constexpr uint64_t kWarning = 768ull << 20, kCritical = 384ull << 20, kMargin = 128ull << 20;
    const uint64_t available = AvailableMemoryBytes();
    MemoryLevel level = s.budget_level;
    switch (s.budget_level)
    {
    case MemoryLevel::Normal:
        if (available < kCritical)
            level = MemoryLevel::Critical;
        else if (available < kWarning)
            level = MemoryLevel::Warning;
        break;
    case MemoryLevel::Warning:
        if (available < kCritical)
            level = MemoryLevel::Critical;
        else if (available >= kWarning + kMargin)
            level = MemoryLevel::Normal;
        break;
    case MemoryLevel::Critical:
        if (available >= kCritical + kMargin)
            level = available >= kWarning + kMargin ? MemoryLevel::Normal : MemoryLevel::Warning;
        break;
    }
    if (level != s.budget_level)
    {
        s.budget_level = level;
        ReconcileMemory(MemorySource::Budget);
    }
#endif
}
} // namespace Internal

void Update()
{
    Internal::PollMemoryBudget();
}

} // namespace AppleShell
