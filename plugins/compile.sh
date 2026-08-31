#!/bin/bash

# AMX Mod X
#
# by the AMX Mod X Development Team
#  originally developed by OLO
#
# This file is part of AMX Mod X.

# new code contributed by \malex\

set -e

# A failed compile must be VISIBLE, not merely non-zero. Callers pipe this script
# (`| tail`, `| tee`), and the shell then reports the PIPE's status -- so a failure
# reads as exit 0 unless the log itself says so. Gate on the banners below,
# never on the exit code.
_ktp_build_exit() {
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        echo ""
        echo "========================================"
        echo "[KTP-BUILD] FAILED: KTPAMXX plugins/compile.sh exited $rc"
        echo "========================================"
        echo "Nothing has been staged."
    fi
    exit "$rc"
}
trap _ktp_build_exit EXIT

if [ ! -x ./amxxpc ]; then
    echo "ERROR: ./amxxpc is missing or not executable -- build KTPAMXX first."
    exit 1
fi

test -e compiled || mkdir compiled
rm -f temp.txt

failed=0
built=0

for sourcefile in *.sma
do
    # An unmatched glob expands to the pattern itself; compiling "*.sma" would
    # fail for a reason that has nothing to do with the sources.
    [ -f "$sourcefile" ] || continue

    amxxfile="$(echo "$sourcefile" | sed -e 's/\.sma$/.amxx/')"
    outfile="compiled/$amxxfile"

    # Delete first: amxxpc leaves the PREVIOUS .amxx in place when it fails, and a
    # stale artifact satisfying the -s test below would read as a fresh success.
    rm -f "$outfile"

    echo -n "Compiling $sourcefile ..."
    if ./amxxpc "$sourcefile" -o"$outfile" >> temp.txt 2>&1 && [ -s "$outfile" ]; then
        echo "done"
        built=$((built + 1))
    else
        echo "FAILED"
        failed=$((failed + 1))
    fi
done

# `less` blocks (or garbles) when stdout is not a tty, and this runs from builds.
if [ -f temp.txt ]; then
    cat temp.txt
    rm -f temp.txt
fi

# Compiling nothing is a failure too -- it used to report success.
if [ "$failed" -ne 0 ] || [ "$built" -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "BUILD FAILED! (failed: $failed, compiled: $built)"
    echo "========================================"
    exit 1
fi

# Success sentinel, last line on the only path that reaches here. A caller checks
# for this rather than for `$?`, which a pipe launders.
echo "[KTP-BUILD] OK: KTPAMXX plugins/compile.sh"
