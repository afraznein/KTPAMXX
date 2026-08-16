// vim: set ts=4 sw=4 tw=99 noet:
//
// AMX Mod X, based on AMX Mod by Aleksander Naszko ("OLO").
// Copyright (C) The AMX Mod X Development Team.
//
// This software is licensed under the GNU General Public License, version 3 or higher.
// Additional exceptions apply. For full license details, see LICENSE.txt or visit:
//     https://alliedmods.net/amxmodx-license

//
// DODX Functions
//

#if defined _dodx_included
	#endinput
#endif
#define _dodx_included

#include <dodconst>
#include <dodstats>

#pragma reqclass xstats
#if !defined AMXMODX_NOAUTOLOAD
	#pragma defclasslib xstats dodx
#endif

/************* Shared Natives Start ********************************/

/* Forward types */
enum {
  XMF_DAMAGE = 0,
  XMF_DEATH,
  XMF_SCORE,
};

/* Compatibility no-op — stats forwards are registered unconditionally at
   plugin load; calling this changes nothing. */
native register_statsfwd(ftype);

/* Function is called after player to player attacks ,
* if players were damaged by teammate TA is set to 1 */
forward client_damage(attacker, victim, damage, wpnindex, hitplace, TA);

/**
 * KTP: Pre-damage forward for damage modification.
 * Called BEFORE damage is finalized - return value modifies the damage.
 *
 * @param attacker   Attacker player index. Never 0: for world/environment or
 *                   self damage the module falls back to the victim, so
 *                   attacker == victim (client_damage behaves the same way)
 * @param victim     Victim player index
 * @param damage     Original damage amount
 * @param wpnindex   Weapon ID (DODW_* constants from dodconst.inc)
 * @param hitplace   Hitgroup (1=head, etc.)
 * @param TA         Team attack (1 if attacker and victim are same team)
 * @return           Modified damage value:
 *                   - Return same damage value to keep unchanged
 *                   - Return lower value to reduce damage (player is healed by difference)
 *                   - Return 0 to completely block damage
 *
 * @note Reduction is a post-hoc heal-back off the Damage usermsg, gated on the
 *       victim still being alive — if the raw damage kills, the heal-back never
 *       runs, so 100% reduction cannot save a raw-lethal hit. A true
 *       pre-TakeDamage hook is deliberately not built.
 * @note Grenade weapon IDs: DODW_HANDGRENADE (13), DODW_STICKGRENADE (14), DODW_MILLS_BOMB (36)
 *       DODW_STICKGRENADE_EX (15), DODW_HANDGRENADE_EX (16)
 */
forward dod_damage_pre(attacker, victim, damage, wpnindex, hitplace, TA);

/* Function is called after player death ,
* if player was killed by teammate TK is set to 1.
* Suicides and world kills arrive via the DeathMsg path with TK=0, hitplace=0,
* killer possibly 0, and the weapon resolved from the kill logname. If the
* Damage hook and DeathMsg race past the 33ms dedup window, the TK flag can
* be lost. */
forward client_death(killer, victim, wpnindex, hitplace, TK);

/* Function is called if player scored  */
forward client_score(id, score, total);

/* This Forward is called when a player changes team */
forward dod_client_changeteam(id, team, oldteam);

/* This Forward is called if a player changes class, but just after spawn */
forward dod_client_changeclass(id, class, oldclass);

/* This Forward is called when a player spawns */
forward dod_client_spawn(id);

/* This will be called whenever a player scopes or unscopes
value = 1 scope up
value = 0 scope down */
forward dod_client_scope(id, value);

/* This will be called whenever a player drops a weapon 
weapon is weapon dropped or picked up 
value = 1 picked up
value = 0 dropped */
forward dod_client_weaponpickup(id, weapon, value);

/* Called whenever the the player goes to or comes from prone position 
value = 1 going down
value = 0 getting up */
forward dod_client_prone(id, value);

/* This will be called whenever a player switches a weapon */
forward dod_client_weaponswitch(id, wpnew, wpnold);

/* Fired per weapon actuation, including pure-miss shots the client_damage
   hits-stream never sees. Coverage envelope:
   - Firearm shots are detected via CurWeapon clip decrement: never fires for
     bots, suppressed while stats are paused (dodx_set_stats_paused), no
     dry-fire events, and MG42 uses a clip+2 heuristic.
   - Grenades/rockets also dispatch here via a separate TraceLine path that
     is NOT pause-gated; melee arrives via the damage path (pause-gated,
     hit-only). Filter by firearm weapon id for shots-only use.
   gametime is gpGlobals->time at the fire. */
forward dod_client_weapon_fire(id, weapon, Float:gametime);

/* Forward for when a grenade explodes and its location */
forward dod_grenade_explosion(id, Float:pos[3], wpnid);

/* Forward for when a rocket explodes and its location */
forward dod_rocket_explosion(id, Float:pos[3], wpnid);

/* Forward for when a player picks up a object.
   objid is the dod_object ENTITY INDEX, not an object-type id. */
forward dod_client_objectpickup(id, objid, Float:pos[3], value);

/* Forward for when a users stamina decreases */
forward dod_client_stamina(id, stamina);

/* We want to get just the weapon of whichever type that the player is on him
Use DODWT_* in dodconst.inc for type.
Note: the Webley (id 28) is typed PRIMARY in the module's weapon table, so
DODWT_SECONDARY queries never return it. */
native dod_weapon_type(id, type);

/* This native will change the position of a weapon within the users slots and its ammo ammount.
   slot is the 1-based HUD bucket (module stores slot-1); dropslot is the
   position within that bucket (stored as-is). */
native dod_set_weaponlist(id, wpnID, slot, dropslot, totalrds);

/* Sets the model for a player */
native dod_set_model(id, const model[]);

/* Sets the model for a player */
native dod_set_body_number(id, bodynumber);

/* Un-Sets the model for a player */
native dod_clear_model(id);

/* Custom Weapon Support */
/* function will return index of new weapon */
native custom_weapon_add( const wpnname[], melee = 0, const logname[]="" ); 

