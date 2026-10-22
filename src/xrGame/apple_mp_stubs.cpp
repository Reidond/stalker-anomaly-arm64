////////////////////////////////////////////////////////////////////////////
//	Module 		: apple_mp_stubs.cpp
//	Description : stand-ins for the multiplayer entry points that single-player code still calls.
//	              Compiled by the Apple port only (cmake/targets/xrGame.cmake, ANOMALY_XRGAME_MULTIPLAYER=OFF);
//	              the MSVC project does not list this file. Owned by the xrgame-mp-stub shard.
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

// The mp-stub shard adds the stubs here, as the link of the full xrGame library asks for them
// (see docs/ios-port/XRGAME-SHARDS.md, "Multiplayer exclusion").
