#!/usr/bin/env python3
"""Deterministic source-contract tests for the re-entry depth guards.

Three managers defer destructive work while a callback is on the stack — the
task vector, a single-plugin forward, and the message-hook table — and each
must let only the OUTERMOST frame carry that work out. Doing it on an inner
frame frees memory the outer frame is still walking.

This class has now bitten twice. 2.7.20 fixed a CTask active-count
double-decrement by giving one frame sole ownership of a transition; 2.7.25
converted two boolean re-entry guards to depth counters because a nested call
cleared the guard on the inner return — but converted only the sites that
already tested against zero, and CTask's deferred clear tests the deferral
flag rather than the depth, so it kept running at every depth.

Behavioural coverage needs a live server, so these read the sources and assert
the guard shape. Run:

    python3 scripts/test_reentry_guards.py            # check this tree
    python3 scripts/test_reentry_guards.py --selftest # prove the gate can fail
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
CTASK = (ROOT / "amxmodx" / "CTask.cpp").read_text(encoding="utf-8")
CTASK_H = (ROOT / "amxmodx" / "CTask.h").read_text(encoding="utf-8")
CFORWARD = (ROOT / "amxmodx" / "CForward.cpp").read_text(encoding="utf-8")
MESSAGES = (ROOT / "amxmodx" / "messages.h").read_text(encoding="utf-8")


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


# Each check takes its source(s) so the selftest can hand it mutated text. A check
# that only ever ran against the real tree could not be shown to discriminate.

def check_task_depth_is_a_counter(header: str) -> None:
    """A bool re-entry guard is cleared by the inner return — the 2.7.25 defect."""
    declaration = re.search(r"^\s*(\w+)\s+m_bInStartFrame\s*;", header, re.M)
    assert declaration, "m_bInStartFrame declaration not found -- dead probe"
    assert declaration.group(1) == "int", (
        f"m_bInStartFrame is declared {declaration.group(1)}, not int. As a bool a "
        "nested startFrame() clears the guard on the inner return while the outer "
        "one is still iterating."
    )


def check_task_deferred_clear_is_depth_gated(source: str) -> None:
    """Only the outermost startFrame() may destroy the task vector."""
    body = function_body(source, "void CTaskMngr::startFrame()")

    assert "m_Tasks.clear();" in body, "deferred clear not found in startFrame -- dead probe"
    assert "--m_bInStartFrame;" in body, "depth decrement not found -- dead probe"

    guard = re.search(
        r"if\s*\(\s*(?:m_bInStartFrame\s*==\s*0|!\s*m_bInStartFrame)\s*&&\s*m_bDeferredClear\s*\)"
        r"|if\s*\(\s*m_bDeferredClear\s*&&\s*(?:m_bInStartFrame\s*==\s*0|!\s*m_bInStartFrame)\s*\)",
        body,
    )
    assert guard, (
        "the deferred clear in startFrame() is not gated on m_bInStartFrame reaching 0. "
        "m_bInStartFrame is a depth counter, so an inner frame would destroy m_Tasks "
        "while outer frames are still iterating it."
    )

    # The decrement must come first, or the outermost frame tests its own depth as 1
    # and nothing ever performs the clear.
    assert body.index("--m_bInStartFrame;") < guard.start(), (
        "the depth decrement must precede the deferred-clear guard, or the outermost "
        "frame never satisfies it and the clear is dropped entirely."
    )


def check_task_loop_breaks_on_deferral(source: str) -> None:
    """The flag must survive to the outer frame, which relies on this break."""
    body = function_body(source, "void CTaskMngr::startFrame()")
    assert re.search(r"if\s*\(\s*m_bDeferredClear\s*\)\s*\n\s*break\s*;", body), (
        "the iteration loop no longer breaks on m_bDeferredClear. With the clear "
        "deferred to the outermost frame, an inner frame that kept iterating would "
        "walk tasks that clear() already freed."
    )


def check_forward_deferred_delete_is_depth_gated(source: str) -> None:
    """A forward that re-enters itself must not be freed by the inner frame."""
    assert re.search(r"if\s*\(\s*fwd->m_ToDelete\s*&&\s*!\s*fwd->m_InExec\s*\)", source), (
        "the deferred forward free is not gated on m_InExec. Freeing while an outer "
        "execute() of the same forward is on the stack is a use-after-free."
    )


def check_message_hook_cleanup_is_depth_gated(source: str) -> None:
    """The hook table may only be torn down once every nested dispatch has returned."""
    assert re.search(
        r"if\s*\(\s*m_InExecution\.size\(\)\s*==\s*0\s*&&\s*m_Cleanup\s*\)", source
    ), (
        "the deferred message-hook cleanup is not gated on an empty m_InExecution "
        "stack. RemoveHook while a nested dispatch is live frees hooks it is walking."
    )


def test_task_depth_is_a_counter() -> None:
    check_task_depth_is_a_counter(CTASK_H)


def test_task_deferred_clear_is_depth_gated() -> None:
    check_task_deferred_clear_is_depth_gated(CTASK)


def test_task_loop_breaks_on_deferral() -> None:
    check_task_loop_breaks_on_deferral(CTASK)


def test_forward_deferred_delete_is_depth_gated() -> None:
    check_forward_deferred_delete_is_depth_gated(CFORWARD)


def test_message_hook_cleanup_is_depth_gated() -> None:
    check_message_hook_cleanup_is_depth_gated(MESSAGES)


def selftest() -> int:
    """Prove each check still rejects the defect it exists for.

    Every mutation below is the real historical shape, applied to the real source,
    so a check that silently stopped matching shows up here rather than passing
    forever on text it no longer understands.
    """
    cases = [
        (
            "CTask deferred clear ungated (the pre-fix shape)",
            check_task_deferred_clear_is_depth_gated,
            re.sub(
                r"if\s*\(\s*m_bInStartFrame\s*==\s*0\s*&&\s*m_bDeferredClear\s*\)",
                "if (m_bDeferredClear)",
                CTASK,
            ),
        ),
        (
            "CTask clear guarded but decrement moved after it",
            check_task_deferred_clear_is_depth_gated,
            CTASK.replace(
                "\t--m_bInStartFrame;\n", "\t/* moved */\n", 1
            ).replace("\t}\n}\n\nvoid CTaskMngr::clear()",
                      "\t}\n\t--m_bInStartFrame;\n}\n\nvoid CTaskMngr::clear()", 1),
        ),
        (
            "CTask depth reverted to a bool (the 2.7.25 defect)",
            check_task_depth_is_a_counter,
            CTASK_H.replace("int m_bInStartFrame;", "bool m_bInStartFrame;", 1),
        ),
        (
            "CTask loop no longer breaks on deferral",
            check_task_loop_breaks_on_deferral,
            CTASK.replace("if (m_bDeferredClear)\n\t\t\tbreak;", "/* removed */", 1),
        ),
        (
            "CForward frees while an outer execute() is live",
            check_forward_deferred_delete_is_depth_gated,
            CFORWARD.replace("if (fwd->m_ToDelete && !fwd->m_InExec)",
                             "if (fwd->m_ToDelete)", 1),
        ),
        (
            "message hooks torn down mid-dispatch",
            check_message_hook_cleanup_is_depth_gated,
            MESSAGES.replace("if (m_InExecution.size() == 0 && m_Cleanup)",
                             "if (m_Cleanup)", 1),
        ),
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
    print(f"PASS {len(tests)} re-entry guard contract tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