/* Function will pass damage done by this custom weapon to stats module and other plugins */
native custom_weapon_dmg( weapon, att, vic, damage, hitplace=0 ); 

/* Function will pass info about custom weapon shot to stats module */
native custom_weapon_shot( weapon, index ); // weapon id , player id

/* function will return 1 if true */
native xmod_is_melee_wpn(wpnindex);

/* Returns weapon name. */
native xmod_get_wpnname(wpnindex, name[], len);

/* Returns weapon logname. */
native xmod_get_wpnlogname(wpnindex, name[], len);

/* Returns weapons array size */
native xmod_get_maxweapons();

/* Returns stats array size ex. 8 in TS , 9 in DoD */
native xmod_get_stats_size();

/* Returns 1 if true */
native xmod_is_custom_wpn(wpnindex);

/************* Shared Natives End ********************************/

/* weapon logname to weapon name convertion */
native dod_wpnlog_to_name(const logname[],name[],len);

/* weapon logname to weapon index convertion */
native dod_wpnlog_to_id(const logname[]);

native dod_get_map_info( info );

/* Returns id of currently carried weapon. Gets also 
* ammount of ammo in clip and backpack. */
native dod_get_user_weapon(index,&clip=0,&ammo=0);

/* Returns team score */
native dod_get_team_score(teamId);

/* Returns player class id */
native dod_get_user_class(index);

/* Returns player score */
native dod_get_user_score(index);

/* values are: 0-no prone, 1-prone, 2-prone + w_deploy */
native dod_get_pronestate(index);

/* Registered aliases that were historically undeclared. The module also
 * registers get_weaponname/get_user_weapon overrides of the core natives —
 * those keep their amxmodx.inc declarations. */

/* Returns player team (1=Allies, 2=Axis) from the edict. */
native dod_get_user_team(index);

/* Alias of xmod_get_wpnname. */
native dod_get_wpnname(wpnindex, name[], len);

/* Alias of xmod_get_wpnlogname. */
native dod_get_wpnlogname(wpnindex, name[], len);

/* Alias of xmod_is_melee_wpn. */
native dod_is_melee(wpnindex);

/* It is not as safe as original but player deaths will not be increased */
native dod_user_kill(index);

/************* KTP: HLStatsX Integration Natives ********************************/

/**
 * Forward called by dodx_flush_all_stats() for each connected player.
 * Stats logging plugins should register for this to log pending weaponstats.
 *
 * @param id        Player index to flush stats for
 */
forward dod_stats_flush(id);

// =============== KTP: HLStatsX Integration Natives ===============

/**
 * Flush all pending player stats by firing dod_stats_flush forward.
 * Call this before match starts to log warmup stats.
 *
 * @return          Number of players for which stats were flushed; 0 (logged,
 *                  no abort) if the dod_stats_flush forward isn't registered
 */
native dodx_flush_all_stats();

/**
 * Reset all accumulated stats for all connected players.
 * Call this after flushing warmup stats to start fresh for match.
 * Clears: weapons[], attackers[], victims[], weaponsLife[], weaponsRnd[], life, round
 * Also zeroes the observed-deaths counter (dodx_get_observed_deaths) for
 * every slot.
 *
 * @return          Number of players reset
 */
native dodx_reset_all_stats();

/**
 * Set the match ID for stats logging.
 * When set, weaponstats log lines will include (matchid "xxx") property.
 * Pass empty string to clear match context (for post-match/warmup).
 *
 * @param matchId   Match ID string (e.g., "KTP-1734355200-dod_charlie").
 *                  Silently truncated at 63 chars.
 * @return          1 on success
 */
native dodx_set_match_id(const matchId[]);

/**
 * Get the current match ID.
 *
 * @param output    Buffer to store match ID
 * @param maxlen    Maximum buffer length
 * @return          Length of match ID string, 0 if not set
 */
native dodx_get_match_id(output[], maxlen);

/**
 * Pause or unpause stats collection.
 * When paused, kills/damage/shots are not accumulated in DODX stats.
 * Used by KTPMatchHandler to filter round-freeze kills during matches.
 *
 * Pausing gates the module's message hooks, so it suppresses more than
 * accumulation: dod_damage_pre stops firing (its damage-modification effect
 * included — grenade damage reduction plugins go inert), along with
 * client_damage, the Damage-path client_death, score events, the
 * CurWeapon-driven weapon_fire, and the PreThink-polled forwards
 * (changeteam/changeclass/prone/stamina/weaponpickup). DeathMsg-path
 * client_death still fires (with degraded args); the TraceLine
 * grenade/rocket forwards are NOT gated.
 *
 * @param paused    1 to pause, 0 to resume
 * @return          1 on success
 */
native dodx_set_stats_paused(paused);

/**
 * Set player's team name in private data.
 * This affects server-side logs but NOT the scoreboard column
 * (DoD client hardcodes "Allies"/"Axis" team names).
 * Works in extension mode (no metamod required).
 *
 * @param id        Player index
 * @param szName    Team name string (max 15 chars)
 * @return          1 on success, 0 on failure
 */
native dodx_set_pl_teamname(id, const szName[]);

// =============== KTP: Per-player score / deaths pdata access ===============
// Direct read+write of the DoD game-DLL player private data m_iScore and
// m_iDeaths fields (offsets in CMisc.h: STEAM_PDOFFSET_SCORE / _DEATHS).
// Backs the mid-match disconnect/reconnect score-persistence path in
// KTPMatchHandler. The AMXX/ReAPI set_member route for these fields
// requires CS-only ReGameDll and crashes on DoD with runtime error 10.

/** Set the player's deaths counter (the scoreboard "Deaths" column).
 *  Also re-baselines the observed-deaths counter (dodx_get_observed_deaths)
 *  to the written value. */
native dodx_set_user_deaths(id, deaths);

/** Get the player's current deaths counter. */
native dodx_get_user_deaths(id);

/** Set the player's score (the DoD scoreboard "Score" column — caps + kills
 *  + bonus aggregate, computed by DoD on each scoring event). */
