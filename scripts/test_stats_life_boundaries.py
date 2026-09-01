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
DODX_MODULE = (ROOT / "modules" / "dod" / "dodx" / "moduleconfig.cpp").read_text(encoding="utf-8")
DODX_NATIVE = (ROOT / "modules" / "dod" / "dodx" / "NBase.cpp").read_text(encoding="utf-8")
DODX_INCLUDE = (ROOT / "plugins" / "include" / "dodx.inc").read_text(encoding="utf-8")


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


def function_body_last(source: str, signature: str) -> str:
    """Return the last definition body when C++ also has a prototype."""
    start = source.rfind(signature)
    assert start >= 0, f"missing function: {signature}"
    return function_body(source[start:], signature)


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


def test_schema22_team_membership_wire_and_health_order() -> None:
    emit = function_body(CAPTURE, "stock ksc_emit_team_membership")
    assert r'triggered ^\"team_membership^\"' in emit
    for field in ("matchid", "half", "team", "old_team", "game_time", "event_epoch", "sequence"):
        assert f'({field} ^\\"%' in emit, f"team membership wire field missing: {field}"
    assert "ksc_event_context(matchid" in emit
    assert "ksc_buffer(line, KSC_EVENT_TEAM_MEMBERSHIP)" in emit
    assert "g_kscAttempted[KSC_EVENT_TEAM_MEMBERSHIP]++" in emit
    assert "g_kscDroppedByType[KSC_EVENT_TEAM_MEMBERSHIP]++" in emit
    forward = function_body(CAPTURE, "public dod_client_changeteam")
    assert "ksc_emit_team_membership(id, team, oldteam)" in forward

    manifest = re.search(r'#define\s+KSC_CAPABILITIES\s+"([^"]+)"', CAPTURE)
    assert manifest and "team_membership" in manifest.group(1)
    enum = function_body(CAPTURE, "enum {")
    assert enum.index("KSC_EVENT_TEAM_MEMBERSHIP") < enum.index("KSC_EVENT_GRENADE_ENTITY")
    names = function_body(CAPTURE, "new const g_kscEventNames")
    assert names.index('"team_membership"') < names.index('"grenade_entity"')


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
    assert "g_kscLifeBufferOrder[life_i] < g_kscBufferOrder[data_i]" in flush
    assert "while (data_i < g_kscBufferCount || life_i < g_kscLifeBufferCount)" in flush
    assert "g_kscLifeBuffer[life_i]" in flush
    assert "g_kscBuffer[data_i]" in flush
    assert "dropped %d LIFE boundary" in flush


def _confirmation_activation_model(pending_facts: int, trigger: str,
                                   confirms: bool = True):
    """Executable truth table for candidate -> confirmed producer activation."""
    wire = [
        {"kind": "pending", "matchid": "-", "half": 0, "sequence": 0}
        for _ in range(pending_facts)
    ]
    counters = {"attempted": 0, "enqueued": 0, "emitted": 0}

    if not confirms:
        # Context end drains diagnostics but emits no authorization or health.
        return wire, counters, None

    # Confirmation starts a fresh stream only after the pending ring drains.
    wire.append({"kind": "manifest", "matchid": "confirmed",
                 "half": 1, "sequence": 1})
    counters = {"attempted": 1, "enqueued": 1, "emitted": 1}
    wire.append({"kind": trigger, "matchid": "confirmed",
                 "half": 1, "sequence": 2})
    health = {**counters, "sequence_last": 2, "emitted_health": True}
    return wire, counters, health


