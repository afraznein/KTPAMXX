#!/usr/bin/env bash
# Pre-push hook for KTPAMXX.
#
# A KTPAMXX break silently corrupts every downstream plugin that depends on
# amxxpc or the runtime (MatchHandler, HLTVRecorder, CvarChecker, our
# KTPHudObserver, etc.). This hook runs:
#   1. make build-amxx    — the scripting platform itself compiles
#   2. make build-plugins — the fresh amxxpc can still compile plugins
#
# Step 2 is the bit that catches API-breaking changes (removed natives,
# renamed forwards, changed signatures).
#
# Requires KTPInfrastructure checked out as a sibling directory.
# Install with: scripts/install-hooks.sh
# Bypass once  : git push --no-verify
# Disable      : export KTP_SKIP_PREPUSH=1
set -euo pipefail

if [[ "${KTP_SKIP_PREPUSH:-0}" == "1" ]]; then
  echo "[pre-push] KTP_SKIP_PREPUSH=1 — skipping build"
  exit 0
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"

# Cheap gates before the Docker builds. CRLF-mangled scripts are invisible to
# `git status` (the index is LF) and make `set -e` fail as a command, so the
# script runs on unguarded. The CR probe is the load-bearing half: Git Bash's
# bash tolerates CRLF, so `bash -n` alone passes here and only fails under WSL.
# grep needs -U on Windows -- text mode strips the CR before matching sees it.
CR="$(printf '\r')"
GATE_FAIL=0
while IFS= read -r sh; do
  if grep -qU "$CR" "$REPO_ROOT/$sh"; then
    echo "[pre-push] CRLF line endings in: $sh (fails under WSL bash; renormalize it)"
    GATE_FAIL=1
  fi
  if ! bash -n "$REPO_ROOT/$sh"; then
    echo "[pre-push] bash -n FAILED: $sh"
    GATE_FAIL=1
  fi
done < <(git -C "$REPO_ROOT" ls-files '*.sh')
if [[ "$GATE_FAIL" -ne 0 ]]; then
  echo "[pre-push] shell script gate failed — push aborted."
  exit 1
fi

INFRA_DIR="$REPO_ROOT/../KTPInfrastructure"

if [[ ! -d "$INFRA_DIR" ]]; then
  echo "[pre-push] KTPInfrastructure not found at $INFRA_DIR"
  echo "[pre-push] Clone it as a sibling dir, or bypass with --no-verify"
  exit 1
fi

# KTPInfrastructure's Makefile passes $KTP_PROJECT_ROOT as the Docker build context.
# On Windows (git-bash/msys), the default fallback resolves to a broken relative
# path (observed as "<cwd>/d/Git"), so the build aborts before compiling.
# Export a forward-slash Windows path (d:/Git) when unset — Docker Desktop accepts it.
if [[ -z "${KTP_PROJECT_ROOT:-}" ]]; then
  case "${OSTYPE:-}" in
    msys*|cygwin*|win32*)
      export KTP_PROJECT_ROOT="$(cd "$REPO_ROOT/.." && pwd -W)"
      echo "[pre-push] Windows detected — set KTP_PROJECT_ROOT=$KTP_PROJECT_ROOT"
      ;;
  esac
fi

VERSION="prepush-$(date +%Y%m%d-%H%M%S)"

cd "$INFRA_DIR"
echo "[pre-push] make build-amxx VERSION=$VERSION"
echo "[pre-push] (bypass with --no-verify or KTP_SKIP_PREPUSH=1)"

if ! make build-amxx VERSION="$VERSION"; then
  echo "[pre-push] AMXX BUILD FAILED — push aborted."
  exit 1
fi

echo "[pre-push] make build-plugins VERSION=$VERSION"
if ! make build-plugins VERSION="$VERSION"; then
  echo "[pre-push] PLUGIN BUILD FAILED — the amxxpc produced by this KTPAMXX"
  echo "[pre-push] checkout cannot compile downstream plugins."
  echo "[pre-push] Push aborted. Bypass with --no-verify only if you know why."
  exit 1
fi

echo "[pre-push] build OK (artifacts: $INFRA_DIR/artifacts/$VERSION/)"
