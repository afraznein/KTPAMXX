#!/usr/bin/env python3
"""Fail when the Lane B caller feeds the harness a repository ref from a
different lineage than the harness itself.

`.github/workflows/corpus-regression.yml` calls KTPInfrastructure's
lane-b-stats-e2e.yml, and GitHub requires the `uses:` ref to be a literal — it
cannot be an expression. The reusable workflow defaults every unsupplied `*_ref`
input to its own harness lineage, so any ref the caller DOES supply is an
opportunity to straddle two lineages.

That is not hypothetical. `daemon_ref` was wired to the pull request's base ref,
which is the harness lineage for a PR into preprod and a different one for a PR
into main. It stayed invisible until preprod's harness began requiring a schema
migration that main's daemon did not carry, at which point the lane failed
during artifact assembly on every PR into main, for a reason no PR could fix and
no PR had caused. It would recur on the next migration, because preprod moving
ahead of main is the normal direction of travel.

The rule this enforces:

  - every `*_ref` input except `amxx_ref` is a literal equal to the fixed
    lineage name below, so the harness and everything it assembles come from
    one release train
  - `amxx_ref` is an expression, because it is the artifact under test and
    pinning it would leave the lane measuring something other than the PR
  - the `uses:` pin itself is NOT `preprod` (or any other moving branch name)
    -- it is a sha or tag, so a rename of the reusable workflow's job upstream
    cannot silently stop the required status check
    `corpus-regression / Lane B (corpus, preprod, run 1)` from ever reporting
    again
  - the `lane:` input still composes that exact required context string, so an
    in-repo edit to this file cannot silently rename the context either (it
    cannot see a rename made upstream in KTPInfrastructure -- that half is what
    the `uses:` pin is for)

The lineage check used to compare every `*_ref` against whatever the `uses:`
pin said, because that pin was always the literal branch name `preprod`. Once
`uses:` is a sha, that comparison is meaningless -- no branch name equals a
sha -- so the lineage is now a fixed constant instead of being re-derived from
the pin.

No toolchain, no build, stdlib only:

    python3 support/check_lane_refs.py            # check this tree
    python3 support/check_lane_refs.py --selftest # prove the gate can fail
"""

import argparse
import os
import re
import sys

WORKFLOW = os.path.join(".github", "workflows", "corpus-regression.yml")

# The reusable workflow this caller is expected to invoke. Matching on it keeps the
# check pointed at the Lane B call even if the file grows unrelated jobs.
LANE_B = "KTPInfrastructure/.github/workflows/lane-b-stats-e2e.yml"

# The one ref that must track the pull request rather than the harness.
UNDER_TEST = "amxx_ref"

# The release train every non-UNDER_TEST ref must name. Independent of the `uses:`
# pin on purpose -- see the module docstring.
LINEAGE = "preprod"

# Branch names the `uses:` pin must never be. A literal branch keeps moving after
# it is written, which is the exact defect this file exists to catch.
MOVING_REFS = {"preprod", "main"}

# The local job id this call lives under, and the reusable workflow's own job name
# template with this call's `lane` substituted in. Both are read off `main`/
# `preprod` branch protection as one required status check; composing it here and
# comparing is the only part of that check this file can still perform once the
# `uses:` pin freezes which copy of the upstream template applies.
JOB_ID = "corpus-regression"
REQUIRED_CONTEXT = "{} / Lane B ({}, {}, run 1)".format(JOB_ID, "corpus", LINEAGE)

EXPRESSION = re.compile(r"\$\{\{")


