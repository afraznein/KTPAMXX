# Handover: this worktree is now tracked in the coordination repo

**Coordination workstream:** `infra-hitreg-diagnostics`
**Read first:** `G:\GIT\KTP\coordination\state\infra-hitreg-diagnostics.md`

Goal: diagnose the fleet-wide hit→damage gap. 7 diagnostic PRs across this repo, KTPHLStatsX,
KTPMatchHandler, and KTPInfrastructure are already merged to main/preprod — **nothing deployed,
flag off**. Deliberately holding: no Lane B/preprod exercise or fleet deploy until well after
S10 matches finish tonight, per explicit user instruction. Don't advance past that without
checking first.

This checkout sits in a heavily shared tree — `git fetch` before trusting your local ref is
current; other sessions' merges have landed here with no other signal. Going forward, launch
sessions for this work through the coordination repo (`G:\GIT\KTP\coordination\start.ps1` or
`sync-dirs.ps1` + a VS Code tab — see its README) and run `/handoff` before stopping.
