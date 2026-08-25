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
// Cap-break attribution reads bounding boxes through dodx natives, NOT fakemeta:
// `fakemeta.inc` carries `#pragma reqlib fakemeta`, and this fleet's modules.ini
// lists only amxxcurl/reapi/dodx -- a plugin including it does not LOAD.
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
// firing it for each non-teamkill frag with headshot as one more property is
// one queued line and one daemon UPDATE per Frags row instead of two.
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
#define KSC_ROUND_REBASE_EPSILON 0.01

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
#define KSC_BUF_LINE_LEN    832
#define KSC_BUF_FLUSH_SECS  5.0

// Life boundaries are control-plane facts. Keep them out of the high-volume
// damage/frag queue so a hit burst cannot consume their capacity. This queue
// is drained first, reports its own drops, and has a truthful bool enqueue API.
#define KSC_LIFE_BUF_MAX_ENTRIES 64
#define KSC_LIFE_BUF_LINE_LEN    416

// Increment when the producer/daemon storage contract changes. This is
// emitted in every per-half manifest so mixed fleet deployments are visible
// without reverse-engineering them from which rows happened to arrive.
#define KSC_SCHEMA_CONTRACT 21
#define KSC_CAPABILITIES "frag_context,damage,position,assist,life,break,flag_state,flag_position,sequence,health"

enum {
	KSC_EVENT_LIFE = 0,
	KSC_EVENT_DAMAGE,
	KSC_EVENT_POSITION,
	KSC_EVENT_FRAG,
	KSC_EVENT_ASSIST,
	KSC_EVENT_BREAK,
	KSC_EVENT_FLAG_STATE,
	KSC_EVENT_FLAG_POSITION,
	KSC_EVENT_COUNT
}

// Distinct from stats_logging.sma's own flush task, which registers with the
// default id -- sharing an id would make one remove_task() cancel both.
#define KSC_TASK_FLUSH          88010
#define KSC_TASK_ZONES          88011
#define KSC_TASK_POSITIONS      88012
#define KSC_TASK_FLAG_POSITIONS 88013

new g_kscBuffer[KSC_BUF_MAX_ENTRIES][KSC_BUF_LINE_LEN]
new g_kscBufferType[KSC_BUF_MAX_ENTRIES]
new g_kscBufferSequence[KSC_BUF_MAX_ENTRIES]
new g_kscBufferCount = 0
new g_kscDropped = 0
new g_kscLifeBuffer[KSC_LIFE_BUF_MAX_ENTRIES][KSC_LIFE_BUF_LINE_LEN]
new g_kscLifeBufferType[KSC_LIFE_BUF_MAX_ENTRIES]
new g_kscLifeBufferSequence[KSC_LIFE_BUF_MAX_ENTRIES]
new g_kscLifeBufferCount = 0
new g_kscLifeDropped = 0
new g_kscAttempted[KSC_EVENT_COUNT]
new g_kscEnqueued[KSC_EVENT_COUNT]
new g_kscDroppedByType[KSC_EVENT_COUNT]
new g_kscEmitted[KSC_EVENT_COUNT]
new g_kscSequence = 0
new const g_kscEventNames[KSC_EVENT_COUNT][] = {
	"life", "damage", "position", "frag", "assist", "break",
	"flag_state", "flag_position"
}

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

// Latched by controlpoints_init, cleared once the positions actually reach the
// log. Two independent callers arm the emit task and either may be the one that
// survives, so the flag is what keeps it to one emit per cache refresh.
new bool:g_kscFlagPosPending = false

new g_kscCapTeam[KSC_MAX_FLAGS]     // team currently capturing this point, 0 = none
new g_kscOwner[KSC_MAX_FLAGS]       // last seen CA_owning_team, to spot a completed cap
new bool:g_kscOwnershipBaselinePending = true
new g_kscOwnershipMatchId[64]
new g_kscPrevAllies[KSC_MAX_FLAGS]  // in-zone counts as of the previous poll
new g_kscPrevAxis[KSC_MAX_FLAGS]