def test_delayed_dodx_context_activates_only_on_exact_confirmation() -> None:
    start = function_body(CAPTURE, "stock ksc_on_match_start")
    assert "ksc_close_producer_context()" in start
    assert "copy(g_kscProducerMatchId" in start
    for forbidden in ("ksc_reset_health()", "ksc_emit_manifest(",
                      "ksc_flag_positions_task()"):
        assert forbidden not in start

    activate = function_body(CAPTURE, "stock bool:ksc_activate_producer_context")
    before(activate, "ksc_flush()", "ksc_reset_health()")
    before(activate, "ksc_reset_health()",
           "g_kscProducerContextConfirmed = true")
    before(activate, "g_kscProducerContextConfirmed = true",
           "ksc_emit_manifest(")
    assert "g_kscOwnershipBaselinePending = true" in activate
    assert "g_kscLifeBaselinePending = true" in activate

    context = function_body(CAPTURE, "stock bool:ksc_event_context")
    before(context, "!equal(dodx_matchid, g_kscProducerMatchId)",
           "ksc_activate_producer_context()")
    before(context, "ksc_activate_producer_context()",
           "copy(matchid, len, g_kscProducerMatchId)")

    # Confirmation by an ordinary fact: 0-3 pending facts drain first, then the
    # manifest is sequence 1 and the triggering fact sequence 2 every time.
    for pending_facts in range(4):
        wire, counters, health = _confirmation_activation_model(
            pending_facts, "frag")
        assert [row["sequence"] for row in wire[:pending_facts]] == (
            [0] * pending_facts)
        assert [row["kind"] for row in wire[pending_facts:]] == (
            ["manifest", "frag"])
        assert [row["sequence"] for row in wire[pending_facts:]] == [1, 2]
        assert counters == {"attempted": 1, "enqueued": 1, "emitted": 1}
        assert health == {"attempted": 1, "enqueued": 1, "emitted": 1,
                          "sequence_last": 2, "emitted_health": True}

    # Confirmation by the zone path has the same contract; its first direct
    # flag-position fact is sequence 2, then the loop may emit the remainder.
    wire, counters, health = _confirmation_activation_model(2, "flag_position")
    assert [(row["kind"], row["sequence"]) for row in wire] == [
        ("pending", 0), ("pending", 0),
        ("manifest", 1), ("flag_position", 2),
    ]
    assert counters == {"attempted": 1, "enqueued": 1, "emitted": 1}
    assert health["sequence_last"] == 2

    # A candidate that never confirms has no manifest and no health record.
    wire, counters, health = _confirmation_activation_model(
        3, "frag", confirms=False)
    assert [row["kind"] for row in wire] == ["pending"] * 3
    assert [row["sequence"] for row in wire] == [0, 0, 0]
    assert counters == {"attempted": 0, "enqueued": 0, "emitted": 0}
    assert health is None

    close = function_body(CAPTURE, "stock ksc_close_producer_context")
    assert "if (g_kscProducerContextConfirmed" in close
    before(close, "ksc_flush()", "ksc_emit_health(")

    untracked = function_body(CAPTURE, "stock bool:ksc_untracked_buffer")
    assert "g_kscAttempted" not in untracked
    assert "g_kscEnqueued" not in untracked
    assert "g_kscDroppedByType" not in untracked
    assert "g_kscBufferType[g_kscBufferCount] = KSC_EVENT_UNTRACKED" in untracked

    flush = function_body(CAPTURE, "stock ksc_flush")
    assert "g_kscBufferType[data_i] >= 0" in flush
    assert "g_kscBufferType[data_i] < KSC_EVENT_COUNT" in flush

    for signature in (
        "stock ksc_emit_damage",
        "stock ksc_emit_frag_context",
        "stock ksc_on_death",
        "stock ksc_emit_break",
        "stock ksc_emit_break_context",
    ):
        body = function_body(CAPTURE, signature)
        assert "bool:tracked = ksc_optional_event_context(" in body
        assert "if (!tracked && !untracked)" in body
        assert "tracked ? ksc_next_sequence() : 0" in body
        assert "ksc_buffer_event(" in body

    # Candidate-unconfirmed direct flag metadata must not leak a claimed half
    # (or even a sentinel per-half retry); map-load metadata without a candidate
    # keeps its historical sequence-0 form.
    flag = function_body(CAPTURE, "stock ksc_emit_flag_position")
    assert "bool:had_candidate = g_kscProducerMatchId[0]" in flag
    assert "if (had_candidate || !g_kscAllowUntrackedEvents)" in flag
    before(flag, "if (had_candidate || !g_kscAllowUntrackedEvents)",
           'log_message("KTP_FLAG_POSITION')
    before(flag, "ksc_event_context(", "ksc_next_sequence()")

    ownership = function_body(CAPTURE, "stock ksc_ensure_ownership_baseline")
    before(ownership, "ksc_emit_flag_position(f)", "ksc_emit_flag_state(")


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
    assert "ksc_close_producer_context()" in confirmed_invalidation
    close = function_body(CAPTURE, "stock ksc_close_producer_context")
    assert "ksc_break_reset_boundary()" in close
    before(close, "ksc_break_reset_boundary()", "g_kscProducerMatchId[0] = 0")
    assert "g_kscProducerMatchId[0] = 0" in close
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


