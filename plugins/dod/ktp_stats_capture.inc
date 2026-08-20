// KTP Stats Capture -- extra per-match stats logged for KTPHLStatsX.
//
// Scope today: assists, durable private life boundaries, cap breaks,
// assist/break positions, frag context
// (prone/scope/ammo/headshot at kill), the per-hit damage ledger, static
// flag positions, flag-ownership changes, break context
// (contester count/time remaining/capout),
// last-flag-defense (kill-position-based, not break-queue-based), and a
// periodic roster-position broadcast for positional/"holding" stats and
// (later) ninja-cap detection.
//
// SELF-CONTAINED BY DESIGN. Everything below declares its own state and reads
// nothing from stats_logging.sma's globals -- deliberately unlike
// ktp_matchhandler_discord.inc, whose header notes it depends on its parent's.
// Keeping that boundary clean is what makes a later lift into a standalone
// plugin a file move instead of an untangling job.
//
// Frame safety: client_damage and client_death run inside SV_RunCmd postthink,
// where synchronous log I/O stalls the whole frame (26-122ms measured on this
// stack). Everything here buffers and flushes from a task with its OWN buffer,
// so a burst of capture traffic can never force a mid-frame flush of the stock
// plugin's own buffered logging.
//
// The "frag_context" line (below) retired stats_logging.sma's old dedicated
// headshot_kill marker: both used the identical technique -- buffer a marker
// after the kill line, then have the daemon flush and UPDATE the just-inserted
// Frags row by (killerId, victimId, weapon) ORDER BY id DESC LIMIT 1 -- so
// firing it unconditionally with headshot as one more property is one queued
// line and one daemon UPDATE per kill instead of two.
//
// Most lines intentionally omit a match id. hlstats.pl's recordEvent() injects
// match_id server-side and gates it on its KTP_ROUND_LIVE/FREEZE state. The one
// exception is `life_boundary`: it is a private standalone ledger whose markers
// can sit in this five-second buffer across a context transition, so each line
// carries its own matchid. Freeze-time boundaries are retained rather than
// silently dropped. Their precise live/freeze state is deliberately not stamped:
// MatchHandler's dodx_set_stats_paused() state is private to DODX, and the public
// dodstats_pause cvar is not an authoritative mirror of it.

#if defined _ktp_stats_capture_included
	#endinput
#endif
#define _ktp_stats_capture_included

// Enemy damage a third party must have dealt to a victim, since that victim's
// last spawn, to be credited an assist when they die.
#define KSC_ASSIST_DAMAGE_MIN 50

// A player's HP pool is 0-100; DoD's raw per-hit damage value is the nominal
// weapon value with multipliers applied (headshot, wallbang) and is NOT
// clamped to what a body could actually lose -- a single hit can log 400+.
// Un-capped, that number measures "how strong this weapon+hitzone combo is
// on paper", not "how much this hit mattered", which is the wrong quantity
// for a per-player stat. Same convention CS2 uses. The raw value is still
// captured alongside this -- see ksc_emit_damage -- so nothing is lost, but
// any KTPR-facing consumer should read the capped value, not the raw one.
#define KSC_DAMAGE_CAP 100

// DoD team ids, as reported by get_user_team() and by the CA_* area reads
// (dodx.inc documents m_nCapturingTeam as 1=allies, 2=axis, 0=none).
#define KSC_TEAM_ALLIES 1
#define KSC_TEAM_AXIS   2

#define KSC_MAX_FLAGS       32
#define KSC_ZONE_POLL_SECS  0.5
#define KSC_BREAK_QUEUE_MAX 6   // concurrent pending killers per flag
#define KSC_BREAK_WINDOW    5   // polls a candidate stays live (~2.5s)
#define KSC_BREAK_RADIUS    512.0

// Roster-position broadcast for positional/"holding" stats (map presence
// beyond a discrete capture event) and, later, ninja-cap detection -- both
// deferred items that need the exact same underlying data (see
// tests/e2e_stats/NEXT_PHASES.md "Ninja-cap detection" in KTPInfrastructure).
//
// Raised from an initial 30s per the operator's call: at 30+ deaths/half over
// a 20-minute half, 30s left too few samples per life to be useful for
// holding/positional analysis. 5s is still a REASONED value, not a
// live-EPS-profiled one: 2026-08-13's real match-1 data measured ~20 combined
// damage+frag rows/min at full ~17-player rosters, and the damage ledger
// alone already runs at 1,100-1,500 direct-INSERT rows/match -- at 5s and a
// full roster this adds ~204 rows/min, well under that existing per-hit
// volume, so it is not a new dominant source. That is arithmetic from
// already-measured production volume, not a guess, but it is still NOT the
// same as watching real EPS impact -- and specifically the KSC_BUF_MAX_ENTRIES
// buffer's "[KTP-STATS] dropped" counter -- on a live Lane B run, which is
// what KSC_LAST_FLAG_RADIUS's own unmeasured-constant precedent calls for.
// Validate the same way before trusting this in production.
#define KSC_POSITION_BROADCAST_SECS 5.0

// Bumped 48 -> 128 for the damage ledger: ~1,100-1,500 hits/match (measured
// on a prior live audit) dwarfs the ~150-400 kills/match that sized this
// buffer before. Client_damage can fire many times per second in a heavy
// fight, all sharing this one buffer with assists/breaks/frag_context, so
// headroom matters more here than line length does. Validate empirically via
// a Lane B run -- watch for `[KTP-STATS] dropped` lines -- rather than trust
// this number; the same rule the line-length budget below already follows.
#define KSC_BUF_MAX_ENTRIES 128
// Sized for the longest line we emit: frag_context, with two full player
// strings (32-char name + userid + steamid + team each, ~70 apiece) plus nine
// properties plus producer match/half/clocks. A synthetic maximum-width
// contract line is 764 bytes; 768 preserves it including the NUL terminator.
// A truncated line does not fail loudly -- it silently loses trailing clocks.
#define KSC_BUF_LINE_LEN    768
#define KSC_BUF_FLUSH_SECS  5.0

