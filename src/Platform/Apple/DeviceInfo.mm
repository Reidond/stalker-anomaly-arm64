// Device, memory budget, storage and display queries.

#include "ShellInternal.h"

#include <SDL3/SDL.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#include <os/proc.h>
#else
#import <AppKit/AppKit.h>
#include <IOKit/IOKitLib.h>
#endif

#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

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

void RunInAutoreleasePool(void (*fn)(void*), void* arg)
{
    @autoreleasepool
    {
        fn(arg);
    }
}

uint64_t ProcessFootprint()
{
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
}

ThermalState CurrentThermalState()
{
    return ThermalFromNS(NSProcessInfo.processInfo.thermalState);
}

uint64_t AvailableMemory()
{
#if TARGET_OS_IOS
    return uint64_t(os_proc_available_memory());
#else
    return 0;
#endif
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


// ---------------------------------------------------------------------------------------------
// Graphics profile hardware (GraphicsProfile.h)
// ---------------------------------------------------------------------------------------------

namespace Internal
{
Graphics::Hardware DetectHardware()
{
    Graphics::Hardware hw;
    @autoreleasepool
    {
#if TARGET_OS_IOS
        hw.family = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? "ipad" : "iphone";
        hw.machine = SysctlString("hw.machine");
#else
        hw.family = "mac";
        hw.machine = SysctlString("hw.model");
        hw.chip = SysctlString("machdep.cpu.brand_string");
#endif
        hw.memory_bytes = NSProcessInfo.processInfo.physicalMemory;

        // "Apple M2 Pro" (a Mac), "Apple M3 GPU" or "Apple A17 Pro GPU" (iPadOS): the Metal device's name
        if (id<MTLDevice> device = MTLCreateSystemDefaultDevice())
        {
            std::string name = device.name.UTF8String ? device.name.UTF8String : "";
            if (name.size() > 4 && name.compare(name.size() - 4, 4, " GPU") == 0)
                name.resize(name.size() - 4);
            if (!name.empty())
                hw.chip = name;
        }
        // M1 = 1, M2 = 2 ...; A14 = 1, A15 = 2 ...
        for (size_t i = 0; i + 1 < hw.chip.size(); ++i)
        {
            if ((hw.chip[i] == 'M' || hw.chip[i] == 'A') && isdigit((unsigned char)hw.chip[i + 1]) && (i == 0 || hw.chip[i - 1] == ' '))
            {
                const int number = atoi(hw.chip.c_str() + i + 1);
                hw.chip_generation = hw.chip[i] == 'M' ? number : std::max(0, number - 13);
                break;
            }
        }
#if !TARGET_OS_IOS
        // the GPU core count is in the registry on Apple Silicon Macs
        io_service_t accelerator = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"));
        if (accelerator)
        {
            CFTypeRef cores = IORegistryEntryCreateCFProperty(accelerator, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0);
            if (cores)
            {
                if (CFGetTypeID(cores) == CFNumberGetTypeID())
                {
                    int n = 0;
                    CFNumberGetValue((CFNumberRef)cores, kCFNumberIntType, &n);
                    hw.gpu_cores = n;
                }
                CFRelease(cores);
            }
            IOObjectRelease(accelerator);
        }
        hw.builtin_display = CGDisplayIsBuiltin(CGMainDisplayID()) != 0;
#endif
    }
    const DisplayInfo display = QueryDisplayInfo(nullptr);
    hw.native_width = display.native_width;
    hw.native_height = display.native_height;
    hw.max_refresh_hz = display.max_refresh_hz;
    return hw;
}
} // namespace Internal
} // namespace AppleShell
