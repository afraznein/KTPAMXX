#!/usr/bin/env python3
"""Deterministic source-contract tests for DoD life-boundary capture.

The Pawn plugin needs a running DoD server for behavioral execution. These
tests cover the failure-prone integration contract locally and in KTPAMXX CI:
wire fields, warmup/round-live semantics, event-hook placement, transition
baseline ordering, and flush-before-context-clear ordering. KTPInfrastructure's
Lane A/B tests remain responsible for runtime forward dispatch and ingestion.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
STATS = (ROOT / "plugins" / "dod" / "stats_logging.sma").read_text(encoding="utf-8")
CAPTURE = (ROOT / "plugins" / "dod" / "ktp_stats_capture.inc").read_text(encoding="utf-8")


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


def test_life_boundaries_have_truthful_priority_queue() -> None:
    life_queue = function_body(CAPTURE, "stock bool:ksc_life_buffer")
    assert "KSC_LIFE_BUF_MAX_ENTRIES" in life_queue
    assert "g_kscLifeDropped++" in life_queue
    assert "return false" in life_queue
    assert "return true" in life_queue

    emit = function_body(CAPTURE, "stock bool:ksc_emit_life_boundary")
    assert "return ksc_life_buffer(line)" in emit
    assert "ksc_buffer(line)" not in emit

    flush = function_body(CAPTURE, "stock ksc_flush")
    before(flush, "g_kscLifeBufferCount", "g_kscBufferCount")
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
    assert re.search(r'#define\s+PLUGIN_VERSION\s+"1\.16\.0"', STATS)


def main() -> None:
    tests = [value for name, value in sorted(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} life-boundary contract tests")


if __name__ == "__main__":
    main()
