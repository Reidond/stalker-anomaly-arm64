// Apple stand-ins for Windows-only pieces of the engine that other modules still reference by name. Compiled by
// cmake/targets/xrEngine.cmake only; the Windows build gets the real ones from ReShadeCompat and the Discord SDK glue.
#include "stdafx.h"

#ifdef XR_PLATFORM_APPLE

// ReShade is a Windows add-on (src/ReShadeCompat). The callers check `use_reshade` (defined in x_ray.cpp) first.
bool init_reshade() { return false; }
void unregister_reshade() {}
void render_reshade_effects() {}

#endif // XR_PLATFORM_APPLE