// Life boundaries are control-plane facts. Keep them out of the high-volume
// damage/frag queue so a hit burst cannot consume their capacity. This queue
// is drained first, reports its own drops, and has a truthful bool enqueue API.
#define KSC_LIFE_BUF_MAX_ENTRIES 64
#define KSC_LIFE_BUF_LINE_LEN    384

// Distinct from stats_logging.sma's own flush task, which registers with the
// default id -- sharing an id would make one remove_task() cancel both.
#define KSC_TASK_FLUSH          88010
#define KSC_TASK_ZONES          88011
#define KSC_TASK_POSITIONS      88012
#define KSC_TASK_FLAG_POSITIONS 88013

new g_kscBuffer[KSC_BUF_MAX_ENTRIES][KSC_BUF_LINE_LEN]
new g_kscBufferCount = 0
new g_kscDropped = 0
new g_kscLifeBuffer[KSC_LIFE_BUF_MAX_ENTRIES][KSC_LIFE_BUF_LINE_LEN]
new g_kscLifeBufferCount = 0
new g_kscLifeDropped = 0

// [victim][attacker] -- enemy damage dealt to victim since their last spawn.
new g_kscDmgTaken[MAX_PLAYERS + 1][MAX_PLAYERS + 1]

// Most recent enemy attacker for each victim. DODX's client_death killer can
// be missing or degraded for projectile kills, while client_damage still has
// the authoritative final attacker. Keep that attacker out of the assist set.
new g_kscLastEnemyAttacker[MAX_PLAYERS + 1]

// Live scope state per player, tracked from the dod_client_scope forward
// (DODX has no getter -- the forward is the only way to know). 0/1, matching
// the forward's own "value" parameter (1 = scope up, 0 = scope down); starts
// at 0, which is correct for anyone who has never scoped this life.
new g_kscScoped[MAX_PLAYERS + 1]

// Durable life-boundary capture. `g_kscLifePhysicalAlive` follows the actual
// spawn/death lifecycle even outside a match context, so an alive player who
// disconnects just after context becomes available still gets a physical end
// marker. `g_kscLifeOpen` means this match context already has a start marker
// for the slot; it prevents the periodic context baseline from duplicating a
// spawn marker that raced just ahead of the next poll.
new bool:g_kscLifePhysicalAlive[MAX_PLAYERS + 1]
new bool:g_kscLifeOpen[MAX_PLAYERS + 1]
new g_kscLifeMatchId[64]
new bool:g_kscLifeBaselinePending = true

// Authoritative producer context comes from KTPMatchHandler's AMXX
// multi-forwards, not from receipt-time daemon memory. DODX's match id must
// agree before any event is stamped; half 101+ is normalized to DB half 3+.
new g_kscProducerMatchId[64]
new g_kscProducerHalf = 0
new bool:g_kscProducerContextConfirmed = false

new g_kscCvarEnabled

// ---- Cap-break state, all in dodx objective/area index space ----
new g_kscFlagCount = 0
new g_kscFlagName[KSC_MAX_FLAGS][32]

new g_kscCapTeam[KSC_MAX_FLAGS]     // team currently capturing this point, 0 = none
new g_kscOwner[KSC_MAX_FLAGS]       // last seen CA_owning_team, to spot a completed cap
new bool:g_kscOwnershipBaselinePending = true
new g_kscOwnershipMatchId[64]
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
	set_task(KSC_POSITION_BROADCAST_SECS, "ksc_position_broadcast_task", KSC_TASK_POSITIONS, .flags="b")
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

stock ksc_normalize_match_half(half) {
	if (half == 1 || half == 2)
		return half
	if (half >= 101 && half <= 353)
		return half - 98
	return 0
}

stock ksc_on_match_start(const matchid[], half) {
	new normalized_half = ksc_normalize_match_half(half)
	if (!matchid[0] || normalized_half == 0) {
		g_kscProducerMatchId[0] = 0
		g_kscProducerHalf = 0
		g_kscProducerContextConfirmed = false
		g_kscLifeBaselinePending = true
		ksc_clear_open_lives()
		return
	}

	copy(g_kscProducerMatchId, charsmax(g_kscProducerMatchId), matchid)
	g_kscProducerHalf = normalized_half
	g_kscProducerContextConfirmed = false
	g_kscLifeBaselinePending = true
	ksc_clear_open_lives()
}

stock ksc_on_match_context_end(const matchid[]) {
	if (!g_kscProducerMatchId[0] || !equal(matchid, g_kscProducerMatchId))
		return

	// Preserve ordering: drain boundaries while this exact producer context is
	// still known, then make later warmup/disconnect events fail closed.
	ksc_flush()
	g_kscProducerMatchId[0] = 0
	g_kscProducerHalf = 0
	g_kscProducerContextConfirmed = false
	g_kscLifeBaselinePending = true
	ksc_clear_open_lives()
}

stock bool:ksc_event_context(matchid[], len, &half) {
	// `-` is an explicit non-context sentinel for the optional damage/frag/
	// assist fields. Never emit an empty quoted property: older property parsers
	// could consume following fields when faced with `(matchid "")`.
	copy(matchid, len, "-")
	half = 0
	if (!g_kscProducerMatchId[0] || g_kscProducerHalf == 0)
		return false

	new dodx_matchid[64]
	dodx_get_match_id(dodx_matchid, charsmax(dodx_matchid))
	if (!dodx_matchid[0] || !equal(dodx_matchid, g_kscProducerMatchId)) {
		// Some teardown paths omit a match-end forward. DODX is the authoritative
		// invalidation edge once it has agreed. Before first agreement the producer
		// forward may legitimately precede DODX's round-live id write; keep that
		// pending context but emit nothing. Never reconstruct a half mid-match.
		if (g_kscProducerContextConfirmed) {
			g_kscProducerMatchId[0] = 0
			g_kscProducerHalf = 0
			g_kscProducerContextConfirmed = false
			g_kscLifeBaselinePending = true
			ksc_clear_open_lives()
		}
		return false
	}

	g_kscProducerContextConfirmed = true
	copy(matchid, len, g_kscProducerMatchId)
	half = g_kscProducerHalf
	return true
}

