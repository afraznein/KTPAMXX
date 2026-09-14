#!/usr/bin/env python3
"""Deterministic source-contract tests for DODX's once-per-map grenade slot log.

The registry falls back to DoD's fixed-order 9/11 for any grenade slot it never
observed, so a map where WeaponList reported 9/11 and a map where nothing was
reported return the same slot, and the drift tripwire is silent on both. The
"[DODX] grenade slots" line is the only thing that tells them apart. These
assert what keeps it honest: both sources say where a slot came from, the line
is latched to the registry epoch, an unobserved map is flushed before the epoch
moves on, and the strict switch defaults off.

Behavioural coverage needs a live server: grep the AMXX log for
"[DODX] grenade slots" on each map. Run:

    python3 scripts/test_grenade_slot_log.py            # check this tree
    python3 scripts/test_grenade_slot_log.py --selftest # prove the gate can fail
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
DODX = ROOT / "modules" / "dod" / "dodx"
MODULECONFIG = (DODX / "moduleconfig.cpp").read_text(encoding="utf-8")
USERMSG = (DODX / "usermsg.cpp").read_text(encoding="utf-8")
NBASE = (DODX / "NBase.cpp").read_text(encoding="utf-8")

LOG_FORMAT = '"[DODX] grenade slots map=%s pdata_offset=%d w13=%d(%s) w14=%d(%s)"'
FLUSH = "\tif (s_grenadeSlotMap[0] && s_grenadeSlotLoggedEpoch != g_ammoRegistryEpoch)\n\t\tDODX_LogGrenadeSlots();\n"


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
    """Body of a function definition; a declaration ending in ';' never matches."""
    match = re.search(re.escape(signature) + r"\s*\{", source)
    assert match, f"missing definition: {signature} -- dead probe"
    return block_at(source, match.end() - 1)


# Checks take their sources so the selftest can hand them mutated text.

def check_log_line(mc: str) -> None:
    body = definition_body(mc, "static void DODX_LogGrenadeSlots()")
    assert LOG_FORMAT in body, (
        "the per-map grenade slot line lost its format. Operators grep it on every "
        "pool map; map=, pdata_offset=, w13= and w14= are the contract."
    )
    assert "s_grenadeSlotLoggedEpoch = g_ammoRegistryEpoch;" in body, (
        "DODX_LogGrenadeSlots must latch the epoch it logged, or a later detection "
        "and the map-end flush both write the same map."
    )
    for weapon in (13, 14):
        assert f"DODX_GrenadeAmmoIndex({weapon})" in body, (
            f"weapon {weapon}'s slot must be the RESOLVED value the natives use, not "
            "the raw table, or a fallback map prints -1 and reads as a fault."
        )


def check_clear_flushes_before_the_epoch_moves(mc: str) -> None:
    body = definition_body(mc, "void DODX_ClearAmmoRegistry()")
    flush = body.find("DODX_LogGrenadeSlots();")
    wipe = body.find("g_ammoIndexByWeapon[i] = -1;")
    bump = body.find("++g_ammoRegistryEpoch;")
    assert wipe >= 0 and bump >= 0, "the clear no longer wipes or bumps -- dead probe"
    assert flush >= 0, (
        "DODX_ClearAmmoRegistry no longer flushes the grenade slot line. A map where "
        "nothing was observed is only ever logged there."
    )
    assert flush < wipe and flush < bump, (
        "the flush must run before the table is wiped and the epoch bumped, or it "
        "logs an empty registry under the previous map's name."
    )
    assert "s_grenadeSlotLoggedEpoch != g_ammoRegistryEpoch" in body[:flush], (
        "the map-end flush must skip a map that already logged on detection."
    )


def check_both_sources_report(mc: str, usermsg: str) -> None:
    weaponlist = definition_body(usermsg, "void Client_WeaponList(void* mValue)")
    assert "DODX_NoteGrenadeSlotSource(wpnId, DODX_SLOT_SRC_WEAPONLIST);" in weaponlist, (
        "WeaponList fills the registry without saying so, so its slots would log as "
        "fallback: the exact ambiguity this line exists to remove."
    )
    observe = definition_body(mc, "void DODX_ObserveGrenadeAmmoIndex(int grenadeType, int slot)")
    first_fill = re.search(r"if\s*\(\s*observed\s*<\s*0\s*\)\s*\{", observe)
    assert first_fill, "the pickup probe's first-fill branch is gone -- dead probe"
    branch = block_at(observe, first_fill.end() - 1)
    assert "DODX_NoteGrenadeSlotSource(weaponId, DODX_SLOT_SRC_PICKUP);" in branch, (
        "the pickup probe fills an empty slot without saying so, so a map resolved "
        "only through dodx_give_grenade would log as fallback."
    )


def check_detection_logs_once_both_are_observed(mc: str) -> None:
    body = definition_body(mc, "void DODX_NoteGrenadeSlotSource(int weaponId, int source)")
    log = body.find("DODX_LogGrenadeSlots();")
    assert log >= 0, "a detection never writes the line, so only map end would"
    head = body[:log]
    assert re.search(r"if\s*\(\s*s_grenadeSlotLoggedEpoch\s*==\s*g_ammoRegistryEpoch\s*\)\s*return;", head), (
        "WeaponList repeats for every client that joins; without the epoch latch the "
        "line is written per player instead of per map."
    )
    assert head.count("DODX_SLOT_SRC_FALLBACK") >= 2, (
        "detection must wait for BOTH grenades, or a half-resolved map logs early and "
        "the other weapon's later detection is never recorded."
    )


def check_map_name_captured_after_clear(mc: str) -> None:
    body = definition_body(mc, "static void DODX_OnSV_ActivateServer(IVoidHookChain<int> *chain, int runPhysics)")
    clear = body.find("DODX_ClearAmmoRegistry();")
    note = body.find("DODX_NoteGrenadeSlotMap();")
    assert clear >= 0, "activation no longer clears the registry -- dead probe"
    assert note > clear, (
        "the map name must be captured AFTER the clear, which flushes the previous map "
        "under the name it had; capturing first stamps that line with the new map."
    )


def check_strict_defaults_off(mc: str) -> None:
    assert "static bool s_grenadeSlotStrict = false;" in mc, (
        "grenade_slot_strict must default off: on, every unobserved grenade slot "
        "resolves to -1 and the grenade natives stop writing until a slot is observed."
    )
    body = definition_body(mc, "int DODX_GrenadeAmmoIndex(int grenadeType)")
    assert "return s_grenadeSlotStrict ? -1 : DODX_DefaultGrenadeAmmoIndex(weaponId);" in body, (
        "the fallback no longer honours grenade_slot_strict, or no longer returns the "
        "fixed-order default when it is off."
    )


def check_unresolved_log_is_latched(nbase: str) -> None:
    body = definition_body(
        nbase, "static int *DODX_GrenadeAmmoCell(CPlayer *pPlayer, int grenadeType, const char *nativeName)")
    assert "s_unresolvedEpoch != g_ammoRegistryEpoch" in body, (
        "an unresolved slot under grenade_slot_strict must log once per map; callers "
        "poll dodx_get_grenade_ammo per player."
    )


def test_log_line() -> None:
    check_log_line(MODULECONFIG)


def test_clear_flushes_before_the_epoch_moves() -> None:
    check_clear_flushes_before_the_epoch_moves(MODULECONFIG)


def test_both_sources_report() -> None:
    check_both_sources_report(MODULECONFIG, USERMSG)


def test_detection_logs_once_both_are_observed() -> None:
    check_detection_logs_once_both_are_observed(MODULECONFIG)


def test_map_name_captured_after_clear() -> None:
    check_map_name_captured_after_clear(MODULECONFIG)


def test_strict_defaults_off() -> None:
    check_strict_defaults_off(MODULECONFIG)


def test_unresolved_log_is_latched() -> None:
    check_unresolved_log_is_latched(NBASE)


def mutate(source: str, old: str, new: str) -> str:
    assert source.count(old) >= 1, f"selftest mutation anchor missing: {old!r}"
    return source.replace(old, new, 1)


def selftest() -> int:
    """Prove each check still rejects the defect it exists for."""
    flush_late = mutate(mutate(MODULECONFIG, FLUSH, ""),
                        "\t++g_ammoRegistryEpoch;\n", "\t++g_ammoRegistryEpoch;\n" + FLUSH)

    cases = [
        ("the line drops map=",
         check_log_line, mutate(MODULECONFIG, "grenade slots map=%s ", "grenade slots ")),
        ("the line stops latching its epoch",
         check_log_line, mutate(MODULECONFIG, "s_grenadeSlotLoggedEpoch = g_ammoRegistryEpoch;", "(void)0;")),
        ("the line prints the raw table",
         check_log_line,
         mutate(MODULECONFIG, "DODX_GrenadeAmmoIndex(13), DODX_GrenadeSlotSourceName",
                "g_ammoIndexByWeapon[13], DODX_GrenadeSlotSourceName")),
        ("the flush runs after the epoch bump",
         check_clear_flushes_before_the_epoch_moves, flush_late),
        ("the map-end flush removed",
         check_clear_flushes_before_the_epoch_moves, mutate(MODULECONFIG, FLUSH, "")),
        ("WeaponList stops reporting its source",
         lambda s: check_both_sources_report(MODULECONFIG, s),
         mutate(USERMSG, "DODX_NoteGrenadeSlotSource(wpnId, DODX_SLOT_SRC_WEAPONLIST);", "")),
        ("the pickup probe stops reporting its source",
         lambda s: check_both_sources_report(s, USERMSG),
         mutate(MODULECONFIG, "DODX_NoteGrenadeSlotSource(weaponId, DODX_SLOT_SRC_PICKUP);", "")),
        ("detection logs per player",
         check_detection_logs_once_both_are_observed,
         mutate(MODULECONFIG, "\tif (s_grenadeSlotLoggedEpoch == g_ammoRegistryEpoch)\n\t\treturn;\n", "")),
        ("detection logs after one grenade",
         check_detection_logs_once_both_are_observed,
         mutate(MODULECONFIG,
                "if (s_grenadeSlotSource[0] == DODX_SLOT_SRC_FALLBACK || s_grenadeSlotSource[1] == DODX_SLOT_SRC_FALLBACK)",
                "if (s_grenadeSlotSource[weaponId - 13] == DODX_SLOT_SRC_FALLBACK)")),
        ("the map name captured before the clear",
         check_map_name_captured_after_clear,
         mutate(MODULECONFIG, "\tDODX_ClearAmmoRegistry();\n\tDODX_NoteGrenadeSlotMap();\n",
                "\tDODX_NoteGrenadeSlotMap();\n\tDODX_ClearAmmoRegistry();\n")),
        ("strict defaults on",
         check_strict_defaults_off,
         mutate(MODULECONFIG, "static bool s_grenadeSlotStrict = false;", "static bool s_grenadeSlotStrict = true;")),
        ("the unresolved log fires per call",
         check_unresolved_log_is_latched, mutate(NBASE, "s_unresolvedEpoch != g_ammoRegistryEpoch", "true")),
    ]

    failures = []
    for label, check, mutated in cases:
        try:
            check(mutated)
        except AssertionError:
            print(f"PASS selftest rejects: {label}")
            continue
        failures.append(label)

    for label in failures:
        print(f"SELFTEST FAILED: accepted a tree with {label}", file=sys.stderr)
    if failures:
        return 1
    print(f"PASS {len(cases)} selftest mutations, each rejected")
    return 0


def main() -> int:
    if "--selftest" in sys.argv[1:]:
        return selftest()

    tests = [value for name, value in sorted(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} grenade slot log tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