def test_match_start_emits_outgoing_half_health_before_reset() -> None:
    # ktp_half_end fires only with half=1 and ktp_match_end only at true match
    # end, so H2->OT and OT->OT boundaries arrive solely as the next
    # ktp_match_start. The outgoing half's health record must be emitted there
    # before the later context activation resets its counters.
    start = function_body(CAPTURE, "stock ksc_on_match_start")
    assert "ksc_emit_health(g_kscProducerMatchId, g_kscProducerHalf)" in start
    activate = function_body(CAPTURE, "stock bool:ksc_activate_producer_context")
    assert "ksc_reset_health()" in activate
    before(start, "ksc_emit_health(g_kscProducerMatchId, g_kscProducerHalf)",
           "ksc_close_producer_context()")
    # The emit must be flushed-behind and guarded on a still-populated context,
    # and must run before either branch overwrites that context.
    before(start, "ksc_flush()",
           "ksc_emit_health(g_kscProducerMatchId, g_kscProducerHalf)")
    before(start, "ksc_emit_health(g_kscProducerMatchId, g_kscProducerHalf)",
           "copy(g_kscProducerMatchId, charsmax(g_kscProducerMatchId), matchid)")


def test_plugin_end_drains_private_capture() -> None:
    plugin_end = function_body(STATS, "public plugin_end")
    assert "ksc_shutdown()" in plugin_end
    shutdown = function_body(CAPTURE, "stock ksc_shutdown")
    before(shutdown, "ksc_objective_reset_all(true)", "ksc_flush()")
    assert "ksc_grenade_cache_clear_all()" in shutdown


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
    before(start, "ksc_close_producer_context()", "ksc_normalize_match_half")
    end = function_body(CAPTURE, "stock ksc_on_match_context_end")
    assert "ksc_close_producer_context()" in end
    close = function_body(CAPTURE, "stock ksc_close_producer_context")
    before(close, "ksc_break_reset_boundary()", "ksc_flush()")

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
    assert re.search(r'#define\s+PLUGIN_VERSION\s+"1\.19\.0"', STATS)


def test_schema23_manifest_and_two_second_position_contract() -> None:
    assert re.search(r"#define\s+KSC_SCHEMA_CONTRACT\s+23(?:\s|$)", CAPTURE)
    assert re.search(r"#define\s+KSC_POSITION_BROADCAST_SECS\s+2\.0(?:\s|$)", CAPTURE)
    for capability in ("objective_attempt", "grenade_entity", "position_state", "map_revision"):
        assert capability in re.search(
            r'#define\s+KSC_CAPABILITIES\s+"([^"]+)"', CAPTURE).group(1)
    for capability in ("objective_attempt", "grenade_entity"):
        assert f'"{capability}"' in CAPTURE
    assert re.search(r"#define\s+KSC_BUF_FLUSH_SECS\s+5\.0", CAPTURE)
    assert re.search(r"#define\s+KSC_BUF_MAX_ENTRIES\s+128", CAPTURE)
    positions = function_body(CAPTURE, "public ksc_position_broadcast_task")
    before(positions, "if (!is_user_connected(id))", "new alive = is_user_alive(id)")
    assert 'new spectator = (team == 3) ? 1 : 0' in positions
    assert "if (!alive || spectator)" in positions
    for field in ("alive", "spectator", "map_revision"):
        assert f'({field} ^"%' in positions
    assert "g_kscMapRevision" in positions

    revision = function_body(CAPTURE, "stock bool:ksc_capture_map_revision")
    assert 'formatex(map_path, charsmax(map_path), "maps/%s.bsp", mapname)' in revision
    assert "hash_file(map_path, Hash_Sha256" in revision
    assert "!= 64" in revision
    activation = function_body(CAPTURE, "stock bool:ksc_activate_producer_context")
    before(activation, "if (!g_kscMapRevisionReady)", "g_kscProducerContextConfirmed = true")


def _objective_model(active, owner: int, is_capping: bool, team: int,
                     allies: int, axis: int):
    """Executable truth table matching ksc_objective_observe."""
    occupancy = allies if team == 1 else axis
    valid = is_capping and team in (1, 2) and team != owner and occupancy > 0
    events = []
    if active is None:
        return (["start"], (team, owner)) if valid else (events, None)
    old_team, owner_before = active
    if owner == old_team and owner != owner_before:
        return ["complete"], None
    if not valid:
        return ["stop"], None
    if team != old_team:
        return ["stop", "start"], (team, owner)
    return events, active