stock bool:ksc_life_buffer(const line[]) {
	if (g_kscLifeBufferCount >= KSC_LIFE_BUF_MAX_ENTRIES) {
		g_kscLifeDropped++
		return false
	}

	copy(g_kscLifeBuffer[g_kscLifeBufferCount], KSC_LIFE_BUF_LINE_LEN - 1, line)
	g_kscLifeBufferCount++
	return true
}

public ksc_flush_task() {
	ksc_flush()
}

stock ksc_flush() {
	// MatchHandler calls this before it clears context. Drain the durable,
	// low-volume control-plane queue before the high-volume capture queue.
	for (new i = 0; i < g_kscLifeBufferCount; i++)
		log_message("%s", g_kscLifeBuffer[i])

	g_kscLifeBufferCount = 0

	for (new i = 0; i < g_kscBufferCount; i++)
		log_message("%s", g_kscBuffer[i])

	g_kscBufferCount = 0

	if (g_kscDropped > 0) {
		log_amx("[KTP-STATS] dropped %d capture line(s) -- buffer full", g_kscDropped)
		g_kscDropped = 0
	}
	if (g_kscLifeDropped > 0) {
		log_amx("[KTP-STATS] dropped %d LIFE boundary line(s) -- dedicated buffer full", g_kscLifeDropped)
		g_kscLifeDropped = 0
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

	g_kscLastEnemyAttacker[victim] = 0
}

// Slots get recycled. A player inheriting a disconnected one's index must not
// inherit their pending assist credit either, so clear both directions.
stock ksc_clear_player(id) {
	if (id < 1 || id > MAX_PLAYERS)
		return

	for (new i = 0; i <= MAX_PLAYERS; i++) {
		g_kscDmgTaken[id][i] = 0
		g_kscDmgTaken[i][id] = 0

		if (g_kscLastEnemyAttacker[i] == id)
			g_kscLastEnemyAttacker[i] = 0
	}

	g_kscLastEnemyAttacker[id] = 0
	g_kscScoped[id] = 0
	g_kscLifePhysicalAlive[id] = false
	g_kscLifeOpen[id] = false
}

// ---------------------------------------------------------------------------
// Durable private life boundaries
//
// DoD has continuous respawns, so a half-level "survived" flag is not a
// meaningful KOST analogue. These markers expose the physical life windows
// without assigning any rating or public interpretation in the game plugin.
// The daemon stores them in a private canonical ledger; analytics can derive
// survival, per-life K/A/D, and KAT-style coverage later.

stock ksc_clear_open_lives() {
	for (new id = 1; id <= MAX_PLAYERS; id++)
		g_kscLifeOpen[id] = false
}

// Observe the match-id edge without adding a DODX/core API. The 0.5-second
// zone task supplies the normal transition baseline, while spawn/death/
// disconnect call this too so an event racing ahead of that poll is stamped
// with the correct context. Empty is a real state: remembering the gap lets
// the same match id used by the next half trigger a fresh baseline.
stock ksc_observe_life_context(const matchid[]) {
	if (!matchid[0]) {
		if (g_kscLifeMatchId[0] || !g_kscLifeBaselinePending) {
			g_kscLifeMatchId[0] = 0
			g_kscLifeBaselinePending = true
			ksc_clear_open_lives()
		}
		return
	}

	if (equal(matchid, g_kscLifeMatchId))
		return

	copy(g_kscLifeMatchId, charsmax(g_kscLifeMatchId), matchid)
	g_kscLifeBaselinePending = true
	ksc_clear_open_lives()
}

// One player-action marker per physical boundary. Explicit matchid makes the
// record self-describing even if the daemon sees it after a context transition.
// `event_epoch` is get_systime() (whole seconds) while `game_time` preserves
// intra-map ordering at sub-second precision.
stock bool:ksc_emit_life_boundary(id, const matchid[], const kind[], const reason[]) {
	if (!ksc_enabled() || !matchid[0])
		return false
	if (id < 1 || id > MAX_PLAYERS)
		return false
	new producer_matchid[64], producer_half
	if (!ksc_event_context(producer_matchid, charsmax(producer_matchid), producer_half) ||
		!equal(matchid, producer_matchid))
		return false

	new player_str[96]
	ksc_player_str(id, player_str, charsmax(player_str))

	new team = get_user_team(id)
	// The engine can report spectator as team 3 during a late disconnect.
	// The durable schema uses 0 for unassigned/non-playing state and reserves
	// 1/2 for Allies/Axis, so normalize instead of emitting a marker the daemon
	// must reject.
	if (team != 1 && team != 2)
		team = 0
	new class_id = dod_get_user_class(id)
	new event_epoch = get_systime()
	new Float:game_time = get_gametime()

	new line[KSC_LIFE_BUF_LINE_LEN]
	formatex(line, charsmax(line),
		"^"%s^" triggered ^"life_boundary^" (matchid ^"%s^") (half ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^") (kind ^"%s^") (reason ^"%s^") (team ^"%d^") (class ^"%d^") (slot ^"%d^")",
		player_str, producer_matchid, producer_half, event_epoch, game_time,
		kind, reason, team, class_id, id)
	return ksc_life_buffer(line)
}

stock ksc_life_start(id, const reason[]) {
	if (id < 1 || id > MAX_PLAYERS)
		return

	// Do not suppress an authoritative spawn merely because the prior start is
	// still open. That shape means its end was missed; the next start is the
	// truthful boundary that lets the derivation layer right-censor the old
	// life. Only the synthetic context baseline is duplicate-suppressed below.
	g_kscLifePhysicalAlive[id] = true

	new matchid[64]
	dodx_get_match_id(matchid, charsmax(matchid))
	ksc_observe_life_context(matchid)

	if (ksc_emit_life_boundary(id, matchid, "start", reason))
		g_kscLifeOpen[id] = true
	else
		// Retry as a left-censored context baseline on the next zone poll.
		g_kscLifeBaselinePending = true
}

