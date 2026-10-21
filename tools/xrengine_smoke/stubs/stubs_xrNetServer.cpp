// Stand-in for xrNetServer: the file-allow list x_ray.cpp builds the authenticated settings with (multiplayer integrity
// checks). Everything is allowed, nothing is checked.
#include "../../../src/xrEngine/stdafx.h"
typedef const GUID& REFIID; // TODO(shim): REFIID is requested for the shim; net_shared.h names it
#include "../../../src/xrNetServer/NET_AuthCheck.h"

bool allow_to_include_path(const xr_auth_strings_t&, LPCSTR) { return true; }

void fill_auth_check_params(xr_auth_strings_t& ignore, xr_auth_strings_t& check)
{
	ignore.clear();
	check.clear();
}
