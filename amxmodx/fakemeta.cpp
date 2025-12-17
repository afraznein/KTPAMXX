// vim: set ts=4 sw=4 tw=99 noet:
//
// KTP AMX, based on AMX Mod X by Aleksander Naszko ("OLO").
// Copyright (C) The AMX Mod X Development Team, 2025 KTP.
//
// This software is licensed under the GNU General Public License, version 3 or higher.
// Additional exceptions apply. For full license details, see LICENSE.txt or visit:
//     https://alliedmods.net/amxmodx-license

#include "amxmodx.h"
#include "fakemeta.h"

int LoadMetamodPlugin(const char *path, void **handle, PLUG_LOADTIME now)
{
	// KTP: If running without Metamod, we can't load Metamod plugins
	// But modules can still work via AMXX_Query/AMXX_Attach for natives
	if (!g_bRunningWithMetamod)
	{
		// Return success but don't actually load via Metamod
		// The module's AMXX_Attach was already called, so natives are registered
		return 1;
	}

	int err = 0;
	if ( (err = LOAD_PLUGIN(PLID, path, now, handle)) || !*handle)
	{
		LOG_MESSAGE(PLID, "Can't Attach Module \"%s\".", path);
		return 0;
	}

	return 1;
}

int UnloadMetamodPlugin(void *handle)
{
	// KTP: If running without Metamod, nothing to unload
	if (!g_bRunningWithMetamod)
	{
		return 1;
	}

	if (UNLOAD_PLUGIN_BY_HANDLE(PLID, (void *)handle, PT_ANYTIME, PNL_PLUGIN))
	{
		return 0;
	}

	return 1;
}