new g_kscBreakQ[KSC_MAX_FLAGS][KSC_BREAK_QUEUE_MAX]    // FIFO of candidate killers
new g_kscBreakVictim[KSC_MAX_FLAGS][KSC_BREAK_QUEUE_MAX]
new g_kscBreakTtl[KSC_MAX_FLAGS][KSC_BREAK_QUEUE_MAX]  // polls left before expiry
new g_kscBreakCount[KSC_MAX_FLAGS]
new g_kscBreakTeam[KSC_MAX_FLAGS]   // team being broken, latched with the first candidate
new g_kscBreakBase[KSC_MAX_FLAGS]   // rolling in-zone baseline to measure drops against
new Float:g_kscLastRoundTime = -1.0
new bool:g_kscBreakBaselineSuppressed = true
new g_kscCvarTimelimit

// Defined up here only because Pawn resolves stocks in declaration order and
// ksc_cfg() below needs it.
stock ksc_arm_flag_positions() {
	remove_task(KSC_TASK_FLAG_POSITIONS)
	set_task(1.0, "ksc_flag_positions_task", KSC_TASK_FLAG_POSITIONS)
}

stock ksc_init() {
	// Runtime kill switch, so new capture can be shut off on a live server
	// without a redeploy. Mirrors the dodstats_pause/isDSMActive() gate this
	// plugin already uses for its stock output.
	g_kscCvarEnabled = register_cvar("ktp_stats_capture", "1")
	g_kscCvarTimelimit = get_cvar_pointer("mp_timelimit")
}