stock ksc_life_end(id, const reason[]) {
	if (id < 1 || id > MAX_PLAYERS)
		return

	new matchid[64]
	dodx_get_match_id(matchid, charsmax(matchid))
	ksc_observe_life_context(matchid)

	// Emit an authoritative death even if its start raced the context baseline;
	// that is an honest left-censored life and is preferable to losing the end.
	ksc_emit_life_boundary(id, matchid, "end", reason)
	g_kscLifePhysicalAlive[id] = false
	g_kscLifeOpen[id] = false
}

// A context can become live after the clan-restart spawn has already fired.
// Baseline every connected+alive player exactly once on the empty->populated
// or changed-id edge. This runs even on maps with no objective entities.
stock ksc_sync_life_context() {
	new matchid[64]
	dodx_get_match_id(matchid, charsmax(matchid))
	ksc_observe_life_context(matchid)

	// Validate even on the empty-context branch below. Some teardown paths do
	// not fire match_end; this periodic check is what invalidates an already
	// confirmed producer half as soon as DODX clears or changes its id.
	if (g_kscProducerContextConfirmed) {
		new checked_matchid[64], checked_half
		ksc_event_context(checked_matchid, charsmax(checked_matchid), checked_half)
	}

	if (!matchid[0] || !g_kscLifeBaselinePending || !ksc_enabled())
		return

	new bool:all_queued = true
	for (new id = 1; id <= MAX_PLAYERS; id++) {
		if (!is_user_connected(id) || !is_user_alive(id))
			continue

		g_kscLifePhysicalAlive[id] = true
		if (g_kscLifeOpen[id])
			continue

		if (ksc_emit_life_boundary(id, matchid, "start", "context_live"))
			g_kscLifeOpen[id] = true
		else
			all_queued = false
	}

	// A partial pass remains pending. Successfully queued slots are already open
	// and are skipped when the 0.5s poll retries the remainder.
	g_kscLifeBaselinePending = !all_queued
}

// Called from stats_logging.sma before recycled-slot state is cleared. AMXX's
// disconnect forward can no longer be trusted to report is_user_alive(), so
// use the spawn/death-tracked physical state instead.
stock ksc_on_disconnect(id) {
	if (id < 1 || id > MAX_PLAYERS || !g_kscLifePhysicalAlive[id])
		return

	new matchid[64]
	dodx_get_match_id(matchid, charsmax(matchid))
	ksc_observe_life_context(matchid)
	ksc_emit_life_boundary(id, matchid, "end", "disconnect")
	g_kscLifePhysicalAlive[id] = false
	g_kscLifeOpen[id] = false
}

// DODX forward. The only source for live scope state; there is no getter.
public dod_client_scope(id, value) {
	if (id < 1 || id > MAX_PLAYERS)
		return

	g_kscScoped[id] = value
}

// DODX forward. Feeds the enemy-damage matrix that ksc_on_death reads.
//
// Deliberately a separate hook from KTPMatchHandler's client_damage (which
// feeds the anti-cheat weapon timeline): stats capture must not inherit that
// system's dependency on the AC API being configured, which is known to fail on
// LAN deployments. AMXX dispatches the forward to every plugin implementing it,
// so the two coexist without interfering.
public client_damage(attacker, victim, damage, wpnindex, hitplace, TA) {
	if (!ksc_enabled())
		return

	if (attacker < 1 || attacker > MAX_PLAYERS)
		return
	if (victim < 1 || victim > MAX_PLAYERS)
		return

	// Ledger every hit -- self-damage and team hits included, unlike assist
	// attribution below. A ledger that silently dropped those couldn't answer
	// "how much friendly fire happened" or "how much self-damage from own
	// grenades", and the query layer can filter enemy-only when that's what a
	// consumer wants. attacker == victim is real (a player's own grenade).
	ksc_emit_damage(attacker, victim, damage, wpnindex, hitplace)

	// Enemy damage only from here down -- self-damage and team hits earn no
	// assist credit.
	if (attacker == victim || TA)
		return

	g_kscDmgTaken[victim][attacker] += damage
	g_kscLastEnemyAttacker[victim] = attacker
}

