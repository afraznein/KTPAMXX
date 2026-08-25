#!/usr/bin/env python3
"""Deterministic source-contract tests for DoD stats capture.

The Pawn plugin needs a running DoD server for behavioral execution. These
tests cover the failure-prone integration contract locally and in KTPAMXX CI:
wire fields, warmup/round-live semantics, event-hook placement, transition
baseline ordering, restart fail-safety, teamkill routing, flush-before-context-
clear ordering, and the narrow extension-mode game-DLL resolver used by Lane
clear ordering, objective-topology fail-safety, and the narrow extension-mode
game-DLL resolver used by Lane B's split loader. KTPInfrastructure's Lane A/B tests remain responsible for
runtime forward dispatch, ingestion, and behavioral gamerules/clock preflight.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
STATS = (ROOT / "plugins" / "dod" / "stats_logging.sma").read_text(encoding="utf-8")
CAPTURE = (ROOT / "plugins" / "dod" / "ktp_stats_capture.inc").read_text(encoding="utf-8")
GAMECONFIGS = (ROOT / "amxmodx" / "CGameConfigs.cpp").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    """Return a Pawn function body, retaining nested blocks."""
    start = source.find(signature)
    assert start >= 0, f"missing function: {signature}"
    brace = source.find("{", start)
    assert brace >= 0, f"missing opening brace: {signature}"

    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"missing closing brace: {signature}")


def before(body: str, earlier: str, later: str) -> None:
    first = body.find(earlier)
    second = body.find(later)
    assert first >= 0, f"missing expected statement: {earlier}"
    assert second >= 0, f"missing expected statement: {later}"
    assert first < second, f"expected {earlier!r} before {later!r}"


def test_wire_contract() -> None:
    emit = function_body(CAPTURE, "stock bool:ksc_emit_life_boundary")
    assert 'triggered ^"life_boundary^"' in emit
    for field in (
        "matchid",
        "half",
        "event_epoch",
        "game_time",
        "kind",
        "reason",
        "team",
        "class",
        "slot",
    ):
        assert f'({field} ^"%' in emit, f"wire field missing: {field}"

    assert "get_systime()" in emit
    assert "get_gametime()" in emit
    assert "round_live" not in emit
    assert 'get_cvar_num("dodstats_pause")' not in emit
    assert "if (!ksc_enabled() || !matchid[0])" in emit
    assert "dod_get_user_class(id)" in emit


def test_wire_line_fits_capture_buffer() -> None:
    # ksc_player_str itself caps at 95 characters and DODX caps match ids at
    # 63. Use maximum-width signed numeric values as well; truncating this line
    # would silently make the daemon regex stop matching trailing properties.
    line = (
        f'"{"P" * 95}" triggered "life_boundary" '
        f'(matchid "{"M" * 63}") (half "255") (event_epoch "-2147483648") '
        f'(game_time "-2147483648.00") '
        f'(kind "start") (reason "context_live") (team "-2147483648") '
        f'(class "-2147483648") (slot "32")'
    )
    capture_buffer = re.search(r"#define\s+KSC_LIFE_BUF_LINE_LEN\s+(\d+)", CAPTURE)
    assert capture_buffer, "missing KSC_LIFE_BUF_LINE_LEN"
    assert len(line) < int(capture_buffer.group(1)), (
        f"worst-case life marker is {len(line)} bytes, buffer is "
        f"{capture_buffer.group(1)}"
    )


def test_clocked_frag_and_assist_lines_fit_general_buffer() -> None:
    player = "P" * 95
    victim = "V" * 95
    weapon = "W" * 31
    number = "-2147483648"
    position = f"{number} {number} {number}"
    matchid = "M" * 63
    frag = (
        f'"{player}" triggered "frag_context" against "{victim}" with "{weapon}" '
        + " ".join(
            f'({field} "{number}")'
            for field in (
                "headshot", "k_prone", "v_prone", "k_scope", "v_scope",
                "k_clip", "k_ammo", "v_clip", "v_ammo",
            )
        )
        + f' (k_position "{position}") (v_position "{position}") '
        + f'(is_last_flag_defense "{number}") (matchid "{matchid}") '
        + f'(half "255") (game_time "{number}.00") (event_epoch "2147483647")'
    )
    assist = (
        f'"{player}" triggered "assist" against "{victim}" '
        f'(assister_position "{position}") (victim_position "{position}") '
        f'(matchid "{matchid}") (half "255") (event_epoch "2147483647") '
        f'(game_time "{number}.00")'
    )
    general = re.search(r"#define\s+KSC_BUF_LINE_LEN\s+(\d+)", CAPTURE)
    assert general
    size = int(general.group(1))
    assert len(frag) < size, (len(frag), size)
    assert len(assist) < size, (len(assist), size)


def test_allowed_boundary_pairs() -> None:
    assert 'ksc_life_start(id, "spawn")' in CAPTURE
    assert 'ksc_emit_life_boundary(id, matchid, "start", "context_live")' in CAPTURE
    assert 'ksc_life_end(victim, "death")' in CAPTURE


def test_non_playing_team_is_normalized_for_wire_contract() -> None:
    body = function_body(CAPTURE, "ksc_emit_life_boundary")
    assert "if (team != 1 && team != 2)" in body
    assert "team = 0" in body
    assert 'ksc_emit_life_boundary(id, matchid, "end", "disconnect")' in CAPTURE


def test_spawn_and_death_ordering() -> None:
    spawn = function_body(CAPTURE, "public dod_client_spawn")
    before(spawn, "ksc_clear_damage_row(id)", 'ksc_life_start(id, "spawn")')

    death = function_body(CAPTURE, "stock ksc_on_death")
    before(death, 'ksc_life_end(victim, "death")', "ksc_queue_break_candidate")
    before(death, 'ksc_life_end(victim, "death")', "if (ksc_enabled())")


def test_disconnect_precedes_slot_clear_and_pause_gate() -> None:
    disconnected = function_body(STATS, "public client_disconnected")
    before(disconnected, "ksc_on_disconnect(id)", "ksc_clear_player(id)")
    before(disconnected, "ksc_on_disconnect(id)", "if ( is_user_bot(id) || !isDSMActive() )")


def test_disconnect_purges_break_queue_by_zeroing() -> None:
    # A queued candidate outlives its killer by up to KSC_BREAK_WINDOW polls,
    # and slots recycle. The purge must ZERO matching killer/victim entries,
    # never remove them: the victim's delayed zone decrement still consumes a
    # queue slot in FIFO order, so removal would shift that drop onto the next
    # queued candidate. Zeroed entries are inert in both emitters (range guard
    # in ksc_emit_break; "-" victim in ksc_emit_break_context).
    purge = function_body(CAPTURE, "stock ksc_break_purge_player")
    assert "g_kscBreakQ[f][q] = 0" in purge
    assert "g_kscBreakVictim[f][q] = 0" in purge
    assert "g_kscBreakCount[f]--" not in purge  # zero, never compact
    assert "ksc_break_shift" not in purge

    clear = function_body(CAPTURE, "stock ksc_clear_player")
    assert "ksc_break_purge_player(id)" in clear

    emit = function_body(CAPTURE, "stock ksc_emit_break")
    before(emit, "if (breaker < 1 || breaker > MAX_PLAYERS)",
           "ksc_player_str(breaker")
    context = function_body(CAPTURE, "stock ksc_emit_break_context")
    assert 'copy(victim_str, charsmax(victim_str), "-")' in context


def test_context_baseline_precedes_objective_early_return() -> None:
    poll = function_body(CAPTURE, "public ksc_zone_poll_task")
    before(poll, "ksc_sync_life_context()", "if (!ksc_enabled())")
    before(poll, "ksc_sync_life_context()", "if (g_kscFlagCount <= 0)")

    sync = function_body(CAPTURE, "stock ksc_sync_life_context")
    assert "!is_user_connected(id) || !is_user_alive(id)" in sync
    before(sync, "ksc_event_context(checked_matchid", "if (!matchid[0]")
    before(sync, "if (g_kscLifeOpen[id])", '"start", "context_live"')
    assert '"start", "context_live"' in sync
    assert "g_kscLifeBaselinePending = !all_queued" in sync
    assert "else\n\t\t\tall_queued = false" in sync

    observe = function_body(CAPTURE, "stock ksc_observe_life_context")
    assert "if (!matchid[0])" in observe
    assert "g_kscLifeMatchId[0] = 0" in observe
    assert "g_kscLifeBaselinePending = true" in observe
    assert "if (equal(matchid, g_kscLifeMatchId))" in observe


def test_capture_flush_precedes_every_stats_flush_gate() -> None:
    flush = function_body(STATS, "public dod_stats_flush")
    before(flush, "ksc_flush()", "if ( !is_user_connected(id) || !isDSMActive() )")
    before(flush, "ksc_flush()", "if ( is_user_bot(id) )")


def test_life_boundaries_have_truthful_ordered_queue() -> None:
    life_queue = function_body(CAPTURE, "stock bool:ksc_life_buffer")
    assert "KSC_LIFE_BUF_MAX_ENTRIES" in life_queue
    assert "g_kscLifeDropped++" in life_queue
    assert "return false" in life_queue
    assert "return true" in life_queue

    emit = function_body(CAPTURE, "stock bool:ksc_emit_life_boundary")
    assert "return ksc_life_buffer(line)" in emit
    assert "ksc_buffer(line)" not in emit

    flush = function_body(CAPTURE, "stock ksc_flush")
    assert "g_kscLifeBufferSequence[life_i] < g_kscBufferSequence[data_i]" in flush
    assert "while (data_i < g_kscBufferCount || life_i < g_kscLifeBufferCount)" in flush
    assert "g_kscLifeBuffer[life_i]" in flush
    assert "g_kscBuffer[data_i]" in flush
    assert "dropped %d LIFE boundary" in flush


def test_authoritative_producer_context_and_clocks() -> None:
    assert "public ktp_match_start" in STATS
    assert "public ktp_half_end" in STATS
    assert "public ktp_match_end" in STATS
    normalize = function_body(CAPTURE, "stock ksc_normalize_match_half")
    assert "half == 1 || half == 2" in normalize
    assert "half - 98" in normalize  # MatchHandler 101=OT1 -> DB half 3.
    context = function_body(CAPTURE, "stock bool:ksc_event_context")
    assert "dodx_get_match_id" in context
    assert "!equal(dodx_matchid, g_kscProducerMatchId)" in context
    confirmed_start = context.index("if (g_kscProducerContextConfirmed)")
    confirmed_end = context.index("\t\treturn false", confirmed_start)
    confirmed_invalidation = context[confirmed_start:confirmed_end]
    assert "ksc_break_reset_boundary()" in confirmed_invalidation
    before(confirmed_invalidation, "ksc_break_reset_boundary()",
           "g_kscProducerMatchId[0] = 0")
    assert "g_kscProducerMatchId[0] = 0" in context
    assert 'copy(matchid, len, "-")' in context
    assert "matchid[0] = 0" not in context

    for signature in (
        "stock ksc_emit_damage",
        "stock ksc_emit_frag_context",
        "stock ksc_on_death",
    ):
        body = function_body(CAPTURE, signature)
        for field in ("matchid", "half", "event_epoch", "game_time"):
            assert field in body, f"{signature} missing producer field {field}"


def test_plugin_end_drains_private_capture() -> None:
    plugin_end = function_body(STATS, "public plugin_end")
    assert "ksc_flush()" in plugin_end


def _round_clock_model(previous: float, current: float, limit: float,
                       pending: bool) -> tuple[bool, float, bool]:
    """Executable truth table for the Pawn round-clock suppression contract."""
    if current < 0.0:
        return True, -1.0, True

    countdown = limit > 0.0 and current > limit
    rebased = previous >= 0.0 and current > previous + 0.01
    pending = pending or rebased or countdown
    suppress_now = pending
    if pending and not countdown:
        pending = False
    return suppress_now, current, pending


def test_round_clock_rebase_algorithm() -> None:
    # Ordinary countdown samples only decrease and must not discard candidates.
    assert _round_clock_model(900.0, 899.5, 1800.0, False) == (
        False, 899.5, False)

    # An upward engine rebase suppresses the current baseline even when the
    # restart completes too quickly to expose an above-limit countdown sample.
    assert _round_clock_model(899.5, 1800.0, 1800.0, False) == (
        True, 1800.0, False)

    # The underlying clock is monotone between engine rebases. Keep only a
    # tiny float-noise tolerance so a short/stalled restart cannot hide a
    # real completion jump behind a 250ms threshold.
    assert _round_clock_model(100.0, 100.02, 1800.0, False) == (
        True, 100.02, False)
    assert _round_clock_model(100.0, 100.005, 1800.0, False) == (
        False, 100.005, False)

    # During a projected restart countdown the suppression stays armed and
    # clears candidates even when a new death queues one between polls. The
    # first at-limit completion sample is also suppressed as the fresh zeroed
    # baseline, then ordinary evaluation resumes.
    pending = False
    queue_count = 1
    suppress, previous, pending = _round_clock_model(
        899.5, 1801.0, 1800.0, pending)
    if suppress:
        queue_count = 0
    assert (suppress, previous, pending, queue_count) == (
        True, 1801.0, True, 0)

    queue_count = 1  # a kill arrives during the projected countdown
    suppress, previous, pending = _round_clock_model(
        previous, 1800.01, 1800.0, pending)
    if suppress:
        queue_count = 0
    assert (suppress, previous, pending, queue_count) == (
        True, 1800.01, True, 0)

    queue_count = 1  # completion must suppress this candidate as well
    suppress, previous, pending = _round_clock_model(
        previous, 1800.0, 1800.0, pending)
    if suppress:
        queue_count = 0
    assert (suppress, previous, pending, queue_count) == (
        True, 1800.0, False, 0)
    assert _round_clock_model(1800.0, 1799.5, 1800.0, False) == (
        False, 1799.5, False)

    # An unavailable authoritative clock stays fail-closed indefinitely. Every
    # poll clears candidates while refreshing live count baselines.
    assert _round_clock_model(-1.0, -1.0, -1.0, True) == (
        True, -1.0, True)
    assert _round_clock_model(-1.0, -1.0, -1.0, False) == (
        True, -1.0, True)

    pending = False
    for _ in range(3):
        queue_count = 1  # a death can queue between any two unavailable polls
        suppress, previous, pending = _round_clock_model(
            -1.0, -1.0, -1.0, pending)
        if suppress:
            queue_count = 0
        assert (suppress, previous, pending, queue_count) == (
            True, -1.0, True, 0)

    # Recovery consumes one fresh baseline before ordinary evaluation resumes.
    suppress, previous, pending = _round_clock_model(
        -1.0, 900.0, 1800.0, True)
    assert (suppress, previous, pending) == (True, 900.0, False)
    assert _round_clock_model(previous, 899.5, 1800.0, pending) == (
        False, 899.5, False)


def test_round_clock_source_and_ordering_contract() -> None:
    assert re.search(
        r"#define\s+KSC_ROUND_REBASE_EPSILON\s+0\.01(?:\s|$)", CAPTURE)
    observe = function_body(CAPTURE, "stock bool:ksc_break_observe_round_clock")
    assert "dodx_get_round_time()" in observe
    unavailable = observe[observe.index("if (current < 0.0)"):
                          observe.index("new Float:limit")]
    assert "g_kscLastRoundTime = -1.0" in unavailable
    assert "g_kscBreakBaselineSuppressed = true" in unavailable
    assert "return true" in unavailable
    assert "current > g_kscLastRoundTime + KSC_ROUND_REBASE_EPSILON" in observe
    assert "current > limit" in observe
    assert "KSC_ROUND_RESTART_MARGIN" not in CAPTURE
    before(observe, "new bool:suppress_now", "!countdown")

    poll = function_body(CAPTURE, "public ksc_zone_poll_task")
    before(poll, "if (!ksc_enabled())", "ksc_break_observe_round_clock()")
    disabled = poll[poll.index("if (!ksc_enabled())"):
                    poll.index("ksc_ensure_ownership_baseline()")]
    assert "ksc_break_clear_all()" in disabled
    assert "g_kscBreakBaselineSuppressed = true" in disabled
    before(poll, "ksc_break_observe_round_clock()", "for (new f = 0;")
    before(poll, "ksc_break_clear_all()", "for (new f = 0;")
    assert "if (!suppress_breaks && g_kscBreakCount[f] > 0)" in poll
    before(poll, "if (!suppress_breaks", "g_kscPrevAllies[f] = ac")

    clear_all = function_body(CAPTURE, "stock ksc_break_clear_all")
    assert "f < g_kscFlagCount" in clear_all
    assert "ksc_break_clear(f)" in clear_all

    reset = function_body(CAPTURE, "stock ksc_break_reset_boundary")
    assert "ksc_break_clear_all()" in reset
    assert "g_kscLastRoundTime = -1.0" in reset
    assert "g_kscBreakBaselineSuppressed = true" in reset

    start = function_body(CAPTURE, "stock ksc_on_match_start")
    before(start, "ksc_break_reset_boundary()", "ksc_normalize_match_half")
    end = function_body(CAPTURE, "stock ksc_on_match_context_end")
    assert "ksc_break_reset_boundary()" in end
    before(end, "ksc_break_reset_boundary()", "ksc_flush()")

    controlpoints = function_body(CAPTURE, "public controlpoints_init")
    assert "g_kscLastRoundTime = -1.0" in controlpoints
    assert "g_kscBreakBaselineSuppressed = true" in controlpoints


def _effective_teamkill_model(*, tk: bool, killer: int, victim: int,
                              connected: set[int], killer_team: int,
                              victim_team: int) -> bool:
    """Executable truth table for the producer's degraded-TK repair."""
    if tk:
        return True
    if not (1 <= killer <= 32 and 1 <= victim <= 32) or killer == victim:
        return False
    if killer not in connected or victim not in connected:
        return False
    return killer_team in (1, 2) and killer_team == victim_team


