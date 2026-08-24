// vim: set ts=4 sw=4 tw=99 noet:
//
// AMX Mod X, based on AMX Mod by Aleksander Naszko ("OLO").
// Copyright (C) The AMX Mod X Development Team.
// Copyright (C) 2004 Lukasz Wlasinski.
//
// This software is licensed under the GNU General Public License, version 3 or higher.
// Additional exceptions apply. For full license details, see LICENSE.txt or visit:
//     https://alliedmods.net/amxmodx-license

//
// DODX Module - Control Point Natives
// Ported from dodfun NPD.cpp for KTP extension mode
//

#include "amxxmodule.h"
#include "dodx.h"

// dodx_objectives_get_num()
static cell AMX_NATIVE_CALL dodx_objectives_get_num(AMX *amx, cell *params)
{
	return mObjects.count;
}

// dodx_objective_get_data(index, CP_VALUE:key, szvalue[] = "", len = 0)
static cell AMX_NATIVE_CALL dodx_objective_get_data(AMX *amx, cell *params)
{
	int index = params[1];
	if (index < 0 || index >= mObjects.count)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "CP index out of range (%d)", index);
		return 0;
	}

	int len = params[4];
	CP_VALUE key = (CP_VALUE)params[2];

	switch (key)
	{
		case CP_edict:
			return ENTINDEX_SAFE(mObjects.obj[index].pEdict);
		case CP_area:
			GET_CAPTURE_AREA(index)
			return mObjects.obj[index].areaflags == 2 ? ENTINDEX_SAFE(mObjects.obj[index].pAreaEdict) : 0;
		case CP_index:
			return mObjects.obj[index].index;
		case CP_owner:
			return mObjects.obj[index].owner;
		case CP_default_owner:
			return mObjects.obj[index].default_owner;
		case CP_visible:
			return mObjects.obj[index].visible;
		case CP_icon_neutral:
			return mObjects.obj[index].icon_neutral;
		case CP_icon_allies:
			return mObjects.obj[index].icon_allies;
		case CP_icon_axis:
			return mObjects.obj[index].icon_axis;
		case CP_origin_x:
			return (int)mObjects.obj[index].origin_x;
		case CP_origin_y:
			return (int)mObjects.obj[index].origin_y;
		case CP_can_touch:
			if (!mObjects.obj[index].pEdict || FNullEnt(mObjects.obj[index].pEdict))
				return 0;
			return GET_CP_PD(mObjects.obj[index].pEdict).can_touch;
		case CP_pointvalue:
			if (!mObjects.obj[index].pEdict || FNullEnt(mObjects.obj[index].pEdict))
				return 0;
			return GET_CP_PD(mObjects.obj[index].pEdict).pointvalue;
		case CP_points_for_cap:
			if (!mObjects.obj[index].pEdict || FNullEnt(mObjects.obj[index].pEdict))
				return 0;
			return GET_CP_PD(mObjects.obj[index].pEdict).points_for_player;
		case CP_team_points:
			if (!mObjects.obj[index].pEdict || FNullEnt(mObjects.obj[index].pEdict))
				return 0;
			return GET_CP_PD(mObjects.obj[index].pEdict).points_for_team;
		case CP_model_body_neutral:
			if (!mObjects.obj[index].pEdict || FNullEnt(mObjects.obj[index].pEdict))
				return 0;
			return GET_CP_PD(mObjects.obj[index].pEdict).model_body_neutral;
		case CP_model_body_allies:
			if (!mObjects.obj[index].pEdict || FNullEnt(mObjects.obj[index].pEdict))
				return 0;
			return GET_CP_PD(mObjects.obj[index].pEdict).model_body_allies;
		case CP_model_body_axis:
			if (!mObjects.obj[index].pEdict || FNullEnt(mObjects.obj[index].pEdict))
				return 0;
			return GET_CP_PD(mObjects.obj[index].pEdict).model_body_axis;

		// strings
		case CP_name:
			if (len && mObjects.obj[index].pEdict)
				MF_SetAmxString(amx, params[3], STRING(mObjects.obj[index].pEdict->v.netname), len);
			return 1;
		case CP_cap_message:
			if (len && mObjects.obj[index].pEdict && !FNullEnt(mObjects.obj[index].pEdict))
				MF_SetAmxString(amx, params[3], GET_CP_PD(mObjects.obj[index].pEdict).cap_message, len);
			return 1;
		case CP_reset_capsound:
			if (len && mObjects.obj[index].pEdict)
				MF_SetAmxString(amx, params[3], STRING(mObjects.obj[index].pEdict->v.noise), len);
			return 1;
		case CP_allies_capsound:
			if (len && mObjects.obj[index].pEdict)
				MF_SetAmxString(amx, params[3], STRING(mObjects.obj[index].pEdict->v.noise1), len);
			return 1;
		case CP_axis_capsound:
			if (len && mObjects.obj[index].pEdict)
				MF_SetAmxString(amx, params[3], STRING(mObjects.obj[index].pEdict->v.noise2), len);
			return 1;
		case CP_targetname:
			if (len && mObjects.obj[index].pEdict)
				MF_SetAmxString(amx, params[3], STRING(mObjects.obj[index].pEdict->v.targetname), len);
			return 1;
		case CP_model_neutral:
			if (len && mObjects.obj[index].pEdict && !FNullEnt(mObjects.obj[index].pEdict))
				MF_SetAmxString(amx, params[3], GET_CP_PD(mObjects.obj[index].pEdict).model_neutral, len);
			return 1;
		case CP_model_allies:
			if (len && mObjects.obj[index].pEdict && !FNullEnt(mObjects.obj[index].pEdict))
				MF_SetAmxString(amx, params[3], GET_CP_PD(mObjects.obj[index].pEdict).model_allies, len);
			return 1;
		case CP_model_axis:
			if (len && mObjects.obj[index].pEdict && !FNullEnt(mObjects.obj[index].pEdict))
				MF_SetAmxString(amx, params[3], GET_CP_PD(mObjects.obj[index].pEdict).model_axis, len);
			return 1;

		default:
			break;
	}
	return 1;
}