stock ksc_cfg() {
	// plugin_cfg, not plugin_init: in extension mode plugin_init runs inside a
	// hookchain handler, where a repeating set_task intermittently fails to
	// register. Same reason the stock buffer's task is registered there.
	set_task(KSC_BUF_FLUSH_SECS, "ksc_flush_task", KSC_TASK_FLUSH, .flags="b")
	set_task(KSC_ZONE_POLL_SECS, "ksc_zone_poll_task", KSC_TASK_ZONES, .flags="b")
	set_task(KSC_POSITION_BROADCAST_SECS, "ksc_position_broadcast_task", KSC_TASK_POSITIONS, .flags="b")

	// The arm that actually survives a changelevel. controlpoints_init runs
	// before the extension-mode map-change path wipes the task list, so its own
	// arm only reaches the log on a cold boot; plugin_cfg runs after the wipe.
	ksc_arm_flag_positions()
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

stock ksc_next_sequence() {
	g_kscSequence++
	return g_kscSequence
}

stock ksc_reset_health() {
	g_kscSequence = 0
	for (new i = 0; i < KSC_EVENT_COUNT; i++) {
		g_kscAttempted[i] = 0
		g_kscEnqueued[i] = 0
		g_kscDroppedByType[i] = 0
		g_kscEmitted[i] = 0
	}
}

stock ksc_emit_manifest(const matchid[], half) {
	new mapname[32]
	get_mapname(mapname, charsmax(mapname))
	new sequence = ksc_next_sequence()
	log_message("KTP_CAPTURE_MANIFEST (matchid ^"%s^") (half ^"%d^") (map ^"%s^") (producer ^"stats_logging^") (producer_version ^"%s^") (schema ^"%d^") (capabilities ^"%s^") (position_interval ^"%.1f^") (buffer_entries ^"%d^") (life_buffer_entries ^"%d^") (sequence ^"%d^") (event_epoch ^"%d^")",
		matchid, half, mapname, PLUGIN_VERSION, KSC_SCHEMA_CONTRACT,
		KSC_CAPABILITIES, KSC_POSITION_BROADCAST_SECS,
		KSC_BUF_MAX_ENTRIES, KSC_LIFE_BUF_MAX_ENTRIES, sequence, get_systime())
}

stock ksc_emit_health(const matchid[], half) {
	new last_data_sequence = g_kscSequence
	for (new event_type = 0; event_type < KSC_EVENT_COUNT; event_type++) {
		new sequence = ksc_next_sequence()
		log_message("KTP_CAPTURE_HEALTH (matchid ^"%s^") (half ^"%d^") (event_type ^"%s^") (attempted ^"%d^") (enqueued ^"%d^") (dropped ^"%d^") (emitted ^"%d^") (sequence_first ^"1^") (sequence_last ^"%d^") (sequence ^"%d^") (event_epoch ^"%d^")",
			matchid, half, g_kscEventNames[event_type],
			g_kscAttempted[event_type], g_kscEnqueued[event_type],
			g_kscDroppedByType[event_type], g_kscEmitted[event_type],
			last_data_sequence, sequence, get_systime())
	}
}

stock ksc_buffer(const line[], event_type) {
	g_kscAttempted[event_type]++
	if (g_kscBufferCount >= KSC_BUF_MAX_ENTRIES) {
		// Deliberately NOT an inline flush (which is what the stock buffer_log
		// does when full): this runs in postthink, and a synchronous write here
		// is precisely the frame stall the buffering exists to prevent. Drop and
		// count instead -- the task drains within KSC_BUF_FLUSH_SECS, and a
		// non-zero counter in the logs is the signal to raise the size.
		g_kscDropped++
		g_kscDroppedByType[event_type]++
		return
	}

	copy(g_kscBuffer[g_kscBufferCount], KSC_BUF_LINE_LEN - 1, line)
	g_kscBufferType[g_kscBufferCount] = event_type
	g_kscBufferSequence[g_kscBufferCount] = g_kscSequence
	g_kscBufferCount++
	g_kscEnqueued[event_type]++
}

stock ksc_normalize_match_half(half) {
	if (half == 1 || half == 2)
		return half
	if (half >= 101 && half <= 353)
		return half - 98
	return 0
}

stock ksc_on_match_start(const matchid[], half) {
	// KTPMatchHandler fires ktp_half_end only with half=1 and ktp_match_end
	// only at true match end, so an H2->OT or OT->OT boundary arrives ONLY as
	// this next start. Emit the outgoing half's health record before its
	// counters are reset below, or that half's record is silently lost.
	// The normal H1 end is unaffected: ktp_half_end already emitted and
	// cleared this context, so the guard is false there.
	if (g_kscProducerMatchId[0] && g_kscProducerHalf != 0) {
		ksc_flush()
		ksc_emit_health(g_kscProducerMatchId, g_kscProducerHalf)
	}

	// A match/half boundary invalidates every observation made against the
	// previous round. Suppress one fresh zone baseline even when the clock is
	// unavailable, so a warmup candidate cannot drain into go-live counts.
	ksc_break_reset_boundary()

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
	ksc_flush()
	ksc_reset_health()
	g_kscLifeBaselinePending = true
	ksc_clear_open_lives()
	ksc_emit_manifest(matchid, normalized_half)
	// Static metadata is an idempotent upsert. Re-emit it at the authoritative
	// match boundary so a map-load log-path miss cannot disable objective
	// distance analytics for the whole half.
	ksc_flag_positions_task()
}

stock ksc_on_match_context_end(const matchid[]) {
	if (!g_kscProducerMatchId[0] || !equal(matchid, g_kscProducerMatchId))
		return
	ksc_break_reset_boundary()

	// Preserve ordering: drain boundaries while this exact producer context is
	// still known, then make later warmup/disconnect events fail closed.
	ksc_flush()
	ksc_emit_health(matchid, g_kscProducerHalf)
	g_kscProducerMatchId[0] = 0
	g_kscProducerHalf = 0
	g_kscProducerContextConfirmed = false
	g_kscLifeBaselinePending = true
	ksc_clear_open_lives()
}

// hlstats.pl joins frag_context and damage on an EXACT second via this
// context; without it the daemon falls back to a 10s receipt window and
// misses -- the KTP_NO_ROW_MATCHED flood, despite its message blaming UDP.
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
			// Some force-reset/abandon exits clear DODX context without firing
			// ktp_half_end/ktp_match_end. Treat that authoritative mismatch as
			// the same capture boundary so a queued break cannot drain in warmup.
			ksc_break_reset_boundary()
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
	g_kscAttempted[KSC_EVENT_LIFE]++
	if (g_kscLifeBufferCount >= KSC_LIFE_BUF_MAX_ENTRIES) {
		g_kscLifeDropped++
		g_kscDroppedByType[KSC_EVENT_LIFE]++
		return false
	}

	copy(g_kscLifeBuffer[g_kscLifeBufferCount], KSC_LIFE_BUF_LINE_LEN - 1, line)
	g_kscLifeBufferType[g_kscLifeBufferCount] = KSC_EVENT_LIFE
	g_kscLifeBufferSequence[g_kscLifeBufferCount] = g_kscSequence
	g_kscLifeBufferCount++
	g_kscEnqueued[KSC_EVENT_LIFE]++
	return true
}

public ksc_flush_task() {
	ksc_flush()
}