def test_sustained_round_clock_unavailability_is_announced_once() -> None:
    # mp_timelimit 0 keeps dodx_get_round_time() at -1.0 forever, which keeps
    # break suppression fail-closed on every poll. That stays deliberate, but
    # it must announce itself: one log_amx per unavailable episode, re-armed
    # only by a valid clock reading.
    assert re.search(
        r"#define\s+KSC_ROUND_CLOCK_WARN_POLLS\s+\d+", CAPTURE)
    observe = function_body(CAPTURE, "stock bool:ksc_break_observe_round_clock")
    unavailable = observe[observe.index("if (current < 0.0)"):
                          observe.index("new Float:limit")]
    assert "KSC_ROUND_CLOCK_WARN_POLLS" in unavailable
    assert "log_amx(" in unavailable
    # One-shot: the counter parks at -1 after warning...
    before(unavailable, "log_amx(", "g_kscRoundClockUnavailPolls = -1")
    # ...and only a valid reading re-arms it, after the unavailable branch.
    before(observe, "return true", "g_kscRoundClockUnavailPolls = 0")
    assert "g_kscRoundClockUnavailPolls = 0" in observe


def test_effective_teamkill_covers_degraded_deathmsg_path() -> None:
    both = {1, 2}
    # The regression case: DeathMsg reports TK=0, but two live Allies are still
    # authoritatively a teamkill and must target neither Frags nor cap_break.
    assert _effective_teamkill_model(
        tk=False, killer=1, victim=2, connected=both,
        killer_team=1, victim_team=1)
    assert _effective_teamkill_model(
        tk=False, killer=1, victim=2, connected=both,
        killer_team=2, victim_team=2)
    assert _effective_teamkill_model(
        tk=True, killer=1, victim=2, connected=both,
        killer_team=1, victim_team=2)
    assert not _effective_teamkill_model(
        tk=False, killer=1, victim=2, connected=both,
        killer_team=1, victim_team=2)
    assert not _effective_teamkill_model(
        tk=False, killer=1, victim=1, connected={1},
        killer_team=1, victim_team=1)
    assert not _effective_teamkill_model(
        tk=False, killer=1, victim=2, connected=both,
        killer_team=3, victim_team=3)
    assert not _effective_teamkill_model(
        tk=False, killer=1, victim=2, connected={1},
        killer_team=1, victim_team=1)


