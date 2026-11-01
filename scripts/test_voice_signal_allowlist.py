#!/usr/bin/env python3
"""Deterministic source-contract tests for the voice/signal emit allowlist.

`ktp_stats_capture.inc`'s client_command forward used to emit any command on the
`voice_`/`signal_` prefix minus two named exclusions. The daemon drops a code
`hlstats_Actions` does not carry, so a deny-list hands it every token a player
invents with a bind -- `voice_bazooka` reached production exactly that way. These
assert the forward cannot reach the emit without passing the allowlist, and that
the allowlist still holds the 43 codes KTPHLStatsX migrations 045 and 048 seed.

The expected names below are restated from those migrations on purpose: a test
that read the allowlist to decide what the allowlist should be would pass on any
list at all. Checking them against the migration files themselves is not an
option here -- that reads a sibling checkout, whose staleness would decide the
verdict.

Behavioural coverage needs a live server: after a match, no `Unresolved action
'voice_` or `'signal_` in the daemon log, against a non-zero count of resolved
`voice_*`/`signal_*` events for the same match.

    python3 scripts/test_voice_signal_allowlist.py            # check this tree
    python3 scripts/test_voice_signal_allowlist.py --selftest # prove the gate can fail
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / "plugins" / "dod" / "ktp_stats_capture.inc"

# KTPHLStatsX migration 048: every signal_* string dod.so handles by name.
SIGNALS = {
    "areaclear", "backup", "coveringfire", "enemyleft", "enemyright",
    "enemyspotted", "fallback", "flankleft", "flankright", "grenade",
    "holdposition", "moveout", "no", "sniper", "sticktogether", "yes",
}
# KTPHLStatsX migration 045: every voice_* string dod.so handles by name, less
# voice_menu<N> and voice_serverdebug, which are not picks and are not seeded.
VOICES = {
    "areaclear", "attack", "backup", "bazookaspotted", "ceasefire", "cover",
    "displace", "enemyahead", "enemybehind", "fallback", "fireinhole",
    "fireleft", "fireright", "gogogo", "grenade", "hold", "left", "mgahead",
    "moveupmg", "needammo", "negative", "right", "sniper", "sticktogether",
    "usebazooka", "usegrens", "yessir",
}
# Codes a prefix match used to admit and the allowlist must not. `bazooka` is a
# player's own bind; the other two share the prefix and are deliberately unseeded.
REJECTED = ("bazooka", "serverdebug", "menu1")


def block_at(source: str, brace: int) -> str:
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError("missing closing brace")


def definition_body(source: str, signature: str) -> str:
    """Body of a definition; brace-matched, so prose quoting it never matches."""
    hits = list(re.finditer(re.escape(signature) + r"\s*\{", source))
    assert len(hits) == 1, (
        f"expected exactly one definition of {signature!r}, found {len(hits)} -- "
        "the probe located nothing or located the wrong thing"
    )
    return block_at(source, hits[0].end() - 1)


def array_names(source: str, declaration: str) -> list[str]:
    """Every string literal in an initialiser, in order, with duplicates kept."""
    hits = list(re.finditer(re.escape(declaration) + r"\s*=\s*\{", source))
    assert len(hits) == 1, (
        f"expected exactly one declaration of {declaration!r}, found {len(hits)} -- dead probe"
    )
    return re.findall(r'"([^"]*)"', block_at(source, hits[0].end() - 1))


# Checks take their source so the selftest can hand them mutated text.

def check_emit_is_guarded(inc: str) -> None:
    body = definition_body(inc, "public client_command(id)")
    emits = [m.start() for m in re.finditer(r"ksc_log_player_action\(id, cmd\)", body)]
    assert len(emits) == 1, (
        f"expected exactly one raw-command emit in client_command, found {len(emits)}. "
        "Each one needs its own allowlist guard and this check only proves one."
    )
    guards = [m.start() for m in re.finditer(r"if \(!ksc_is_seeded_chatter\(cmd\)\)", body)]
    assert len(guards) == 1, (
        f"client_command emits the raw command past no allowlist guard ({len(guards)} found). "
        "A prefix match admits every voice_/signal_ token a player binds; the daemon cannot "
        "resolve one and drops it."
    )
    assert guards[0] < emits[0], (
        "the allowlist guard runs AFTER the emit in client_command, so it gates nothing. "
        "An unseeded code is already on the wire by the time it is rejected."
    )


def check_allowlist_consults_both(inc: str) -> None:
    body = definition_body(inc, "stock bool:ksc_is_seeded_chatter(const cmd[])")
    # Assert on the comparison, not on the identifier: naming the table in a loop
    # bound keeps an `in body` check green while nothing is actually compared.
    for table, compare in (
        ("g_kscSignalNames", "equal(cmd[7], g_kscSignalNames[i])"),
        ("g_kscVoiceMenuSlots", "equal(cmd[6], g_kscVoiceMenuSlots[m][s])"),
    ):
        hits = body.count(compare)
        assert hits == 1, (
            f"ksc_is_seeded_chatter never compares the code against {table} "
            f"({hits} occurrences of that comparison). The family is then admitted or "
            "rejected wholesale, or matched with its prefix still attached -- which no "
            "entry in either table carries."
        )


def check_signal_names(inc: str) -> None:
    names = array_names(inc, "new const g_kscSignalNames[16][]")
    assert len(names) == len(set(names)), f"duplicate hand signal in the allowlist: {names}"
    assert set(names) == SIGNALS, (
        "the hand-signal allowlist no longer matches what KTPHLStatsX migration 048 seeds. "
        f"extra={sorted(set(names) - SIGNALS)} missing={sorted(SIGNALS - set(names))}. "
        "An extra code is emitted and dropped; a missing one loses a real signal."
    )


def check_voice_names(inc: str) -> None:
    names = array_names(inc, "new const g_kscVoiceMenuSlots[3][9][]")
    assert len(names) == len(set(names)), f"duplicate voice name in the slot table: {names}"
    assert set(names) == VOICES, (
        "the voice slot table no longer matches what KTPHLStatsX migration 045 seeds. "
        f"extra={sorted(set(names) - VOICES)} missing={sorted(VOICES - set(names))}. "
        "That table is both the menu-pick map and the voice allowlist, so a wrong entry "
        "mislabels a pick AND admits or drops a bound-key command."
    )


def check_rejects_known_bad(inc: str) -> None:
    allowed = set(array_names(inc, "new const g_kscSignalNames[16][]"))
    allowed |= set(array_names(inc, "new const g_kscVoiceMenuSlots[3][9][]"))
    for bad in REJECTED:
        assert bad not in allowed, (
            f"{bad!r} is in the allowlist. It is not a code hlstats_Actions carries, so "
            "emitting it only feeds the daemon's unresolved-action path."
        )
    # Control: the probe can see the names it is clearing. Without this, a parse
    # that returned nothing would clear every rejected code.
    assert "negative" in allowed and "yes" in allowed, (
        "the allowlist parsed empty or wrong, so the rejection check above cleared "
        "nothing -- a false pass"
    )


CHECKS = (
    check_emit_is_guarded,
    check_allowlist_consults_both,
    check_signal_names,
    check_voice_names,
    check_rejects_known_bad,
)


def run(inc: str) -> None:
    for check in CHECKS:
        check(inc)


# Each mutation names the substring the right failure must contain, so a
# mutation that reddens a DIFFERENT check is reported as a miss, not a pass.
MUTATIONS = (
    (
        "drop the allowlist guard",
        "\tif (!ksc_is_seeded_chatter(cmd))\n\t\treturn PLUGIN_CONTINUE\n",
        "",
        "past no allowlist guard",
    ),
    (
        "guard after the emit",
        "\tif (!ksc_is_seeded_chatter(cmd))\n\t\treturn PLUGIN_CONTINUE\n\tksc_log_player_action(id, cmd)\n",
        "\tksc_log_player_action(id, cmd)\n\tif (!ksc_is_seeded_chatter(cmd))\n\t\treturn PLUGIN_CONTINUE\n",
        "runs AFTER the emit",
    ),
    (
        "admit an unseeded signal",
        '\t"no", "sniper", "sticktogether", "yes"',
        '\t"no", "sniper", "sticktogether", "yes", "wilco"',
        "migration 048 seeds",
    ),
    (
        "drop a seeded signal",
        '"coveringfire", ',
        "",
        "migration 048 seeds",
    ),
    (
        "admit a player's own bind as a voice name",
        '"usebazooka", "bazookaspotted"',
        '"usebazooka", "bazooka"',
        "migration 045 seeds",
    ),
    (
        "stop consulting the voice table",
        "\t\t\tif (equal(cmd[6], g_kscVoiceMenuSlots[m][s]))\n",
        "\t\t\tif (equal(cmd[6], cmd[6]))\n",
        "never compares the code against g_kscVoiceMenuSlots",
    ),
    (
        "match the name with its prefix still on",
        "if (equal(cmd[7], g_kscSignalNames[i]))",
        "if (equal(cmd, g_kscSignalNames[i]))",
        "never compares the code against g_kscSignalNames",
    ),
    # Dead-anchor control: this text is not in the file. It must report
    # "could not apply", which is what separates "my mutation reddened the
    # suite" from "my mutation never landed".
    (
        "DEAD ANCHOR (control)",
        "if (ksc_allowlist_that_never_existed(cmd))",
        "",
        None,
    ),
)


def selftest(inc: str) -> int:
    print("selftest: baseline must be green before any mutation is believable")
    try:
        run(inc)
    except AssertionError as exc:
        print(f"  BASELINE RED -- every mutation below is meaningless: {exc}")
        return 1
    print("  baseline green")

    failures = 0
    for label, old, new, want in MUTATIONS:
        hits = inc.count(old)
        if hits != 1:
            verdict = "could not apply" if hits == 0 else f"ambiguous anchor ({hits} matches)"
            if want is None and hits == 0:
                print(f"  {label}: could not apply -- control behaved")
                continue
            print(f"  {label}: {verdict} -- BROKEN MUTATION, not a result")
            failures += 1
            continue
        if want is None:
            print(f"  {label}: anchor FOUND, so it is not a dead anchor -- control broken")
            failures += 1
            continue
        try:
            run(inc.replace(old, new, 1))
        except AssertionError as exc:
            if want in str(exc):
                print(f"  {label}: caught, on the right assertion")
            else:
                print(f"  {label}: caught on the WRONG assertion -- unearned pass")
                print(f"    wanted {want!r}, got: {exc}")
                failures += 1
            continue
        print(f"  {label}: NOT CAUGHT -- the gate does not discriminate")
        failures += 1

    if failures:
        print(f"FAIL: {failures} of {len(MUTATIONS)} mutations did not behave")
        return 1
    print(f"OK: all {len(MUTATIONS)} mutations behaved (one of them the dead-anchor control)")
    return 0


def main() -> int:
    inc = INC.read_text(encoding="utf-8")
    if "--selftest" in sys.argv:
        return selftest(inc)
    run(inc)
    print(f"OK: the voice/signal emit is allowlisted ({len(VOICES)} voice + {len(SIGNALS)} signal codes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