stock ksc_flush() {
	// Both capacity pools share one producer sequence. Merge their already
	// ordered contents so neither queue can jump ahead of the other on the wire.
	new data_i = 0, life_i = 0
	while (data_i < g_kscBufferCount || life_i < g_kscLifeBufferCount) {
		if (life_i < g_kscLifeBufferCount &&
			(data_i >= g_kscBufferCount || g_kscLifeBufferSequence[life_i] < g_kscBufferSequence[data_i])) {
			log_message("%s", g_kscLifeBuffer[life_i])
			g_kscEmitted[g_kscLifeBufferType[life_i]]++
			life_i++
		} else {
			log_message("%s", g_kscBuffer[data_i])
			g_kscEmitted[g_kscBufferType[data_i]]++
			data_i++
		}
	}

	g_kscBufferCount = 0
	g_kscLifeBufferCount = 0

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
	new sequence = ksc_next_sequence()

	new line[KSC_LIFE_BUF_LINE_LEN]
	formatex(line, charsmax(line),
		"^"%s^" triggered ^"life_boundary^" (matchid ^"%s^") (half ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^") (kind ^"%s^") (reason ^"%s^") (team ^"%d^") (class ^"%d^") (slot ^"%d^") (sequence ^"%d^")",
		player_str, producer_matchid, producer_half, event_epoch, game_time,
		kind, reason, team, class_id, id, sequence)
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

stock ksc_break_clear_all() {
	for (new f = 0; f < g_kscFlagCount; f++)
		ksc_break_clear(f)
}

stock ksc_break_reset_boundary() {
	ksc_break_clear_all()
	g_kscLastRoundTime = -1.0
	g_kscBreakBaselineSuppressed = true
}

// The DODX clock projects from m_flRestartRoundTime as soon as a clan restart
// is scheduled, before the engine zeroes objective occupancies. That produces
// an authoritative upward rebase without depending on RoundState user messages
// (which are absent in all-bot Lane B). Once observed, suppress and clear break
// candidates for the whole projected countdown, then suppress one final poll
// at completion so its zeroed counts become the new baseline rather than a
// burst of phantom breaks.
stock bool:ksc_break_observe_round_clock() {
	new Float:current = dodx_get_round_time()
	if (current < 0.0) {
		// There is no safe way to distinguish an ordinary occupancy drop from
		// a round-reset collapse while the authoritative clock is unavailable.
		// Stay fail-closed on every poll, not just the first one: callers clear
		// all candidates and the per-flag loop below still refreshes the live
		// count baselines. Keeping suppression armed also makes recovery consume
		// one fresh baseline before break evaluation resumes.
		g_kscLastRoundTime = -1.0
		g_kscBreakBaselineSuppressed = true
		return true
	}

	new Float:limit = -1.0
	if (g_kscCvarTimelimit)
		limit = get_pcvar_float(g_kscCvarTimelimit) * 60.0

	// The native's countdown projection is mathematically above the nominal
	// limit until the scheduled restart target. Do not add a tolerance here:
	// even a small one can release suppression before objective counts reset.
	new bool:countdown = (limit > 0.0 && current > limit)
	new bool:rebased = (g_kscLastRoundTime >= 0.0 &&
		current > g_kscLastRoundTime + KSC_ROUND_REBASE_EPSILON)
	g_kscLastRoundTime = current

	if (rebased || countdown)
		g_kscBreakBaselineSuppressed = true

	new bool:suppress_now = g_kscBreakBaselineSuppressed
	if (g_kscBreakBaselineSuppressed && !countdown)
		g_kscBreakBaselineSuppressed = false

	return suppress_now
}