native dodx_set_user_score(id, score);

/** Get the player's current score. */
native dodx_get_user_score(id);

/**
 * Get the engine-authoritative deaths count for a player, observed via
 * DODX's internal Client_DeathMsg hook. Independent from pdata reads above
 * (dodx_get_user_deaths reads pvPrivateData at STEAM_PDOFFSET_DEATHS;
 *  this reads DODX's own counter incremented per engine DeathMsg broadcast).
 *
 * Use case: validation gate. Call BOTH at SAVE time; if they disagree, the
 * score_deaths_offset is wrong for the current dod_i386.so (likely an OS
 * bump shifted the struct). Log + skip persist for that player. Do NOT
 * call dodx_set_user_deaths/score until offsets validate.
 *
 * Lifecycle: the counter resets per-slot on client disconnect and player
 * init, is zeroed for every slot by dodx_reset_all_stats, and is
 * re-baselined to the written value by dodx_set_user_deaths. Increments are
 * gated per life (a victim counts at most one death between spawns), so
 * duplicate death reports can never skew it.
 *
 * Returns 0 for invalid id or before any DeathMsg has fired. Bots DO
 * accumulate observed deaths; HLTV reads 0 only because it never dies.
 */
native dodx_get_observed_deaths(id);

/**
 * Broadcast a ScoreShort message for `id` to all clients. Refreshes the
 * scoreboard row immediately by sending the player's current m_iObjScore +
 * pev->frags + m_iDeaths via DoD's native scoreboard-update message.
 *
 * Use case: after dodx_set_user_deaths / dodx_set_user_score writes, the
 * engine doesn't auto-broadcast until the next score-changing event. Call
 * this to make the scoreboard update immediately.
 *
 * Uses the same direct engine-func pattern as dodx_broadcast_team_score
 * (proven safe). Avoids the AMX message_begin Pawn native that crashed
 * on 2026-05-21 (see KTPMatchHandler/research/OFFSETS_RESEARCH).
 *
 * Returns 1 on success, 0 if id invalid or ScoreShort not registered
 * (logged, no abort — the 0 return is real as of 2.7.21).
 */
native dodx_broadcast_scoreboard(id);

// =============== KTP: Gamerules Score Modification ===============

/**
 * Check if gamerules score modification is available.
 * Returns true if dodx_set_team_score/dodx_get_team_score will work.
 * Requires g_pGameRules address to be found via gamedata signature.
 *
 * @return          1 if available, 0 if not
 */
native dodx_has_gamerules();

/**
 * Set team score in gamerules (modifies the scoreboard directly).
 * This allows restoring cumulative scores from 1st half when 2nd half starts.
 * The score is written directly to CDoDTeamPlay::m_iTeamScores array.
 *
 * @param team      Team index (1=Allies, 2=Axis)
 * @param score     Score value to set
 * @return          1 on success, 0 on failure (gamerules not available or
 *                  invalid team). Failures log and return — they no longer
 *                  abort the calling public (2.7.21), so check the return.
 */
native dodx_set_team_score(team, score);

/**
 * Get team score from gamerules (reads the scoreboard value directly).
 * If gamerules is not available, falls back to DODX message-tracked score.
 *
 * @param team      Team index (1=Allies, 2=Axis)
 * @return          Current team score; 0 for an invalid team (logged, no abort)
 */
native dodx_get_team_score(team);

/**
 * Broadcast TeamScore message to all clients and update gamerules.
 * This properly updates client scoreboards after modifying scores.
 * Use this instead of trying to send TeamScore via AMX message natives.
 *
 * @param team      Team index (1=Allies, 2=Axis)
 * @param score     Score value to broadcast
 * @return          1 on success, 0 on failure (invalid team or TeamScore
 *                  message not registered; logged, no abort)
 */
native dodx_broadcast_team_score(team, score);

/**
 * Engine-authoritative seconds remaining in the current half, read straight
 * from DoD's own half-clock accounting (CDoDTeamPlay::m_flDoDMapTime, which a
 * clan restart rebases and the client HUD clock counts from) — the closed-loop
 * time source for broadcast overlays, replacing open-loop anchor estimates.
 *
 * During an mp_clan_restartround countdown the post-rebase clock is projected
 * from the engine's scheduled completion time (m_flRestartRoundTime), so this
 * already returns the correct post-go-live value for the whole countdown
 * window. On pubs (no clan restart) it matches get_timeleft(). Tracks every
 * engine rebase with no signal dependency: go-live, .restarthalf, OT restarts.
 *
 * The countdown projection is why the value can legitimately EXCEED
 * mp_timelimit*60: during the restart window it reports the post-go-live half
 * length measured from a completion time still in the future. That is correct,
 * not drift — a consumer that clamps to mp_timelimit*60 will misreport the
 * clock for the whole countdown.
 *
 * @return  Seconds remaining as a float; -1.0 when unavailable (no gamerules
 *          pointer, offsets unresolved, mp_timelimit 0 = no time limit, or an
 *          implausible read — e.g. struct drift on a future dod.so update).
 *          Callers MUST handle -1.0 by falling back to their own estimate.
 */
native Float:dodx_get_round_time();

/**
 * Seconds until the map's next TERRITORIAL scoring tick — the periodic team
 * point award for holding control points, read straight off the map's
 * dod_control_point_master (CControlPointMaster::m_fGivePointsTime).
 *
 * The DoD client shows this clock NOWHERE: not the countdown, not the amount.
 * Without this native an observer can only infer the phase by watching the
 * score move, which costs a lock-on period at every half start and again after
 * every round restart — a restart rebases the master's clock.
 *
 * The master gates its own scoring, so an inactive master reports unavailable
 * rather than counting down to an award that will never land.
 *
 * @return  Seconds remaining as a float; -1.0 when unavailable (map has no
 *          control-point master — e.g. a pure objective map, offsets
 *          unresolved, master inactive, or an implausible read). Callers MUST
 *          handle -1.0 by falling back to their own estimate.
 */
native Float:dodx_get_score_tick_time();

