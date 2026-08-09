// KTP Stats Capture -- extra per-match stats logged for KTPHLStatsX.
//
// Scope today: assists. Cap breaks, kill/flag positions and the per-hit damage
// ledger land here in later phases.
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

#define KSC_BUF_MAX_ENTRIES 48
#define KSC_BUF_LINE_LEN    288
#define KSC_BUF_FLUSH_SECS  5.0

// Distinct from stats_logging.sma's own flush task, which registers with the
// default id -- sharing an id would make one remove_task() cancel both.
#define KSC_TASK_FLUSH 88010

new g_kscBuffer[KSC_BUF_MAX_ENTRIES][KSC_BUF_LINE_LEN]
new g_kscBufferCount = 0
new g_kscDropped = 0

// [victim][attacker] -- enemy damage dealt to victim since their last spawn.
new g_kscDmgTaken[MAX_PLAYERS + 1][MAX_PLAYERS + 1]

new g_kscCvarEnabled

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

// Called from stats_logging.sma's client_death, ahead of its headshot-only
// early return, so assists are credited on every death rather than only
// headshot ones.
//
// The row is cleared whether or not capture is enabled: leaving a populated row
// behind while the cvar is off would let stale damage credit an assist on some
// much later death once it is switched back on.
stock ksc_on_death(killer, victim) {
	if (victim < 1 || victim > MAX_PLAYERS)
		return

	if (ksc_enabled()) {
		new victim_team = get_user_team(victim)
		new victim_str[96]
		new bool:built_victim_str = false

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
				built_victim_str = true
			}

			new assister_str[96], line[KSC_BUF_LINE_LEN]
			ksc_player_str(a, assister_str, charsmax(assister_str))

			// Player-vs-player action shape (hlstats.pl's "triggered ... against"
			// regex). Requires an hlstats_Actions row for game='dod',
			// code='assist' with for_PlayerPlayerActions='1' -- without it the
			// daemon parses the line and silently drops it.
			formatex(line, charsmax(line),
				"^"%s^" triggered ^"assist^" against ^"%s^"",
				assister_str, victim_str)

			ksc_buffer(line)
		}
	}

	ksc_clear_damage_row(victim)
}