// dodx_objective_set_data(index, CP_VALUE:key, ivalue, const szvalue[] = "")
static cell AMX_NATIVE_CALL dodx_objective_set_data(AMX *amx, cell *params)
{
	int index = params[1];
	if (index < 0 || index >= mObjects.count)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "CP index out of range (%d)", index);
		return 0;
	}

	edict_t *pent = mObjects.obj[index].pEdict;
	if (!pent || FNullEnt(pent))
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "CP %d has invalid entity", index);
		return 0;
	}

	int iLen;
	int ivalue = params[3];
	char *szValue = MF_GetAmxString(amx, params[4], 0, &iLen);

	CP_VALUE key = (CP_VALUE)params[2];
	switch (key)
	{
		case CP_owner:
			mObjects.UpdateOwner(index, ivalue);
			return 1;
		case CP_default_owner:
			mObjects.obj[index].default_owner = ivalue;
			GET_CP_PD(pent).default_owner = ivalue;
			return 1;
		case CP_visible:
			mObjects.obj[index].visible = ivalue;
			mObjects.obj[index].pEdict->v.spawnflags = 1 - ivalue;
			return 1;
		case CP_icon_neutral:
			mObjects.obj[index].icon_neutral = ivalue;
			GET_CP_PD(pent).icon_neutral = ivalue;
			return 1;
		case CP_icon_allies:
			mObjects.obj[index].icon_allies = ivalue;
			GET_CP_PD(pent).icon_allies = ivalue;
			return 1;
		case CP_icon_axis:
			mObjects.obj[index].icon_axis = ivalue;
			GET_CP_PD(pent).icon_axis = ivalue;
			return 1;
		case CP_origin_x:
			mObjects.obj[index].origin_x = (float)ivalue;
			return 1;
		case CP_origin_y:
			mObjects.obj[index].origin_y = (float)ivalue;
			return 1;
		case CP_can_touch:
			GET_CP_PD(pent).can_touch = ivalue;
			return 1;
		case CP_pointvalue:
			GET_CP_PD(pent).pointvalue = ivalue;
			return 1;
		case CP_points_for_cap:
			GET_CP_PD(pent).points_for_player = ivalue;
			return 1;
		case CP_team_points:
			GET_CP_PD(pent).points_for_team = ivalue;
			return 1;
		case CP_model_body_neutral:
			GET_CP_PD(pent).model_body_neutral = ivalue;
			return 1;
		case CP_model_body_allies:
			GET_CP_PD(pent).model_body_allies = ivalue;
			return 1;
		case CP_model_body_axis:
			GET_CP_PD(pent).model_body_axis = ivalue;
			return 1;

		// Strings — ALLOC_STRING copies into the engine string pool. MAKE_STRING would
		// alias AMXX's transient shared string buffer, which the next native string
		// fetch overwrites, corrupting the CP/area name.
		case CP_name:
			mObjects.obj[index].pEdict->v.netname = ALLOC_STRING(szValue);
			return 1;
		case CP_cap_message:
			strncpy(GET_CP_PD(mObjects.obj[index].pEdict).cap_message, szValue, 255);
			GET_CP_PD(mObjects.obj[index].pEdict).cap_message[255] = '\0';
			return 1;
		case CP_reset_capsound:
			mObjects.obj[index].pEdict->v.noise = ALLOC_STRING(szValue);
			return 1;
		case CP_allies_capsound:
			mObjects.obj[index].pEdict->v.noise1 = ALLOC_STRING(szValue);
			return 1;
		case CP_axis_capsound:
			mObjects.obj[index].pEdict->v.noise2 = ALLOC_STRING(szValue);
			return 1;
		case CP_targetname:
			mObjects.obj[index].pEdict->v.targetname = ALLOC_STRING(szValue);
			return 1;
		case CP_model_neutral:
			strncpy(GET_CP_PD(pent).model_neutral, szValue, 255);
			GET_CP_PD(pent).model_neutral[255] = '\0';
			return 1;
		case CP_model_allies:
			strncpy(GET_CP_PD(pent).model_allies, szValue, 255);
			GET_CP_PD(pent).model_allies[255] = '\0';
			return 1;
		case CP_model_axis:
			strncpy(GET_CP_PD(pent).model_axis, szValue, 255);
			GET_CP_PD(pent).model_axis[255] = '\0';
			return 1;

		default:
			break;
	}

	return 1;
}

