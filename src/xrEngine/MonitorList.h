#pragma once

struct xr_token;

ENGINE_API extern xr_token* vid_monitor_token;
ENGINE_API extern xr_string  vid_monitor_name;

ENGINE_API void     fill_vid_monitor_list();
ENGINE_API void     free_vid_monitor_list();
ENGINE_API void     refresh_vid_monitor_list();
ENGINE_API HMONITOR ResolveSelectedMonitor();

ENGINE_API extern volatile long g_monitor_list_dirty;

ENGINE_API HMONITOR GetStartupMonitor();
ENGINE_API void     SetStartupMonitor(HMONITOR h);
ENGINE_API void     ResetStartupMonitor();

#ifdef XR_PLATFORM_APPLE
// The SDL_DisplayID behind a monitor handle from ResolveSelectedMonitor()/GetStartupMonitor(), 0 for Auto.
ENGINE_API unsigned MonitorToSdlDisplay(HMONITOR h);
#endif
