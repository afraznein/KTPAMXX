// KTP Stats Capture -- extra per-match stats logged for KTPHLStatsX.
//
// Scope today: assists and cap breaks. Kill/flag positions and the per-hit
// damage ledger land here in later phases.
//
// SELF-CONTAINED BY DESIGN. Everything below declares its own state and reads
// nothing from stats_logging.sma's globals -- deliberately unlike
// ktp_matchhandler_discord.inc, whose header notes it depends on its parent's.
// Keeping that boundary clean is what makes a later lift into a standalone
// plugin a file move instead of an untangling job.
//
// Frame safety: client_damage and client_death run inside SV_RunCmd postthink,
// where synchronous log I/O stalls the whole frame (26-122ms measured on this
// stack). Everything here buffers and flushes from a task -- the same shape
// stats_logging.sma uses for its headshot markers, but with its OWN buffer, so
// a burst of capture traffic can never force a mid-frame flush of that one.
//
// No match id is stamped on these lines, on purpose. hlstats.pl's recordEvent()
// injects match_id server-side for every event it writes and gates it on
// round_live, which is strictly better than anything this side can supply -- the
// plugin has no equivalent of that freeze-time gate.

#if defined _ktp_stats_capture_included
	#endinput
#endif
#define _ktp_stats_capture_included

// Enemy damage a third party must have dealt to a victim, since that victim's
// last spawn, to be credited an assist when they die.
#define KSC_ASSIST_DAMAGE_MIN 50

// DoD team ids, as reported by get_user_team() and by the CA_* area reads
// (dodx.inc documents m_nCapturingTeam as 1=allies, 2=axis, 0=none).
#define KSC_TEAM_ALLIES 1
#define KSC_TEAM_AXIS   2

#define KSC_MAX_FLAGS       32
#define KSC_ZONE_POLL_SECS  0.5
#define KSC_BREAK_QUEUE_MAX 6   // concurrent pending killers per flag
#define KSC_BREAK_WINDOW    5   // polls a candidate stays live (~2.5s)

#define KSC_BUF_MAX_ENTRIES 48
// Sized for the longest line we emit: an assist carrying two player strings
// (32-char name + userid + steamid + team each) plus both position properties,
// which measures ~256. Headroom kept deliberately -- a truncated line does not
// fail loudly, it just silently stops matching the daemon's regex.
#define KSC_BUF_LINE_LEN    384
#define KSC_BUF_FLUSH_SECS  5.0

// Distinct from stats_logging.sma's own flush task, which registers with the
// default id -- sharing an id would make one remove_task() cancel both.
#define KSC_TASK_FLUSH 88010
#define KSC_TASK_ZONES 88011

new g_kscBuffer[KSC_BUF_MAX_ENTRIES][KSC_BUF_LINE_LEN]
new g_kscBufferCount = 0
new g_kscDropped = 0

// [victim][attacker] -- enemy damage dealt to victim since their last spawn.
new g_kscDmgTaken[MAX_PLAYERS + 1][MAX_PLAYERS + 1]

new g_kscCvarEnabled

// ---- Cap-break state, all in dodx objective/area index space ----
new g_kscFlagCount = 0
new g_kscFlagName[KSC_MAX_FLAGS][32]

new g_kscCapTeam[KSC_MAX_FLAGS]     // team currently capturing this point, 0 = none
new g_kscOwner[KSC_MAX_FLAGS]       // last seen CA_owning_team, to spot a completed cap
new g_kscPrevAllies[KSC_MAX_FLAGS]  // in-zone counts as of the previous poll
new g_kscPrevAxis[KSC_MAX_FLAGS]

new g_kscBreakQ[KSC_MAX_FLAGS][KSC_BREAK_QUEUE_MAX]    // FIFO of candidate killers
new g_kscBreakTtl[KSC_MAX_FLAGS][KSC_BREAK_QUEUE_MAX]  // polls left before expiry
new g_kscBreakCount[KSC_MAX_FLAGS]
new g_kscBreakTeam[KSC_MAX_FLAGS]   // team being broken, latched with the first candidate
new g_kscBreakBase[KSC_MAX_FLAGS]   // rolling in-zone baseline to measure drops against