// dodx_objectives_reinit(player = 0)
static cell AMX_NATIVE_CALL dodx_objectives_reinit(AMX *amx, cell *params)
{
	int player = params[1];
	if (player < 0 || player > gpGlobals->maxClients)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "Player index out of range (%d)", player);
		return 0;
	}
	mObjects.InitObj(player == 0 ? MSG_ALL : MSG_ONE, player == 0 ? NULL : GETEDICT(player));

	return 1;
}

// dodx_area_get_data(index, CA_VALUE:key, szvalue[] = "", len = 0)
static cell AMX_NATIVE_CALL dodx_area_get_data(AMX *amx, cell *params)
{
	int index = params[1];
	if (index < 0 || index >= mObjects.count)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "CP index out of range (%d)", index);
		return 0;
	}
	int len = params[4];
	CA_VALUE key = (CA_VALUE)params[2];

	GET_CAPTURE_AREA(index)

	switch (key)
	{
		case CA_edict:
			return ENTINDEX_SAFE(mObjects.obj[index].pAreaEdict);
		case CA_allies_numcap:
			return GET_CA_PD(mObjects.obj[index].pAreaEdict).allies_numcap;
		case CA_axis_numcap:
			return GET_CA_PD(mObjects.obj[index].pAreaEdict).axis_numcap;
		case CA_timetocap:
			return GET_CA_PD(mObjects.obj[index].pAreaEdict).cap_time;
		case CA_can_cap:
			return GET_CA_PD(mObjects.obj[index].pAreaEdict).can_cap;
		case CA_num_allies:
			return GET_CA_PD(mObjects.obj[index].pAreaEdict).num_allies;
		case CA_num_axis:
			return GET_CA_PD(mObjects.obj[index].pAreaEdict).num_axis;
		case CA_is_capturing:
			return GET_CA_PD(mObjects.obj[index].pAreaEdict).is_capturing;
		case CA_capturing_team:
			return GET_CA_PD(mObjects.obj[index].pAreaEdict).capturing_team;
		case CA_owning_team:
			return GET_CA_PD(mObjects.obj[index].pAreaEdict).owning_team;
		case CA_cap_mode:
			return GET_CA_PD(mObjects.obj[index].pAreaEdict).cap_mode;
		case CA_time_remaining:
		{
			float t = GET_CA_PD(mObjects.obj[index].pAreaEdict).time_remaining;
			return amx_ftoc(t);
		}

		// strings
		case CA_target:
			if (len)
				MF_SetAmxString(amx, params[3], STRING(mObjects.obj[index].pAreaEdict->v.target), len);
			return 1;
		case CA_sprite:
			if (len)
				MF_SetAmxString(amx, params[3], GET_CA_PD(mObjects.obj[index].pAreaEdict).hud_sprite, len);
			return 1;
	}
	return 1;
}

