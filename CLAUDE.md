# KTPAMXX - Claude Code Context

> **IPs here are placeholders** — this repo is public. Real addresses resolve in
> the private root context (`KTP Git Projects/CLAUDE.md` § IP Addresses),
> which is deliberately not in any git repository.

**REQUIRED: Before modifying any C++ or include files in this repo, invoke the `cpp-dev` skill** (`.claude/skills/cpp-dev/SKILL.md`). It carries the fork-delta rules, the extension-mode init-parity trap, and the build/verify workflow; do not edit source without it loaded.

🔴 **A design that ships as a document is NOT done** — every proposal in a docs-only PR becomes a
tracked board item in the same act (operator ruling 2026-09-14). See `DESIGN_DOCS_ARE_NOT_DONE.md`.

## Build Command
To build this project, use:
```bash
wsl bash -c "cd '/mnt/n/Nein_/KTP Git Projects/KTPAMXX' && bash build_linux.sh"
```

This will:
1. Build KTPAMXX using AMBuild (Python-based build system)
2. Output to `obj-linux/packages/`
3. Auto-stage to `N:\Nein_\KTP Git Projects\KTP DoD Server\serverfiles\dod\addons\ktpamx\`

⛔ **Build from a real clone, never a git worktree.** `support/Versioning` opens
`<sourcePath>/.git/HEAD` as a path and a worktree's `.git` is a FILE, so the build dies at
`NotADirectoryError` before anything compiles — and callers pipe this script, so the shell still
reports exit 0. Judge it by `build_linux.sh`'s `[KTP-BUILD]` banners, never the exit code.

## Project Structure
- `build_linux.sh` - Main WSL build script
- `configure.py` - AMBuild configuration
- `obj-linux/` - Build output directory
- `plugins/include/` - AMX include files (used by plugin compiler)
- `obj-linux/packages/base/addons/ktpamx/scripting/amxxpc` - Linux plugin compiler

## Build Output
| File | Destination |
|------|-------------|
| `ktpamx_i386.so` | Main AMXX binary |
| `dodx_ktp_i386.so` | DoD stats module |

## Purpose
Custom AMX Mod X fork that operates in extension mode (loaded directly by KTP-ReHLDS without Metamod). This is the scripting platform layer that provides the plugin compiler and runtime.

## Key Include Files
| Include | Purpose |
|---------|---------|
| `dodx.inc` | DODX natives including score/stats functions |
| `ktp_discord.inc` | Shared Discord integration (curl-based) |
| `reapi_engine_const.inc` | KTP-ReHLDS hook constants |

## DODX Natives (KTP Additions)

### Score Management
| Native | Purpose |
|--------|---------|
| `dodx_has_gamerules()` | Check if gamerules pointer is valid |
| `dodx_set_team_score(team, score)` | Set team score in gamerules |
| `dodx_get_team_score(team)` | Get team score from gamerules |
| `dodx_broadcast_team_score(team, score)` | Broadcast TeamScore message to clients |
| `dodx_set_scoreboard_team_name(team, name)` | Set custom team name on scoreboard |

### Round Timer (v2.7.23+)
| Native | Purpose |
|--------|---------|
| `Float:dodx_get_round_time()` | Engine-authoritative seconds remaining in the current half (reads CDoDTeamPlay gamerules; projects through the clan-restart countdown; `-1.0` fail-soft). For the KTPHudObserver closed-loop broadcast clock. |

### HLStatsX Integration
| Native | Purpose |
|--------|---------|
| `dodx_set_match_id(id)` | Set match ID for stats correlation |
| `dodx_get_match_id(output[], maxlen)` | Get current match ID |
| `dodx_flush_all_stats()` | Fire dod_stats_flush for all players |
| `dodx_reset_all_stats()` | Clear all accumulated stats |

### Player Data
| Native | Purpose |
|--------|---------|
| `dodx_set_pl_teamname(id, szName[])` | Set player's team name in private data |

### Grenade Manipulation (v2.6.4+)

Ammo lives in `CBasePlayer::m_rgAmmo` (int-offset **285** on Linux, byte `0x474`, measured in
`dod_i386.so` md5 `4f4727b2…`) at an ammo-type index. As of v2.7.29 DODX reads that index live from
the DLL's `WeaponList` and from what a `dodx_give_grenade` pickup credits, rather than hardcoding it,
and logs if either disagrees with DoD's fixed precache order. Setting ammo no longer needs a
follow-up `dodx_send_ammox` — the DLL emits its own `AmmoX` on the right slot.

**Confirming the resolved slots on a live map:** grep the AMXX log (`addons/ktpamx/logs/L*.log`) for
`[DODX] grenade slots`. One line per map, e.g.
`[DODX] grenade slots map=dod_anzio pdata_offset=5 w13=9(source=weaponlist) w14=11(source=weaponlist)`.
`source=weaponlist` or `source=pickup` means the slot was observed on that map; `fallback` means nothing
was observed and the fixed-order default (9/11) is in use. `pdata_offset` is the effective `m_rgAmmo`
base adjust; the `wNN=` value is the slot inside that array. The line is written as soon as both slots
are observed (normally the first client's `WeaponList`), otherwise when the next map activates, so a
map still unresolved when the server shuts down logs nothing. The drift warnings stay silent either
way, which is why this line and not their absence is the evidence. `grenade_slot_strict = 1` in
`dodx.ini` (default off) makes an unobserved slot resolve to -1 instead of 9/11.

| Native | Purpose |
|--------|---------|
| `dodx_set_grenade_ammo(id, type, count)` | Set grenade count (0-10) for player |
| `dodx_get_grenade_ammo(id, type)` | Current count, or -1 on a bad argument |
| `dodx_get_grenade_ammo_index(type)` | The ammo slot in use for that grenade. Use it instead of a literal |
| `dodx_send_ammox(id, slot, count)` | Send AmmoX message to sync client HUD |
| `dodx_give_grenade(id, type)` | Give a grenade weapon to player |

### Player Manipulation (v2.6.5+)
| Native | Purpose |
|--------|---------|
| `dodx_set_user_noclip(id, noclip)` | Set player noclip mode (0/1) |
| `dodx_set_user_class(id, classId)` | Set player class (1-6 or 0=random) |
| `dodx_set_user_team(id, teamId, refresh)` | Set player team (1=Allies, 2=Axis, 3=Spec) |
| `dodx_get_user_origin(id, Float:origin[3])` | Get player position |
| `dodx_set_user_origin(id, Float:origin[3])` | Teleport player |
| `dodx_get_user_angles(id, Float:angles[3])` | Get player view angles |
| `dodx_set_user_angles(id, Float:angles[3])` | Set player view angles |

### Damage Forward (v2.6.7)
| Forward | Purpose |
|---------|---------|
| `dod_damage_pre(att, vic, dmg, wpn, hit, TA)` | Pre-damage hook, return modified damage |

### Aim / movement sampling (measure-only)
Sampled once per usercmd from `SV_PlayerRunPreThink`. **Sensor, not detector** — reports geometry, applies no threshold, reaches no conclusion. A threshold added here would be a published one; keep the judgement in the private consumer. Counters reset on connect **and** disconnect so a mid-map substitute never inherits the previous occupant's numbers.
| Native | Purpose |
|--------|---------|
| `dodx_get_aim_stats(id, stats[5])` | Windows recorded/retained, ground touches, short ground contacts, longest run of them. Excludes the window still in progress |
| `dodx_get_aim_window(id, slot, window[4])` | One retained fire window: duration (ms), pitch slope (milli-deg/s), residual (micro-deg), samples. Retained by smallest residual, so slot order is not chronological |
| `dodx_reset_aim_stats(id)` | Clear the counters. Separate from the read so a failed flush cannot silently discard what justified it |

### Crouch input / footstep emission census (measure-only)
Crouch presses and time-in-state sampled once per usercmd from `SV_PlayerRunPreThink`; footsteps counted on the ReHLDS `SV_StartSound` chain that dodx registers itself. **Sensor, not detector** — no threshold, no ratio, no conclusion, and there is no calibrated positive class for what it measures, so a cut-point added here would be a guess *and* a published one. Time ships as a histogram over speed rather than "time above a bound" for exactly that reason.

⚠️ **Read both halves or neither.** `dodx_get_move_stats` returns the tap census and the footstep counts together on purpose: a footstep figure on its own describes an ordinary crouch-walker just as well as anything else. The producer keeps them in one marker and the daemon in one row for the same reason.

⚠️ **`step_timer_fires` is the control, not decoration.** It reads the engine's own `v.flTimeStepSound` resetting — an independent sensor for the same event as the counted sounds. Timer fires with no steps means the *server* stopped emitting footsteps (`mp_footsteps`, or a step path that no longer reaches the hook); without it that is indistinguishable from every player moving silently. Check it before any per-player figure.

| Native | Purpose |
|--------|---------|
| `dodx_get_move_stats(id, stats[8])` | Taps, stamina at tap (sum and min, min `-1` when no tap was seen), steps by family, total movement sounds, step-timer fires |
| `dodx_get_move_hist(id, which, hist[], size = sizeof hist)` | One speed histogram: `0` taps on ground, `1` taps airborne, `2` ground ms standing, `3` ground ms ducked, `4` airborne ms ducked. Returns cells written, bounded by your array — never assume the bucket count |
| `dodx_get_move_geom(geom[2])` | Bucket count and bucket width. Ship both with any stored row, or a later geometry change silently reinterprets everything already written |
| `dodx_reset_move_stats(id)` | Clear the counters; in-flight sampling state is preserved. Separate from the read for the same reason as the aim counters |

Ground and airborne are never folded together — a duck-jump is crouching at running speed by construction, so a combined row cannot be read.

### Test Dispatch Natives (extension-mode forward drivers)
Synthetic dispatchers for AC/integration tests — fire a forward directly (no fakemeta).
| Native | Purpose |
|--------|---------|
| `dodx_test_dispatch_weapon_fire(id, weapon, Float:gametime)` | Exercises the `dod_client_weapon_fire` forward (mirrors `dodx_test_dispatch_damage`) |
| `dodx_test_dispatch_damage(att, vic, dmg, wpn, hit, TA)` | Exercises `client_damage` |
| `dodx_test_dispatch_grenade_explosion(id, Float:pos[3], wpnid)` | Exercises `dod_grenade_explosion` |
| `dodx_test_dispatch_grenade_entity_tracked(owner, entindex, serial, Float:pos[3], wpnid, Float:gametime)` | Exercises factual grenade tracking; IDs 13/14/36 only |
| `dodx_test_dispatch_grenade_entity_removed(owner, entindex, serial, Float:pos[3], wpnid, Float:gametime)` | Exercises generic grenade removal; not a detonation claim |
| `dodx_test_dispatch_grenade_entity_tracker_drop(owner, entindex, serial, wpnid, Float:gametime)` | Exercises native tracker saturation accounting; IDs 13/14/36 only |
| `dodx_test_dispatch_score(id, delta, total, cp_index)` | Exercises `client_score` + `dod_score_event` (tandem, matches production) |
| `dodx_test_dispatch_cp_captured(cp_index, new_owner, old_owner)` | Exercises `dod_control_point_captured` |
| `dodx_test_dispatch_client_spawn(id)` | Exercises `dod_client_spawn` (added `127f39fc`, 2026-07-04) |
| `dodx_test_dispatch_changeteam(id, team, oldteam)` | Exercises `dod_client_changeteam` |
| `dodx_test_dispatch_changeclass(id, class, oldclass)` | Exercises `dod_client_changeclass` |
| `dodx_test_dispatch_client_death(killer, victim, wpn, hit, TK)` | Exercises `client_death` (killer-first, production order) |
| `dodx_test_dispatch_stats_flush(id)` | Exercises `dod_stats_flush` for one slot (production loops connected players) |

> See CHANGELOG for what shipped in each cut and on what date.

## Dependencies
- KTPhlsdk (HLSDK headers)
- Python 3 with AMBuild
- GCC with 32-bit support

## Server Deployment

Deploy compiled binaries to production servers using Python/Paramiko.

**Server Credentials:** the dodserver SSH password was **rotated 2026-05-31** — the
prior `ktp` is dead. Do NOT hardcode it in scripts (this repo is PUBLIC). See the
main `N:\Nein_\KTP Git Projects\CLAUDE.md` § Server Credentials for the current value.

| Server | Host | User | Password |
|--------|------|------|----------|
| Atlanta | <ATL_BM_GAME_IP> | dodserver | (rotated — see main CLAUDE.md) |
| Dallas | <DAL_GAME_IP> | dodserver | (rotated — see main CLAUDE.md) |
| Denver | <DEN_GAME_IP> | dodserver | (rotated — see main CLAUDE.md) |
| New York | <NYC_GAME_IP> | dodserver | (rotated — see main CLAUDE.md) |
| Chicago | <CHI_GAME_IP> | dodserver | (rotated — see main CLAUDE.md) |

All five hosts are deploy targets. Atlanta/Dallas/Denver/New York run 5 instances
each (27015-27019); Chicago runs 4 (27015-27018). 24 instances total.

**Remote Paths:**
- `~/dod-{port}/serverfiles/dod/addons/ktpamx/dlls/ktpamx_i386.so`
- `~/dod-{port}/serverfiles/dod/addons/ktpamx/modules/*.so`

See `N:\Nein_\KTP Git Projects\CLAUDE.md` for paramiko SSH documentation.

## Identifying deployed artifacts

KTPAMXX ships as more than one artifact, and they version independently:
- **core** — `addons/ktpamx/dlls/ktpamx_i386.so`. It is *not* under `modules/`, so a sweep of
  `modules/` reads as "core absent".
- **dodx** — `addons/ktpamx/modules/dodx_ktp_i386.so`. A probe for `dodx_amxx_i386.so` finds nothing
  on every instance and reads as "dodx missing".

The md5 is the identity. To confirm a specific change is in a module, grep the binary for a symbol
the change added (a new forward name, say) alongside one that must be present in both builds — a
zero without that control means nothing.

dodx requires a minimum `REHLDS_API_VERSION_MINOR` (`modules/dod/dodx/moduleconfig.cpp`). Before
shipping a dodx that raises it, read MINOR from the commit the **live** engine bakes (see KTP-ReHLDS
`CLAUDE.md`), not from the engine repo's tip.

### `.amxx` plugins

- **Byte-reproducible against the same `amxxpc` — and the compiler is pinned by no commit.** This
  repo's `plugins/compile.sh` generates no `build_info.inc`, and no in-tree `.sma` includes
  `ktp_version_reporter`, so `stats_logging.amxx` and `admin.amxx` bake no timestamp: five compiles of
  1.23.1, two straddling a minute rollover and one from a different directory, gave one md5
  (2026-09-17). ⚠️ **A different `amxxpc` still changes the bytes** — which is why a 09-07 rebuild
  at an old pin mismatched, and `amxxpc` is a gitignored build output. So **still** never rebuild an
  artifact whose md5 is pinned to a review, and never try to recover a build base by rebuilding
  candidates and comparing hashes — the correct base mismatches whenever the compiler moved.
  ⚠️ **The standalone plugin repos are the OPPOSITE case:** their own `compile.sh` writes a
  per-minute `KTP_BUILD_TIME` into a generated `build_info.inc`, which
  `plugins/include/ktp_version_reporter.inc` `#tryinclude`s. Measured on KTPMatchHandler `526da23`:
  two builds a minute apart differ by 72,498 bytes and by one byte of size.
- **Reading strings out of one takes two decodes.** The payload is compressed, and AMX stores
  unpacked strings as one 32-bit cell per character. `strings` or a byte search on the raw file — or
  on the inflated blob — returns a false zero. Inflate the payload, then search for the text encoded
  as little-endian 32-bit cells, with a control string you know is there.
- If you can't decode it, the build base comes from the source repo's reflog and the artifact's mtime.

### `stats_logging` and the daemon's schema contract

`KSC_SCHEMA_CONTRACT` (`plugins/dod/ktp_stats_capture.inc`) must not run ahead of what the
**deployed** KTPHLStatsX daemon accepts. The daemon gates each capability on the schema a server
announces, so a plugin ahead of the daemon loses those streams on that server while kills and damage
keep flowing — it looks partly healthy. Deploy the daemon first, and build `stats_logging` from a
base whose contract the live daemon accepts.

## Branch protection — editing required checks

The runbook for `repos/afraznein/KTPAMXX/branches/main/protection`. Three ways to break `main` here,
all of which look like they worked.

🔴 **The required-check string has two plausible spellings and only one works.** The workflow's
DISPLAY name is `Version Consistency`; the check-run name branch protection matches is
**`version-consistency`**, set by the job's own `name:`. **Read it off
`repos/<owner>/<repo>/commits/<sha>/check-runs`, never from the YAML's top-level `name:`.**
⛔ Requiring the display name requires a check that never reports, which **blocks every PR** until
somebody works out why.

⚠️ **`gh api -f strict=true` fails 422 — `"true" is not a boolean`.** `-f` sends a string, `-F` sends
a typed value; booleans and numbers need `-F`. ✅ Good failure mode: the request is rejected whole, so
it cannot half-apply and silently drop a context.

⛔ **ADD to the contexts list, never REPLACE it.** The API takes the full array and overwrites, so a
PUT carrying only the check you care about **un-requires the compile gate** — and nothing announces
it. Read the current array first, append, then send the whole thing back.

## Related Projects
- `N:\Nein_\KTP Git Projects\KTPhlsdk` - SDK headers
- `N:\Nein_\KTP Git Projects\KTP DoD Server` - Test server with staged binaries

## Notes
- The plugin compiler (`amxxpc`) from this build is used by all AMX plugin projects
- Include files at `plugins/include/` are the authoritative source for plugin compilation