def test_objective_attempt_transition_contract() -> None:
    events, active = _objective_model(None, 2, True, 1, 2, 0)
    assert events == ["start"] and active == (1, 2)
    assert _objective_model(active, 2, True, 1, 2, 0) == ([], active)
    assert _objective_model(active, 1, False, 0, 0, 0) == (["complete"], None)
    assert _objective_model(active, 2, False, 0, 0, 0) == (["stop"], None)
    assert _objective_model(active, 0, True, 2, 1, 1) == (
        ["stop", "start"], (2, 0))
    assert _objective_model(None, 0, True, 1, 0, 0) == ([], None)

    observe = function_body(CAPTURE, "stock ksc_objective_observe")
    assert "active_occupancy > 0" in observe
    assert "owner == g_kscAttemptTeam[f]" in observe
    assert "owner != g_kscAttemptOwnerBefore[f]" in observe
    team_flip = observe[observe.index("if (capturing_team != g_kscAttemptTeam[f])"):]
    before(team_flip, 'ksc_objective_finish(f, "stop", "capture_stopped"',
           "ksc_objective_start(f, capturing_team")
    for forbidden in ("participant", "walked_off", "killed", "cap_break"):
        assert forbidden not in observe


def test_objective_attempt_restart_state_machine() -> None:
    # Restart suppression is a hard context edge: an open attempt closes once,
    # no start/complete is admitted during countdown, and ordinary observation
    # resumes only after the fresh-baseline poll has been consumed.
    active = (1, 2)
    events = [("stop", "context_reset")] if active else []
    active = None
    assert events == [("stop", "context_reset")] and active is None
    assert ([] if active is None else ["complete"]) == []
    assert ([] if True else _objective_model(None, 2, True, 1, 2, 0)[0]) == []
    assert _objective_model(None, 2, True, 1, 2, 0)[0] == ["start"]

    poll = function_body(CAPTURE, "public ksc_zone_poll_task")
    suppression = poll[poll.index("if (suppress_breaks)"):
                       poll.index("for (new f = 0;")]
    assert "ksc_objective_reset_all(true)" in suppression
    observe_call = poll.index("ksc_objective_observe(f")
    guard = poll.rfind("if (!suppress_breaks)", 0, observe_call)
    assert guard >= 0
    before(poll, "ksc_objective_reset_all(true)", "for (new f = 0;")


def test_objective_attempt_wire_identity_and_enum_contract() -> None:
    emit = function_body(CAPTURE, "stock bool:ksc_emit_objective_attempt")
    expected = (
        "kind", "matchid", "half", "map", "attempt_id", "flag_index",
        "flag_name", "capturing_team", "owner_before", "allies_in_zone",
        "axis_in_zone", "stop_reason", "game_time", "event_epoch", "sequence",
    )
    cursor = -1
    for field in expected:
        cursor = emit.index(f'({field} ^"%', cursor + 1)
    assert "wire_attempt_id = attempt_id ? attempt_id : event_sequence" in emit
    start = function_body(CAPTURE, "stock ksc_objective_start")
    assert "g_kscAttemptId[f] = sequence" in start
    finish = function_body(CAPTURE, "stock bool:ksc_objective_finish")
    assert "g_kscAttemptId[f]" in finish
    assert '"capture_stopped"' in CAPTURE
    assert '"context_reset"' in CAPTURE
    assert "walked_off" not in emit and "killed" not in emit


def _wire_sanitize(value: str) -> str:
    return "".join(
        "_" if ord(ch) < 32 or ord(ch) in (34, 40, 41, 92, 127) else ch
        for ch in value
    )


def _player_name_sanitize(value: str) -> str:
    return "".join(
        "_" if ord(ch) < 32 or ord(ch) in (34, 40, 41, 60, 62, 92, 127)
        else ch
        for ch in value
    )


def _parse_player_identity(value: str):
    # Exact field boundaries used by KTPHLStatsX ktpParsePlayerIdentity.
    return re.match(
        r'^(.*?)<(\d+)><([^<>]*)><([^<>]*)>(?:<([^<>]*)>)?.*$', value
    )


def _parse_marker_properties(line: str) -> dict[str, str]:
    # Same flat `(field "value")` convention consumed by the stats daemon.
    return dict(re.findall(r'\(([a-z_]+) "([^"]*)"\)', line))