def parse(text):
    """Return (uses_ref, {input: value}, errors) for the Lane B call in `text`.

    Hand-rolled rather than PyYAML so the gate has no install step and cannot be
    skipped by a missing dependency. Only the shape this file actually uses is
    understood: a `uses:` line naming the lane, then a `with:` block of scalars.
    """
    errors = []
    uses_ref = None
    inputs = {}

    lines = text.splitlines()
    lane_at = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("uses:") and LANE_B in stripped:
            lane_at = i
            target = stripped.split("uses:", 1)[1].strip()
            if "@" not in target:
                errors.append(
                    "the Lane B `uses:` has no @ref pin: {!r}".format(target))
            else:
                uses_ref = target.rsplit("@", 1)[1].strip()
            break

    if lane_at is None:
        errors.append("no `uses:` line referencing {} was found in {}".format(
            LANE_B, WORKFLOW))
        return uses_ref, inputs, errors

    # Walk forward to the `with:` block that belongs to this call, then collect the
    # scalars indented under it. `with:` is a SIBLING of `uses:`, at the same indent,
    # so only a strict dedent ends the job -- that is what keeps a later job's inputs
    # from being attributed to this one.
    uses_indent = len(lines[lane_at]) - len(lines[lane_at].lstrip())
    with_indent = None
    for line in lines[lane_at + 1:]:
        if not line.strip() or line.strip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if with_indent is None:
            if indent < uses_indent:
                break
            if line.strip() == "with:":
                with_indent = indent
            continue
        if indent <= with_indent:
            break
        key, sep, value = line.strip().partition(":")
        if sep:
            inputs[key.strip()] = value.strip()

    if with_indent is None:
        errors.append("the Lane B call has no `with:` block")

    return uses_ref, inputs, errors


def check_text(text):
    """Return a list of error strings. Empty means the pairing invariant holds."""
    uses_ref, inputs, errors = parse(text)
    if errors:
        return errors

    if uses_ref in MOVING_REFS:
        errors.append(
            "the Lane B `uses:` is pinned to {!r}, a branch that keeps moving. "
            "A rename of the reusable workflow's job on that branch would mean "
            "the required status check {!r} can never report again. Pin to a "
            "sha or tag instead.".format(uses_ref, REQUIRED_CONTEXT))

    refs = sorted(k for k in inputs if k.endswith("_ref"))
    if not refs:
        # No `*_ref` inputs at all means the extraction died rather than that the
        # call is clean, and a dead probe must not read as agreement.
        return ["extracted no *_ref inputs from the Lane B call -- dead probe"]

    if UNDER_TEST not in inputs:
        errors.append(
            "{} is not passed; the lane would test the harness lineage's own "
            "KTPAMXX instead of this pull request".format(UNDER_TEST))
    elif not EXPRESSION.search(inputs[UNDER_TEST]):
        errors.append(
            "{} is pinned to the literal {!r}. It must follow the pull request "
            "head, or the lane reports on code the PR did not change.".format(
                UNDER_TEST, inputs[UNDER_TEST]))

    for name in refs:
        if name == UNDER_TEST:
            continue
        value = inputs[name]
        if EXPRESSION.search(value):
            errors.append(
                "{} is a GitHub expression ({}). Every ref but {} must be a "
                "literal equal to {!r}, or the harness and the repositories it "
                "assembles can come from different lineages.".format(
                    name, value, UNDER_TEST, LINEAGE))
        elif value != LINEAGE:
            errors.append(
                "{} is {!r} but the harness lineage is {!r}. Straddling two "
                "lineages fails during artifact assembly for reasons unrelated "
                "to the pull request.".format(name, value, LINEAGE))

    # The required status check's name is composed from this call's `lane` input
    # plus the fixed lineage name -- neither the reusable workflow's template nor
    # this call's job id is visible from here, so this cannot catch a rename made
    # upstream (the `uses:` sha pin is what protects against that). What it does
    # catch is an edit to THIS file quietly renaming the context, e.g. changing
    # `lane` away from `corpus`.
    lane_value = inputs.get("lane", "").strip()
    composed = "{} / Lane B ({}, {}, run 1)".format(JOB_ID, lane_value or "full", LINEAGE)
    if composed != REQUIRED_CONTEXT:
        errors.append(
            "this call composes the status check {!r}, not the required {!r}. "
            "Branch protection is waiting for the required context -- fix `lane` "
            "(or update REQUIRED_CONTEXT if the required check itself was "
            "deliberately changed).".format(composed, REQUIRED_CONTEXT))

    return errors