stock ksc_break_shift(f) {
	for (new i = 1; i < g_kscBreakCount[f]; i++) {
		g_kscBreakQ[f][i - 1]   = g_kscBreakQ[f][i]
		g_kscBreakVictim[f][i - 1] = g_kscBreakVictim[f][i]
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

// A team owning one flag is not enough to call a capout threat. Neutral or
// otherwise unaccounted-for flags make that inference false (observed live on
// dod_thunder2, where the old one-sided check marked every central-point break
// as a capout). Require a complete two-team partition: the defender owns this
// final flag and the attacker owns every other objective.
stock bool:ksc_is_full_capout_threat(defending_team) {
	if (g_kscFlagCount < 2)
		return false
	if (defending_team != KSC_TEAM_ALLIES && defending_team != KSC_TEAM_AXIS)
		return false

	new attacking_team = (defending_team == KSC_TEAM_ALLIES) ? KSC_TEAM_AXIS : KSC_TEAM_ALLIES
	if (ksc_team_flag_count(defending_team) != 1)
		return false
	if (ksc_team_flag_count(attacking_team) != g_kscFlagCount - 1)
		return false

	return true
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
	new matchid[64], half
	if (!ksc_event_context(matchid, charsmax(matchid), half))
		return

	new mapname[32]
	get_mapname(mapname, charsmax(mapname))

	// Direct from the repeating zone task, not a player postthink callback.
	// These lines are rare and their ordering relative to KTP_MATCH_START /
	// KTP_ROUND_FREEZE matters, so do not hold them in the five-second buffer.
	ksc_flush()
	new sequence = ksc_next_sequence()
	g_kscAttempted[KSC_EVENT_FLAG_STATE]++
	g_kscEnqueued[KSC_EVENT_FLAG_STATE]++
	log_message("KTP_FLAG_STATE (map ^"%s^") (flag_index ^"%d^") (flag_name ^"%s^") (owner ^"%d^") (initial ^"%d^") (game_time ^"%.2f^") (matchid ^"%s^") (half ^"%d^") (sequence ^"%d^") (event_epoch ^"%d^")",
		mapname, f, g_kscFlagName[f], owner, is_initial, get_gametime(),
		matchid, half, sequence, get_systime())
	g_kscEmitted[KSC_EVENT_FLAG_STATE]++
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
	// Re-emit the static coordinates only after DODX confirms the same match
	// context. The match-start forward can precede that write by a frame, so its
	// eager attempt is useful but not sufficient for a sequenced health record.
	for (new f = 0; f < g_kscFlagCount; f++)
		ksc_emit_flag_position(f)
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

stock ksc_emit_break(f, breaker, victim, contester_count) {
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
	new is_capout = ksc_is_full_capout_threat(defending_team) ? 1 : 0
	new sequence = ksc_next_sequence()
	new producer_matchid[64], producer_half
	copy(producer_matchid, charsmax(producer_matchid), "-")
	ksc_event_context(producer_matchid, charsmax(producer_matchid), producer_half)
	new event_epoch = get_systime()
	new Float:game_time = get_gametime()

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
			"^"%s^" triggered ^"cap_break^" (flag ^"%s^") (flag_index ^"%d^") (position ^"%s^") (matchid ^"%s^") (half ^"%d^") (sequence ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^")",
			breaker_str, g_kscFlagName[f], f, pos, producer_matchid,
			producer_half, sequence, event_epoch, game_time)
	else
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"cap_break^" (flag ^"%s^") (flag_index ^"%d^") (matchid ^"%s^") (half ^"%d^") (sequence ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^")",
			breaker_str, g_kscFlagName[f], f, producer_matchid,
			producer_half, sequence, event_epoch, game_time)

	ksc_buffer(line, KSC_EVENT_BREAK)
	ksc_emit_break_context(breaker, victim, f, contester_count, time_remaining, is_capout)
}

// Follow-up marker for the cap_break line just buffered above. Same
// technique as frag_context/headshot_kill: the daemon flushes PlayerActions,
// then UPDATEs the most recent matching row by (playerId, actionId) rather
// than this being a second independent event.
stock ksc_emit_break_context(breaker, victim, flag_index, contester_count, Float:time_remaining, is_capout) {
	new breaker_str[96], line[KSC_BUF_LINE_LEN]
	ksc_player_str(breaker, breaker_str, charsmax(breaker_str))
	new victim_str[96]
	if (victim >= 1 && victim <= MAX_PLAYERS && is_user_connected(victim))
		ksc_player_str(victim, victim_str, charsmax(victim_str))
	else
		copy(victim_str, charsmax(victim_str), "-")
	new producer_matchid[64], producer_half
	copy(producer_matchid, charsmax(producer_matchid), "-")
	ksc_event_context(producer_matchid, charsmax(producer_matchid), producer_half)
	new sequence = ksc_next_sequence()

	formatex(line, charsmax(line),
		"^"%s^" triggered ^"break_context^" (victim ^"%s^") (flag ^"%s^") (flag_index ^"%d^") (contester_count ^"%d^") (time_remaining ^"%.1f^") (is_capout ^"%d^") (matchid ^"%s^") (half ^"%d^") (sequence ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^")",
		breaker_str, victim_str, g_kscFlagName[flag_index], flag_index, contester_count,
		time_remaining, is_capout, producer_matchid, producer_half, sequence,
		get_systime(), get_gametime())

	ksc_buffer(line, KSC_EVENT_BREAK)
}

