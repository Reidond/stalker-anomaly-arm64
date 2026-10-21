// Device, memory budget, storage and display queries.

#include "ShellInternal.h"

#include <SDL3/SDL.h>

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#include <os/proc.h>
#else
#import <AppKit/AppKit.h>
#endif

#include <sys/mount.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <cstdio>

namespace AppleShell
{
namespace
{
std::string SysctlString(const char* name)
{
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0)
        return {};
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0)
        return {};
    value.resize(strlen(value.c_str()));
    return value;
}

ThermalState ThermalFromNS(NSProcessInfoThermalState s)
{
    switch (s)
    {
    case NSProcessInfoThermalStateFair: return ThermalState::Fair;
    case NSProcessInfoThermalStateSerious: return ThermalState::Serious;
    case NSProcessInfoThermalStateCritical: return ThermalState::Critical;
    default: return ThermalState::Nominal;
    }
}
} // namespace

std::string FormatBytes(uint64_t bytes)
{
    char buffer[32];
    const double gb = double(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0)
        snprintf(buffer, sizeof(buffer), "%.2f GiB", gb);
    else
        snprintf(buffer, sizeof(buffer), "%.0f MiB", double(bytes) / (1024.0 * 1024.0));
    return buffer;
}

DeviceInfo QueryDeviceInfo()
{
    DeviceInfo info;
    @autoreleasepool
    {
        NSProcessInfo* process = NSProcessInfo.processInfo;
        NSOperatingSystemVersion v = process.operatingSystemVersion;
#if TARGET_OS_IOS
        info.model = SysctlString("hw.machine");
        const char* os_name = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? "iPadOS" : "iOS";
        info.available_memory = uint64_t(os_proc_available_memory());
        info.has_memory_budget = true;
#else
        info.model = SysctlString("hw.model");
        const char* os_name = "macOS";
#endif
        char os[64];
        snprintf(os, sizeof(os), "%s %ld.%ld.%ld", os_name, long(v.majorVersion), long(v.minorVersion), long(v.patchVersion));
        info.os = os;
        info.cpu_cores = uint32_t(process.activeProcessorCount);
        info.physical_memory = process.physicalMemory;
        info.thermal = ThermalFromNS(process.thermalState);
        info.low_power_mode = process.lowPowerModeEnabled;

        NSURL* home = [NSURL fileURLWithPath:NSHomeDirectory()];
        NSDictionary<NSURLResourceKey, id>* values = [home resourceValuesForKeys:@[
            NSURLVolumeTotalCapacityKey, NSURLVolumeAvailableCapacityForImportantUsageKey] error:nil];
        info.storage_total = [values[NSURLVolumeTotalCapacityKey] unsignedLongLongValue];
        info.storage_free_important = [values[NSURLVolumeAvailableCapacityForImportantUsageKey] unsignedLongLongValue];

        struct statfs fs;
        if (statfs(NSHomeDirectory().fileSystemRepresentation, &fs) == 0)
            info.storage_free_raw = uint64_t(fs.f_bavail) * uint64_t(fs.f_bsize);
    }
    return info;
}

DisplayInfo QueryDisplayInfo(SDL_Window* window)
{
    DisplayInfo info;

    SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
    if (!display)
        display = SDL_GetPrimaryDisplay();
    if (const char* name = SDL_GetDisplayName(display))
        info.name = name;

    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display))
    {
        info.refresh_hz = mode->refresh_rate > 0 ? mode->refresh_rate : 60.0f;
        info.max_refresh_hz = info.refresh_hz;
        info.scale = info.native_scale = mode->pixel_density;
        info.native_width = int(mode->w * mode->pixel_density + 0.5f);
        info.native_height = int(mode->h * mode->pixel_density + 0.5f);
    }

#if TARGET_OS_IOS
    UIScreen* screen = nil;
    if (window)
    {
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        UIWindow* ui_window = (__bridge UIWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
        screen = ui_window.screen;
    }
    if (!screen)
        screen = UIScreen.mainScreen;
    if (screen)
    {
        CGSize native = screen.nativeBounds.size;
        info.native_width = int(std::max(native.width, native.height));
        info.native_height = int(std::min(native.width, native.height));
        info.native_scale = float(screen.nativeScale);
        info.max_refresh_hz = float(screen.maximumFramesPerSecond);
    }
#else
    if (NSScreen* screen = NSScreen.mainScreen)
    {
        info.max_refresh_hz = float(screen.maximumFramesPerSecond);
        info.native_scale = float(screen.backingScaleFactor);
    }
#endif
    info.max_refresh_hz = std::max(info.max_refresh_hz, info.refresh_hz);
    info.promotion = info.max_refresh_hz > 61.0f;

    if (window)
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        info.window_width = float(w);
        info.window_height = float(h);
        SDL_GetWindowSizeInPixels(window, &info.drawable_width, &info.drawable_height);
        info.scale = SDL_GetWindowPixelDensity(window);

        SDL_Rect safe{};
        if (SDL_GetWindowSafeArea(window, &safe) && safe.w > 0 && safe.h > 0)
        {
            info.safe_area_points.left = float(safe.x);
            info.safe_area_points.top = float(safe.y);
            info.safe_area_points.right = float(w - (safe.x + safe.w));
            info.safe_area_points.bottom = float(h - (safe.y + safe.h));
        }
        const float s = info.scale;
        info.safe_area_pixels = {info.safe_area_points.left * s, info.safe_area_points.top * s,
            info.safe_area_points.right * s, info.safe_area_points.bottom * s};
    }
    return info;
}

} // namespace AppleShell