/**
 * The map's nominal territorial scoring period in seconds
 * (CControlPointMaster::m_iGivePointsDelay).
 *
 * NOTE this is the configured delay, not the observed spacing: the master only
 * awards on its own think, so real awards land slightly later than the delay
 * (measured 30.50s against a delay of 30 on the KTP fleet). Use it as a sanity
 * bound or a bootstrap hint — never as a phase, and never to extrapolate a
 * countdown across more than one cycle.
 *
 * @return  Period in seconds; -1 when unavailable.
 */
native dodx_get_score_tick_period();

/**
 * Set custom team name on scoreboard for all players on a team.
 * Sends TeamInfo message to all clients - may override "Allies"/"Axis" display.
 * Call this after players join teams to show custom names on scoreboard.
 *
 * @param team      Team index (1=Allies, 2=Axis)
 * @param name      Custom team name string (e.g., "Team Alpha")
 * @return          Number of players updated; 0 on failure (invalid team,
 *                  empty name, or TeamInfo not registered; logged, no abort)
 */
native dodx_set_scoreboard_team_name(team, const name[]);

// =============== KTP: Grenade Ammo Manipulation ===============
//
// Grenade ammo lives in CBasePlayer::m_rgAmmo at an ammo-type index the game DLL
// assigns during map precache. DoD precaches every weapon of every nationality in
// a fixed order, so that index is the same on every map — but DODX no longer
// assumes it: it reads the live value from the DLL's WeaponList message and from
// the ammo a successful dodx_give_grenade credits, and logs if either ever
// disagrees with the expected one. Nothing here needs the caller's help.

/**
 * Set grenade ammo count for a player.
 * Use this to customize grenade loadouts per class on spawn.
 * Works in extension mode (no Metamod/dodfun required).
 *
 * @param id            Player index
 * @param grenade_type  Grenade weapon ID: DODW_HANDGRENADE (13), DODW_STICKGRENADE (14), DODW_MILLS_BOMB (36)
 * @param count         Number of grenades to set (0-10)
 * @return              1 on success, 0 on failure (invalid player state or
 *                      invalid grenade_type; logged, no abort)
 *
 * @note DODW_HANDGRENADE and DODW_MILLS_BOMB share the same ammo pool (Allies grenades)
 * @note DODW_STICKGRENADE is the Axis grenade type
 * @note The client HUD updates itself — the game DLL emits its own AmmoX once the
 *       count changes, on the correct slot. A follow-up dodx_send_ammox is not needed.
 * @note Call this in dod_client_spawn forward with a short delay for best results
 */
native dodx_set_grenade_ammo(id, grenade_type, count);

/**
 * Get current grenade ammo count for a player.
 *
 * @param id            Player index
 * @param grenade_type  Grenade weapon ID: DODW_HANDGRENADE (13), DODW_STICKGRENADE (14), DODW_MILLS_BOMB (36)
 * @return              Current grenade count, or -1 on failure (invalid player
 *                      state or a grenade_type that is not a grenade; logged,
 *                      no abort). Changed from 0 in 2.7.29 so that an empty
 *                      player is distinguishable from a failed call.
 */
native dodx_get_grenade_ammo(id, grenade_type);

/**
 * Ammo-type index in use for a grenade type on this map.
 * Needed by callers that send their own AmmoX, so the slot comes from the game
 * DLL rather than from a literal repeated in every plugin.
 *
 * @param grenade_type  Grenade weapon ID: DODW_HANDGRENADE (13), DODW_STICKGRENADE (14), DODW_MILLS_BOMB (36)
 * @return              Ammo slot (0-31), or -1 if grenade_type is not a grenade
 */
native dodx_get_grenade_ammo_index(grenade_type);

/**
 * Set player noclip mode.
 * Ported from fun module for extension mode compatibility.
 *
 * @param id       Player index
 * @param noclip   0 = disable noclip (normal walk), 1 = enable noclip
 * @return         1 on success, 0 on failure
 */
native dodx_set_user_noclip(id, noclip);

/**
 * Get player movetype. Replaces engine module entity_get_int(id, EV_INT_movetype)
 * for extension mode where the engine module is not available.
 *
 * @param id         Player index
 * @return           Movetype value (3=WALK, 8=NOCLIP, etc.) or -1 on error
 */
native dodx_get_user_movetype(id);

/**
 * Debug: check CPlayer state flags for a player index.
 * Returns bitmask: bit0=ingame, bit1=pEdict, bit2=!free, bit3=!FNullEnt
 * A fully valid player returns 15 (0xF).
 */
native dodx_debug_player_state(id);

/**
 * Send AmmoX message to update client HUD ammo display.
 * Use this after modifying grenade ammo to sync the client HUD.
 *
 * @param id         Player index
 * @param ammo_slot  Raw ammo-type index — take the grenade ones from
 *                   dodx_get_grenade_ammo_index() rather than repeating a literal
 * @param count      Ammo count to display (0-254)
 * @return           1 on success, 0 on failure (AmmoX not registered logs
 *                   and returns 0, no abort)
 */
native dodx_send_ammox(id, ammo_slot, count);

/**
 * Give a grenade weapon to a player.
 * Creates a weapon entity and makes the player pick it up.
 * Use this for infinite grenade functionality in practice mode.
 *
 * @param id            Player index
 * @param grenade_type  Grenade weapon ID: DODW_HANDGRENADE (13), DODW_STICKGRENADE (14), DODW_MILLS_BOMB (36)
 * @return              1 on success, 0 on failure, -1 if the created weapon
 *                      entity was removed on pickup (typically the player
 *                      already has that grenade weapon). Callers set ammo
 *                      separately, so treat -1 as "entity not granted",
 *                      not success.
 * @error               Invalid grenade_type raises a native error that
 *                      aborts the calling public — the 0 return is not
 *                      observable for that case.
 */
native dodx_give_grenade(id, grenade_type);

