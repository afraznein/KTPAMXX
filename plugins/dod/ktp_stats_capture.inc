// KTP Stats Capture -- extra per-match stats logged for KTPHLStatsX.
//
// Scope today: assists, durable private life boundaries, cap breaks,
// assist/break positions, frag context
// (prone/scope/ammo/headshot at kill), the per-hit damage ledger, static
// flag positions, flag-ownership changes, factual objective-attempt and
// grenade-entity lifecycles, break context
// (contester count/time remaining/capout),
// last-flag-defense (kill-position-based, not break-queue-based), and a
// periodic roster-position broadcast for positional/"holding" stats.
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
#define KSC_BREAK_RADIUS    512.0
#define KSC_ROUND_REBASE_EPSILON 0.01

// Roster-position broadcast for positional/"holding" stats (map presence
// beyond a discrete capture event) and, later, ninja-cap detection -- both
// deferred items that need the exact same underlying data (see
// tests/e2e_stats/NEXT_PHASES.md "Ninja-cap detection" in KTPInfrastructure).
//
// Denver 4's four audited 12-mans produced 18,943 rows at the old five-second
// cadence. Two seconds projects to about 47,358 rows across the same matches
// (~4.9 inserts/s/match) while retaining useful buffer headroom. One second is
// deliberately deferred until a soak proves it does not produce drops. The
// alive-and-connected filter below, five-second flush, and 128-entry buffer are
// unchanged; verify the health/drop counters on Lane B before production.
#define KSC_POSITION_BROADCAST_SECS 2.0

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

// Allocation-free owner cache for grenade lifecycle identity. This is double
// the existing DODX damage-attribution grenade pool and comfortably above a
// legal match's plausible concurrent grenade count.
#define KSC_GRENADE_CACHE_MAX 64

// Increment when the producer/daemon storage contract changes. This is
// emitted in every per-half manifest so mixed fleet deployments are visible
// without reverse-engineering them from which rows happened to arrive.
#define KSC_SCHEMA_CONTRACT 22
#define KSC_CAPABILITIES "frag_context,damage,position,assist,life,break,flag_state,flag_position,objective_attempt,team_membership,grenade_entity,sequence,health"

enum {
	KSC_EVENT_LIFE = 0,
	KSC_EVENT_DAMAGE,
	KSC_EVENT_POSITION,
	KSC_EVENT_FRAG,
	KSC_EVENT_ASSIST,
	KSC_EVENT_BREAK,
	KSC_EVENT_FLAG_STATE,
	KSC_EVENT_FLAG_POSITION,
	KSC_EVENT_OBJECTIVE_ATTEMPT,
	// Keep membership before grenade_entity: schema-22 health finalizes after
	// grenade_entity, so every stream is reconciled before state is cleared.
	KSC_EVENT_TEAM_MEMBERSHIP,
	KSC_EVENT_GRENADE_ENTITY,
	KSC_EVENT_COUNT
}

#define KSC_EVENT_UNTRACKED -1

// Distinct from stats_logging.sma's own flush task, which registers with the
// default id -- sharing an id would make one remove_task() cancel both.
#define KSC_TASK_FLUSH          88010
#define KSC_TASK_ZONES          88011
#define KSC_TASK_POSITIONS      88012
#define KSC_TASK_FLAG_POSITIONS 88013