stock ksc_init() {
	// Runtime kill switch, so new capture can be shut off on a live server
	// without a redeploy. Mirrors the dodstats_pause/isDSMActive() gate this
	// plugin already uses for its stock output.
	g_kscCvarEnabled = register_cvar("ktp_stats_capture", "1")
}

stock ksc_cfg() {
	// plugin_cfg, not plugin_init: in extension mode plugin_init runs inside a
	// hookchain handler, where a repeating set_task intermittently fails to
	// register. Same reason the stock buffer's task is registered there.
	set_task(KSC_BUF_FLUSH_SECS, "ksc_flush_task", KSC_TASK_FLUSH, .flags="b")
	set_task(KSC_ZONE_POLL_SECS, "ksc_zone_poll_task", KSC_TASK_ZONES, .flags="b")
}

stock bool:ksc_enabled() {
	// Explicit true/false returns rather than returning the comparison, to keep
	// the bool: tag unambiguous for the compiler.
	if (!g_kscCvarEnabled)
		return false
	if (get_pcvar_num(g_kscCvarEnabled) == 0)
		return false

	return true
}

stock ksc_buffer(const line[]) {
	if (g_kscBufferCount >= KSC_BUF_MAX_ENTRIES) {
		// Deliberately NOT an inline flush (which is what the stock buffer_log
		// does when full): this runs in postthink, and a synchronous write here
		// is precisely the frame stall the buffering exists to prevent. Drop and
		// count instead -- the task drains within KSC_BUF_FLUSH_SECS, and a
		// non-zero counter in the logs is the signal to raise the size.
		g_kscDropped++
		return
	}

	copy(g_kscBuffer[g_kscBufferCount], KSC_BUF_LINE_LEN - 1, line)
	g_kscBufferCount++
}

public ksc_flush_task() {
	ksc_flush()
}

stock ksc_flush() {
	for (new i = 0; i < g_kscBufferCount; i++)
		log_message("%s", g_kscBuffer[i])

	g_kscBufferCount = 0

	if (g_kscDropped > 0) {
		log_amx("[KTP-STATS] dropped %d capture line(s) -- buffer full", g_kscDropped)
		g_kscDropped = 0
	}
}

// "Name<userid><STEAM_x:y:z><Team>" -- the player string every hlstats.pl regex
// expects. Team is capitalised to match the engine's own kill lines
// (allies -> Allies); guarded on the range rather than subtracting blind, so an
// already-capitalised or empty value can't be corrupted into punctuation.
stock ksc_player_str(id, out[], len) {
	new name[MAX_NAME_LENGTH], authid[32], team[16]

	get_user_name(id, name, charsmax(name))
	get_user_authid(id, authid, charsmax(authid))
	get_user_info(id, "team", team, charsmax(team))

	if (team[0] >= 'a' && team[0] <= 'z')
		team[0] -= 32

	formatex(out, len, "%s<%d><%s><%s>", name, get_user_userid(id), authid, team)
}

// "X Y Z" for the (position ...) family of log-line properties. hlstats.pl
// splits these on a single space and stores them in the event row's pos_x/y/z,
// so the format is fixed by the daemon, not chosen here.
//
// Rounded to integers: HL world units are already coarse relative to anything
// we would ask of this data (which flag was this near, how far was the kill),
// and it keeps the line short enough that a full assist line with two positions
// still fits the buffer.
//
// Returns false if the origin could not be read, so callers can omit the
// property entirely rather than emit a bogus "0 0 0" that looks like the map
// origin.
stock bool:ksc_origin_str(id, out[], len) {
	out[0] = 0

	if (id < 1 || id > MAX_PLAYERS)
		return false
	if (!is_user_connected(id))
		return false

	new Float:origin[3]
	if (!dodx_get_user_origin(id, origin))
		return false

	formatex(out, len, "%d %d %d",
		floatround(origin[0]), floatround(origin[1]), floatround(origin[2]))

	return true
}

stock ksc_clear_damage_row(victim) {
	for (new a = 0; a <= MAX_PLAYERS; a++)
		g_kscDmgTaken[victim][a] = 0
}

// Slots get recycled. A player inheriting a disconnected one's index must not
// inherit their pending assist credit either, so clear both directions.
stock ksc_clear_player(id) {
	if (id < 1 || id > MAX_PLAYERS)
		return

	for (new i = 0; i <= MAX_PLAYERS; i++) {
		g_kscDmgTaken[id][i] = 0
		g_kscDmgTaken[i][id] = 0
	}
}