/**
 * Strip grenade ammo from a player.
 * Zeroes the ammo slots for the grenade type — it never removes a weapon
 * entity. Use this before giving grenades to ensure clean state.
 *
 * @param id            Player index
 * @param grenade_type  Grenade weapon ID: DODW_HANDGRENADE (13), DODW_STICKGRENADE (14), DODW_MILLS_BOMB (36)
 * @return              1 on success, 0 on failure (no edict or no private data;
 *                      note it does NOT check `ingame`)
 * @error               Invalid grenade_type raises a native error that
 *                      aborts the calling public — the 0 return is not
 *                      observable for that case.
 */
native dodx_strip_grenade(id, grenade_type);

/**
 * Debug: Dump this map's resolved grenade ammo slots and the player's whole
 * m_rgAmmo array (non-zero entries only) to the server log.
 *
 * @param id  Player index
 * @return    1 on success
 */
native dodx_debug_dump_ammo(id);

/**
 * Set player class.
 * Ported from dodfun module for extension mode compatibility.
 *
 * @param id        Player index
 * @param classId   Class ID (DODC_* in dodconst.inc: Allies 1-8, Axis 10-19,
 *                  British 21-25), or 0 for random class. The value is
 *                  written to player data unvalidated.
 * @return          1 on success, 0 on failure
 */
native dodx_set_user_class(id, classId);

/**
 * Set player team.
 * Ported from dodfun module for extension mode compatibility.
 * Note: This kills the player and sets random class.
 *
 * @param id        Player index
 * @param teamId    Team ID: 1 = Allies, 2 = Axis, 3 = Spectators
 * @param refresh   1 = broadcast team change to all clients, 0 = silent
 * @return          1 on success, 0 on failure (invalid teamId logs and
 *                  returns 0, no abort)
 */
native dodx_set_user_team(id, teamId, refresh = 1);

/**
 * Get player origin (position).
 * Extension mode compatible alternative to pev/get_user_origin.
 *
 * @param id        Player index
 * @param origin    Float array[3] to store origin (x, y, z)
 * @return          1 on success, 0 on failure
 */
native dodx_get_user_origin(id, Float:origin[3]);

/**
 * Set player origin (position/teleport).
 * Extension mode compatible alternative to set_pev/entity_set_origin.
 *
 * @param id        Player index
 * @param origin    Float array[3] with new origin (x, y, z)
 * @return          1 on success, 0 on failure
 */
native dodx_set_user_origin(id, Float:origin[3]);

/**
 * Get player view angles.
 * Extension mode compatible alternative to pev.
 *
 * @param id        Player index
 * @param angles    Float array[3] to store angles (pitch, yaw, roll)
 * @return          1 on success, 0 on failure
 */
native dodx_get_user_angles(id, Float:angles[3]);

/**
 * Set player view angles.
 * Extension mode compatible alternative to set_pev.
 *
 * @param id        Player index
 * @param angles    Float array[3] with new angles (pitch, yaw, roll)
 * @return          1 on success, 0 on failure
 */
native dodx_set_user_angles(id, Float:angles[3]);

// =============== KTP: Control Point Tracking ===============

/**
 * Control point data keys for dodx_objective_get_data/dodx_objective_set_data.
 */
enum CP_VALUE {
	CP_edict = 1,
	CP_area,
	CP_index,
	CP_owner,
	CP_default_owner,
	CP_visible,
	CP_icon_neutral,
	CP_icon_allies,
	CP_icon_axis,
	CP_origin_x,
	CP_origin_y,

	CP_can_touch,
	CP_pointvalue,

	CP_points_for_cap,
	CP_team_points,

	CP_model_body_neutral,
	CP_model_body_allies,
	CP_model_body_axis,

	// strings
	CP_name,
	CP_cap_message,
	CP_reset_capsound,
	CP_allies_capsound,
	CP_axis_capsound,
	CP_targetname,

	CP_model_neutral,
	CP_model_allies,
	CP_model_axis,
};

/**
 * Capture area data keys for dodx_area_get_data/dodx_area_set_data.
 */
enum CA_VALUE {
	CA_edict = 1,
	CA_allies_numcap,
	CA_axis_numcap,
	CA_timetocap,
	CA_can_cap,

	// Live cap state (read-only). Pulled directly from CAreaCapture pvPrivateData.
	CA_num_allies,       // m_nNumAllies: allies-team players currently in zone
	CA_num_axis,         // m_nNumAxis: axis-team players currently in zone
	CA_is_capturing,     // m_bCapturing: non-zero while a cap is in progress
	CA_capturing_team,   // m_nCapturingTeam: 1=allies, 2=axis, 0=none
	CA_owning_team,      // m_nOwningTeam: 1=allies, 2=axis, 0=neutral
	CA_cap_mode,         // m_iCapMode
	CA_time_remaining,   // m_fTimeRemaining (float; retrieve and cast with Float:)

	// strings
	CA_target,
	CA_sprite,
};

/**
 * Called after map control point data is initialized.
 * Use dodx_objectives_get_num() and dodx_objective_get_data() to query CP state.
 */
forward controlpoints_init();

/**
 * Called when a control point changes ownership.
 *
 * @param cp_index    Control point index (0-based)
 * @param new_owner   New owning team (0=neutral, 1=allies, 2=axis)
 * @param old_owner   Previous owning team
 */
forward dod_control_point_captured(cp_index, new_owner, old_owner);

/**
 * Called when a player receives score points, enriched with CP context.
 * Fires alongside client_score but includes which control point triggered the score.
 *
 * @param id          Player index
 * @param score_delta Points earned (positive delta)
 * @param total_score Player's new total score
 * @param cp_index    Control point index that triggered the score, or -1 if not CP-related
 */
forward dod_score_event(id, score_delta, total_score, cp_index);

/**
 * Get the number of control points on the current map.
 *
 * @return          Number of control points (0 if not yet initialized)
 */
native dodx_objectives_get_num();

/**
 * Get control point data.
 *
 * @param index     CP array index (0 to dodx_objectives_get_num()-1)
 * @param key       CP_VALUE key to query
 * @param szvalue   Buffer for string values (CP_name, CP_cap_message, etc.)
 * @param len       Buffer length for string values
 * @return          Integer value for int keys, 1 for string keys
 */