new g_kscBuffer[KSC_BUF_MAX_ENTRIES][KSC_BUF_LINE_LEN]
new g_kscBufferType[KSC_BUF_MAX_ENTRIES]
new g_kscBufferOrder[KSC_BUF_MAX_ENTRIES]
new g_kscBufferCount = 0
new g_kscDropped = 0
new g_kscLifeBuffer[KSC_LIFE_BUF_MAX_ENTRIES][KSC_LIFE_BUF_LINE_LEN]
new g_kscLifeBufferType[KSC_LIFE_BUF_MAX_ENTRIES]
new g_kscLifeBufferOrder[KSC_LIFE_BUF_MAX_ENTRIES]
new g_kscLifeBufferCount = 0
new g_kscLifeDropped = 0
new g_kscBufferOrderNext = 0
new g_kscAttempted[KSC_EVENT_COUNT]
new g_kscEnqueued[KSC_EVENT_COUNT]
new g_kscDroppedByType[KSC_EVENT_COUNT]
new g_kscEmitted[KSC_EVENT_COUNT]
new g_kscSequence = 0
new const g_kscEventNames[KSC_EVENT_COUNT][] = {
	"life", "damage", "position", "frag", "assist", "break",
	"flag_state", "flag_position", "objective_attempt", "team_membership",
	"grenade_entity"
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
new bool:g_kscAllowUntrackedEvents = true

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

// Factual objective-attempt lifecycle from the existing DODX capture-area
// state. attempt_id is the start marker's producer sequence and is scoped by
// that marker's explicit (matchid, half).
new bool:g_kscAttemptActive[KSC_MAX_FLAGS]
new g_kscAttemptId[KSC_MAX_FLAGS]
new g_kscAttemptTeam[KSC_MAX_FLAGS]
new g_kscAttemptOwnerBefore[KSC_MAX_FLAGS]
new g_kscAttemptAllies[KSC_MAX_FLAGS]
new g_kscAttemptAxis[KSC_MAX_FLAGS]
new g_kscAttemptMatchId[KSC_MAX_FLAGS][64]
new g_kscAttemptHalf[KSC_MAX_FLAGS]

// Owner identity must survive a disconnect between the tracked and removed
// forwards. Edict indices are reused, so index+serial is the cache key.
new bool:g_kscGrenadeCacheActive[KSC_GRENADE_CACHE_MAX]
new g_kscGrenadeCacheEntindex[KSC_GRENADE_CACHE_MAX]
new g_kscGrenadeCacheSerial[KSC_GRENADE_CACHE_MAX]
new g_kscGrenadeCacheWeapon[KSC_GRENADE_CACHE_MAX]
new g_kscGrenadeCacheOwner[KSC_GRENADE_CACHE_MAX][96]

new g_kscBreakQ[KSC_MAX_FLAGS][KSC_BREAK_QUEUE_MAX]    // FIFO of candidate killers
new g_kscBreakVictim[KSC_MAX_FLAGS][KSC_BREAK_QUEUE_MAX]
new g_kscBreakTtl[KSC_MAX_FLAGS][KSC_BREAK_QUEUE_MAX]  // polls left before expiry
new g_kscBreakCount[KSC_MAX_FLAGS]
new g_kscBreakTeam[KSC_MAX_FLAGS]   // team being broken, latched with the first candidate
new g_kscBreakBase[KSC_MAX_FLAGS]   // rolling in-zone baseline to measure drops against
new Float:g_kscLastRoundTime = -1.0
new bool:g_kscBreakBaselineSuppressed = true
new g_kscCvarTimelimit

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

stock ksc_next_buffer_order() {
	g_kscBufferOrderNext++
	return g_kscBufferOrderNext
}

stock ksc_reset_health() {
	g_kscSequence = 0
	g_kscBufferOrderNext = 0
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

stock bool:ksc_buffer(const line[], event_type) {
	g_kscAttempted[event_type]++
	if (g_kscBufferCount >= KSC_BUF_MAX_ENTRIES) {
		// Deliberately NOT an inline flush (which is what the stock buffer_log
		// does when full): this runs in postthink, and a synchronous write here
		// is precisely the frame stall the buffering exists to prevent. Drop and
		// count instead -- the task drains within KSC_BUF_FLUSH_SECS, and a
		// non-zero counter in the logs is the signal to raise the size.
		g_kscDropped++
		g_kscDroppedByType[event_type]++
		return false
	}

	copy(g_kscBuffer[g_kscBufferCount], KSC_BUF_LINE_LEN - 1, line)
	g_kscBufferType[g_kscBufferCount] = event_type
	g_kscBufferOrder[g_kscBufferCount] = ksc_next_buffer_order()
	g_kscBufferCount++
	g_kscEnqueued[event_type]++
	return true
}

// Damage/frag/assist/break markers remain useful factual diagnostics when the
// MatchHandler start forward has fired but DODX has not confirmed the same id.
// Keep those explicit matchid="-" / half=0 records on the existing async ring,
// but never assign them a producer sequence or charge them to the pending
// match's health counters. Buffer order is independent from producer sequence
// so an untracked marker cannot reorder a tracked life/data marker at flush.
stock bool:ksc_untracked_buffer(const line[]) {
	if (g_kscBufferCount >= KSC_BUF_MAX_ENTRIES) {
		g_kscDropped++
		return false
	}

	copy(g_kscBuffer[g_kscBufferCount], KSC_BUF_LINE_LEN - 1, line)
	g_kscBufferType[g_kscBufferCount] = KSC_EVENT_UNTRACKED
	g_kscBufferOrder[g_kscBufferCount] = ksc_next_buffer_order()
	g_kscBufferCount++
	return true
}

stock bool:ksc_buffer_event(const line[], event_type, bool:tracked) {
	if (tracked)
		return ksc_buffer(line, event_type)

	return ksc_untracked_buffer(line)
}

stock ksc_normalize_match_half(half) {
	if (half == 1 || half == 2)
		return half
	if (half >= 101 && half <= 353)
		return half - 98
	return 0
}

stock ksc_close_producer_context() {
	// Close cached old-context facts before clearing the candidate. A start can
	// be the only boundary delivered after an abandon/reset, so it must finish a
	// previously confirmed stream rather than silently overwrite its health.
	ksc_objective_reset_all(true)
	ksc_grenade_cache_clear_all()
	ksc_break_reset_boundary()
	ksc_flush()
	if (g_kscProducerContextConfirmed && g_kscProducerMatchId[0] &&
			g_kscProducerHalf != 0)
		ksc_emit_health(g_kscProducerMatchId, g_kscProducerHalf)

	g_kscProducerMatchId[0] = 0
	g_kscProducerHalf = 0
	g_kscProducerContextConfirmed = false
	g_kscAllowUntrackedEvents = false
	g_kscLifeBaselinePending = true
	ksc_clear_open_lives()
	g_kscOwnershipMatchId[0] = 0
	g_kscOwnershipBaselinePending = true
}

stock ksc_on_match_start(const matchid[], half) {
	// A match/half boundary invalidates every observation made against the
	// previous round. Suppress one fresh zone baseline even when the clock is
	// unavailable, so a warmup candidate cannot drain into go-live counts.
	ksc_close_producer_context()

	new normalized_half = ksc_normalize_match_half(half)
	if (!matchid[0] || normalized_half == 0)
		return

	// This is only a candidate until DODX reports the exact same id. Do not
	// reset health, emit a manifest, or publish per-half metadata here: the
	// MatchHandler forward legitimately precedes DODX confirmation by frames.
	copy(g_kscProducerMatchId, charsmax(g_kscProducerMatchId), matchid)
	g_kscProducerHalf = normalized_half
	g_kscProducerContextConfirmed = false
	g_kscAllowUntrackedEvents = true
	g_kscLifeBaselinePending = true
	ksc_clear_open_lives()
	g_kscOwnershipMatchId[0] = 0
	g_kscOwnershipBaselinePending = true
}

stock ksc_on_match_context_end(const matchid[]) {
	if (!g_kscProducerMatchId[0] || !equal(matchid, g_kscProducerMatchId))
		return

	// Never-confirmed candidates have no manifest and therefore no health or
	// authorization record. Their pending sentinel facts still drain in order.
	ksc_close_producer_context()
}

stock ksc_shutdown() {
	// plugin_end is the last reliable old-map point in the Pawn lifecycle.
	// Close attempts while their cached context is still available, then drain.
	ksc_objective_reset_all(true)
	ksc_grenade_cache_clear_all()
	ksc_flush()
}

stock ksc_activate_producer_context() {
	// Pending matchid="-" facts are transport diagnostics, not members of the
	// candidate stream. Drain them before sequence 1 so no buffered sentinel can
	// appear after the authorization manifest on the wire.
	ksc_flush()
	ksc_reset_health()
	g_kscProducerContextConfirmed = true
	g_kscAllowUntrackedEvents = false
	g_kscLifeBaselinePending = true
	g_kscOwnershipMatchId[0] = 0
	g_kscOwnershipBaselinePending = true
	ksc_emit_manifest(g_kscProducerMatchId, g_kscProducerHalf)
}

stock bool:ksc_event_context(matchid[], len, &half) {
	// `-` is an explicit non-context sentinel for the optional damage/frag/
	// assist/break fields. Never emit an empty quoted property: older parsers
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
			ksc_close_producer_context()
		}
		return false
	}

	if (!g_kscProducerContextConfirmed)
		ksc_activate_producer_context()
	copy(matchid, len, g_kscProducerMatchId)
	half = g_kscProducerHalf
	return true
}

