#!/usr/bin/env python3
"""Source-contract test for the zero-initialised plugin image in amxxpc.

`ReadFileIntoPl()` sizes the image as `AMX_HEADER.size + AMX_DBG_HDR.size`,
allocates that and `fread`s the `.amx` into it. `AMX_DBG_HDR.size` overstates
what `sc6.c`'s `append_dbginfo()` writes, so the read is short by that
difference and whatever the allocator handed back ships in the `.amxx`. The
one character that stops it is the `()` on the `new char[size]()`.

A same-md5 reproducibility check is deliberately NOT the gate here, and that
is the point worth carrying: the tail only varies between runs when the heap
it comes off varies. Measured on one workstation while closing #131, three
back-to-back builds with the allocation's `()` removed gave ONE md5, because
the tail held recycled compiler text rather than pointers -- so a build-twice
-and-compare test passes on a tree that ships uninitialised memory. What makes
the artifact reproducible is that the tail cannot hold anything, and that is a
property of this allocation, not of a sample of builds.

    python3 scripts/test_amxxpc_image_zeroinit.py            # check this tree
    python3 scripts/test_amxxpc_image_zeroinit.py --selftest # prove the gate can fail
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
AMXXPC = (ROOT / "compiler" / "amxxpc" / "amxxpc.cpp").read_text(encoding="utf-8")

SIGNATURE = "void ReadFileIntoPl(abl *pl, FILE *fp)"
ALLOC = re.compile(r"pl->data\s*=\s*new\s+char\s*\[\s*size\s*\]\s*(\(\s*\))?\s*;")


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


# Checks take their source so the selftest can hand them mutated text.

def check_image_is_zero_initialised(src: str) -> None:
    body = definition_body(src, SIGNATURE)
    match = ALLOC.search(body)
    assert match, "ReadFileIntoPl no longer allocates pl->data with new char[size] -- dead probe"
    assert match.group(1), (
        "the plugin image allocation lost its (), so the tail the short fread never "
        "reaches ships as raw heap and the artifact stops rebuilding to its own md5"
    )


def check_the_allocation_is_the_only_filler(src: str) -> None:
    """A memset after the read would zero the whole image including what was read."""
    body = definition_body(src, SIGNATURE)
    alloc = ALLOC.search(body)
    assert alloc, "dead probe"
    read = body.find("fread(pl->data")
    assert read >= 0, "ReadFileIntoPl no longer reads into pl->data -- dead probe"
    assert alloc.start() < read, "the image is allocated after it is read into"
    assert "memset(pl->data" not in body and "memset((void *)pl->data" not in body, (
        "a memset here runs either before the read (redundant with the zero-init) or "
        "after it (erasing the image); the allocation is where the zeroing belongs"
    )


def check_the_premise_short_read(src: str) -> None:
    """The zero-init matters because the read is short. If the sizing is ever
    corrected the tail disappears, and this comment's reasoning has to be revisited
    rather than silently kept."""
    body = definition_body(src, SIGNATURE)
    assert "size += dbg.size;" in body, (
        "the image is no longer sized by AMX_DBG_HDR.size, so the short read this "
        "zero-init covers may be gone; re-derive before trusting the comment on it"
    )
    assert re.search(r"fread\(pl->data,\s*1,\s*size,\s*fp\);", body), (
        "the read into the image changed shape -- re-check that it is still short"
    )


def check_the_tripwire_survives(src: str) -> None:
    """The () is one character with no local reason visible. Without the comment the
    next reader removes it as noise, which is how this defect would come back."""
    body = definition_body(src, SIGNATURE)
    head = body[: ALLOC.search(body).start()]
    assert "Zero-init is load-bearing" in head, (
        "the comment explaining why the allocation is zero-initialised is gone; the () "
        "then reads as a stray and gets tidied away"
    )


def test_image_is_zero_initialised():
    check_image_is_zero_initialised(AMXXPC)


def test_the_allocation_is_the_only_filler():
    check_the_allocation_is_the_only_filler(AMXXPC)


def test_the_premise_short_read():
    check_the_premise_short_read(AMXXPC)


def test_the_tripwire_survives():
    check_the_tripwire_survives(AMXXPC)


def mutate(source: str, old: str, new: str) -> str:
    assert source.count(old) >= 1, f"selftest mutation anchor missing: {old!r}"
    return source.replace(old, new, 1)


def selftest() -> int:
    """Prove each check still rejects the defect it exists for."""
    cases = [
        ("the zero-init is simplified away",
         check_image_is_zero_initialised,
         mutate(AMXXPC, "new char[size]();", "new char[size];")),
        ("the image is zeroed after the read instead",
         check_the_allocation_is_the_only_filler,
         mutate(AMXXPC, "\trewind(fp);", "\tmemset(pl->data, 0, size);\n\trewind(fp);")),
        ("the sizing stops reaching for the debug header",
         check_the_premise_short_read,
         mutate(AMXXPC, "\t\tsize += dbg.size;", "\t\t(void)dbg;")),
        ("the read into the image changes shape",
         check_the_premise_short_read,
         mutate(AMXXPC, "fread(pl->data, 1, size, fp);", "fread(pl->data, size, 1, fp);")),
        ("the tripwire comment is tidied away",
         check_the_tripwire_survives,
         mutate(AMXXPC, "\t// Zero-init is load-bearing", "\t// allocate")),
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
    print(f"PASS {len(tests)} amxxpc image zero-init tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