def check(root):
    path = os.path.join(root, WORKFLOW)
    try:
        with open(path, "r", encoding="utf-8") as fp:
            text = fp.read()
    except OSError as exc:
        return ["cannot read {}: {}".format(WORKFLOW, exc)]
    return check_text(text)


SHA_FIXTURE = "3b6ac496c86d59ef81dd23a9c76193be312449d8"


def _fixture(uses_ref=SHA_FIXTURE, amxx="${{ github.event.pull_request.head.sha }}",
             daemon="preprod", infra="preprod", lane="corpus", extra=""):
    return (
        "name: Corpus Regression\n"
        "on:\n"
        "  pull_request:\n"
        "    branches: [preprod, main]\n"
        "jobs:\n"
        "  corpus-regression:\n"
        "    uses: afraznein/" + LANE_B + "@" + uses_ref + "\n"
        "    with:\n"
        "      lane: " + lane + "\n"
        "      infrastructure_ref: " + infra + "\n"
        "      amxx_ref: " + amxx + "\n"
        "      daemon_ref: " + daemon + "\n"
        + extra +
        "    secrets: inherit\n"
    )


def selftest():
    """Prove the gate discriminates, in both directions.

    A gate is only evidence if it still fails on input it is meant to reject, so
    the negative cases below include the exact regression this check exists for.
    """
    failures = []

    def expect_pass(label, text):
        errs = check_text(text)
        if errs:
            failures.append("{}: expected clean, got {}".format(label, errs))

    def expect_fail(label, text):
        if not check_text(text):
            failures.append("{}: expected a failure, got a clean result".format(label))

    expect_pass("all refs on the harness lineage", _fixture())

    expect_fail(
        "daemon_ref follows the PR base ref (the real regression)",
        _fixture(daemon="${{ github.event.pull_request.base.ref || github.ref_name }}"))
    expect_fail(
        "daemon_ref literal from another lineage",
        _fixture(daemon="main"))
    expect_fail(
        "infrastructure_ref drifts off the uses: pin",
        _fixture(infra="main"))
    expect_fail(
        "amxx_ref pinned, so the lane cannot see the PR",
        _fixture(amxx="preprod"))
    expect_fail(
        "matchhandler_ref supplied from a context",
        _fixture(extra="      matchhandler_ref: ${{ github.ref_name }}\n"))
    expect_fail(
        "no @ref on the uses: line",
        _fixture().replace("@" + SHA_FIXTURE + "\n", "\n", 1))
    expect_fail(
        "the Lane B call is absent",
        "name: Corpus Regression\njobs:\n  other:\n    runs-on: ubuntu-latest\n")
    expect_fail(
        "uses: pin regresses to the moving preprod branch (the exact defect "
        "this file exists to catch)",
        _fixture(uses_ref="preprod"))
    expect_fail(
        "uses: pin regresses to main",
        _fixture(uses_ref="main"))
    expect_fail(
        "lane changed away from corpus silently renames the required context",
        _fixture(lane="full"))

    if failures:
        for line in failures:
            print("SELFTEST FAILED: " + line, file=sys.stderr)
        return 1
    print("selftest: the gate accepts a paired call and rejects each way it can straddle lineages")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=".", help="repository root to check")
    ap.add_argument("--selftest", action="store_true",
                    help="prove the gate can fail, then exit")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    errors = check(args.root)
    if errors:
        for err in errors:
            print("::error::" + err, file=sys.stderr)
        return 1

    print("OK: every Lane B ref but {} is a literal on the harness lineage".format(UNDER_TEST))
    return 0


if __name__ == "__main__":
    sys.exit(main())