// Optional high-volume diagnostics retain a sentinel only while warmup or a
// new candidate is still pending. Once a stream activates, an authoritative
// mismatch closes it and later facts fail closed instead of reverting to '-'.
stock bool:ksc_optional_event_context(matchid[], len, &half, &bool:untracked) {
	if (ksc_event_context(matchid, len, half)) {
		untracked = false
		return true
	}

	untracked = g_kscAllowUntrackedEvents
	return false
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
	g_kscLifeBufferOrder[g_kscLifeBufferCount] = ksc_next_buffer_order()
	g_kscLifeBufferCount++
	g_kscEnqueued[KSC_EVENT_LIFE]++
	return true
}

public ksc_flush_task() {
	ksc_flush()
}

stock ksc_flush() {
	// Both capacity pools share one enqueue order. Merge their already ordered
	// contents so neither queue can jump ahead of the other on the wire. This is
	// deliberately separate from producer sequence: untracked sentinel records
	// have sequence=0 and do not belong to a match health stream.
	new data_i = 0, life_i = 0
	while (data_i < g_kscBufferCount || life_i < g_kscLifeBufferCount) {
		if (life_i < g_kscLifeBufferCount &&
			(data_i >= g_kscBufferCount || g_kscLifeBufferOrder[life_i] < g_kscBufferOrder[data_i])) {
			log_message("%s", g_kscLifeBuffer[life_i])
			g_kscEmitted[g_kscLifeBufferType[life_i]]++
			life_i++
		} else {
			log_message("%s", g_kscBuffer[data_i])
			if (g_kscBufferType[data_i] >= 0 && g_kscBufferType[data_i] < KSC_EVENT_COUNT)
				g_kscEmitted[g_kscBufferType[data_i]]++
			data_i++
		}
	}

	g_kscBufferCount = 0
	g_kscLifeBufferCount = 0

	if (g_kscDropped > 0) {
		log_amx("[KTP-STATS] dropped %d capture event(s) -- buffer/tracker capacity full", g_kscDropped)
		g_kscDropped = 0
	}
	if (g_kscLifeDropped > 0) {
		log_amx("[KTP-STATS] dropped %d LIFE boundary line(s) -- dedicated buffer full", g_kscLifeDropped)
		g_kscLifeDropped = 0
	}
}