// The area API exposes aggregate team counts, not per-player membership, so
// the flag a death belongs to has to be inferred. Ask whether the victim was
// INSIDE the zone rather than how near the flag prop they were: a prop can sit
// thousands of units from its own zone, so any prop-anchored radius is wrong on
// some map however it is tuned.
//
// Box-vs-box, not point-in-box -- GoldSrc decides zone membership by bbox
// overlap, so a point test would reject players the engine counts as in-zone.
stock ksc_break_flag_for_victim(victim, victim_team) {
	new Float:vmin[3], Float:vmax[3]
	if (!dodx_get_user_bounds(victim, vmin, vmax))
		return -1

	new best_flag = -1
	new Float:best_d2 = 0.0

	for (new f = 0; f < g_kscFlagCount; f++) {
		// A capture can begin between the 0.2-second zone poll and this death.
		// Candidate selection must use the engine's current area state; the
		// delayed poll still requires a real count drop before crediting it.
		if (!dodx_area_get_data(f, CA_is_capturing) ||
			dodx_area_get_data(f, CA_capturing_team) != victim_team)
			continue

		// Returns 0 on a missing, freed or zero-volume area -- a flag that cannot
		// answer is skipped, never silently matched.
		new Float:zmin[3], Float:zmax[3]
		if (!dodx_area_get_bounds(f, zmin, zmax))
			continue

		if (vmax[0] < zmin[0] || vmin[0] > zmax[0]) continue
		if (vmax[1] < zmin[1] || vmin[1] > zmax[1]) continue
		if (vmax[2] < zmin[2] || vmin[2] > zmax[2]) continue

		// Zones can overlap. Nearest zone centre orders the survivors; it never
		// admits or rejects one, so it is a tie-break and not a threshold.
		new Float:dx = ((vmin[0] + vmax[0]) - (zmin[0] + zmax[0])) * 0.5
		new Float:dy = ((vmin[1] + vmax[1]) - (zmin[1] + zmax[1])) * 0.5
		new Float:d2 = (dx * dx) + (dy * dy)

		if (best_flag < 0 || d2 < best_d2) {
			best_d2 = d2
			best_flag = f
		}
	}

	return best_flag
}

// DeathMsg is the no-Damage fallback for world/suicide deaths. Its DODX
// client_death dispatch has degraded TK=0 semantics, and the documented 33ms
// Damage/DeathMsg race can put a real teamkill on that path. Reconstruct the
// same-team fact while both slots are still live so neither break attribution
// nor the Frags-only context marker trusts that lossy bit alone.
stock bool:ksc_effective_teamkill(killer, victim, TK) {
	if (TK)
		return true
	if (killer < 1 || killer > MAX_PLAYERS ||
			victim < 1 || victim > MAX_PLAYERS || killer == victim)
		return false
	if (!is_user_connected(killer) || !is_user_connected(victim))
		return false

	new killer_team = get_user_team(killer)
	if (killer_team != KSC_TEAM_ALLIES && killer_team != KSC_TEAM_AXIS)
		return false

	return killer_team == get_user_team(victim)
}

// Called from ksc_on_death. A kill only becomes a break if the victim's team
// was mid-capture somewhere and their in-zone count then drops.
stock ksc_queue_break_candidate(killer, victim, bool:teamkill) {
	if (!ksc_enabled())
		return
	if (teamkill || killer == victim)
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
		g_kscBreakVictim[f][q] = victim
		g_kscBreakTtl[f][q] = KSC_BREAK_WINDOW
		g_kscBreakCount[f]++
	}
}

