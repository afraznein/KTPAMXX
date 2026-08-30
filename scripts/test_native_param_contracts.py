#!/usr/bin/env python3
"""Deterministic source-contract tests for DODX native parameter validation.

The failure these guard is not a missing check — it is the .inc and the native
disagreeing about what a caller may pass. `dodx_send_ammox` documented an
ammo slot and pointed callers at `dodx_get_grenade_ammo_index()`, whose
documented failure return is -1, while the native bounded only `count`. A
caller doing exactly what the docs said could hand it -1.

So these assert the two sides agree: the range the native enforces is the range
the include documents, and an aborting parameter is documented as aborting.

Behavioural coverage needs a live server. Run:

    python3 scripts/test_native_param_contracts.py            # check this tree
    python3 scripts/test_native_param_contracts.py --selftest # prove the gate can fail
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
NBASE = (ROOT / "modules" / "dod" / "dodx" / "NBase.cpp").read_text(encoding="utf-8")
DODX_H = (ROOT / "modules" / "dod" / "dodx" / "dodx.h").read_text(encoding="utf-8")
INC = (ROOT / "plugins" / "include" / "dodx.inc").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    """Return a C++ function body, retaining nested blocks."""
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


def doc_block(source: str, native: str) -> str:
    """Return the /** ... */ immediately preceding a native declaration."""
    decl = re.search(r"^native\s+" + re.escape(native) + r"\s*\(", source, re.M)
    assert decl, f"native {native} not declared in dodx.inc -- dead probe"
    start = source.rfind("/**", 0, decl.start())
    assert start >= 0, f"no doc block above native {native}"
    end = source.find("*/", start)
    assert 0 <= end < decl.start(), f"unterminated doc block above native {native}"
    return source[start:end]


def max_ammo_slots(header: str) -> int:
    match = re.search(r"#define\s+DODX_MAX_AMMO_SLOTS\s+(\d+)", header)
    assert match, "DODX_MAX_AMMO_SLOTS not found -- dead probe"
    return int(match.group(1))


# Checks take their sources so the selftest can hand them mutated text.

def check_send_ammox_bounds_the_slot(source: str, header: str) -> None:
    body = function_body(source, "static cell AMX_NATIVE_CALL dodx_send_ammox")
    limit = max_ammo_slots(header)

    assert "WRITE_BYTE(ammoSlot);" in body, "the slot write is gone -- dead probe"

    guard = re.search(
        r"if\s*\(\s*ammoSlot\s*<\s*0\s*\|\|\s*ammoSlot\s*>=\s*DODX_MAX_AMMO_SLOTS\s*\)",
        body,
    )
    assert guard, (
        "dodx_send_ammox does not bound ammoSlot against DODX_MAX_AMMO_SLOTS. The "
        "include directs callers to dodx_get_grenade_ammo_index(), which returns -1 "
        f"on failure, and MSG_WriteByte truncates silently (-1 becomes {2 ** 8 - 1})."
    )
    assert body.index("WRITE_BYTE(ammoSlot);") > guard.start(), (
        "the bound must precede the message write, or it guards nothing."
    )

    # An index parameter aborts here, matching CHECK_PLAYER in this same native.
    # A clamp would write real ammo to a real slot on a failed lookup.
    tail = body[guard.start():]
    assert re.search(r"MF_LogError\s*\(\s*amx\s*,\s*AMX_ERR_NATIVE", tail), (
        "an out-of-range ammo slot must raise a native error, not log-and-continue. "
        "CHECK_PLAYER aborts for this native's other index parameter; a value clamp "
        "would turn a failed lookup into a write on slot 0."
    )
    assert limit > 0


def check_send_ammox_docs_match_the_code(inc: str, header: str) -> None:
    block = doc_block(inc, "dodx_send_ammox")
    limit = max_ammo_slots(header)

    documented = re.search(r"ammo_slot\s+Raw ammo-type index \((\d+)-(\d+)\)", block)
    assert documented, (
        "dodx_send_ammox's doc block does not state a numeric range for ammo_slot. "
        "It directs callers to a native that can return -1, so the accepted range "
        "has to be written down."
    )
    low, high = int(documented.group(1)), int(documented.group(2))
    assert (low, high) == (0, limit - 1), (
        f"dodx_send_ammox documents ammo_slot as {low}-{high} but the native enforces "
        f"0-{limit - 1} (DODX_MAX_AMMO_SLOTS). The include is the contract plugin "
        "authors read; a drifting literal here is the original defect."
    )

    assert "@error" in block, (
        "the ammo_slot bound raises a native error, which aborts the calling public. "
        "MF_LogError-based aborts must be documented with @error -- dodx_give_grenade "
        "sets that precedent -- or the documented 0 return reads as reachable."
    )


def check_grenade_ammo_index_failure_is_documented(inc: str) -> None:
    """The -1 that makes the bound necessary must stay documented as -1."""
    block = doc_block(inc, "dodx_get_grenade_ammo_index")
    assert "-1" in block, (
        "dodx_get_grenade_ammo_index no longer documents its -1 failure return. "
        "dodx_send_ammox's contract is written against that sentinel."
    )


def test_send_ammox_bounds_the_slot() -> None:
    check_send_ammox_bounds_the_slot(NBASE, DODX_H)


def test_send_ammox_docs_match_the_code() -> None:
    check_send_ammox_docs_match_the_code(INC, DODX_H)


def test_grenade_ammo_index_failure_is_documented() -> None:
    check_grenade_ammo_index_failure_is_documented(INC)


def selftest() -> int:
    """Prove each check still rejects the defect it exists for."""
    ungated = re.sub(
        r"\tif \(ammoSlot < 0 \|\| ammoSlot >= DODX_MAX_AMMO_SLOTS\)\n\t\{.*?\n\t\}\n\n",
        "",
        NBASE,
        flags=re.S,
    )
    clamped = NBASE.replace(
        "MF_LogError(amx, AMX_ERR_NATIVE, \"dodx_send_ammox: ammo slot %d out of range (max %d)\",",
        "MF_Log(\"dodx_send_ammox: ammo slot %d out of range (max %d)\",",
        1,
    )

    cases = [
        ("the slot bound removed entirely (the pre-fix shape)",
         lambda s: check_send_ammox_bounds_the_slot(s, DODX_H), ungated),
        ("the bound downgraded from abort to a log",
         lambda s: check_send_ammox_bounds_the_slot(s, DODX_H), clamped),
        ("the documented range drifts off DODX_MAX_AMMO_SLOTS",
         lambda s: check_send_ammox_docs_match_the_code(s, DODX_H),
         INC.replace("Raw ammo-type index (0-31)", "Raw ammo-type index (0-63)", 1)),
        ("the @error line dropped while the native still aborts",
         lambda s: check_send_ammox_docs_match_the_code(s, DODX_H),
         INC.replace(" * @error            ammo_slot outside 0-31 raises a native error", " * @note              ammo_slot", 1)),
        ("the -1 sentinel undocumented",
         check_grenade_ammo_index_failure_is_documented,
         INC.replace("or -1 if grenade_type is not a grenade", "or a negative value", 1)),
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
    print(f"PASS {len(tests)} native parameter contract tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