def test_new_marker_strings_are_protocol_safe() -> None:
    sanitize = function_body(CAPTURE, "stock ksc_wire_sanitize")
    for unsafe in ("ch < 32", "ch == 34", "ch == 40", "ch == 41",
                   "ch == 92", "ch == 127"):
        assert unsafe in sanitize

    hostile_flag = 'Middle ") (sequence "999")\\\r\n'
    hostile_name = 'Bad\\Name") (kind "removed")<42><STEAM_0:1:9><Allies>'
    safe_flag = _wire_sanitize(hostile_flag)
    safe_owner = (
        _player_name_sanitize(hostile_name) +
        '<7><STEAM_0:1:123><Axis>'
    )
    line = (
        'KTP_TEST (flag_name "' + safe_flag + '") '
        '(owner "' + safe_owner + '") (sequence "7")'
    )
    parsed = _parse_marker_properties(line)
    assert parsed == {
        "flag_name": safe_flag,
        "owner": safe_owner,
        "sequence": "7",
    }
    assert parsed["sequence"] == "7"  # hostile text did not inject 999
    assert "<7><STEAM_0:1:123><Axis>" in parsed["owner"]
    for value in parsed.values():
        assert not any(ord(ch) < 32 or ord(ch) in (34, 40, 41, 92, 127)
                       for ch in value)

    objective = function_body(CAPTURE, "stock bool:ksc_emit_objective_attempt")
    for wire in ("wire_kind", "wire_matchid", "wire_map",
                 "wire_flag_name", "wire_stop_reason"):
        assert f"ksc_wire_sanitize(" in objective and wire in objective
    grenade = function_body(CAPTURE, "stock bool:ksc_emit_grenade_entity")
    for wire in ("wire_kind", "wire_matchid", "wire_map",
                 "wire_weapon_type", "wire_owner"):
        assert wire in grenade
    player = function_body(CAPTURE, "stock ksc_player_str")
    before(player, "ksc_player_name_sanitize(name", "formatex(raw")
    assert "safe_name, get_user_userid(id), safe_authid, safe_team" in player
    assert "ksc_wire_sanitize(raw, out, len)" in player


def test_hostile_player_name_cannot_spoof_identity_fields() -> None:
    hostile_name = 'Mallory<42><STEAM_0:1:999><Allies>'
    unsafe = hostile_name + '<7><STEAM_0:1:123><Axis>'
    unsafe_match = _parse_player_identity(unsafe)
    assert unsafe_match and unsafe_match.groups()[:4] == (
        "Mallory", "42", "STEAM_0:1:999", "Allies")

    safe = _player_name_sanitize(hostile_name) + '<7><STEAM_0:1:123><Axis>'
    safe_match = _parse_player_identity(safe)
    assert safe_match and safe_match.groups()[:4] == (
        "Mallory_42__STEAM_0:1:999__Allies_",
        "7", "STEAM_0:1:123", "Axis")

    sanitizer = function_body(CAPTURE, "stock ksc_player_name_sanitize")
    assert "ch == 60" in sanitizer and "ch == 62" in sanitizer


def test_grenade_entity_producer_contract() -> None:
    tracked = function_body(CAPTURE, "public dod_grenade_entity_tracked")
    removed = function_body(CAPTURE, "public dod_grenade_entity_removed")
    emit = function_body(CAPTURE, "stock bool:ksc_emit_grenade_entity")
    assert "ksc_grenade_cache_find(entindex, serial)" in tracked
    assert "ksc_player_str(owner" in tracked
    assert "g_kscGrenadeCacheOwner[slot]" in removed
    assert "ksc_grenade_cache_clear(slot)" in removed
    assert "public dod_grenade_explosion" not in CAPTURE
    for weapon in (13, 14, 36):
        assert f"wpnid == {weapon}" in function_body(
            CAPTURE, "stock bool:ksc_grenade_weapon_valid")
    for field in (
        "kind", "matchid", "half", "map", "entindex", "serial", "weapon_id",
        "weapon_type", "owner", "position", "game_time", "event_epoch", "sequence",
    ):
        assert f'({field} ^"%' in emit
    assert 'ksc_emit_grenade_entity("removed"' in removed
    assert "detonation" not in emit.lower()
    drop = function_body(CAPTURE, "public dod_grenade_entity_tracker_drop")
    assert "ksc_event_context" in drop
    assert "ksc_grenade_record_drop()" in drop
    record_drop = function_body(CAPTURE, "stock ksc_grenade_record_drop")
    assert "g_kscAttempted[KSC_EVENT_GRENADE_ENTITY]++" in record_drop
    assert "g_kscDroppedByType[KSC_EVENT_GRENADE_ENTITY]++" in record_drop