// DODX forward. Feeds the enemy-damage matrix that ksc_on_death reads.
//
// Deliberately a separate hook from KTPMatchHandler's client_damage (which
// feeds the anti-cheat weapon timeline): stats capture must not inherit that
// system's dependency on the AC API being configured, which is known to fail on
// LAN deployments. AMXX dispatches the forward to every plugin implementing it,
// so the two coexist without interfering.
public client_damage(attacker, victim, damage, wpnindex, hitplace, TA) {
	// Signature is fixed by the forward; weapon and hitbox aren't needed for
	// assist attribution (the damage ledger phase will use them).
	#pragma unused wpnindex, hitplace

	if (!ksc_enabled())
		return

	if (attacker < 1 || attacker > MAX_PLAYERS)
		return
	if (victim < 1 || victim > MAX_PLAYERS)
		return

	// Enemy damage only -- self-damage and team hits earn nothing.
	if (attacker == victim || TA)
		return

	g_kscDmgTaken[victim][attacker] += damage
}

// DODX forward. "Since their last spawn" -- a fresh life starts from a clean
// row, so damage from a previous life can't carry over if a death was missed.
public dod_client_spawn(id) {
	ksc_clear_damage_row(id)
}

// ---------------------------------------------------------------------------
// Cap breaks
//
// A "break" is killing an enemy standing on a point their team is capturing.
// In DoD that is the only way to stop capture progress -- presence alone does
// not block a tick -- which is why it is a real skill signal worth recording.
//
// It cannot be decided at kill time. The engine applies a dead player's zone
// decrement 0.2-2.5s late (measured in production on KTPHudObserver, where an
// earlier one-shot snapshot caught under 20% of real breaks). So a kill on a
// contested point only QUEUES a candidate; the poll below credits it when the
// capping team's in-zone count actually falls.
//
// Deliberate simplification vs. KTPHudObserver: everything here stays in dodx
// objective/area index space. That plugin has to carry a DLL-index <-> dodx-
// index remap table, with hardcoded per-map exceptions, because it also
// consumes dod_control_point_captured (which reports a DLL index). Nothing
// here needs that hook -- a completed capture is detected from CA_owning_team
// changing, read in the same index space as everything else -- so the entire
// remap and its map-specific special cases drop out.

stock ksc_break_clear(f) {
	g_kscBreakCount[f] = 0
	g_kscBreakTeam[f]  = 0
	g_kscBreakBase[f]  = 0
}

stock ksc_break_shift(f) {
	for (new i = 1; i < g_kscBreakCount[f]; i++) {
		g_kscBreakQ[f][i - 1]   = g_kscBreakQ[f][i]
		g_kscBreakTtl[f][i - 1] = g_kscBreakTtl[f][i]
	}

	g_kscBreakCount[f]--

	if (g_kscBreakCount[f] == 0)
		g_kscBreakTeam[f] = 0
}

stock ksc_emit_break(f, breaker) {
	if (breaker < 1 || breaker > MAX_PLAYERS)
		return
	if (!is_user_connected(breaker))
		return

	new breaker_str[96], line[KSC_BUF_LINE_LEN]
	ksc_player_str(breaker, breaker_str, charsmax(breaker_str))

	// Where the breaker was standing. doEvent_PlayerAction reads a `position`
	// property into pos_x/y/z, so this needs no daemon change -- and it is what
	// later lets the query layer work out WHICH point was broken, since the
	// flag name in the line itself is discarded.
	new pos[40]
	new bool:have_pos = ksc_origin_str(breaker, pos, charsmax(pos))

	// Single-player action shape. Requires an hlstats_Actions row for
	// game='dod', code='cap_break' with for_PlayerActions='1'.
	//
	// The (flag "...") property is log-only for now: doEvent_PlayerAction
	// stores playerId/actionId/reward/position and drops unrecognised
	// properties, so which point was broken is not persisted yet. It is
	// emitted anyway because it makes a raw log readable while verifying, and
	// the later break-context phase is what teaches the daemon to read it.
	if (have_pos)
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"cap_break^" (flag ^"%s^") (position ^"%s^")",
			breaker_str, g_kscFlagName[f], pos)
	else
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"cap_break^" (flag ^"%s^")",
			breaker_str, g_kscFlagName[f])

	ksc_buffer(line)
}

