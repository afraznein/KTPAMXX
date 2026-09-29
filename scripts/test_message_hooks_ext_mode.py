#!/usr/bin/env python3
"""Source-contract tests for register_message / unregister_message / set_msg_block
in extension mode.

In extension mode nothing reads msgHooks or msgBlocks: only the Metamod engine
hooks C_MessageBegin/C_MessageEnd do, and the ReHLDS IMessageManager handler
serves register_event, core and module handlers only. So these natives refuse in
extension mode and say so in the AMXX log. These assert what keeps that honest:
each refusal is gated on the Metamod flag and comes before the store, it logs
through AMXXLOG_Log rather than raising (a raise aborts the rest of the caller's
plugin_init), register_message refuses with 0 rather than a handle or -1, and the
line names the fix. The last check guards the premise: once the extension-mode
handler reads msgHooks, these refusals are wrong and must go.

Behavioural coverage needs a server: in a Lane B `full` run a plugin that calls
register_message logs the line, and the same plugin's register_event does not.

    python3 scripts/test_message_hooks_ext_mode.py            # check this tree
    python3 scripts/test_message_hooks_ext_mode.py --selftest # prove the gate can fail
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
MESSAGES = (ROOT / "amxmodx" / "messages.cpp").read_text(encoding="utf-8")
META_API = (ROOT / "amxmodx" / "meta_api.cpp").read_text(encoding="utf-8")

GATE = "if (!g_bRunningWithMetamod"
LOGGER = "static void LogNotDispatched(AMX *amx, const char *call, const char *reason)"
HANDLER = "void MessageHook_Handler(IVoidHookChain<IMessage *> *chain, IMessage *msg)"


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


def native_body(source: str, name: str) -> str:
    return definition_body(source, f"static cell AMX_NATIVE_CALL {name}(AMX *amx, cell *params)")


def gated_branch(body: str, name: str):
    start = body.find(GATE)
    assert start >= 0, f"{name} has no extension-mode refusal gated on g_bRunningWithMetamod"
    return start, block_at(body, body.index("{", start))


# Checks take their sources so the selftest can hand them mutated text.

def check_register_message(src: str) -> None:
    body = native_body(src, "register_message")
    start, branch = gated_branch(body, "register_message")
    store = body.find("msgHooks[params[1]].AddHook")
    assert store >= 0, "register_message no longer stores a hook -- dead probe"
    assert start < store, (
        "register_message stores the hook before the extension-mode refusal, so the "
        "handler is registered and silently never runs."
    )
    assert "registerSPForwardByName" not in branch and "AddHook" not in branch, (
        "the extension-mode branch registers a forward nothing will ever call"
    )
    assert "LogNotDispatched(" in branch, "register_message refuses without logging"
    assert branch.rstrip().endswith("return 0;"), (
        "register_message must refuse with 0: -1 is truthy to `if (register_message(...))` "
        "and a positive value reads as a handle."
    )
    assert "use register_event" in branch, "the register_message line must name the fix"


def check_unregister_message(src: str) -> None:
    body = native_body(src, "unregister_message")
    start, branch = gated_branch(body, "unregister_message")
    assert start < body.find("RemoveHook"), "unregister_message reaches RemoveHook in extension mode"
    assert "LogNotDispatched(" in branch and branch.rstrip().endswith("return 0;"), (
        "unregister_message must log and return 0 in extension mode"
    )


def check_set_msg_block(src: str) -> None:
    body = native_body(src, "set_msg_block")
    start, branch = gated_branch(body, "set_msg_block")
    assert start < body.find("msgBlocks[msgid] = block;"), (
        "set_msg_block stores the block before the extension-mode refusal; get_msg_block "
        "would then report a block that is never applied."
    )
    condition = body[start : body.index("{", start)]
    assert "block != BLOCK_NOT" in condition, (
        "restoring BLOCK_NOT (user_silentkill does it on every call) must stay quiet"
    )
    assert "LogNotDispatched(" in branch and branch.rstrip().endswith("return 0;"), (
        "set_msg_block must log and return 0 in extension mode"
    )


def check_refusal_logs_without_raising(src: str) -> None:
    body = definition_body(src, LOGGER)
    assert "AMXXLOG_Log(" in body, "the refusal must reach the AMXX log"
    assert "LogError" not in body and "amx_RaiseError" not in body, (
        "LogError raises a native error, which aborts the rest of the caller's plugin_init"
    )
    assert re.search(r"k\.call\.compare\(call\) == 0\)\s*return;", body), (
        "the refusal must be latched per plugin and call, or a runtime set_msg_block floods the log"
    )
    for name in ("register_message", "unregister_message", "set_msg_block"):
        _, branch = gated_branch(native_body(src, name), name)
        after = branch.split("LogNotDispatched(", 1)[1]
        assert "LogError" not in after, f"{name}'s refusal raises after logging"


def check_the_premise(meta: str) -> None:
    handler = definition_body(meta, HANDLER)
    assert "callNext" in handler, "MessageHook_Handler lost its callNext -- dead probe"
    assert "msgHooks" not in handler and "msgBlocks" not in handler, (
        "MessageHook_Handler now reads msgHooks/msgBlocks, so register_message may be "
        "dispatched in extension mode; retire these refusals."
    )


def test_register_message():
    check_register_message(MESSAGES)


def test_unregister_message():
    check_unregister_message(MESSAGES)


def test_set_msg_block():
    check_set_msg_block(MESSAGES)


def test_refusal_logs_without_raising():
    check_refusal_logs_without_raising(MESSAGES)


def test_the_premise():
    check_the_premise(META_API)


def mutate(source: str, old: str, new: str) -> str:
    assert source.count(old) >= 1, f"selftest mutation anchor missing: {old!r}"
    return source.replace(old, new, 1)


REG_TAIL = '"register_message is not dispatched in extension mode; use register_event");\n\t\t}\n\t\treturn 0;'
UNREG_LOG = ('LogNotDispatched(amx, call,\n\t\t\t\t'
             '"register_message is not dispatched in extension mode, so there is no hook to remove");')


def selftest() -> int:
    """Prove each check still rejects the defect it exists for."""
    cases = [
        ("register_message loses its gate",
         check_register_message,
         mutate(MESSAGES, "\tif (!g_bRunningWithMetamod)\n\t{\n\t\tif (params[1] > 0 && params[1] < 256)\n\t\t{\n\t\t\tint func;",
                "\tif (false)\n\t{\n\t\tif (params[1] > 0 && params[1] < 256)\n\t\t{\n\t\t\tint func;")),
        ("register_message refuses with -1",
         check_register_message, mutate(MESSAGES, REG_TAIL, REG_TAIL.replace("return 0;", "return -1;"))),
        ("register_message stops naming the fix",
         check_register_message, mutate(MESSAGES, "; use register_event", "")),
        ("unregister_message stops logging",
         check_unregister_message, mutate(MESSAGES, UNREG_LOG, "(void)call;")),
        ("set_msg_block stores before refusing",
         check_set_msg_block,
         mutate(MESSAGES, "\t// BLOCK_NOT is already the truth", "\tmsgBlocks[msgid] = block;\n\t// BLOCK_NOT is already the truth")),
        ("set_msg_block logs a BLOCK_NOT restore",
         check_set_msg_block, mutate(MESSAGES, " && block != BLOCK_NOT", "")),
        ("the refusal raises",
         check_refusal_logs_without_raising,
         mutate(MESSAGES, 'AMXXLOG_Log("[AMXX] %s in plugin', 'LogError(amx, AMX_ERR_NATIVE, "[AMXX] %s in plugin')),
        ("the refusal is not latched",
         check_refusal_logs_without_raising,
         mutate(MESSAGES, "k.call.compare(call) == 0)\n\t\t\treturn;", "k.call.compare(call) == 0)\n\t\t\tbreak;")),
        ("the extension handler starts reading msgHooks",
         check_the_premise, mutate(META_API, "chain->callNext(msg);", "chain->callNext(msg); (void)msgHooks;")),
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
    print(f"PASS {len(tests)} extension-mode message hook tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