// DODX forward. "Since their last spawn" -- a fresh life starts from a clean
// row, so damage from a previous life can't carry over if a death was missed.
public dod_client_spawn(id) {
	ksc_clear_damage_row(id)
	ksc_life_start(id, "spawn")
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

// How many flags team `team` currently owns, from the same g_kscOwner[]
// state the completed-capture detector already maintains. A team reading 1
// here is one break/loss away from a capout -- the shared test behind both
// is_capout (on breaks) and is_last_flag_defense (on kills, further down).
stock ksc_team_flag_count(team) {
	new n = 0
	for (new f = 0; f < g_kscFlagCount; f++) {
		if (g_kscOwner[f] == team)
			n++
	}
	return n
}

stock ksc_normalize_owner(owner) {
	// DODX can expose an authored/unowned point as -1 during initialization.
	// The persisted contract uses the stable KTP team convention: 0=neutral,
	// 1=Allies, 2=Axis. Treat every non-team engine sentinel as neutral.
	if (owner == KSC_TEAM_ALLIES || owner == KSC_TEAM_AXIS)
		return owner
	return 0
}

// Compact ownership timeline. One baseline row per flag when a KTP match
// context first becomes available, then one row only when the owner changes.
// This makes hold intervals reconstructable without polling ownership into
// the database every five seconds. Positions remain the independently sampled
// stream that is joined to these intervals in the analytics layer.
stock ksc_emit_flag_state(f, owner, is_initial) {
	new matchid[64]
	dodx_get_match_id(matchid, charsmax(matchid))
	if (!matchid[0])
		return

	new mapname[32]
	get_mapname(mapname, charsmax(mapname))

	// Direct from the repeating zone task, not a player postthink callback.
	// These lines are rare and their ordering relative to KTP_MATCH_START /
	// KTP_ROUND_FREEZE matters, so do not hold them in the five-second buffer.
	log_message("KTP_FLAG_STATE (map ^"%s^") (flag_index ^"%d^") (flag_name ^"%s^") (owner ^"%d^") (initial ^"%d^") (game_time ^"%.2f^")",
		mapname, f, g_kscFlagName[f], owner, is_initial, get_gametime())
}

stock ksc_ensure_ownership_baseline() {
	new matchid[64]
	dodx_get_match_id(matchid, charsmax(matchid))

	if (!matchid[0]) {
		// Detect a later empty -> populated transition even if the next half
		// reuses the same match id.
		g_kscOwnershipMatchId[0] = 0
		g_kscOwnershipBaselinePending = true
		return
	}

	if (!equal(matchid, g_kscOwnershipMatchId))
		g_kscOwnershipBaselinePending = true

	if (!g_kscOwnershipBaselinePending)
		return

	copy(g_kscOwnershipMatchId, charsmax(g_kscOwnershipMatchId), matchid)
	for (new f = 0; f < g_kscFlagCount; f++) {
		// Refresh immediately before the baseline. If a round reset happened
		// between polls, emitting the cached pre-reset owner followed by a
		// same-tick correction would create a false zero-length interval.
		g_kscOwner[f] = ksc_normalize_owner(
			dodx_area_get_data(f, CA_owning_team))
		ksc_emit_flag_state(f, g_kscOwner[f], 1)
	}

	g_kscOwnershipBaselinePending = false
}

stock ksc_emit_break(f, breaker, contester_count) {
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

	// Break-context phase: was the capping team's push big (contester_count,
	// the in-zone count just before this break), how close to completing was
	// it (time_remaining -- lower is more "clutch"), and did this break save
	// the defending team from a capout (they'd be down to 0 flags had the cap
	// finished, i.e. they own exactly 1 right now: this one).
	new Float:time_remaining = Float:dodx_area_get_data(f, CA_time_remaining)
	new defending_team = (g_kscBreakTeam[f] == KSC_TEAM_ALLIES) ? KSC_TEAM_AXIS : KSC_TEAM_ALLIES
	new is_capout = (ksc_team_flag_count(defending_team) == 1) ? 1 : 0

	// Single-player action shape. Requires an hlstats_Actions row for
	// game='dod', code='cap_break' with for_PlayerActions='1'.
	//
	// The (flag "...") property is log-only for now: doEvent_PlayerAction
	// stores playerId/actionId/reward/position and drops unrecognised
	// properties, so which point was broken is not persisted through THIS
	// line. contester_count/time_remaining/is_capout land instead via a
	// follow-up break_context marker (ksc_emit_break_context, below), same
	// buffer-then-daemon-UPDATE technique frag_context uses on Frags.
	if (have_pos)
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"cap_break^" (flag ^"%s^") (position ^"%s^")",
			breaker_str, g_kscFlagName[f], pos)
	else
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"cap_break^" (flag ^"%s^")",
			breaker_str, g_kscFlagName[f])

	ksc_buffer(line)
	ksc_emit_break_context(breaker, contester_count, time_remaining, is_capout)
}

// Follow-up marker for the cap_break line just buffered above. Same
// technique as frag_context/headshot_kill: the daemon flushes PlayerActions,
// then UPDATEs the most recent matching row by (playerId, actionId) rather
// than this being a second independent event.
stock ksc_emit_break_context(breaker, contester_count, Float:time_remaining, is_capout) {
	new breaker_str[96], line[KSC_BUF_LINE_LEN]
	ksc_player_str(breaker, breaker_str, charsmax(breaker_str))

	formatex(line, charsmax(line),
		"^"%s^" triggered ^"break_context^" (contester_count ^"%d^") (time_remaining ^"%.1f^") (is_capout ^"%d^")",
		breaker_str, contester_count, time_remaining, is_capout)

	ksc_buffer(line)
}

// The area API exposes aggregate team counts, not per-player membership.
// Select the closest active point and reject distant deaths so an unrelated
// off-point kill cannot consume a later in-zone count drop.
stock ksc_break_flag_for_victim(victim, victim_team) {
	new Float:origin[3]
	if (!dodx_get_user_origin(victim, origin))
		return -1

	new best_flag = -1
	new Float:best_dist2 = KSC_BREAK_RADIUS * KSC_BREAK_RADIUS

	for (new f = 0; f < g_kscFlagCount; f++) {
		// A capture can begin between the 0.2-second zone poll and this death.
		// Candidate selection must use the engine's current area state; the
		// delayed poll still requires a real count drop before crediting it.
		if (!dodx_area_get_data(f, CA_is_capturing) ||
			dodx_area_get_data(f, CA_capturing_team) != victim_team)
			continue

		new Float:dx = origin[0] - float(dodx_objective_get_data(f, CP_origin_x))
		new Float:dy = origin[1] - float(dodx_objective_get_data(f, CP_origin_y))
		new Float:dist2 = (dx * dx) + (dy * dy)

		if (dist2 <= best_dist2) {
			best_dist2 = dist2
			best_flag = f
		}
	}

	return best_flag
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

	new f = ksc_break_flag_for_victim(victim, vt)
	if (f < 0)
		return

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
}

