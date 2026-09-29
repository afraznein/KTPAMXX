#!/usr/bin/env python3
"""Deterministic source-contract tests for when DODX sends a deferred score.

Client_ObjScore accepts a score only while the module is active and defers its
forwards by 0.25 s, so the control point can be resolved once SetObj has
arrived. A round-winning capture starts the round freeze, and a freeze pauses
the module (dodx_set_stats_paused, or dodstats_pause). If the deferred send
sits behind the pause gate in PreThink, it waits out the freeze, resolves its
CP more than 2 s after SetObj, and the round-winning capture's
KTP_SCORE_EVENT carries flag_index -1. These assert the send runs ahead of the
gate on both PreThink paths, that it stays the only send site, and that the
pause still decides which scores are accepted in the first place.

Behavioural coverage needs a round won by a capture while a match is live;
check flag_index on that capture's KTP_SCORE_EVENT. Run:

    python3 scripts/test_score_send_before_pause.py            # check this tree
    python3 scripts/test_score_send_before_pause.py --selftest # prove the gate can fail
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
DODX = ROOT / "modules" / "dod" / "dodx"
MODULECONFIG = (DODX / "moduleconfig.cpp").read_text(encoding="utf-8")
USERMSG = (DODX / "usermsg.cpp").read_text(encoding="utf-8")
CMISC = (DODX / "CMisc.cpp").read_text(encoding="utf-8")

HELPER = "static void DODX_SendPendingScore(CPlayer *pPlayer)"
CALL = "DODX_SendPendingScore(pPlayer);"
EXT_PRETHINK = "static void DODX_OnPlayerPreThink(IVoidHookChain<edict_t *, float> *chain, edict_t *pEntity, float time)"
MM_PRETHINK = "void PlayerPreThink_Post(edict_t *pEntity)"
SCORE_EVENT_SEND = "MF_ExecuteForward(iFScoreEvent,"


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


def gate_at(body: str) -> int:
    match = re.search(r"if\s*\(\s*!\s*isModuleActive\s*\(\s*\)\s*\)", body)
    assert match, "the PreThink body lost its isModuleActive() gate -- dead probe"
    return match.start()


# Checks take their sources so the selftest can hand them mutated text.

def check_helper_sends_what_was_accepted(mc: str) -> None:
    body = definition_body(mc, HELPER)
    clear = body.find("pPlayer->sendScore = 0;")
    send = body.find(SCORE_EVENT_SEND)
    assert clear >= 0 and send >= 0, "the helper no longer clears or sends -- dead probe"
    assert clear < send, (
        "sendScore must be cleared before the forwards run, or a plugin that re-enters "
        "PreThink work from a forward can send the same score twice."
    )
    assert "isModuleActive" not in body, (
        "the helper must not consult the pause. Client_ObjScore already refused scores "
        "that arrived while paused; one accepted before the freeze has to go out on time."
    )
    assert re.search(r"capDelta\s*>=\s*0\.0f\s*&&\s*capDelta\s*<\s*2\.0f", body), (
        "the CP resolution window changed. It is what the timing of this send protects."
    )
    assert "pPlayer->lastScoreCP = g_lastCapturedCP;" in body
    assert "MF_ExecuteForward(iFScore," in body, "client_score must go out with dod_score_event"


def check_ext_prethink_sends_before_the_gate(mc: str) -> None:
    body = definition_body(mc, EXT_PRETHINK)
    call = body.find(CALL)
    assert call >= 0, (
        "the extension-mode PreThink no longer sends deferred scores, so every score "
        "forward is dead on the fleet."
    )
    assert call < gate_at(body), (
        "the deferred score send is behind the pause gate again. A round-winning capture "
        "then waits out the freeze and its KTP_SCORE_EVENT loses its flag_index."
    )
    init = body.find("pPlayer->PutInServer();")
    assert init >= 0 and init < call, (
        "the send must come after the extension-mode ingame/Init handling, or it can "
        "read a slot that has not been initialised."
    )


def check_metamod_prethink_sends_before_the_gate(mc: str) -> None:
    body = definition_body(mc, MM_PRETHINK)
    call = body.find(CALL)
    assert call >= 0, "the Metamod PreThink no longer sends deferred scores"
    assert call < gate_at(body), "the Metamod PreThink sends behind the pause gate"
    ingame = body.find("if (!pPlayer->ingame)")
    assert ingame >= 0 and ingame < call, "the Metamod send must follow the ingame check"


def check_one_send_site(mc: str) -> None:
    assert mc.count(SCORE_EVENT_SEND) == 1, (
        "dod_score_event is executed from more than one place in moduleconfig.cpp. The "
        "helper is the only send path; a second copy sends a score twice."
    )
    helper = definition_body(mc, HELPER)
    assert SCORE_EVENT_SEND in helper, "the one send site is not the helper"
    assert mc.count(CALL) == 2, "the helper must be called from exactly the two PreThink paths"


def check_pause_still_gates_acceptance(usermsg: str) -> None:
    body = definition_body(usermsg, "void Client_ObjScore(void* mValue)")
    accept = re.search(
        r"if\s*\(\s*\(\s*pPlayer->lastScore\s*=\s*score\s*-\s*\(int\)\(pPlayer->savedScore\)\s*\)\s*&&\s*isModuleActive\s*\(\s*\)\s*\)",
        body,
    )
    assert accept, (
        "Client_ObjScore no longer refuses scores while paused. With the send ahead of "
        "the gate, that would emit freeze-time and dodstats_pause scores."
    )
    branch = block_at(body, body.find("{", accept.end()))
    assert "pPlayer->sendScore = gpGlobals->time + 0.25f;" in branch
    assert "pPlayer->lastScoreCP = -2;" in branch


def check_reset_clears_a_pending_send(cmisc: str) -> None:
    body = definition_body(cmisc, "void CPlayer::restartStats(bool all)")
    branch = block_at(body, body.find("{", body.find("if ( all )")))
    assert "sendScore = 0;" in branch, (
        "the match/half reset must drop a pending send, or a pre-reset score is emitted "
        "into the new context now that a pause no longer holds it."
    )


def run_checks(mc: str, usermsg: str, cmisc: str) -> int:
    checks = (
        lambda: check_helper_sends_what_was_accepted(mc),
        lambda: check_ext_prethink_sends_before_the_gate(mc),
        lambda: check_metamod_prethink_sends_before_the_gate(mc),
        lambda: check_one_send_site(mc),
        lambda: check_pause_still_gates_acceptance(usermsg),
        lambda: check_reset_clears_a_pending_send(cmisc),
    )
    for check in checks:
        check()
    return len(checks)


def ext_body_span(mc: str) -> tuple:
    match = re.search(re.escape(EXT_PRETHINK) + r"\s*\{", mc)
    start = match.end()
    return start, start + len(block_at(mc, match.end() - 1))


def move_ext_call_behind_gate(mc: str) -> str:
    start, end = ext_body_span(mc)
    body = mc[start:end]
    body = body.replace("\t" + CALL + "\n", "", 1)
    body = body.replace("\tpPlayer->PreThink();", "\t" + CALL + "\n\tpPlayer->PreThink();", 1)
    return mc[:start] + body + mc[end:]


def drop_ext_call(mc: str) -> str:
    start, end = ext_body_span(mc)
    return mc[:start] + mc[start:end].replace("\t" + CALL + "\n", "", 1) + mc[end:]


def mutations():
    helper_clear = "\tpPlayer->sendScore = 0;\n\n\t// ObjScore fires BEFORE SetObj"
    yield "ext call moved behind the gate", move_ext_call_behind_gate(MODULECONFIG), USERMSG, CMISC
    yield "ext call dropped", drop_ext_call(MODULECONFIG), USERMSG, CMISC
    yield "metamod call moved behind the gate", MODULECONFIG.replace(
        "\tDODX_SendPendingScore(pPlayer);\n\n\tif ( !isModuleActive() )\n\t\tRETURN_META(MRES_IGNORED);",
        "\tif ( !isModuleActive() )\n\t\tRETURN_META(MRES_IGNORED);\n\n\tDODX_SendPendingScore(pPlayer);", 1
    ), USERMSG, CMISC
    yield "helper consults the pause", MODULECONFIG.replace(
        "\tif (!pPlayer->sendScore || pPlayer->sendScore >= gpGlobals->time)",
        "\tif (!isModuleActive() || !pPlayer->sendScore || pPlayer->sendScore >= gpGlobals->time)", 1
    ), USERMSG, CMISC
    yield "sendScore cleared after the send", MODULECONFIG.replace(
        helper_clear, "\t// ObjScore fires BEFORE SetObj", 1
    ).replace("\tpPlayer->lastScoreCP = -1;\n}\n\nvoid PlayerPreThink_Post",
              "\tpPlayer->lastScoreCP = -1;\n\tpPlayer->sendScore = 0;\n}\n\nvoid PlayerPreThink_Post", 1), USERMSG, CMISC
    yield "second send site", MODULECONFIG.replace(
        "\tpPlayer->PreThink();",
        "\tpPlayer->PreThink();\n\tif (iFScoreEvent >= 0)\n\t\tMF_ExecuteForward(iFScoreEvent, pPlayer->index, 0, 0, -1);", 1
    ), USERMSG, CMISC
    yield "CP window widened", MODULECONFIG.replace("capDelta < 2.0f", "capDelta < 30.0f", 1), USERMSG, CMISC
    yield "arrival no longer gated on the pause", MODULECONFIG, USERMSG.replace(
        "(int)(pPlayer->savedScore)) && isModuleActive() )", "(int)(pPlayer->savedScore)) )", 1
    ), CMISC
    yield "reset keeps a pending send", MODULECONFIG, USERMSG, CMISC.replace(
        "\t\tsendScore = 0;\n\t}", "\t}", 1
    )


def selftest() -> int:
    rejected = 0
    for name, mc, usermsg, cmisc in mutations():
        assert (mc, usermsg, cmisc) != (MODULECONFIG, USERMSG, CMISC), (
            f"mutation '{name}' did not change the source -- its anchor is stale"
        )
        try:
            run_checks(mc, usermsg, cmisc)
        except AssertionError:
            rejected += 1
            continue
        raise AssertionError(f"mutation '{name}' passed every check -- the gate cannot fail")
    return rejected


def main() -> int:
    if "--selftest" in sys.argv[1:]:
        count = selftest()
        print(f"PASS {count} selftest mutations, each rejected")
        return 0
    count = run_checks(MODULECONFIG, USERMSG, CMISC)
    print(f"PASS {count} deferred score send tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
