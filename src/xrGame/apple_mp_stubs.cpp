////////////////////////////////////////////////////////////////////////////
//	Module 		: apple_mp_stubs.cpp
//	Description : stand-ins for the multiplayer entry points that single-player code still calls.
//	              Compiled by the Apple port only (cmake/targets/xrGame.cmake, ANOMALY_XRGAME_MULTIPLAYER=OFF);
//	              the MSVC project does not list this file. Owned by the xrgame-mp-stub shard.
//
//	What is here: the multiplayer game types, GameSpy, the award system and the MP screenshots are not built, but
//	single-player objects still name a few of their symbols (the console variables xrServer::GetServerInfo prints, the
//	screenshot proxy that xrServer owns, the CTA getters of the main HUD). Each stand-in logs and refuses; none of
//	them is reachable in a single-player game, because CLevel::net_start1 refuses every game type but "single".
//	What is not here: single-player code that only names a multiplayer class is guarded with XRGAME_NO_MULTIPLAYER
//	(object factory, GameSpy script exports, the MP console commands, RTTI casts: mp_cl_game_cast / mp_sv_game_cast).
//	Find the next missing symbol with tools/xrgame_mp_symbols.py (docs/ios-port/XRGAME-SHARDS.md).
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "game_sv_mp.h"
#include "game_cl_mp.h"
#include "game_cl_capture_the_artefact.h"

namespace
{
void mp_refused(const char* what)
{
	Msg("! Multiplayer is not available in this build (%s)", what);
}
} // namespace

// ---- console variables of the multiplayer game types (xrServer::GetServerInfo reads them) ---------------------------
s32 g_sv_dm_dwFragLimit = 0;
s32 g_sv_dm_dwTimeLimit = 0;
s32 g_sv_ah_dwArtefactsNum = 0;
int g_sv_ah_iReinforcementTime = 0;
int g_sv_mp_iDumpStatsPeriod = 0;
BOOL g_sv_mp_save_proxy_screenshots = FALSE;
BOOL g_sv_mp_save_proxy_configs = FALSE;

// ---- game_sv_mp -------------------------------------------------------------------------------------------------
bool game_sv_mp::IsPlayerBanned(char const* hexstr_digest, shared_str& by_who)
{
	mp_refused("game_sv_mp::IsPlayerBanned");
	return false;
}

void game_sv_mp::async_statistics_collector::set_responded(ClientID clientID)
{
}

// ---- game_cl_mp: MP screenshots (the screenshot proxy of xrServer) --------------------------------------------------
LPCSTR game_cl_mp::make_file_name(LPCSTR session_id, string_path& dest)
{
	mp_refused("game_cl_mp::make_file_name");
	dest[0] = 0;
	return dest;
}

void game_cl_mp::generate_file_name(string_path& file_name, LPCSTR file_suffix, SYSTEMTIME const& date_time)
{
	mp_refused("game_cl_mp::generate_file_name");
	file_name[0] = 0;
}

void game_cl_mp::decompress_and_save_screenshot(LPCSTR file_name, u8* data, u32 data_size, u32 file_size)
{
	mp_refused("screenshots");
}

// ---- game_cl_CaptureTheArtefact (the main HUD warning icon) ---------------------------------------------------------
u16 game_cl_CaptureTheArtefact::GetGreenArtefactOwnerID() const
{
	return 0;
}

u16 game_cl_CaptureTheArtefact::GetBlueArtefactOwnerID() const
{
	return 0;
}