def _grenade_tracker_model(events, capacity=2, max_edicts=128):
    active: dict[tuple[int, int], int] = {}
    overflow_serial = [0] * max_edicts
    emitted = []
    for action, entindex, serial, weapon in events:
        key = (entindex, serial)
        if action == "clear":
            active.clear()
            overflow_serial = [0] * max_edicts
        elif action == "track":
            if (entindex <= 0 or entindex >= max_edicts or serial <= 0 or
                    weapon not in (13, 14, 36)):
                continue
            if key in active or overflow_serial[entindex] == serial:
                continue
            if len(active) < capacity:
                active[key] = weapon
                emitted.append(("tracked", key))
            else:
                overflow_serial[entindex] = serial
                emitted.append(("drop", key))
        elif action == "remove":
            if key in active:
                active.pop(key)
                emitted.append(("removed", key))
            elif 0 < entindex < max_edicts and overflow_serial[entindex] == serial:
                overflow_serial[entindex] = 0
    return emitted, active, overflow_serial


def test_grenade_tracker_saturation_reuse_and_recovery_model() -> None:
    emitted, active, overflow = _grenade_tracker_model([
        ("track", 40, 1, 13), ("track", 40, 1, 13),
        ("track", 41, 1, 14), ("track", 42, 1, 36),
        ("track", 42, 1, 36), ("track", 43, 0, 13),
        ("track", 44, 1, 29), ("track", 45, 1, 40),
        ("remove", 40, 1, 13), ("remove", 40, 1, 13),
        ("track", 40, 2, 13),  # same index, new serial is a new entity
        ("track", 42, 1, 36),  # dropped key stays dropped after recovery
        ("remove", 42, 1, 36),
        ("track", 42, 2, 36),
    ])
    assert emitted.count(("tracked", (40, 1))) == 1
    assert emitted.count(("removed", (40, 1))) == 1
    assert emitted.count(("drop", (42, 1))) == 1
    assert ("tracked", (40, 2)) in emitted
    assert ("tracked", (42, 2)) not in emitted  # capacity still truthfully full

    emitted, active, overflow = _grenade_tracker_model([
        ("track", 40, 1, 13), ("track", 41, 1, 14),
        ("track", 42, 1, 13), ("track", 43, 1, 14),
        ("track", 44, 1, 36), ("track", 42, 1, 13),
        ("track", 43, 1, 14), ("track", 44, 1, 36),
        ("remove", 40, 1, 13), ("track", 45, 1, 13),
        ("track", 42, 1, 13),  # old dropped key stays deduped
        ("remove", 42, 1, 13), ("remove", 41, 1, 14),
        ("track", 42, 2, 13),  # index reuse with new serial recovers
    ])
    for key in ((42, 1), (43, 1), (44, 1)):
        assert emitted.count(("drop", key)) == 1
    assert ("tracked", (45, 1)) in emitted
    assert ("tracked", (42, 2)) in emitted