native dodx_objective_get_data(index, CP_VALUE:key, szvalue[] = "", len = 0);

/**
 * Set control point data.
 *
 * @param index     CP array index (0 to dodx_objectives_get_num()-1)
 * @param key       CP_VALUE key to set
 * @param ivalue    Integer value for int keys
 * @param szvalue   String value for string keys
 * @return          1 on success, 0 on failure
 */
native dodx_objective_set_data(index, CP_VALUE:key, ivalue = 0, const szvalue[] = "");

/**
 * Re-send InitObj message to refresh CP HUD display.
 *
 * @param player    Player index (0 = all players)
 * @return          1 on success, 0 on failure
 */
native dodx_objectives_reinit(player = 0);

/**
 * Get capture area data for a control point.
 *
 * @param index     CP array index (0 to dodx_objectives_get_num()-1)
 * @param key       CA_VALUE key to query
 * @param szvalue   Buffer for string values
 * @param len       Buffer length for string values
 * @return          Integer value for int keys, 1 for string keys, 0 if no area
 */
native dodx_area_get_data(index, CA_VALUE:key, szvalue[] = "", len = 0);

/**
 * Set capture area data for a control point.
 *
 * @param index     CP array index (0 to dodx_objectives_get_num()-1)
 * @param key       CA_VALUE key to set
 * @param ivalue    Integer value for int keys
 * @param szvalue   String value for string keys
 * @return          1 on success, 0 if no area
 */
native dodx_area_set_data(index, CA_VALUE:key, ivalue = 0, const szvalue[] = "");

// ==================================================================
// TEST-ONLY forward dispatch primitives — Tier 2 integration tests
// ==================================================================
//
// These natives bypass the engine event chain and dispatch DODX forwards
// directly. They exist solely so that hot-path Phase 3 integration tests
// can fire client_damage / dod_grenade_explosion / client_score /
// dod_score_event deterministically without relying on bot-vs-bot combat.
//
// PRODUCTION PLUGINS MUST NOT CALL THESE. They are intended exclusively
// for KTPWitness.amxx (the test-only forward observer in
// KTPInfrastructure/tests/integration/witness/) and any future test
// fixture plugins. The natives do not modify engine state — they only
// dispatch the corresponding forward to all subscribed plugins via
// MF_ExecuteForward, the same primitive DODX uses for real engine
// events.
//
// Safety rationale + implementation details:
// modules/dod/dodx/NBase.cpp § "KTP TEST-ONLY: Forward dispatch primitives"

/**
 * Test-only: dispatch the `dod_client_weapon_fire` forward.
 * @param id        Firing player slot (1..maxplayers)
 * @param weapon    Weapon ID (DODW_*)
 * @param gametime  gpGlobals->time at the shot (Float)
 * @return          1 on success, 0 if forward not registered
 */
native dodx_test_dispatch_weapon_fire(id, weapon, Float:gametime);

/**
 * Test-only: dispatch the `client_damage` forward.
 * @param attacker  Attacker client slot
 * @param victim    Victim client slot
 * @param damage    Damage value
 * @param wpnindex  Weapon index (xmod weapon ID)
 * @param hitplace  Hit location enum
 * @param TA        1 if teamattack, else 0
 * @return          1 on success, 0 if forward not registered
 */
native dodx_test_dispatch_damage(attacker, victim, damage, wpnindex, hitplace, TA);

/**
 * Test-only: dispatch the `dod_grenade_explosion` forward.
 * @param id        Grenade owner client slot
 * @param pos       3-float world position [x, y, z]
 * @param wpnid     Grenade weapon ID (DODW_HANDGRENADE etc.)
 * @return          1 on success, 0 if forward not registered
 */
native dodx_test_dispatch_grenade_explosion(id, Float:pos[3], wpnid);

/**
 * Test-only: dispatch BOTH `client_score` (id, score, total) and
 * `dod_score_event` (id, score_delta, total_score, cp_index) forwards
 * — matching the production pattern where they fire in tandem.
 *
 * @param id           Player client slot
 * @param score_delta  Score change (positive for frag, negative for suicide)
 * @param total_score  New running total
 * @param cp_index     CP that triggered the score, or -1 if not CP-related
 * @return             1
 */
native dodx_test_dispatch_score(id, score_delta, total_score, cp_index);

/**
 * Test-only: dispatch the `dod_control_point_captured` forward.
 * @param cp_index   Control point index that changed ownership
 * @param new_owner  New owning team (0=neutral, 1=allies, 2=axis)
 * @param old_owner  Previous owning team
 * @return           1 on success, 0 if forward not registered
 */
native dodx_test_dispatch_cp_captured(cp_index, new_owner, old_owner);

/**
 * Test-only: dispatch the `dod_client_spawn` forward.
 * @param id  Player index (1..maxclients)
 * @return    1 on success, 0 if forward not registered / bad slot
 */
native dodx_test_dispatch_client_spawn(id);

/**
 * Test-only: dispatch the `dod_client_changeteam` forward.
 * @param id       Player index (1..maxclients)
 * @param team     New team (1=Allies, 2=Axis, 3=Spectator)
 * @param oldteam  Previous team
 * @return         1 on success, 0 if forward not registered / bad slot
 */
native dodx_test_dispatch_changeteam(id, team, oldteam);

/**
 * Test-only: dispatch the `dod_client_changeclass` forward.
 * @param id        Player index (1..maxclients)
 * @param class     New class id
 * @param oldclass  Previous class id
 * @return          1 on success, 0 if forward not registered / bad slot
 */
native dodx_test_dispatch_changeclass(id, class, oldclass);

/**
 * Test-only: dispatch the `client_death` forward (killer first, victim
 * second — matches the production dispatch order).
 * @param killer    Killer index (0 = world/no killer)
 * @param victim    Victim index (1..maxclients)
 * @param wpnindex  Weapon id
 * @param hitplace  Hit location
 * @param TK        1 if teamkill, else 0
 * @return          1 on success, 0 if forward not registered / bad slot
 */