// Called from ksc_on_death. A kill only becomes a break if the victim's team
// was mid-capture somewhere and their in-zone count then drops.
stock ksc_queue_break_candidate(killer, victim, TK) {
	if (!ksc_enabled())
		return
	if (TK || killer == victim)
		return
	if (killer < 1 || killer > MAX_PLAYERS || !is_user_connected(killer))
		return
	if (victim < 1 || victim > MAX_PLAYERS)
		return

	new vt = get_user_team(victim)
	if (vt != KSC_TEAM_ALLIES && vt != KSC_TEAM_AXIS)
		return

	for (new f = 0; f < g_kscFlagCount; f++) {
		if (g_kscCapTeam[f] != vt)
			continue

		if (g_kscBreakCount[f] > 0 && g_kscBreakTeam[f] != vt) {
			// Capping team flipped while the previous team's queue was still
			// draining. The flip means that cap already stopped, so those
			// candidates are stale -- re-latch rather than drop the fresh one.
			ksc_break_clear(f)
		}

		if (g_kscBreakCount[f] == 0) {
			// Baseline is min(live read, last poll's cache). The live read can
			// already include an earlier victim's late decrement; the cache can
			// miss a sub-poll voluntary walk-off. The lower never over-credits.
			new fresh = dodx_area_get_data(f,
				(vt == KSC_TEAM_ALLIES) ? CA_num_allies : CA_num_axis)
			new cached = (vt == KSC_TEAM_ALLIES) ? g_kscPrevAllies[f] : g_kscPrevAxis[f]

			g_kscBreakTeam[f] = vt
			g_kscBreakBase[f] = (fresh < cached) ? fresh : cached
		}

		if (g_kscBreakCount[f] < KSC_BREAK_QUEUE_MAX) {
			new q = g_kscBreakCount[f]
			g_kscBreakQ[f][q]   = killer
			g_kscBreakTtl[f][q] = KSC_BREAK_WINDOW
			g_kscBreakCount[f]++
		}

		break   // attribute at most one flag per kill
	}
}

public ksc_zone_poll_task() {
	if (!ksc_enabled())
		return
	if (g_kscFlagCount <= 0)
		return

	for (new f = 0; f < g_kscFlagCount; f++) {
		new ac = dodx_area_get_data(f, CA_num_allies)
		new xc = dodx_area_get_data(f, CA_num_axis)
		if (ac < 0) ac = 0
		if (xc < 0) xc = 0

		// A completed capture is not a break: ownership flipping means the cap
		// succeeded, so the cappers leaving afterwards must not be credited to
		// whoever last got a kill there. Also covers the round-restart cascade,
		// which flips every point back to its default owner.
		new owner = dodx_area_get_data(f, CA_owning_team)
		if (owner != g_kscOwner[f]) {
			g_kscOwner[f] = owner
			ksc_break_clear(f)
		}

		new is_capping = dodx_area_get_data(f, CA_is_capturing)
		g_kscCapTeam[f] = is_capping ? dodx_area_get_data(f, CA_capturing_team) : 0

		if (g_kscBreakCount[f] > 0) {
			new bt = g_kscBreakTeam[f]
			new now_count = (bt == KSC_TEAM_ALLIES) ? ac : xc

			if (now_count > g_kscBreakBase[f]) {
				// Someone joined the point. Re-anchor: a player who arrives and
				// later leaves is not a break.
				g_kscBreakBase[f] = now_count
			} else if (now_count < g_kscBreakBase[f]) {
				new drops = g_kscBreakBase[f] - now_count

				while (drops > 0 && g_kscBreakCount[f] > 0) {
					ksc_emit_break(f, g_kscBreakQ[f][0])
					ksc_break_shift(f)
					drops--
				}

				g_kscBreakBase[f] = now_count
			}

			// Age out candidates whose window elapsed: their victim never
			// produced a count drop, so the kill was off the point.
			for (new q = 0; q < g_kscBreakCount[f]; q++)
				g_kscBreakTtl[f][q]--

			while (g_kscBreakCount[f] > 0 && g_kscBreakTtl[f][0] <= 0)
				ksc_break_shift(f)
		}

		g_kscPrevAllies[f] = ac
		g_kscPrevAxis[f]   = xc
	}
}