def test_dodx_grenade_entity_tracking_and_removal_contract() -> None:
    track = function_body(DODX_MODULE, "static void DODX_TrackGrenadeEntity")
    remove = function_body(DODX_MODULE, "static void DODX_RemoveTrackedGrenadeEntity")
    hook = function_body_last(DODX_MODULE, "static void DODX_OnEdictFree")
    valid = function_body(DODX_MODULE, "static bool DODX_IsTelemetryGrenadeWeapon")
    assert 'strcmp(classname, "grenade")' in track
    assert 'strcmp(classname, "grenade2")' in track
    assert "entindex" in track and "serial" in track
    assert "serial <= 0" in track
    assert "return; // same entity, later TraceLine: tracked exactly once" in track
    assert "weapon == 13 || weapon == 14 || weapon == 36" in valid
    assert "monster_mortar" not in track
    assert "record.entindex != entindex || record.serial != serial" in remove
    before(remove, "record.active = false", "DODX_EmitGrenadeEntityForward")
    before(hook, "DODX_RemoveTrackedGrenadeEntity(entity)", "chain->callNext(entity)")
    assert "ED_Free()->registerHook(DODX_OnEdictFree" in DODX_MODULE
    assert "ED_Free()->unregisterHook(DODX_OnEdictFree" in DODX_MODULE
    assert "PF_Remove_I()->registerHook" not in DODX_MODULE
    assert "g_grenades" not in track and "g_grenades" not in remove

    overflow = function_body(DODX_MODULE, "static bool DODX_RecordGrenadeOverflow")
    assert "g_ktpGrenadeOverflowSerial[entindex] == serial" in overflow
    assert "g_ktpGrenadeOverflowSerial[entindex] = serial" in overflow
    assert "return false" in overflow  # exact index+serial duplicate suppression
    assert "g_ktpGrenadeOverflowSerial[MAX_EDICTS]" in DODX_MODULE
    assert "g_ktpGrenadeOverflowFailClosed" not in DODX_MODULE
    assert "DODX_HasGrenadeOverflow(entindex, serial)" in track
    assert "DODX_EmitGrenadeTrackerDrop" in track
    assert "DODX_ClearGrenadeOverflow(entindex, serial)" in remove
    clear = function_body(DODX_MODULE, "static void DODX_ClearGrenadeEntityTracker")
    assert "g_ktpGrenadeEntities[i].active = false" in clear
    assert "i < MAX_EDICTS" in clear
    assert "g_ktpGrenadeOverflowSerial[i] = 0" in clear


def test_dodx_grenade_entity_forward_and_direct_dispatch_contract() -> None:
    for name in ("dod_grenade_entity_tracked", "dod_grenade_entity_removed"):
        assert f'forward {name}(owner, entindex, serial, Float:pos[3], wpnid, Float:gametime);' in DODX_INCLUDE
        assert f'MF_RegisterForward("{name}"' in DODX_MODULE
        assert f'"dodx_test_dispatch_grenade_entity_{name.rsplit("_", 1)[-1]}"' in DODX_NATIVE
    dispatch = function_body(DODX_NATIVE, "static cell DODX_TestDispatchGrenadeEntity")
    assert "wpnid != 13 && wpnid != 14 && wpnid != 36" in dispatch
    assert "MF_ExecuteForward(forward, owner, entindex, serial, pos, wpnid, gametime)" in dispatch
    assert "serial <= 0" in dispatch
    drop_name = "dod_grenade_entity_tracker_drop"
    assert f"forward {drop_name}(owner, entindex, serial, wpnid, Float:gametime);" in DODX_INCLUDE
    assert f'MF_RegisterForward("{drop_name}"' in DODX_MODULE
    assert '"dodx_test_dispatch_grenade_entity_tracker_drop"' in DODX_NATIVE
    drop_dispatch = function_body(
        DODX_NATIVE,
        "static cell AMX_NATIVE_CALL dodx_test_dispatch_grenade_entity_tracker_drop",
    )
    assert "serial <= 0" in drop_dispatch
    assert "wpnid != 13 && wpnid != 14 && wpnid != 36" in drop_dispatch
    assert re.search(r'#define\s+PLUGIN_VERSION\s+"1\.19\.0"', STATS)


def test_ksc_buffer_detects_and_counts_line_truncation() -> None:
    # ksc_buffer's copy() truncates anything past KSC_BUF_LINE_LEN - 1 with no
    # signal -- a truncated line just stops matching the daemon's regex,
    # which reads identically to the event never having fired. The length
    # check must run, and must run BEFORE the truncating copy(), or counting
    # it is cosmetic.
    body = function_body(CAPTURE, "stock bool:ksc_buffer(const line[], event_type)")
    before(body, "strlen(line) >= KSC_BUF_LINE_LEN - 1", "copy(g_kscBuffer")
    assert "g_kscTruncated++" in body

    # Declared and reset on the same lifecycle as the existing buffer-full
    # counter (g_kscDropped): a plain global, reported and zeroed every flush.
    assert re.search(r"new\s+g_kscTruncated\s*=\s*0", CAPTURE)

    flush_body = function_body(CAPTURE, "stock ksc_flush()")
    before(flush_body, "g_kscTruncated > 0", 'log_amx("[KTP-STATS] truncated')
    assert "KSC_BUF_LINE_LEN" in flush_body
    assert "g_kscTruncated = 0" in flush_body


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