public ksc_zone_poll_task() {
	// Match-context transition detection must not depend on a map having flags.
	// It also deliberately precedes the capture kill switch's early return so
	// an empty context always resets the pending baseline/open-life state.
	ksc_sync_life_context()

	if (!ksc_enabled())
		return
	if (g_kscFlagCount <= 0)
		return

	ksc_ensure_ownership_baseline()

	for (new f = 0; f < g_kscFlagCount; f++) {
		new ac = dodx_area_get_data(f, CA_num_allies)
		new xc = dodx_area_get_data(f, CA_num_axis)
		if (ac < 0) ac = 0
		if (xc < 0) xc = 0

		// A completed capture is not a break: ownership flipping means the cap
		// succeeded, so the cappers leaving afterwards must not be credited to
		// whoever last got a kill there. Also covers the round-restart cascade,
		// which flips every point back to its default owner.
		new owner = ksc_normalize_owner(
			dodx_area_get_data(f, CA_owning_team))
		if (owner != g_kscOwner[f]) {
			g_kscOwner[f] = owner
			ksc_break_clear(f)
			ksc_emit_flag_state(f, owner, 0)
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
				// Captured before the loop below starts shifting g_kscBreakBase[f]
				// down -- the capping team's in-zone count just before this break,
				// i.e. how big the push being repelled was.
				new contester_count = g_kscBreakBase[f]

				while (drops > 0 && g_kscBreakCount[f] > 0) {
					ksc_emit_break(f, g_kscBreakQ[f][0], contester_count)
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
	g_kscOwnershipBaselinePending = true
	g_kscOwnershipMatchId[0] = 0

	// Permanent, low-noise: controlpoints_init's own doc contract is "called
	// AFTER map control point data is initialized", but dodx_objectives_get_num()
	// documents "0 if not yet initialized" as a real return value, not just a
	// theoretical one -- so a silent 0 here is a genuinely different failure
	// than the forward never firing at all, and this is the cheap way to tell
	// them apart if flag positions ever go missing again.
	log_amx("[KTP-STATS] controlpoints_init fired, dodx_objectives_get_num()=%d", g_kscFlagCount)

	if (g_kscFlagCount > KSC_MAX_FLAGS)
		g_kscFlagCount = KSC_MAX_FLAGS
	if (g_kscFlagCount < 0)
		g_kscFlagCount = 0

	for (new f = 0; f < g_kscFlagCount; f++) {
		dodx_objective_get_data(f, CP_name, g_kscFlagName[f], charsmax(g_kscFlagName[]))
		if (!g_kscFlagName[f][0])
			formatex(g_kscFlagName[f], charsmax(g_kscFlagName[]), "flag_%d", f)

		g_kscOwner[f]      = ksc_normalize_owner(
			dodx_area_get_data(f, CA_owning_team))
		g_kscCapTeam[f]    = 0
		g_kscPrevAllies[f] = 0
		g_kscPrevAxis[f]   = 0
		ksc_break_clear(f)
	}

	// Deferred, not emitted here -- found live, with a diagnostic build, not
	// guessed: controlpoints_init fires with correct data (right flag count,
	// right names, right owners, right x/y -- all confirmed by instrumenting
	// every step) and log_message() returns normally, but nothing it writes
	// ever reaches the game log at this exact point in map load. log_amx()
	// calls made in the SAME forward, at the SAME moment, land fine, so this
	// isn't a dead plugin or a dead forward -- just log_message()'s specific
	// path being unready this early under ReHLDS extension mode, the same
	// class of early-init unreliability stats_logging.sma's own plugin_cfg
	// comment already documents for set_task (~10% failure rate calling it
	// from plugin_init instead of plugin_cfg). Same fix shape: don't call
	// the unreliable-this-early thing synchronously from this forward: queue
	// a one-shot task and call it slightly later instead, by which point
	// every other early-init marker in this codebase is already reliable.
	// The data being emitted (g_kscFlagName/g_kscFlagCount) is cached above
	// already, so the delayed task has nothing left to look up.
	set_task(1.0, "ksc_flag_positions_task", KSC_TASK_FLAG_POSITIONS)
}

public ksc_flag_positions_task() {
	for (new f = 0; f < g_kscFlagCount; f++)
		ksc_emit_flag_position(f)
}

// Static per-map fact, fired once per map load (this fires on every map
// load including warmup/halftime reloads, not once per server lifetime --
// harmless, since the daemon side keys on (server, map, flag_index) and
// idempotently upserts). Unbuffered/direct log_message, matching the
// KTP_MATCH_* markers' shape (a bare marker word, no player string) rather
// than ksc_buffer -- these are rare (a handful per map load), not
// per-kill-volume, so there is nothing to batch.
//
// CP_origin_x/CP_origin_y only -- dodx.inc exposes no CP_origin_z, so flag
// position is 2D. That is what last-flag-defense's proximity check (below)
// compares against too, for consistency.
stock ksc_emit_flag_position(f) {
	new x = dodx_objective_get_data(f, CP_origin_x)
	new y = dodx_objective_get_data(f, CP_origin_y)
	new mapname[32]
	get_mapname(mapname, charsmax(mapname))

	log_message("KTP_FLAG_POSITION (map ^"%s^") (flag_index ^"%d^") (flag_name ^"%s^") (x ^"%d^") (y ^"%d^")",
		mapname, f, g_kscFlagName[f], x, y)
}

// Periodic roster-position sample -- raw facts only, on purpose. Per the
// operator's direction on ninja-cap detection (the other consumer of this
// same data), classification ("is this player holding forward territory",
// "is this a solo cap") belongs entirely in the query layer, not here. This
// emits one marker per connected, alive player every
// KSC_POSITION_BROADCAST_SECS -- flat and unconditional, not triggered by
// any in-game event, so it can't bias toward moments something else already
// judged interesting.
//
// Dead players are skipped: they have no meaningful map position between
// death and respawn, and dodx_get_user_origin on a dead player is not
// something any other caller in this file relies on either.
public ksc_position_broadcast_task() {
	if (!ksc_enabled())
		return

	for (new id = 1; id <= MAX_PLAYERS; id++) {
		if (!is_user_connected(id))
			continue
		if (!is_user_alive(id))
			continue

		new pos[64]
		if (!ksc_origin_str(id, pos, charsmax(pos)))
			continue

		new pstr[96]
		ksc_player_str(id, pstr, charsmax(pstr))

		new team = get_user_team(id)
		new Float:gt = get_gametime()

		new line[KSC_BUF_LINE_LEN]
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"position_sample^" (team ^"%d^") (position ^"%s^") (game_time ^"%.2f^")",
			pstr, team, pos, gt)
		ksc_buffer(line)
	}
}

// Reads one player's CURRENT weapon's clip/ammo via dod_get_user_weapon --
// not necessarily the weapon that scored the kill (that's wpnindex, read
// separately). A player who threw a grenade and switched back to their rifle
// before the kill line lands shows the rifle's clip here, not the grenade's;
// verified live -- a grenade kill's k_clip/k_ammo showed real rifle values,
// not a melee/grenade sentinel.
//
// Leaves both at -1 (the caller's default) only if the player is not
// in-game, which in practice means "disconnected between the kill and this
// read" -- a narrow race, not the common case. Melee weapons DO return
// through this native: clip=0, ammo=0 is a real engine reading for a knife,
// confirmed live, not this sentinel. -1 therefore means "could not read",
// never "this weapon has no clip" -- do not treat it as a weapon-type signal.
stock ksc_weapon_state(id, &clip, &ammo) {
	clip = -1
	ammo = -1

	if (id >= 1 && id <= MAX_PLAYERS && is_user_connected(id))
		dod_get_user_weapon(id, clip, ammo)
}

// Per-hit damage ledger. Fires from client_damage for every hit -- enemy,
// team, and self alike; see that forward's comment for why. Buffered like
// everything else here, NOT queued through the daemon's generic recordEvent
// machinery (that's built around hlstats_Events_* tables with a config-driven
// column set); this is a standalone table with its own direct-INSERT handler
// on the daemon side, matching how KTP_MATCH_* and frag_context are handled
// rather than the stock event tables. If per-event INSERT volume ever proves
// to be a real cost at fleet scale it can be batched later -- flagged, not
// silently assumed fine, same as the buffer sizing above.
//
// `game_time` stands in for "tick" from the original phase spec: DoD/AMXX
// exposes no raw network tick counter to Pawn, only get_gametime() (seconds
// since map start, a float). It serves the same ordering/correlation
// purpose -- which hits happened near each other in time -- so this
// substitutes it rather than claiming a tick count that was never captured.
stock ksc_emit_damage(attacker, victim, damage, wpnindex, hitplace) {
	if (!is_user_connected(attacker) || !is_user_connected(victim))
		return

	new attacker_str[96], victim_str[96], weapon[32]
	ksc_player_str(attacker, attacker_str, charsmax(attacker_str))
	ksc_player_str(victim, victim_str, charsmax(victim_str))
	xmod_get_wpnlogname(wpnindex, weapon, charsmax(weapon))

	new damage_capped = (damage > KSC_DAMAGE_CAP) ? KSC_DAMAGE_CAP : damage
	new Float:gt = get_gametime()
	new event_epoch = get_systime()
	new producer_matchid[64], producer_half
	copy(producer_matchid, charsmax(producer_matchid), "-")
	ksc_event_context(producer_matchid, charsmax(producer_matchid), producer_half)

	new line[KSC_BUF_LINE_LEN]
	formatex(line, charsmax(line),
		"^"%s^" triggered ^"damage^" against ^"%s^" with ^"%s^" (damage ^"%d^") (damage_capped ^"%d^") (hitplace ^"%d^") (matchid ^"%s^") (half ^"%d^") (game_time ^"%.2f^") (event_epoch ^"%d^")",
		attacker_str, victim_str, weapon,
		damage, damage_capped, hitplace, producer_matchid, producer_half, gt, event_epoch)

	ksc_buffer(line)
}

// Radius (2D, world units) within which a kill counts as "near" a defended
// flag for last-flag-defense purposes. A starting estimate, not a measured
// one -- validate via a Lane B run with staged defense kills at known
// distances, the same way KSC_BUF_MAX_ENTRIES was sized empirically rather
// than trusted on paper.
#define KSC_LAST_FLAG_RADIUS 1000.0

// Is `team` down to exactly one flag, and was `killer` within
// KSC_LAST_FLAG_RADIUS of it when they got this kill? Per the operator's
// correction: last-flag-defense keys off KILL POSITION relative to the
// defended flag, not off the break queue -- a defender who kills a would-be
// ninja before they start capping is defending just as much, and the break
// queue structurally cannot see a kill that never touched a capture zone.
// 2D only: dodx.inc exposes no CP_origin_z, matching ksc_emit_flag_position.
stock bool:ksc_is_last_flag_defense(killer, killer_team) {
	if (ksc_team_flag_count(killer_team) != 1)
		return false

	new last_flag = -1
	for (new f = 0; f < g_kscFlagCount; f++) {
		if (g_kscOwner[f] == killer_team) {
			last_flag = f
			break
		}
	}
	if (last_flag == -1)
		return false

	new Float:origin[3]
	if (!dodx_get_user_origin(killer, origin))
		return false

	new Float:fx = float(dodx_objective_get_data(last_flag, CP_origin_x))
	new Float:fy = float(dodx_objective_get_data(last_flag, CP_origin_y))
	new Float:dx = origin[0] - fx
	new Float:dy = origin[1] - fy
	new Float:dist2 = (dx * dx) + (dy * dy)

	return dist2 <= (KSC_LAST_FLAG_RADIUS * KSC_LAST_FLAG_RADIUS)
}

// frag_context: fired on every kill (not just headshots), carrying the state
// that makes a kill legible after the fact -- was the victim prone, scoped,
// out of ammo. Retires stats_logging.sma's old headshot-only marker; see the
// file header for why one line replaces two.
//
// Same guard shape the old headshot marker used: skip suicides, skip a
// disconnected participant. dod_get_pronestate returns 0 standing, 1 going
// prone / MG teardown, 2 setting up an MG while down -- stored raw, not
// collapsed to a bool, since the daemon column can hold the real value.
stock ksc_emit_frag_context(killer, victim, wpnindex, hitplace) {
	if (killer < 1 || killer > MAX_PLAYERS || killer == victim)
		return
	if (victim < 1 || victim > MAX_PLAYERS)
		return
	if (!is_user_connected(killer) || !is_user_connected(victim))
		return

	new killer_str[96], victim_str[96], weapon[32]
	ksc_player_str(killer, killer_str, charsmax(killer_str))
	ksc_player_str(victim, victim_str, charsmax(victim_str))
	xmod_get_wpnlogname(wpnindex, weapon, charsmax(weapon))

	new k_clip, k_ammo, v_clip, v_ammo
	ksc_weapon_state(killer, k_clip, k_ammo)
	ksc_weapon_state(victim, v_clip, v_ammo)

	new k_prone = dod_get_pronestate(killer)
	new v_prone = dod_get_pronestate(victim)
	new headshot = (hitplace == HIT_HEAD) ? 1 : 0

	new k_pos[40], v_pos[40]
	new bool:have_k_pos = ksc_origin_str(killer, k_pos, charsmax(k_pos))
	new bool:have_v_pos = ksc_origin_str(victim, v_pos, charsmax(v_pos))
	if (!have_k_pos) k_pos[0] = 0
	if (!have_v_pos) v_pos[0] = 0

	new killer_team = get_user_team(killer)
	new is_last_flag_defense = ksc_is_last_flag_defense(killer, killer_team) ? 1 : 0
	new event_epoch = get_systime()
	new Float:game_time = get_gametime()
	new producer_matchid[64], producer_half
	copy(producer_matchid, charsmax(producer_matchid), "-")
	ksc_event_context(producer_matchid, charsmax(producer_matchid), producer_half)

	new line[KSC_BUF_LINE_LEN]
	if (have_k_pos && have_v_pos)
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"frag_context^" against ^"%s^" with ^"%s^" (headshot ^"%d^") (k_prone ^"%d^") (v_prone ^"%d^") (k_scope ^"%d^") (v_scope ^"%d^") (k_clip ^"%d^") (k_ammo ^"%d^") (v_clip ^"%d^") (v_ammo ^"%d^") (k_position ^"%s^") (v_position ^"%s^") (is_last_flag_defense ^"%d^") (matchid ^"%s^") (half ^"%d^") (game_time ^"%.2f^") (event_epoch ^"%d^")",
			killer_str, victim_str, weapon,
			headshot, k_prone, v_prone,
			g_kscScoped[killer], g_kscScoped[victim],
			k_clip, k_ammo, v_clip, v_ammo,
			k_pos, v_pos, is_last_flag_defense,
			producer_matchid, producer_half, game_time, event_epoch)
	else
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"frag_context^" against ^"%s^" with ^"%s^" (headshot ^"%d^") (k_prone ^"%d^") (v_prone ^"%d^") (k_scope ^"%d^") (v_scope ^"%d^") (k_clip ^"%d^") (k_ammo ^"%d^") (v_clip ^"%d^") (v_ammo ^"%d^") (is_last_flag_defense ^"%d^") (matchid ^"%s^") (half ^"%d^") (game_time ^"%.2f^") (event_epoch ^"%d^")",
			killer_str, victim_str, weapon,
			headshot, k_prone, v_prone,
			g_kscScoped[killer], g_kscScoped[victim],
			k_clip, k_ammo, v_clip, v_ammo,
			is_last_flag_defense,
			producer_matchid, producer_half, game_time, event_epoch)

	ksc_buffer(line)
}