def test_effective_teamkill_controls_both_producer_consumers() -> None:
    helper = function_body(CAPTURE, "stock bool:ksc_effective_teamkill")
    assert "if (TK)" in helper
    assert "killer == victim" in helper
    assert "is_user_connected(killer)" in helper
    assert "is_user_connected(victim)" in helper
    assert "killer_team != KSC_TEAM_ALLIES" in helper
    assert "killer_team != KSC_TEAM_AXIS" in helper
    assert "killer_team == get_user_team(victim)" in helper

    death = function_body(CAPTURE, "stock ksc_on_death")
    assert "new bool:effectiveTeamkill = ksc_effective_teamkill(" in death
    assert "ksc_queue_break_candidate(killer, victim, effectiveTeamkill)" in death
    assert "if (ksc_enabled() && !effectiveTeamkill)" in death
    before(death, "ksc_effective_teamkill(", "ksc_queue_break_candidate(")
    before(death, "ksc_effective_teamkill(", "ksc_emit_frag_context(")

    queue = function_body(CAPTURE, "stock ksc_queue_break_candidate")
    assert "if (teamkill || killer == victim)" in queue


def test_split_loader_resolver_uses_only_declared_loaded_game_dll() -> None:
    helper = function_body(
        GAMECONFIGS, "static bool ResolveLoadedMetamodGameDll")
    assert "GET_GAME_DIR(gameDir)" in helper
    assert "realpath(candidates[i], nullptr)" in helper
    assert "dlopen(resolvedPath, RTLD_LAZY | RTLD_NOLOAD)" in helper
    assert 'dlsym(handle, "GiveFnptrsToDll")' in helper
    assert "GetEntityInit" not in helper
    assert "worldspawn" not in helper
    before(helper, "realpath(candidates[i], nullptr)",
           "dlopen(resolvedPath, RTLD_LAZY | RTLD_NOLOAD)")
    relative = helper[helper.index("GET_GAME_DIR(gameDir)"):]
    before(relative, "candidates[candidateCount++] = modRelative",
           "candidates[candidateCount++] = gameDll")