native dodx_test_dispatch_client_death(killer, victim, wpnindex, hitplace, TK);

/**
 * Test-only: dispatch the `dod_stats_flush` forward for one player.
 * (dodx_flush_all_stats loops it over all connected players; this drives
 * a single synthetic slot.)
 * @param id  Player index (1..maxclients)
 * @return    1 on success, 0 if forward not registered / bad slot
 */
native dodx_test_dispatch_stats_flush(id);

/**
 * Test/diagnostic-only: log every known DoD round/half-timer candidate field
 * (gamerules CDoDTeamPlay::m_flRoundTime, CDoDTeamPlay::m_pParaTimer ->
 * CDodParaRoundTimer::m_fRoundTimer, plus a classname entity scan dumped at
 * the CDodRoundTimer offsets), each with both derived interpretations
 * (remaining-if-end-time / elapsed-if-start-anchor). One `[DODX] rtdump`
 * line per call plus one `rtdump-ent` line per timer-suspect entity.
 * Read-only and safe on any map in any state. Used to empirically identify
 * the authoritative half-clock field for dodx_get_round_time().
 * Production plugins MUST NOT call this.
 *
 * @return  Number of timer-suspect entities found by the classname scan
 */
native dodx_test_dump_round_timers();

/**
 * Test/diagnostic-only: change-scanner over the first 576 bytes (the documented CDoDTeamPlay extent) of the gamerules
 * private data. First call snapshots (logs `grscan baseline`); later calls
 * log one `grscan` line per changed dword (offset + old/new as float and
 * int) then refresh the snapshot. Used to locate the real half-clock anchor
 * member in CDoDTeamPlay by diffing across the mp_clan_restartround
 * completion edge. Log volume capped per call. Read-only.
 * Production plugins MUST NOT call this.
 *
 * @return  Number of changed dwords since the previous call, 0 on the
 *          baseline call, -1 if gamerules is unavailable
 */
native dodx_test_scan_gamerules();

// ==================================================================

// ==================================================================
// KTP: aim / movement sampling (measure-only)
//
// Sampled once per usercmd from SV_PlayerRunPreThink. These report the GEOMETRY
// of sustained-fire aim motion and of ground contact; they apply no threshold and
// reach no conclusion. Whatever consumes them decides what the numbers mean, and
// it should not be this layer -- a threshold here would be a published one.
//
// Counters are cumulative per life-in-slot and reset on connect and disconnect, so
// a mid-map substitute never inherits the previous occupant's numbers.
// ==================================================================

/**
 * Read a player's aim/movement summary.
 *
 * The fire window still in progress is deliberately excluded: it has no final
 * duration, and including it would report the same burst twice once it closes.
 *
 * @param id        Player index
 * @param stats     Output, 5 cells:
 *                    [0] windows recorded
 *                    [1] windows retained for readout (see dodx_get_aim_window)
 *                    [2] COMPLETED ground contacts (counted at exit, so this and the
 *                        value below always describe the same set). A contact the
 *                        player was still standing in when he died is never completed
 *                        and never counted -- a small under-count correlated with
 *                        deaths, in the safe direction.
 *                    [3] shortest completed ground contact in MILLISECONDS, -1 if none
 *
 * @return  1 on success, 0 if the player is not in game
 */
native dodx_get_aim_stats(id, stats[4]);

/**
 * Read one retained fire window's geometry. Windows are retained by LONGEST
 * duration, so slot order is not chronological.
 *
 * A window is one CONTINUOUS burst: it closes when sampling lapses, so a duration
 * can never span a pause, a packet-loss stall or a server-side usercmd drop.
 * Without that a held trigger across a .tech pause would report one window minutes
 * long -- and since retention keeps the longest, artefacts like that would crowd
 * out every genuine burst.
 *
 * Three things a consumer cannot infer and has to be told:
 *  - A burst interrupted by a delivery gap is reported as SEVERAL windows, and the
 *    pieces cannot be reassembled -- nothing marks them as one burst. So both
 *    `dur` and the window count carry some connection quality.
 *  - `dur` is trigger-HELD time, not firing time. IN_ATTACK is key state: holding
 *    through a reload, an empty magazine or a weapon switch extends the window.
 *  - Windows are capped in length. A player idling with fire held is closed and
 *    scored rather than accumulating one window for the whole map. A `dur` of
 *    exactly 30000 ms is that clamp, and is filterable as such.
 *  - Two quantities are irreducibly client-influenced and must be read with that
 *    in mind. The release tolerance is evaluated only AT SAMPLE TIMES, so its real
 *    resolution is the client's frame interval -- an identical trigger release can
 *    split one player's burst and bridge another's, by about one frame. And the
 *    time axis itself is client-declared within a packet (`svtimebase` advances by
 *    the client's own `msec`, re-anchored server-side each packet), while `n` is
 *    simply frames delivered. Compare `n` across players only as `n/dur`.
 *
 * Values are scaled integers, not floats: these cross into Pawn and then into
 * JSON, and a float round-trip through both is where a comparison silently
 * shifts. The residual is in MICRO-degrees so small values keep several
 * significant figures; a coarser unit would quantise them into a few buckets.
 *
 * The residual's divisor is n, not n-2 -- a reported measurement rather than an
 * unbiased variance estimate. n ships alongside, so either can be re-derived.
 *
 * @param id        Player index
 * @param slot      0 .. stats[1]-1 as returned by dodx_get_aim_stats
 * @param window    Output, 4 cells:
 *                    [0] duration, milliseconds
 *                    [1] pitch slope, milli-degrees per second (signed)
 *                    [2] residual about the fitted line, micro-degrees
 *                    [3] usercmd samples in the window
 *
 * @return  1 on success, 0 if the player is not in game or slot is out of range
 */
native dodx_get_aim_window(id, slot, window[4]);

/**
 * Clear a player's shipped counters. The fire window still in progress is PRESERVED,
 * so a burst straddling a flush is still reported when it closes.
 *
 * Separate from the read so a failed flush does not silently discard the window
 * that justified it: read, ship, and only then reset.
 *
 * @param id        Player index
 * @return  1 on success
 */