// dodx_area_set_data(index, CA_VALUE:key, ivalue, const szvalue[] = "")
static cell AMX_NATIVE_CALL dodx_area_set_data(AMX *amx, cell *params)
{
	int index = params[1];
	if (index < 0 || index >= mObjects.count)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "CP index out of range (%d)", index);
		return 0;
	}
	int iLen;
	int ivalue = params[3];
	char *szValue = MF_GetAmxString(amx, params[4], 0, &iLen);

	CA_VALUE key = (CA_VALUE)params[2];

	GET_CAPTURE_AREA(index)

	switch (key)
	{
		case CA_allies_numcap:
			GET_CA_PD(mObjects.obj[index].pAreaEdict).allies_numcap = ivalue;
			return 1;
		case CA_axis_numcap:
			GET_CA_PD(mObjects.obj[index].pAreaEdict).axis_numcap = ivalue;
			return 1;
		case CA_timetocap:
			GET_CA_PD(mObjects.obj[index].pAreaEdict).cap_time = ivalue;
			return 1;
		case CA_can_cap:
			GET_CA_PD(mObjects.obj[index].pAreaEdict).can_cap = ivalue;
			return 1;
		// strings — ALLOC_STRING (copy into engine pool); MAKE_STRING would alias
		// AMXX's transient string buffer and corrupt the area target.
		case CA_target:
			mObjects.obj[index].pAreaEdict->v.target = ALLOC_STRING(szValue);
			return 1;
		case CA_sprite:
			strncpy(GET_CA_PD(mObjects.obj[index].pAreaEdict).hud_sprite, szValue, 255);
			GET_CA_PD(mObjects.obj[index].pAreaEdict).hud_sprite[255] = '\0';
			return 1;

		default:
			break;
	}
	return 1;
}

// KTP: dodx_area_get_bounds(index, Float:mins[3], Float:maxs[3])
//
// The capture zone's world-space bounding box, read straight off the brush
// entity. Extension-mode safe: pEdict->v.* is HL SDK, no fakemeta involved.
//
// This exists because zone membership cannot be inferred from the control
// point's position. The flag prop is a separate entity from its trigger and
// the two are not co-located -- across the DoD pool some control points sit
// entirely OUTSIDE their own capture zone, and on dod_jagd the gap is on the
// order of thousands of units. Any prop-anchored radius is therefore too small
// on one map and far too large on another at the same time, whatever value it
// is given.
//
// absmin/absmax rather than mins/maxs: the former are already world-space and
// account for the entity's origin, which is what a containment test needs.
static cell AMX_NATIVE_CALL dodx_area_get_bounds(AMX *amx, cell *params)
{
	int index = params[1];
	if (index < 0 || index >= mObjects.count)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "CP index out of range (%d)", index);
		return 0;
	}

	GET_CAPTURE_AREA(index)

	edict_t *pArea = mObjects.obj[index].pAreaEdict;
	if (!pArea || pArea->free)
		return 0;

	// A zero-volume box matches nothing, which reads downstream as "this flag
	// never sees a capture" -- indistinguishable from a quiet flag. Fail here
	// so the caller can report it instead of silently under-counting forever.
	if (pArea->v.absmax[0] - pArea->v.absmin[0] <= 0.0f ||
	    pArea->v.absmax[1] - pArea->v.absmin[1] <= 0.0f ||
	    pArea->v.absmax[2] - pArea->v.absmin[2] <= 0.0f)
		return 0;

	cell *mins = MF_GetAmxAddr(amx, params[2]);
	cell *maxs = MF_GetAmxAddr(amx, params[3]);
	for (int i = 0; i < 3; i++)
	{
		mins[i] = amx_ftoc(pArea->v.absmin[i]);
		maxs[i] = amx_ftoc(pArea->v.absmax[i]);
	}

	return 1;
}

AMX_NATIVE_INFO cp_Natives[] = {
	{ "dodx_objectives_get_num",  dodx_objectives_get_num },
	{ "dodx_objective_get_data",  dodx_objective_get_data },
	{ "dodx_objective_set_data",  dodx_objective_set_data },
	{ "dodx_objectives_reinit",   dodx_objectives_reinit },
	{ "dodx_area_get_data",       dodx_area_get_data },
	{ "dodx_area_set_data",       dodx_area_set_data },
	{ "dodx_area_get_bounds",     dodx_area_get_bounds },
	{ NULL, NULL }
};