def test_split_loader_resolver_verifies_anchor_file_identity() -> None:
    helper = function_body(
        GAMECONFIGS, "static bool ResolveLoadedMetamodGameDll")
    for fragment in (
        "dladdr(anchor, &info)",
        "stat(resolvedPath, &requestedStat)",
        "stat(info.dli_fname, &resolvedStat)",
        "requestedStat.st_dev == resolvedStat.st_dev",
        "requestedStat.st_ino == resolvedStat.st_ino",
    ):
        assert fragment in helper
    before(helper, "requestedStat.st_ino == resolvedStat.st_ino",
           "*baseAddress = info.dli_fbase")
    before(helper, "dlclose(handle)", "return true")
    # The mismatch out-param may only be raised for a candidate that was
    # actually mapped (a NOLOAD handle existed) and then failed the identity
    # check -- a merely-absent candidate must not read as a mismatch.
    assert "*identityMismatch = true;" in helper
    before(helper, "dlopen(resolvedPath, RTLD_LAZY | RTLD_NOLOAD)",
           "*identityMismatch = true;")
    before(helper, "if (!valid && identityMismatch)", "if (valid)")


def test_split_loader_resolver_preserves_direct_paths() -> None:
    resolver = function_body(
        GAMECONFIGS, "bool CGameConfigManager::ResolveLibraryInfo")
    assert "g_bRunningWithMetamod" in resolver
    assert 'get_localinfo("mm_gamedll", "")' in resolver
    assert "reinterpret_cast<void*>(MDLL_Spawn)" in resolver
    assert "g_pGameEntityInterface->pfnSpawn" in resolver
    assert resolver.count("ResolveLoadedMetamodGameDll(") == 1
    assert resolver.count("!g_bRunningWithMetamod") == 1
    assert "Unable to prove declared mm_gamedll" in resolver
    assert 'strcmp(library, "server")' in resolver
    assert "if (!dladdr(symbol, &info))" in resolver
    before(resolver, 'get_localinfo("mm_gamedll", "")',
           "g_pGameEntityInterface->pfnSpawn")
    # A declared-but-not-loaded mm_gamedll is a stale localinfo (settable from
    # any cfg/rcon), not a split-loader topology: it must fall through to the
    # engine entity interface rather than permanently null-caching "server"
    # resolution. Only a genuine identity-check mismatch stays failed closed.
    assert "bool identityMismatch = false;" in resolver
    assert "if (identityMismatch)" in resolver
    assert "ignoring stale localinfo" in resolver
    before(resolver, "if (identityMismatch)", "ignoring stale localinfo")
    mismatch_start = resolver.index("if (identityMismatch)")
    mismatch_block = resolver[mismatch_start:resolver.index("ignoring stale localinfo")]
    assert "Unable to prove declared mm_gamedll" in mismatch_block
    assert "return false;" in mismatch_block