public controlpoints_init() {
	g_kscFlagCount = dodx_objectives_get_num()

	if (g_kscFlagCount > KSC_MAX_FLAGS)
		g_kscFlagCount = KSC_MAX_FLAGS
	if (g_kscFlagCount < 0)
		g_kscFlagCount = 0

	for (new f = 0; f < g_kscFlagCount; f++) {
		dodx_objective_get_data(f, CP_name, g_kscFlagName[f], charsmax(g_kscFlagName[]))
		if (!g_kscFlagName[f][0])
			formatex(g_kscFlagName[f], charsmax(g_kscFlagName[]), "flag_%d", f)

		g_kscOwner[f]      = dodx_area_get_data(f, CA_owning_team)
		g_kscCapTeam[f]    = 0
		g_kscPrevAllies[f] = 0
		g_kscPrevAxis[f]   = 0
		ksc_break_clear(f)
	}
}

// Called from stats_logging.sma's client_death, ahead of its headshot-only
// early return, so assists are credited on every death rather than only
// headshot ones.
//
// The row is cleared whether or not capture is enabled: leaving a populated row
// behind while the cvar is off would let stale damage credit an assist on some
// much later death once it is switched back on.
stock ksc_on_death(killer, victim, TK) {
	ksc_queue_break_candidate(killer, victim, TK)

	if (victim < 1 || victim > MAX_PLAYERS)
		return

	if (ksc_enabled()) {
		new victim_team = get_user_team(victim)
		new victim_str[96], victim_pos[40]
		new bool:built_victim_str = false
		new bool:have_victim_pos = false

		for (new a = 1; a <= MAX_PLAYERS; a++) {
			if (a == killer || a == victim)
				continue
			if (g_kscDmgTaken[victim][a] < KSC_ASSIST_DAMAGE_MIN)
				continue
			if (!is_user_connected(a))
				continue
			// Belt-and-braces: the matrix only ever holds enemy damage, but a
			// mid-life team switch could strand a now-same-team row.
			if (get_user_team(a) == victim_team)
				continue

			if (!built_victim_str) {
				ksc_player_str(victim, victim_str, charsmax(victim_str))
				// Read once, before the loop can advance -- this is the victim's
				// position at the moment of death, which is the useful one.
				have_victim_pos = ksc_origin_str(victim, victim_pos, charsmax(victim_pos))
				built_victim_str = true
			}

			new assister_str[96], line[KSC_BUF_LINE_LEN], assister_pos[40]
			ksc_player_str(a, assister_str, charsmax(assister_str))
			new bool:have_assister_pos = ksc_origin_str(a, assister_pos, charsmax(assister_pos))

			// Player-vs-player action shape (hlstats.pl's "triggered ... against"
			// regex). Requires an hlstats_Actions row for game='dod',
			// code='assist' with for_PlayerPlayerActions='1' -- without it the
			// daemon parses the line and silently drops it.
			// assister_position / victim_position are the property names
			// doEvent_PlayerPlayerAction already looks for; they land in the
			// event row's pos_x/y/z and vpos_x/y/z with no daemon change.
			// Emitted only when actually read, so a failed lookup omits the
			// property rather than writing a bogus map-origin coordinate.
			if (have_assister_pos && have_victim_pos)
				formatex(line, charsmax(line),
					"^"%s^" triggered ^"assist^" against ^"%s^" (assister_position ^"%s^") (victim_position ^"%s^")",
					assister_str, victim_str, assister_pos, victim_pos)
			else if (have_assister_pos)
				formatex(line, charsmax(line),
					"^"%s^" triggered ^"assist^" against ^"%s^" (assister_position ^"%s^")",
					assister_str, victim_str, assister_pos)
			else if (have_victim_pos)
				formatex(line, charsmax(line),
					"^"%s^" triggered ^"assist^" against ^"%s^" (victim_position ^"%s^")",
					assister_str, victim_str, victim_pos)
			else
				formatex(line, charsmax(line),
					"^"%s^" triggered ^"assist^" against ^"%s^"",
					assister_str, victim_str)

			ksc_buffer(line)
		}
	}

	ksc_clear_damage_row(victim)
}