// Called from stats_logging.sma's client_death, ahead of its old headshot-only
// early return, so assists (and now frag context) are credited on every
// death rather than only headshot ones.
//
// The row is cleared whether or not capture is enabled: leaving a populated row
// behind while the cvar is off would let stale damage credit an assist on some
// much later death once it is switched back on.
stock ksc_on_death(killer, victim, wpnindex, hitplace, TK) {
	// Physical boundaries are intentionally independent of the stats-pause
	// gate: round-freeze deaths still happened. The plugin cannot truthfully
	// stamp their live/freeze state without a DODX getter, so this raw ledger
	// leaves that classification to later context. An empty match id (warmup)
	// still fails closed.
	ksc_life_end(victim, "death")

	ksc_queue_break_candidate(killer, victim, TK)

	// No TK exclusion, matching the old headshot-only marker it replaces --
	// that fired on any headshot kill regardless of TK, so this stays a
	// superset rather than a narrower replacement.
	if (ksc_enabled())
		ksc_emit_frag_context(killer, victim, wpnindex, hitplace)

	if (victim < 1 || victim > MAX_PLAYERS)
		return

	if (ksc_enabled()) {
		new victim_team = get_user_team(victim)
		new final_attacker = g_kscLastEnemyAttacker[victim]
		new victim_str[96], victim_pos[40]
		new bool:built_victim_str = false
		new bool:have_victim_pos = false
		new producer_matchid[64], producer_half
		copy(producer_matchid, charsmax(producer_matchid), "-")
		ksc_event_context(producer_matchid, charsmax(producer_matchid), producer_half)
		new event_epoch = get_systime()
		new Float:game_time = get_gametime()

		for (new a = 1; a <= MAX_PLAYERS; a++) {
			if (a == killer || a == final_attacker || a == victim)
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
					"^"%s^" triggered ^"assist^" against ^"%s^" (assister_position ^"%s^") (victim_position ^"%s^") (matchid ^"%s^") (half ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^")",
					assister_str, victim_str, assister_pos, victim_pos,
					producer_matchid, producer_half, event_epoch, game_time)
			else if (have_assister_pos)
				formatex(line, charsmax(line),
					"^"%s^" triggered ^"assist^" against ^"%s^" (assister_position ^"%s^") (matchid ^"%s^") (half ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^")",
					assister_str, victim_str, assister_pos,
					producer_matchid, producer_half, event_epoch, game_time)
			else if (have_victim_pos)
				formatex(line, charsmax(line),
					"^"%s^" triggered ^"assist^" against ^"%s^" (victim_position ^"%s^") (matchid ^"%s^") (half ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^")",
					assister_str, victim_str, victim_pos,
					producer_matchid, producer_half, event_epoch, game_time)
			else
				formatex(line, charsmax(line),
					"^"%s^" triggered ^"assist^" against ^"%s^" (matchid ^"%s^") (half ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^")",
					assister_str, victim_str,
					producer_matchid, producer_half, event_epoch, game_time)

			ksc_buffer(line)
		}
	}

	ksc_clear_damage_row(victim)
}
