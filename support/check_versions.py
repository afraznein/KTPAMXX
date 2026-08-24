#!/usr/bin/env python3
"""Fail when product.version, the README version literals, and the newest
CHANGELOG release heading disagree.

Only `product.version` is wired to consumers (AMBuildScript, support/Versioning,
support/buildbot/package.py, support/generate_headers.py). The README literals are
hand-copied with no generator and nothing reading them, which is why they are the
ones that drift. Deleting them is the better fix; until then this is what notices.

No toolchain, no build, stdlib only:

    python3 support/check_versions.py            # check this tree
    python3 support/check_versions.py --selftest # prove the gate can fail
"""

import argparse
import os
import re
import shutil
import sys
import tempfile

SEMVER = r"(\d+\.\d+\.\d+)"

# Hand-maintained duplicates of product.version. Historical mentions elsewhere in the
# README ("2.7.19+", "2.7.25 adds no new engine-version floor") are prose about past
# releases rather than claims about the current one, so they are deliberately not matched.
README_ANCHORS = [
    ("title banner", re.compile(r"^\*\*Version\s+" + SEMVER + r"\*\*", re.M)),
    ("Current Version bullet", re.compile(r"^-\s*\*\*Current Version\*\*:\s*" + SEMVER, re.M)),
]

# Escape hatch for the recommended end state: delete the README literals and leave this
# marker, so the check stops demanding anchors instead of needing an edit here.
README_OPT_OUT = "<!-- version-check: no-version-literal -->"

CHANGELOG_HEADING = re.compile(r"^##\s*\[" + SEMVER + r"\]", re.M)


def _read(root, name, errors):
    try:
        with open(os.path.join(root, name), "r", encoding="utf-8") as fp:
            return fp.read()
    except OSError as exc:
        errors.append("cannot read {}: {}".format(name, exc))
        return None


def check(root):
    """Return (found, errors). found is a list of (source description, version)."""
    errors = []
    found = []

    raw = _read(root, "product.version", errors)
    if raw is not None:
        text = raw.strip()
        if not re.match(r"^" + SEMVER + r"$", text):
            errors.append("product.version is not a bare x.y.z version: {!r}".format(text))
        else:
            found.append(("product.version", text))

    readme = _read(root, "README.md", errors)
    if readme is not None:
        opted_out = README_OPT_OUT in readme
        for label, pattern in README_ANCHORS:
            matches = pattern.findall(readme)
            if not matches:
                # A reformatted README matching nothing would let this check pass while
                # measuring nothing, so a missing anchor is itself a failure.
                if not opted_out:
                    errors.append(
                        "README.md: {} anchor not found. Either restore the line, or delete every "
                        "README version literal and add the marker {}".format(label, README_OPT_OUT))
                continue
            if opted_out:
                errors.append(
                    "README.md carries the no-version-literal marker but still states a version in "
                    "the {} ({}). Remove one or the other.".format(label, ", ".join(matches)))
            for version in matches:
                found.append(("README.md ({})".format(label), version))

    changelog = _read(root, "CHANGELOG.md", errors)
    if changelog is not None:
        headings = CHANGELOG_HEADING.findall(changelog)
        if not headings:
            errors.append("CHANGELOG.md: no '## [x.y.z]' release heading found")
        else:
            # `## [Unreleased]` carries no semver and is skipped by the pattern.
            found.append(("CHANGELOG.md (newest release heading)", headings[0]))

    versions = {version for _, version in found}
    if len(versions) > 1:
        errors.insert(0, "version literals disagree: {}".format(", ".join(sorted(versions))))

    return found, errors


def report(root, found, errors):
    print("KTPAMXX version-consistency check")
    print("  root: {}".format(root))
    for source, version in found:
        print("  {:46s} {}".format(source, version))
    if errors:
        print("")
        print("FAIL: version literals are inconsistent.")
        for err in errors:
            print("  - {}".format(err))
        print("")
        print("product.version is the wired source of truth (AMBuildScript, support/Versioning,")
        print("support/buildbot/package.py and support/generate_headers.py all read it). Bring the")
        print("README and CHANGELOG into line with it, or delete the README literal for good.")
        return 1
    versions = {version for _, version in found}
    print("")
    print("OK: all version literals agree on {}".format(versions.pop() if versions else "(none)"))
    return 0


# --- self-test -------------------------------------------------------------------
# A gate that only ever passes proves nothing, so CI runs this before the real check.