native dodx_reset_aim_stats(id);

/**
 * Read the geometry of the shot the current dod_client_weapon_fire dispatch is
 * reporting, captured at the bullet's own trace inside the lag-compensation window.
 * Measurements only: no threshold, no ratio, no conclusion is applied here.
 *
 * CONTRACT -- read this before calling:
 *   - Call ONLY from inside a dod_client_weapon_fire handler, BEFORE the handler
 *     performs any trace of its own (a plugin trace that ignores the shooter and
 *     crosses a player could otherwise be captured as if it were the shot), and
 *     from NO OTHER forward: a client_damage handler calling this for the same
 *     shot would consume the sample and the fire handler would then read 0.
 *   - Call ONLY for hitscan firearm ids (melee via the damage path also carries real
 *     swing geometry). Grenade/rocket fire dispatches trace from the projectile, not
 *     the shooter, so any geometry returned for them describes some other ray --
 *     do not query for those ids.
 *   - The read is DESTRUCTIVE: a captured shot is returned at most once. A second
 *     call for the same dispatch returns 0.
 *   - 0 means "no geometry belongs to this shot" -- the shot's trace hit no player,
 *     the pairing guard rejected a stale capture, or the shooter's in-hand weapon
 *     at trace time failed to match `weapon` (possible around a weapon switch).
 *     Record NULL. Never substitute a previous sample; a stale value here is
 *     fabricated evidence. A consumer computing NULL rates should not read every
 *     0 as "hit no player".
 *
 * geom[] units, all integers:
 *   [0] angular error between the traced ray and the line to target centre,
 *       micro-degrees. The traced ray is post-spread/recoil, so weapon spread
 *       travels inside this number; target centre is the entity origin (hull
 *       centre), not the head.
 *   [1] range, world units, trace start to target centre.
 *   [2] target bearing rate across the shooter's view, milli-degrees per second,
 *       averaged since the previous captured sighting of the SAME target.
 *       -1 = no prior sighting; NOT the same as 0 (stationary).
 *   [3] the averaging gap for [2], milliseconds; -1 when [2] is -1. Judge validity
 *       with it -- a gap spanning a death/respawn describes a teleport, not motion.
 *       The per-usercmd clock re-anchors at client packet boundaries, so when the
 *       two sightings straddle packets, [3] (and therefore [2]) carries jitter of
 *       the same order as one packet gap.
 *   [4] hitgroup the trace struck (HIT_* / studio hitgroup).
 *   [5] the traced ray's start point, as distance from the shooter's view origin,
 *       world units. Near 0 for a normal fire trace. A wall-penetration
 *       continuation starts at the wall exit point instead of the eye, which
 *       shrinks [1] and inflates [0] for the same aim -- this cell is how those
 *       samples are told apart.
 *
 * @param id        Firing player index (the forward's id argument)
 * @param weapon    The forward's weapon argument; must match what the shooter held
 *                  at trace time or the read returns 0
 * @param geom      Output, 6 cells as above
 * @return  1 with geometry, 0 when no geometry belongs to this shot (record NULL)
 */
native dodx_get_shot_geom(id, weapon, geom[6]);

/**
 * Read a player's aim-vs-transmission counters. Measurements only: no threshold,
 * no ratio, no conclusion is applied here.
 *
 * Each sample is taken when the game's own aim trace lands on a live enemy (at
 * most one per usercmd; bot shooters excluded), and asks whether that enemy was
 * present in ANY entity pack the server sent this player within a window of the
 * engine's default client interpolation depth plus this player's measured ping --
 * how far in the past the player's screen legitimately runs.
 *
 * READ THE THREE STATES CORRECTLY -- they are not two:
 *   - stats[0] samples with an answer (packed-within-window or not);
 *   - stats[1] the subset of stats[0] whose enemy was NOT in any pack inside the
 *     window: the server had not transmitted that entity to that client recently
 *     enough for any renderer, stock or otherwise, to have drawn it;
 *   - stats[2] samples with NO answer (recording had not yet covered a full
 *     window for both slots, recorder inactive, clock boundary). Unknown counts
 *     toward NEITHER side; folding it into either fabricates a rate.
 *   - A packed-within-window sample says nothing about visibility -- PVS is
 *     leaf-based and generous, and entities stay packed while occluded.
 *
 * stats[3]/stats[4] carry the window actually used (ms): sum across stats[0]
 * samples (saturating) and the widest single window. stats[5] is 1 while the
 * pack recorder hook is registered; 0 means every sample lands in stats[2] and
 * the interval says nothing at all.
 *
 * Counters reset on connect, disconnect and map change, so a mid-map substitute
 * never inherits the previous occupant's numbers.
 *
 * @param id        Player index
 * @param stats     Output, 6 cells as above; zeroed before any failure return
 * @return  1 on success, 0 for a not-ingame player (an out-of-range index
 *          raises a native error instead of returning)
 */
native dodx_get_aim_vis_stats(id, stats[6]);

/**
 * Clear a player's aim-vs-transmission counters after a successful flush.
 * Separate from the read so a failed flush does not silently discard the
 * interval that justified it: read, ship, and only then reset.
 *
 * @param id        Player index
 * @return  1 on success
 */
native dodx_reset_aim_vis_stats(id);

/**
 * Test/diagnostic-only: log what dodx_get_score_tick_time() is currently
 * seeing — whether a dod_control_point_master exists, whether it is active,
 * its m_fGivePointsTime / m_iGivePointsDelay, and the derived remaining time.
 *
 * That native returns -1.0 on every failure path rather than a fabricated
 * clock, which makes "no scoring countdown" indistinguishable from outside:
 * no master entity, unresolved offsets, an inactive master and an implausible
 * read all look the same. This says which. One `[DODX] scoretick` line per
 * call. Read-only and safe on any map in any state.
 * Production plugins MUST NOT call this.
 *
 * @return  1 if a master was found and dumped, 0 otherwise
 */
native dodx_test_dump_score_tick();