public ksc_zone_poll_task() {
	// Match-context transition detection must not depend on a map having flags.
	// It also deliberately precedes the capture kill switch's early return so
	// an empty context always resets the pending baseline/open-life state.
	ksc_sync_life_context()

	if (!ksc_enabled()) {
		// The runtime kill switch is also a capture-state boundary. Never let
		// a queued candidate or an old count baseline survive a disabled
		// restart and drain when capture is enabled again.
		ksc_break_clear_all()
		g_kscBreakBaselineSuppressed = true
		return
	}
	if (g_kscFlagCount <= 0)
		return

	ksc_ensure_ownership_baseline()

	// This runs before any per-flag count-drop processing. During a restart the
	// loop still refreshes owner/capping/count caches, but pending killers are
	// empty and break evaluation is explicitly suppressed for this baseline.
	new bool:suppress_breaks = ksc_break_observe_round_clock()
	if (suppress_breaks)
		ksc_break_clear_all()

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

		if (!suppress_breaks && g_kscBreakCount[f] > 0) {
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
					ksc_emit_break(f, g_kscBreakQ[f][0], g_kscBreakVictim[f][0], contester_count)
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
	g_kscLastRoundTime = -1.0
	g_kscBreakBaselineSuppressed = true

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

	new zones_ok = 0
	new Float:zmn[3], Float:zmx[3]
	for (new f = 0; f < g_kscFlagCount; f++)
		if (dodx_area_get_bounds(f, zmn, zmx))
			zones_ok++
	// Loud at map start beats finding it as missing rows a season later: a flag
	// with no usable zone can never report a cap break.
	log_amx("[KTP-STATS] cap-break zones usable on %d of %d flag(s)", zones_ok, g_kscFlagCount)

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
	//
	// This arm alone only ever fires on the boot map: KTPAMX_ReloadPlugins()
	// wipes the task list after dodx fires this forward, so ksc_cfg()'s arm is
	// the surviving one. Keep this one for dodx's post-plugin_cfg InitObj re-fire.
	g_kscFlagPosPending = true
	ksc_arm_flag_positions()
}

public ksc_flag_positions_task() {
	if (!g_kscFlagPosPending || g_kscFlagCount <= 0)
		return

	g_kscFlagPosPending = false

	for (new f = 0; f < g_kscFlagCount; f++)
		ksc_emit_flag_position(f)

	// controlpoints_init's diagnostic proved the forward fired and nothing proved
	// the emit did, so the halves sat in two log files and went uncompared.
	log_amx("[KTP-STATS] flag positions emitted for %d flag(s)", g_kscFlagCount)
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

	new matchid[64], half, sequence = 0
	new bool:tracked = ksc_event_context(matchid, charsmax(matchid), half)
	if (!tracked)
		copy(matchid, charsmax(matchid), "-")
	else {
		// A direct metadata record must follow every older buffered sequence.
		ksc_flush()
		sequence = ksc_next_sequence()
		g_kscAttempted[KSC_EVENT_FLAG_POSITION]++
		g_kscEnqueued[KSC_EVENT_FLAG_POSITION]++
	}

	log_message("KTP_FLAG_POSITION (map ^"%s^") (flag_index ^"%d^") (flag_name ^"%s^") (x ^"%d^") (y ^"%d^") (matchid ^"%s^") (half ^"%d^") (sequence ^"%d^") (event_epoch ^"%d^")",
		mapname, f, g_kscFlagName[f], x, y, matchid, half, sequence, get_systime())
	if (tracked)
		g_kscEmitted[KSC_EVENT_FLAG_POSITION]++
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
	new producer_matchid[64], producer_half
	if (!ksc_event_context(producer_matchid, charsmax(producer_matchid), producer_half))
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
		new event_epoch = get_systime()
		new sequence = ksc_next_sequence()

		new line[KSC_BUF_LINE_LEN]
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"position_sample^" (team ^"%d^") (position ^"%s^") (matchid ^"%s^") (half ^"%d^") (game_time ^"%.2f^") (event_epoch ^"%d^") (sequence ^"%d^")",
			pstr, team, pos, producer_matchid, producer_half, gt, event_epoch, sequence)
		ksc_buffer(line, KSC_EVENT_POSITION)
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
	new sequence = ksc_next_sequence()

	new line[KSC_BUF_LINE_LEN]
	formatex(line, charsmax(line),
		"^"%s^" triggered ^"damage^" against ^"%s^" with ^"%s^" (damage ^"%d^") (damage_capped ^"%d^") (hitplace ^"%d^") (matchid ^"%s^") (half ^"%d^") (game_time ^"%.2f^") (event_epoch ^"%d^") (sequence ^"%d^")",
		attacker_str, victim_str, weapon,
		damage, damage_capped, hitplace, producer_matchid, producer_half, gt, event_epoch, sequence)

	ksc_buffer(line, KSC_EVENT_DAMAGE)
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
	if (!ksc_is_full_capout_threat(killer_team))
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

// frag_context: fired on every non-teamkill player frag (not just headshots),
// carrying the state that makes a kill legible after the fact -- was the
// victim prone, scoped, out of ammo. Teamkills live in a different HLStatsX
// table with no context columns. Retires stats_logging.sma's old headshot-only
// marker; see the file header for why one line replaces two.
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
	new sequence = ksc_next_sequence()

	new line[KSC_BUF_LINE_LEN]
	if (have_k_pos && have_v_pos)
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"frag_context^" against ^"%s^" with ^"%s^" (headshot ^"%d^") (k_prone ^"%d^") (v_prone ^"%d^") (k_scope ^"%d^") (v_scope ^"%d^") (k_clip ^"%d^") (k_ammo ^"%d^") (v_clip ^"%d^") (v_ammo ^"%d^") (k_position ^"%s^") (v_position ^"%s^") (is_last_flag_defense ^"%d^") (matchid ^"%s^") (half ^"%d^") (game_time ^"%.2f^") (event_epoch ^"%d^") (sequence ^"%d^")",
			killer_str, victim_str, weapon,
			headshot, k_prone, v_prone,
			g_kscScoped[killer], g_kscScoped[victim],
			k_clip, k_ammo, v_clip, v_ammo,
			k_pos, v_pos, is_last_flag_defense,
			producer_matchid, producer_half, game_time, event_epoch, sequence)
	else
		formatex(line, charsmax(line),
			"^"%s^" triggered ^"frag_context^" against ^"%s^" with ^"%s^" (headshot ^"%d^") (k_prone ^"%d^") (v_prone ^"%d^") (k_scope ^"%d^") (v_scope ^"%d^") (k_clip ^"%d^") (k_ammo ^"%d^") (v_clip ^"%d^") (v_ammo ^"%d^") (is_last_flag_defense ^"%d^") (matchid ^"%s^") (half ^"%d^") (game_time ^"%.2f^") (event_epoch ^"%d^") (sequence ^"%d^")",
			killer_str, victim_str, weapon,
			headshot, k_prone, v_prone,
			g_kscScoped[killer], g_kscScoped[victim],
			k_clip, k_ammo, v_clip, v_ammo,
			is_last_flag_defense,
			producer_matchid, producer_half, game_time, event_epoch, sequence)

	ksc_buffer(line, KSC_EVENT_FRAG)
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

	new bool:effectiveTeamkill = ksc_effective_teamkill(killer, victim, TK)
	ksc_queue_break_candidate(killer, victim, effectiveTeamkill)

	// frag_context is an UPDATE contract for hlstats_Events_Frags. HLStatsX
	// stores teamkills in hlstats_Events_Teamkills instead, so emitting this
	// marker for a TK can only produce a false no-row diagnostic (and has no
	// Teamkills context columns to populate).
	if (ksc_enabled() && !effectiveTeamkill)
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
			new sequence = ksc_next_sequence()

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
					"^"%s^" triggered ^"assist^" against ^"%s^" (assister_position ^"%s^") (victim_position ^"%s^") (matchid ^"%s^") (half ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^") (sequence ^"%d^")",
					assister_str, victim_str, assister_pos, victim_pos,
					producer_matchid, producer_half, event_epoch, game_time, sequence)
			else if (have_assister_pos)
				formatex(line, charsmax(line),
					"^"%s^" triggered ^"assist^" against ^"%s^" (assister_position ^"%s^") (matchid ^"%s^") (half ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^") (sequence ^"%d^")",
					assister_str, victim_str, assister_pos,
					producer_matchid, producer_half, event_epoch, game_time, sequence)
			else if (have_victim_pos)
				formatex(line, charsmax(line),
					"^"%s^" triggered ^"assist^" against ^"%s^" (victim_position ^"%s^") (matchid ^"%s^") (half ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^") (sequence ^"%d^")",
					assister_str, victim_str, victim_pos,
					producer_matchid, producer_half, event_epoch, game_time, sequence)
			else
				formatex(line, charsmax(line),
					"^"%s^" triggered ^"assist^" against ^"%s^" (matchid ^"%s^") (half ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^") (sequence ^"%d^")",
					assister_str, victim_str,
					producer_matchid, producer_half, event_epoch, game_time, sequence)

			ksc_buffer(line, KSC_EVENT_ASSIST)
		}
	}

	ksc_clear_damage_row(victim)
}