// Quoted marker properties are parsed structurally, not as arbitrary strings.
// Replace every byte that can close/inject a property (quote, backslash, either
// parenthesis, CR/LF/control/DEL) with a visible underscore. This is deliberately
// one byte in/one byte out: it cannot expand past a caller's bounded buffer.
// Printable UTF-8 bytes and durable auth/userid punctuation remain unchanged.
stock ksc_wire_sanitize(const input[], output[], len) {
	new out_pos = 0
	for (new in_pos = 0; input[in_pos] && out_pos < len; in_pos++) {
		new ch = input[in_pos]
		if (ch < 32 || ch == 34 || ch == 40 || ch == 41 || ch == 92 || ch == 127)
			output[out_pos++] = '_'
		else
			output[out_pos++] = ch
	}
	output[out_pos] = 0
}

// Player names are the only untrusted component placed before the structural
// `<userid><auth><team>` delimiters. Strip angle brackets here, before assembly,
// so a crafted name cannot make the daemon's non-greedy identity parser select
// attacker-supplied userid/auth/team fields. Other protocol delimiters receive
// the same treatment as every quoted marker string.
stock ksc_player_name_sanitize(const input[], output[], len) {
	new out_pos = 0
	for (new in_pos = 0; input[in_pos] && out_pos < len; in_pos++) {
		new ch = input[in_pos]
		if (ch < 32 || ch == 34 || ch == 40 || ch == 41 || ch == 60 ||
			ch == 62 || ch == 92 || ch == 127)
			output[out_pos++] = '_'
		else
			output[out_pos++] = ch
	}
	output[out_pos] = 0
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

	new safe_name[MAX_NAME_LENGTH], safe_authid[32], safe_team[16]
	ksc_player_name_sanitize(name, safe_name, charsmax(safe_name))
	ksc_wire_sanitize(authid, safe_authid, charsmax(safe_authid))
	ksc_wire_sanitize(team, safe_team, charsmax(safe_team))
	new raw[96]
	formatex(raw, charsmax(raw), "%s<%d><%s><%s>",
		safe_name, get_user_userid(id), safe_authid, safe_team)
	ksc_wire_sanitize(raw, out, len)
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

// ---------------------------------------------------------------------------
// Factual grenade-entity lifecycle
//
// `removed` is intentionally generic. ED_Free proves the entity is being
// freed; it does not prove an explosion. The old dod_grenade_explosion forward
// remains a legacy damage-attribution/practice hook and is never consumed here
// as a detonation signal. Rockets, mortar entities, and every weapon outside
// 13/14/36 are rejected by both DODX and this defensive consumer gate.

stock bool:ksc_grenade_weapon_valid(wpnid) {
	return wpnid == 13 || wpnid == 14 || wpnid == 36
}

stock ksc_grenade_weapon_type(wpnid, out[], len) {
	switch (wpnid) {
		case 13: copy(out, len, "handgrenade")
		case 14: copy(out, len, "stickgrenade")
		case 36: copy(out, len, "mills_bomb")
		default: out[0] = 0
	}
}

stock ksc_grenade_cache_clear(slot) {
	g_kscGrenadeCacheActive[slot] = false
	g_kscGrenadeCacheEntindex[slot] = 0
	g_kscGrenadeCacheSerial[slot] = 0
	g_kscGrenadeCacheWeapon[slot] = 0
	g_kscGrenadeCacheOwner[slot][0] = 0
}

stock ksc_grenade_cache_clear_all() {
	for (new slot = 0; slot < KSC_GRENADE_CACHE_MAX; slot++)
		ksc_grenade_cache_clear(slot)
}

stock ksc_grenade_cache_find(entindex, serial) {
	for (new slot = 0; slot < KSC_GRENADE_CACHE_MAX; slot++) {
		if (g_kscGrenadeCacheActive[slot] &&
			g_kscGrenadeCacheEntindex[slot] == entindex &&
			g_kscGrenadeCacheSerial[slot] == serial)
			return slot
	}
	return -1
}

stock ksc_grenade_cache_allocate() {
	for (new slot = 0; slot < KSC_GRENADE_CACHE_MAX; slot++) {
		if (!g_kscGrenadeCacheActive[slot])
			return slot
	}
	return -1
}

stock ksc_grenade_record_drop() {
	g_kscAttempted[KSC_EVENT_GRENADE_ENTITY]++
	g_kscDroppedByType[KSC_EVENT_GRENADE_ENTITY]++
	g_kscDropped++
}

stock bool:ksc_emit_grenade_entity(const kind[], entindex, serial, wpnid,
		const owner_str[], const Float:pos[3], Float:game_time) {
	new matchid[64], half
	if (!ksc_event_context(matchid, charsmax(matchid), half))
		return false

	new weapon_type[20]
	ksc_grenade_weapon_type(wpnid, weapon_type, charsmax(weapon_type))
	if (!weapon_type[0])
		return false

	new mapname[32], position[40], line[KSC_BUF_LINE_LEN]
	get_mapname(mapname, charsmax(mapname))
	formatex(position, charsmax(position), "%d %d %d",
		floatround(pos[0]), floatround(pos[1]), floatround(pos[2]))
	new wire_kind[16], wire_matchid[64], wire_map[32]
	new wire_weapon_type[20], wire_owner[96]
	ksc_wire_sanitize(kind, wire_kind, charsmax(wire_kind))
	ksc_wire_sanitize(matchid, wire_matchid, charsmax(wire_matchid))
	ksc_wire_sanitize(mapname, wire_map, charsmax(wire_map))
	ksc_wire_sanitize(weapon_type, wire_weapon_type, charsmax(wire_weapon_type))
	ksc_wire_sanitize(owner_str, wire_owner, charsmax(wire_owner))
	new sequence = ksc_next_sequence()

	formatex(line, charsmax(line),
		"KTP_GRENADE_ENTITY (kind ^"%s^") (matchid ^"%s^") (half ^"%d^") (map ^"%s^") (entindex ^"%d^") (serial ^"%d^") (weapon_id ^"%d^") (weapon_type ^"%s^") (owner ^"%s^") (position ^"%s^") (game_time ^"%.2f^") (event_epoch ^"%d^") (sequence ^"%d^")",
		wire_kind, wire_matchid, half, wire_map, entindex, serial, wpnid,
		wire_weapon_type, wire_owner, position, game_time, get_systime(), sequence)

	return ksc_buffer(line, KSC_EVENT_GRENADE_ENTITY)
}

public dod_grenade_entity_tracked(owner, entindex, serial, Float:pos[3],
		wpnid, Float:gametime) {
	if (!ksc_enabled() || !ksc_grenade_weapon_valid(wpnid))
		return
	if (owner < 1 || owner > MAX_PLAYERS || !is_user_connected(owner))
		return
	if (entindex <= 0 || serial <= 0)
		return

	// Same entity can issue many TraceLines. DODX dedupes at source; retain the
	// exact key guard here so direct-dispatch tests and future module changes
	// cannot create duplicate starts.
	if (ksc_grenade_cache_find(entindex, serial) >= 0)
		return

	new slot = ksc_grenade_cache_allocate()
	if (slot < 0) {
		ksc_grenade_record_drop()
		return
	}

	g_kscGrenadeCacheActive[slot] = true
	g_kscGrenadeCacheEntindex[slot] = entindex
	g_kscGrenadeCacheSerial[slot] = serial
	g_kscGrenadeCacheWeapon[slot] = wpnid
	ksc_player_str(owner, g_kscGrenadeCacheOwner[slot],
		charsmax(g_kscGrenadeCacheOwner[]))

	if (!ksc_emit_grenade_entity("tracked", entindex, serial, wpnid,
			g_kscGrenadeCacheOwner[slot], pos, gametime))
		ksc_grenade_cache_clear(slot)
}

public dod_grenade_entity_tracker_drop(owner, entindex, serial, wpnid,
		Float:gametime) {
	if (!ksc_enabled() || !ksc_grenade_weapon_valid(wpnid))
		return
	if (owner < 1 || owner > MAX_PLAYERS || entindex <= 0 || serial <= 0)
		return

	// Account only while an authoritative producer context is live; otherwise
	// there is no half whose health record could truthfully own this loss.
	new matchid[64], half
	if (!ksc_event_context(matchid, charsmax(matchid), half))
		return
	ksc_grenade_record_drop()
}

public dod_grenade_entity_removed(owner, entindex, serial, Float:pos[3],
		wpnid, Float:gametime) {
	new slot = ksc_grenade_cache_find(entindex, serial)
	if (slot < 0)
		return
	if (!ksc_enabled()) {
		ksc_grenade_cache_clear(slot)
		return
	}

	// Ignore the live owner slot and callback weapon on removal. A disconnect or
	// slot reuse may have changed them; the tracked identity is authoritative.
	ksc_emit_grenade_entity("removed", entindex, serial,
		g_kscGrenadeCacheWeapon[slot], g_kscGrenadeCacheOwner[slot], pos, gametime)
	ksc_grenade_cache_clear(slot)
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

// DODX supplies this at the game-DLL's actual team transition. Do not infer a
// side from a later position sample: death/spectator gaps would otherwise make
// the derived interval claim a team the player no longer belonged to.
stock ksc_emit_team_membership(id, team, oldteam) {
	g_kscAttempted[KSC_EVENT_TEAM_MEMBERSHIP]++
	if (!ksc_enabled() || id < 1 || id > MAX_PLAYERS) {
		g_kscDroppedByType[KSC_EVENT_TEAM_MEMBERSHIP]++
		return
	}
	if (team == oldteam) {
		g_kscDroppedByType[KSC_EVENT_TEAM_MEMBERSHIP]++
		return
	}
	// DODX has a transient spectator value 3; the private protocol uses 0 for
	// all unassigned/non-playing states and only 1/2 for playing teams.
	if (team != 1 && team != 2)
		team = 0
	if (oldteam != 1 && oldteam != 2)
		oldteam = 0
	if (team == oldteam) {
		g_kscDroppedByType[KSC_EVENT_TEAM_MEMBERSHIP]++
		return
	}

	new matchid[64], half
	if (!ksc_event_context(matchid, charsmax(matchid), half)) {
		g_kscDroppedByType[KSC_EVENT_TEAM_MEMBERSHIP]++
		return
	}
	new player_str[96]
	ksc_player_str(id, player_str, charsmax(player_str))
	new sequence = ksc_next_sequence()
	new Float:game_time = get_gametime()
	new line[KSC_BUF_LINE_LEN]
	formatex(line, charsmax(line),
		"^\"%s^\" triggered ^\"team_membership^\" (matchid ^\"%s^\") (half ^\"%d^\") (team ^\"%d^\") (old_team ^\"%d^\") (game_time ^\"%.2f^\") (event_epoch ^\"%d^\") (sequence ^\"%d^\")",
		player_str, matchid, half, team, oldteam, game_time, get_systime(), sequence)
	ksc_buffer(line, KSC_EVENT_TEAM_MEMBERSHIP)
}

public dod_client_changeteam(id, team, oldteam) {
	ksc_emit_team_membership(id, team, oldteam)
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

// ---------------------------------------------------------------------------
// Factual objective-attempt lifecycle
//
// This is deliberately a state ledger, not a judgement. DODX exposes only
// aggregate team occupancy, so there is no participant list and no attempt to
// infer who walked off or was killed. `capture_stopped` means only that the
// area stopped reporting the same active capture. `context_reset` is reserved
// for a producer/map boundary for which the old match/half was cached safely.

stock ksc_objective_clear(f) {
	g_kscAttemptActive[f] = false
	g_kscAttemptId[f] = 0
	g_kscAttemptTeam[f] = 0
	g_kscAttemptOwnerBefore[f] = 0
	g_kscAttemptAllies[f] = 0
	g_kscAttemptAxis[f] = 0
	g_kscAttemptMatchId[f][0] = 0
	g_kscAttemptHalf[f] = 0
}

stock bool:ksc_emit_objective_attempt(const kind[], f, attempt_id,
		capturing_team, owner_before, allies_in_zone, axis_in_zone,
		const stop_reason[], const matchid[], half, &event_sequence) {
	if (!matchid[0] || half <= 0)
		return false

	new mapname[32], line[KSC_BUF_LINE_LEN]
	get_mapname(mapname, charsmax(mapname))
	new wire_kind[16], wire_matchid[64], wire_map[32]
	new wire_flag_name[32], wire_stop_reason[24]
	ksc_wire_sanitize(kind, wire_kind, charsmax(wire_kind))
	ksc_wire_sanitize(matchid, wire_matchid, charsmax(wire_matchid))
	ksc_wire_sanitize(mapname, wire_map, charsmax(wire_map))
	ksc_wire_sanitize(g_kscFlagName[f], wire_flag_name, charsmax(wire_flag_name))
	ksc_wire_sanitize(stop_reason, wire_stop_reason, charsmax(wire_stop_reason))
	event_sequence = ksc_next_sequence()
	new wire_attempt_id = attempt_id ? attempt_id : event_sequence

	formatex(line, charsmax(line),
		"KTP_OBJECTIVE_ATTEMPT (kind ^"%s^") (matchid ^"%s^") (half ^"%d^") (map ^"%s^") (attempt_id ^"%d^") (flag_index ^"%d^") (flag_name ^"%s^") (capturing_team ^"%d^") (owner_before ^"%d^") (allies_in_zone ^"%d^") (axis_in_zone ^"%d^") (stop_reason ^"%s^") (game_time ^"%.2f^") (event_epoch ^"%d^") (sequence ^"%d^")",
		wire_kind, wire_matchid, half, wire_map, wire_attempt_id, f,
		wire_flag_name, capturing_team, owner_before,
		allies_in_zone, axis_in_zone, wire_stop_reason,
		get_gametime(), get_systime(), event_sequence)

	return ksc_buffer(line, KSC_EVENT_OBJECTIVE_ATTEMPT)
}

stock ksc_objective_start(f, team, owner, allies, axis,
		const matchid[], half) {
	new sequence = 0
	if (!ksc_emit_objective_attempt("start", f, 0, team, owner,
			allies, axis, "", matchid, half, sequence))
		return

	g_kscAttemptActive[f] = true
	g_kscAttemptId[f] = sequence
	g_kscAttemptTeam[f] = team
	g_kscAttemptOwnerBefore[f] = owner
	g_kscAttemptAllies[f] = allies
	g_kscAttemptAxis[f] = axis
	copy(g_kscAttemptMatchId[f], charsmax(g_kscAttemptMatchId[]), matchid)
	g_kscAttemptHalf[f] = half
}

stock bool:ksc_objective_finish(f, const kind[], const stop_reason[], allies, axis) {
	if (!g_kscAttemptActive[f])
		return true

	new sequence = 0
	if (!ksc_emit_objective_attempt(kind, f, g_kscAttemptId[f],
			g_kscAttemptTeam[f], g_kscAttemptOwnerBefore[f], allies, axis,
			stop_reason, g_kscAttemptMatchId[f], g_kscAttemptHalf[f], sequence))
		return false

	ksc_objective_clear(f)
	return true
}

stock ksc_objective_reset_all(bool:emit_context_reset) {
	for (new f = 0; f < KSC_MAX_FLAGS; f++) {
		if (!g_kscAttemptActive[f])
			continue

		// An open attempt always cached the context that admitted its start.
		// Map-load init can run before log_message is reliable, so its caller
		// requests a silent reset instead of fabricating a terminal marker.
		if (emit_context_reset) {
			new sequence = 0
			ksc_emit_objective_attempt("stop", f, g_kscAttemptId[f],
				g_kscAttemptTeam[f], g_kscAttemptOwnerBefore[f],
				g_kscAttemptAllies[f], g_kscAttemptAxis[f], "context_reset",
				g_kscAttemptMatchId[f], g_kscAttemptHalf[f], sequence)
		}

		// A boundary is terminal even when the bounded buffer reports a drop;
		// health exposes that loss and stale state must never cross contexts.
		ksc_objective_clear(f)
	}
}

stock ksc_objective_observe(f, owner, is_capping, capturing_team, allies, axis) {
	new matchid[64], half
	new bool:have_context = ksc_event_context(matchid, charsmax(matchid), half)
	new active_occupancy = (capturing_team == KSC_TEAM_ALLIES) ? allies : axis
	new bool:valid_capture = have_context && is_capping &&
		(capturing_team == KSC_TEAM_ALLIES || capturing_team == KSC_TEAM_AXIS) &&
		capturing_team != owner && active_occupancy > 0

	if (!g_kscAttemptActive[f]) {
		if (valid_capture)
			ksc_objective_start(f, capturing_team, owner, allies, axis, matchid, half)
		return
	}

	g_kscAttemptAllies[f] = allies
	g_kscAttemptAxis[f] = axis

	// Completion is intentionally narrow: only an ownership change to the team
	// latched by the start event qualifies.
	if (owner == g_kscAttemptTeam[f] && owner != g_kscAttemptOwnerBefore[f]) {
		ksc_objective_finish(f, "complete", "", allies, axis)
		return
	}

	if (!have_context) {
		// ksc_event_context already closes reliably stamped old context on a
		// confirmed mismatch. A never-confirmed/unstampable state is cleared
		// silently rather than assigning it to the new context.
		ksc_objective_clear(f)
		return
	}

	if (!valid_capture) {
		// An unrelated ownership flip is a reset, not evidence that the active
		// team simply stopped capturing.
		if (owner != g_kscAttemptOwnerBefore[f])
			ksc_objective_finish(f, "stop", "context_reset", allies, axis)
		else
			ksc_objective_finish(f, "stop", "capture_stopped", allies, axis)
		return
	}

	if (capturing_team != g_kscAttemptTeam[f]) {
		// Team flip is two factual edges in order: stop old, then start new.
		if (ksc_objective_finish(f, "stop", "capture_stopped", allies, axis))
			ksc_objective_start(f, capturing_team, owner, allies, axis, matchid, half)
	}
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
	new producer_matchid[64], producer_half
	new bool:untracked
	new bool:tracked = ksc_optional_event_context(
		producer_matchid, charsmax(producer_matchid), producer_half, untracked)
	if (!tracked && !untracked)
		return
	new sequence = tracked ? ksc_next_sequence() : 0
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

	ksc_buffer_event(line, KSC_EVENT_BREAK, tracked)
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
	new bool:untracked
	new bool:tracked = ksc_optional_event_context(
		producer_matchid, charsmax(producer_matchid), producer_half, untracked)
	if (!tracked && !untracked)
		return
	new sequence = tracked ? ksc_next_sequence() : 0

	formatex(line, charsmax(line),
		"^"%s^" triggered ^"break_context^" (victim ^"%s^") (flag ^"%s^") (flag_index ^"%d^") (contester_count ^"%d^") (time_remaining ^"%.1f^") (is_capout ^"%d^") (matchid ^"%s^") (half ^"%d^") (sequence ^"%d^") (event_epoch ^"%d^") (game_time ^"%.2f^")",
		breaker_str, victim_str, g_kscFlagName[flag_index], flag_index, contester_count,
		time_remaining, is_capout, producer_matchid, producer_half, sequence,
		get_systime(), get_gametime())

	ksc_buffer_event(line, KSC_EVENT_BREAK, tracked)
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
		ksc_objective_reset_all(false)
		ksc_grenade_cache_clear_all()
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
	if (suppress_breaks) {
		ksc_break_clear_all()
		// A restart/countdown invalidates the old capture-area state just as
		// authoritatively as it invalidates break candidates. Close any active
		// attempt against its cached old context, then suppress all lifecycle
		// starts/completions until the fresh-baseline poll has been consumed.
		ksc_objective_reset_all(true)
	}

	for (new f = 0; f < g_kscFlagCount; f++) {
		new ac = dodx_area_get_data(f, CA_num_allies)
		new xc = dodx_area_get_data(f, CA_num_axis)
		if (ac < 0) ac = 0
		if (xc < 0) xc = 0
		if (ac > MAX_PLAYERS) ac = MAX_PLAYERS
		if (xc > MAX_PLAYERS) xc = MAX_PLAYERS

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
		new capturing_team = is_capping ? dodx_area_get_data(f, CA_capturing_team) : 0
		g_kscCapTeam[f] = capturing_team
		if (!suppress_breaks)
			ksc_objective_observe(f, owner, is_capping, capturing_team, ac, xc)

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
	// This forward fires too early for reliable log_message output. Normal
	// plugin/match teardown already emitted context_reset where possible; clear
	// any residual state silently rather than stamping it onto the new map.
	ksc_objective_reset_all(false)
	ksc_grenade_cache_clear_all()
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
		new raw_flag_name[32]
		dodx_objective_get_data(f, CP_name, raw_flag_name, charsmax(raw_flag_name))
		ksc_wire_sanitize(raw_flag_name, g_kscFlagName[f], charsmax(g_kscFlagName[]))
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

	new bool:had_candidate = g_kscProducerMatchId[0] && g_kscProducerHalf != 0
	new matchid[64], half, sequence = 0
	new bool:tracked = ksc_event_context(matchid, charsmax(matchid), half)
	if (!tracked) {
		// Map-load coordinates remain useful global metadata before any candidate.
		// Once a MatchHandler candidate exists, however, no flag-position marker
		// may leak before exact DODX equality activates that half.
		if (had_candidate || !g_kscAllowUntrackedEvents)
			return
		copy(matchid, charsmax(matchid), "-")
	} else {
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
	new bool:untracked
	new bool:tracked = ksc_optional_event_context(
		producer_matchid, charsmax(producer_matchid), producer_half, untracked)
	if (!tracked && !untracked)
		return
	new sequence = tracked ? ksc_next_sequence() : 0

	new line[KSC_BUF_LINE_LEN]
	formatex(line, charsmax(line),
		"^"%s^" triggered ^"damage^" against ^"%s^" with ^"%s^" (damage ^"%d^") (damage_capped ^"%d^") (hitplace ^"%d^") (matchid ^"%s^") (half ^"%d^") (game_time ^"%.2f^") (event_epoch ^"%d^") (sequence ^"%d^")",
		attacker_str, victim_str, weapon,
		damage, damage_capped, hitplace, producer_matchid, producer_half, gt, event_epoch, sequence)

	ksc_buffer_event(line, KSC_EVENT_DAMAGE, tracked)
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
	new bool:untracked
	new bool:tracked = ksc_optional_event_context(
		producer_matchid, charsmax(producer_matchid), producer_half, untracked)
	if (!tracked && !untracked)
		return
	new sequence = tracked ? ksc_next_sequence() : 0

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

	ksc_buffer_event(line, KSC_EVENT_FRAG, tracked)
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
		new bool:untracked
		new bool:tracked = ksc_optional_event_context(
			producer_matchid, charsmax(producer_matchid), producer_half, untracked)
		if (!tracked && !untracked) {
			ksc_clear_damage_row(victim)
			return
		}
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
			new sequence = tracked ? ksc_next_sequence() : 0

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

			ksc_buffer_event(line, KSC_EVENT_ASSIST, tracked)
		}
	}

	ksc_clear_damage_row(victim)
}