def test_split_loader_resolver_is_linux_and_no_load_only() -> None:
    assert "#define _GNU_SOURCE" in GAMECONFIGS
    assert GAMECONFIGS.count(
        "defined PLATFORM_LINUX && defined RTLD_NOLOAD") >= 2
    assert "dlopen(resolvedPath, RTLD_NOW)" not in GAMECONFIGS
    assert "dlopen(resolvedPath, RTLD_LAZY)" not in GAMECONFIGS
    # RTLD_NOW would promote a lazily bound game DLL to eager relocation and
    # can hard-fail resolution; the helper only dlsyms one exported symbol.
    assert "dlopen(resolvedPath, RTLD_NOW | RTLD_NOLOAD)" not in GAMECONFIGS


def test_physical_boundaries_do_not_use_stats_pause_gate() -> None:
    for signature in (
        "stock ksc_life_start",
        "stock ksc_life_end",
        "stock ksc_on_disconnect",
    ):
        body = function_body(CAPTURE, signature)
        assert "isDSMActive" not in body
        assert "dodstats_pause" not in body


def test_plugin_version() -> None:
    assert re.search(r'#define\s+PLUGIN_VERSION\s+"1\.17\.0"', STATS)


def test_capout_requires_a_complete_two_team_partition() -> None:
    predicate = function_body(CAPTURE, "stock bool:ksc_is_full_capout_threat")
    assert "g_kscFlagCount < 2" in predicate
    assert "ksc_team_flag_count(defending_team) != 1" in predicate
    assert "ksc_team_flag_count(attacking_team) != g_kscFlagCount - 1" in predicate

    break_body = function_body(CAPTURE, "stock ksc_emit_break")
    defense_body = function_body(CAPTURE, "stock bool:ksc_is_last_flag_defense")
    assert "ksc_is_full_capout_threat(defending_team)" in break_body
    assert "ksc_is_full_capout_threat(killer_team)" in defense_body


def main() -> None:
    tests = [value for name, value in sorted(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} stats-capture contract tests")


if __name__ == "__main__":
    main()