def _fixture(tmp, name, product, readme, changelog):
    root = os.path.join(tmp, name)
    os.makedirs(root)
    if product is not None:
        with open(os.path.join(root, "product.version"), "w", encoding="utf-8") as fp:
            fp.write(product)
    if readme is not None:
        with open(os.path.join(root, "README.md"), "w", encoding="utf-8") as fp:
            fp.write(readme)
    if changelog is not None:
        with open(os.path.join(root, "CHANGELOG.md"), "w", encoding="utf-8") as fp:
            fp.write(changelog)
    return root


def _readme(banner, bullet, extra=""):
    return (
        "# KTP AMX\n\n"
        "**Version {}** | Modified AMX Mod X\n\n"
        "Some prose mentioning 2.7.19+ and 2.7.25 behaviour, which must be ignored.\n\n"
        "- **Current Version**: {} (2026-08)\n{}".format(banner, bullet, extra))


def selftest():
    cases = []
    tmp = tempfile.mkdtemp(prefix="ktpamxx-versioncheck-")
    try:
        # product.version deliberately has no trailing newline, matching the real file.
        cases.append(("consistent", 0, _fixture(
            tmp, "consistent", "2.7.32", _readme("2.7.32", "2.7.32"),
            "# Changelog\n\n## [Unreleased]\n\n## [2.7.32] - 2026-08-18\n\n## [2.7.31] - 2026-08-16\n")))

        cases.append(("readme lags product.version", 1, _fixture(
            tmp, "readme_lags", "2.7.32", _readme("2.7.31", "2.7.31"),
            "# Changelog\n\n## [2.7.32] - 2026-08-18\n")))

        cases.append(("changelog lags product.version", 1, _fixture(
            tmp, "changelog_lags", "2.7.32", _readme("2.7.32", "2.7.32"),
            "# Changelog\n\n## [2.7.31] - 2026-08-16\n")))

        cases.append(("one README anchor stale", 1, _fixture(
            tmp, "half_stale", "2.7.32", _readme("2.7.32", "2.7.31"),
            "# Changelog\n\n## [2.7.32] - 2026-08-18\n")))

        cases.append(("README anchors removed without the marker", 1, _fixture(
            tmp, "anchors_gone", "2.7.32", "# KTP AMX\n\nNo version here at all.\n",
            "# Changelog\n\n## [2.7.32] - 2026-08-18\n")))

        cases.append(("README anchors removed with the marker", 0, _fixture(
            tmp, "opted_out", "2.7.32",
            "# KTP AMX\n\n" + README_OPT_OUT + "\n\nNo version literal here.\n",
            "# Changelog\n\n## [2.7.32] - 2026-08-18\n")))

        cases.append(("marker present but a literal remains", 1, _fixture(
            tmp, "opt_out_contradicted", "2.7.32",
            README_OPT_OUT + "\n" + _readme("2.7.32", "2.7.32"),
            "# Changelog\n\n## [2.7.32] - 2026-08-18\n")))

        cases.append(("CHANGELOG has only Unreleased", 1, _fixture(
            tmp, "only_unreleased", "2.7.32", _readme("2.7.32", "2.7.32"),
            "# Changelog\n\n## [Unreleased]\n\n- nothing yet\n")))

        cases.append(("product.version malformed", 1, _fixture(
            tmp, "bad_product", "2.7", _readme("2.7.32", "2.7.32"),
            "# Changelog\n\n## [2.7.32] - 2026-08-18\n")))

        cases.append(("files missing entirely", 1, _fixture(
            tmp, "empty", None, None, None)))

        failures = 0
        for name, expected, root in cases:
            _, errors = check(root)
            actual = 1 if errors else 0
            ok = actual == expected
            if not ok:
                failures += 1
            print("  [{}] {:46s} expected {} got {}".format(
                "ok" if ok else "BAD", name, expected, actual))

        print("")
        if failures:
            print("SELFTEST FAILED: {} of {} cases wrong".format(failures, len(cases)))
            return 1
        passed = sum(1 for _, e, _ in cases if e == 0)
        print("SELFTEST OK: {} cases, {} expected-pass and {} expected-fail, "
              "so the gate discriminates in both directions".format(
                  len(cases), passed, len(cases) - passed))
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."),
                    help="repository root (default: parent of this script)")
    ap.add_argument("--selftest", action="store_true",
                    help="run the gate against synthetic fixtures and exit")
    args = ap.parse_args()

    if args.selftest:
        print("KTPAMXX version-consistency self-test")
        return selftest()

    root = os.path.abspath(args.root)
    found, errors = check(root)
    return report(root, found, errors)


if __name__ == "__main__":
    sys.exit(main())
