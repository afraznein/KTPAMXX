# Changelog

All notable changes to KTP AMX will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [2.7.26] - 2026-08-08

DODX-only delta over 2.7.25. Contributed as [#9](https://github.com/afraznein/KTPAMXX/pull/9) by
JimmyLockhart65616 and rebased onto master here. No `.inc` change, so no dependent plugin needs
recompiling.

**Shipping artifact — `dodx_ktp_i386.so` md5 `717ad410745be0e1e33b9885658b0d38`** (built 2026-08-08
from `da12e368`, `build_linux.sh`, GLIBC 2.35 / Ubuntu 22.04).
*(Supersedes `c2975aedf0cc8da640efd1af2b791b3a` from `3bd51b2d` — the source changed after that
build, so this is a required rebuild, not a re-verification of the same tree.)*
⚠️ **Do not rebuild to re-verify** — AMXX bakes a per-minute build timestamp, so a rebuild churns
this md5 and you would stage a binary nobody reviewed. Verify by this md5, never by the banner.

> **Core source is unchanged, but the core BINARY is not identical** — `product.version` feeds
> `support/generate_headers.py`, so the version bump alone changes `ktpamx_i386.so` (this build:
> `80decf574c51bbabb1aa327cb5f0f0fd`). Only the DODX artifact is meant to ship here; the core is a
> byproduct of the build, exactly as in the 2.7.23 DODX-only cut. The only source files that differ
> from 2.7.25 are `modules/dod/dodx/moduleconfig.cpp`, `CHANGELOG.md` and `product.version`.

> **Includes everything in 2.7.25, which never shipped.** 2.7.25 was cut and review-corrected but no
> fleet instance ever ran it — the fleet is still on 2.7.24 (`d599452d…`) with nothing staged. This
> is a new version rather than an amendment to that entry so the reviewed artifact and the shipped
> artifact stay the same thing; folding a later contribution into an already-reviewed cut is how
> "2.7.25" would come to mean two different binaries.

> **No upstream-file edits in this cut.** The only file touched is
> `modules/dod/dodx/moduleconfig.cpp`, which is KTP-owned.

### Fixed

#### dodx: control points reported neutral for a map's whole lifetime when the map starts them owned

In extension mode a CP's owner is seeded from `cpd.owner`, a pdata field that reads **0 for every
CP**, and `mObjects[].owner` is thereafter corrected only by `Client_SetObj` — which the game DLL
sends only when a flag actually changes hands. So on any map whose flags start owned, every consumer
read neutral from map load until the flag was first captured. Affects `dod_donner`, `dod_kalt`,
`dod_flash` and `dod_saints2_*` in the league pool, and is why the KTP HUD overlay showed an
all-neutral flag bar.

> 🔻 **Scope corrected during review.** The contributed change claimed *"a round restart does not
> heal it"*, i.e. that the whole map lifetime was affected. It doesn't: the engine resets every CP to
> its default at a round restart and dodx replays that as a `dod_control_point_captured` cascade —
> `KTPHudObserver.sma:243` and `:2268` document it and name these maps specifically — so `owner`
> self-heals at the first restart. The genuine gap is **map load → first round restart**, which is
> the whole first round, so this still matters exactly where a match is decided. Recorded because the
> overstated version was in the code comments and would have outlived the PR.

`point_default_owner` is now read from the BSP entity lump and used to seed both `default_owner` and
`owner`, matched to the scanned entities by origin — the same identifier the existing `point_index`
reorder uses, since `targetname` is empty on many maps. Seeding happens before any reorder, and the
reorder moves whole `objinfo_t` structs, so the values travel with their CP either way.

- **The BSP parser now returns every `dod_control_point`, not only the indexed ones**, because the
  ownership seed matches by origin and has to see unindexed entities too. The reorder's input is
  unchanged: it keys off the new `*outWithIndex` (the indexed subset), never the return value.
- **Origin matches are consumed one-to-one.** Without that the match is many-to-one and silently
  first-wins, so two CPs stacked in z — or two entities that both fell back to `(0,0,0)` because the
  key was absent — would take the same BSP entry with no diagnostic. Unmatched CPs are logged: a
  failed match is otherwise indistinguishable from a genuinely neutral map, because `owner` just
  stays at the pdata read of 0, i.e. it looks fixed and silently isn't.
- **`point_default_owner` is clamped to 0..2 at the parse site.** Out of range is not a crash —
  `MSG_WriteByte` casts without a range error — but a nonsense value would truncate to something
  plausible on its way to Pawn (`CP_owner` / `CP_default_owner`), and a public fork should not
  propagate it.
- `BSP_MAX_CPS` replaces the repeated literal `12` now that a parallel `bool[]` indexes the same
  range as `bspAll`/`bspCPs`: widening one array and not the others would be a stack overwrite rather
  than a compile error. It deliberately does **not** cover `sortedObj[]`/`used[]`, which are bounded
  by `mObjects.count` — i.e. by `objinfo_t obj[12]` in `dodx.h` — so *shrinking* the constant would
  overflow them. A `static_assert` pins that invariant.

Diagnostics tightened in the same pass, all on paths that previously failed quietly:

- **An unreadable BSP no longer reads as bad map data.** It reached the "NONE carried a usable
  `point_index`" warning, which blamed the map; the count parsed from the BSP is now printed, so `0`
  identifies the real cause. That path also made every CP log its own "no default_owner match" line,
  burying the loader's single line saying why — the seed loop is skipped when nothing parsed.
- **The out-of-range clamp now logs.** It silently produced neutral, turning a map bug into
  invisible behaviour.
- **`default_owner` restored to the final CP dump** — the one field that distinguishes "seeded from
  the BSP" from "read 0 out of pdata" in a live log.
- **A short read on the BSP version field said the wrong thing.** `fread(...) != 1 || version != 30`
  folded two failures into one message, so a truncated file was reported as an invalid *version*
  number. Split into separate branches. (Contributed on #9 as `1c3f7e03` and ported here; the
  uninitialised-`version` half of his fix did not apply, since master already zero-initialised it.)
- **The entity-lump `malloc` failure was the only exit from the reader with no log**, so CP init
  fell back to the pdata read with no trace — indistinguishable from a map that has no defaults.
- `totalDCP` dropped: it counted every CP while `cpCount` counted the indexed ones, but the parser
  now stores every CP, so the two increment on the same branch and cannot diverge.

⚠️ **This does not fix the three maps that reorder.** `dod_escape`, `dod_jagd` and `dod_kraftstoff`
are the only 3 of 18 pool maps observed to emit a full `newCount=N` InitObj, and there the rebuild in
`Client_InitObj` takes `default_owner`/`owner` straight off message field 3 — which arrives as **0** —
so the reorder wipes the seed within about a second of map load rather than restoring it. On those
maps this change is a no-op and the flags read exactly as they did before. That is a separate defect
in a different function, filed rather than folded in here; it pairs with the
`g_cpOrderingFinalized`-never-reset mechanism in
[#10](https://github.com/afraznein/KTPAMXX/issues/10). On the maps this was written for the seed
survives — verified on `dod_kalt` by a `CP_owner` read ~16 minutes and one client connect after map
load — so it is a permanent fix there, not a window fix.

⚠️ **Plugin-visible contract change — `dod_control_point_captured`.** Seeding `owner` changes both
*when* the forward fires and what it carries: a `SetObj` that merely restates the BSP default is no
longer a transition, so it fires nothing (suppressing a spurious map-start capture), and the first
genuine capture now reports `old_owner=1/2` instead of `0`. `KTPHudObserver` is already written for
this module build and degrades safely on an un-upgraded fleet.
✅ **`KTPScoreTracker` checked against the new semantics and is unaffected** — its
`dod_control_point_captured` handler never reads `old_owner`, and `check_all_cps_owned()` (the
`CP_owner` capout read) is unreachable at map load: its only caller returns unless a real flip
happened inside `CAPOUT_RECOVERY_WINDOW`, and `g_lastCPFlipTime` is reset to `0.0` at match start.
The seed makes that capout check slightly *more* correct, since previously-neutral default-owned
flags now read their real owner.

⚠️ **Behavioural note from the rebase:** the BSP parse moved from `mObjects.count > 1` to
`> 0`, since the ownership seed must run on a single-CP map too. Same number of read sites, one
slightly wider condition, and it runs at map load rather than in the frame loop.

## [2.7.25] - 2026-08-08

> **Upstream-file edits in this cut**, flagged per the fork-delta rule. Each is a minimal diff to
> upstream AMX Mod X code, made because the defect lives there: `CForward.cpp/.h` and `CTask.cpp/.h`
> (bool re-entry guards → depth counters), `CMenu.cpp` (dedup returns leaked the caller's forward),
> `CMisc.h` (`CPlayer::Authorize` takes the Steam id so it can be cached), `amxmodx.cpp`
> (`get_user_authid` consults the seizure-replay marker + the `ktp_drop_client` param-count idiom),
> `CCmd.cpp` (`findPrefix` compares case-insensitively, matching `matchCommandLine`), `CForward.cpp`
> a second time (the deferred delete frees directly instead of re-entering the release path), and
> `amxxlog.cpp` (stack-local format buffers, writer-loop simplification, `queued` out-param).
> Everything else is KTP-owned.


### Fixed

- **A seized slot reported the wrong SteamID to disconnect handlers (AX-02).** When a different
  player takes over a timed-out slot from behind the same NAT, AMXX replays the old session's
  disconnect forwards — but the engine has already swapped the incoming player's identity into that
  slot (`sv_main.cpp:2669-2676`, upstream of the hook), so `get_user_authid()` returned the
  NEWCOMER and a handler saving stats by SteamID filed the leaver's under someone else's.
  `CPlayer` now caches the id, taken in `Authorize()` — which takes it as a **parameter** so it
  cannot be cached late or missed: there turned out to be **five** `Authorize()` sites, not three,
  and two surfaced only as compile errors. `get_user_authid` prefers the cache **only** for the
  seized index and **only** while those forwards run, behind an RAII guard. Not a general
  preference — that would return a stale id whenever the engine's is fresher.
  The guard covers `client_remove` as well as the two disconnect forwards — it is part of the same
  replay, and because it fires after `Disconnect()` has cleared the field, the guard holds a value
  copy rather than a pointer into it. An empty cached id falls through to the engine rather than
  publishing `""`, since `ke::AString::chars()` returns `""` and never NULL.
  ⚠️ **No test coverage** (needs two clients behind one NAT + a ≥10s timeout). Watch
  disconnect-save SteamIDs after the cut that ships it.

- **`client_connect`/`client_authorized` fired twice per player per map change (AX-08).**
  `Steam_NotifyClientConnect_RH` runs first in `SV_ConnectClient` and already does
  `Connect()` + `client_connect` + `Authorize()` + `client_authorized`; `ClientConnected_RH` then
  re-ran `Connect()` unconditionally. The duplicate forwards are the visible symptom — the damaging
  part is that `Connect()` sets `authorized=false` and memsets `flags[]`, so the second call
  discards the admin flags the first authorize just resolved, and `SV_Spawn_f_RH` re-authorizes and
  fires `client_authorized` again. Metamod fires each exactly once, so this is an extension-mode
  parity gap. Now gated on `initialized`, which `Disconnect()` clears at map change so a genuine new
  session still connects. `client_connectex` is deliberately **outside** the gate — it has no
  counterpart in the Steam hook, and Metamod fires it every map change, so gating it alongside
  `client_connect` would have silently stopped delivering it. `name`/`ip` are refreshed on the skip
  path too: the Steam hook caches the name before `SV_ExtractFromUserinfo` runs, and those fields
  feed `admin.sma`'s `FLAG_IP` matching. ⚠️ Untested against a live map change with players.

- **Unguarded DODX message sends could kill the server (AX-20, and four more).**
  `MESSAGE_BEGIN` with type 0 is `Sys_Error` — the engine terminates the process — and every
  `gmsg*` id is 0 until `RegUserMsg` interception captures it. The 2.7.22 sweep guarded three send
  sites; `dodx_set_user_team` was missed (AX-20). A first sweep wrongly reported a second miss in
  `dodx_set_scoreboard_team_name` — that site was **already guarded**; the duplicate has been
  removed. Re-sweeping for both send forms found the ones actually missing: `dod_set_weaponlist`
  (`NBase.cpp`), where `GET_USER_MSG_ID` resolves through Metamod's `gpMetaUtilFuncs` — never
  assigned in extension mode, so a **NULL deref**, not a type-0 `Sys_Error`; plus
  `CObjective::InitObj`, `CObjective::SetObj` and the heal-back `gmsgHealth` send, all on
  objective/health paths and far more reachable than the natives. All nine sends in the module are
  now guarded, verified by enclosing function rather than a fixed line window.
  `set_user_team` returns 1 (the team change already applied; only the client refresh is skipped).

- **Extension mode never loaded `cmdaccess.ini` (AX-09/AX-22).** The extension init
  called `FlagMan.SetFile()` and stopped; `LoadFile()`'s only other caller is
  `C_Spawn`, reached solely through the Metamod function table, so the file was
  named and never parsed and every rule in it was silently discarded. `LoadFile()`
  now runs at two sites mirroring the `g_log.MapChange()` pattern beside it — the
  init path for the first map, `KTPAMX_ReloadPlugins()` for every map change after
  — which reproduces Metamod's per-map reload, so an edit takes effect on the next
  map change as the file's own header promises. Blast radius was measured before
  arming: of the 62 rules on the live fleet, 59 name plugins that are not loaded
  and the 3 that touch loaded plugins already match the flags those plugins
  register, so switching the loader on is a behavioural no-op today.

- **`g_players_num` never decremented on disconnect in extension mode.** Metamod
  decrements in `C_ClientDisconnect`, which the extension path never reaches, while
  the increment (`SV_Spawn_f_RH`) is a ReHLDS hook that does run — so
  `get_playersnum()` (the no-argument form, which returns the cached counter rather
  than iterating) over-counted after every mid-map disconnect until the next map
  change zeroed it. `SV_DropClient_PostHook` now decrements under the same `ingame`
  guard as the Metamod path, placed before `Disconnect()` clears that flag.

- **A dropped changelevel could wedge AMXX for the rest of the map (AX-01
  residual).** `PF_changelevel_I_internal` burns its once-per-spawncount queue guard
  whether or not the map was valid, so an invalid `pfnChangeLevel` followed by a
  valid one in the same spawncount latches `g_bMapChangeInProgress` while the queue
  is dropped. `SV_ActivateServer` never runs, nothing clears the flag, and every
  AMXX path that early-outs on it stays dead: tasks, module frame callbacks,
  events, logevents, `.` commands. `SV_Frame_RH` now releases the flag if it is
  still set after 30s of frames — a real transition barely ticks `SV_Frame` — and
  logs it, since reaching that state means the window fired. Teardown latches the
  same flag deliberately and is excluded via a file-scope marker set before the
  latch.

- **Nested execution cleared two re-entry guards early.** `m_InExec` and
  `m_bInStartFrame` were booleans, so a nested call cleared the guard on the inner
  return while the outer was still on the stack. For `m_InExec` that is a
  use-after-free: `unregisterSPForward` frees a forward when `!m_InExec`, so a
  forward re-entering itself could be freed mid-execution. Both are now depth
  counters; all existing tests are against zero, so consumers are unchanged.

- **`registerMenuCmd` leaked an SP-forward slot per duplicate registration.** Its two
  dedup paths returned without storing the forward the caller had just created for
  it — ownership transfers on every call — so a re-registration leaked one slot,
  once per map until the nightly restart. Both paths now release it.

- **DODX conflated "no `point_index` key" with "`point_index` is -1".** The BSP parser used `-1`
  as its key-absent sentinel while `-1` is also a value maps write, so the two were
  indistinguishable and both were silently dropped. `dod_saints2_b3e` and `dod_saints2_b2` carry
  five control points each with an explicit `-1`, so the parser yielded zero usable entries, the
  BSP reorder short-circuited, and CP ordering fell back to entity-scan order — which does not
  match the game DLL's. Presence is now tracked separately from value. A *total* reorder failure
  also stopped being reported as the neutral "BSP parse returned 0 CPs": when the entity scan found
  control points and none survived, it now warns that CP order is unreliable on that map. Ordering
  policy is deliberately unchanged pending KTPAMXX #5.

- **DODX `dodx_test_dump_round_timers` read past the end of unrelated entities.** The
  entity scan matched classnames by substring (`timer`/`round`/`clock`) and then
  dereferenced the `CDodRoundTimer` offsets — 344/348/352 on Linux, so ~356 bytes
  into `pvPrivateData` — on whatever it matched. The substring stays as a discovery
  filter (the native exists because the classname was unknown), but the offsets are
  now only read on an exact classname match; anything else is logged as `SKIPPED`
  with its classname, which is what the scan wanted anyway.

- **DODX control-point / area string setters aliased a transient buffer.** The six
  CP/area string setters in `NCP.cpp` (`CP_name`, `CP_reset_capsound`,
  `CP_allies_capsound`, `CP_axis_capsound`, `CP_targetname`, `CA_target`) stored
  `MAKE_STRING(szValue)`, which aliases AMXX's shared string buffer instead of
  copying — the next native string fetch overwrites it, corrupting the CP/area
  name. Switched to `ALLOC_STRING` (copies into the engine string pool), matching
  the module's other string-setter sites. Latent today: no plugin calls these
  setters, so the fix is inert until a caller exists. Landed on `master` now so it
  is in place before the CP-init work adds the first consumer. Not cut yet — rides
  the next DODX build; no version bump, no md5 (not shipped).

- **Every client command dispatched through a full linear scan (AX-21/AX-24).** The seven
  `registerPrefix` calls that bucket commands by prefix live only in `C_Spawn`, so in extension
  mode no buckets existed, `registerCmdPrefix` always failed, every registration landed in the
  flat `clcmdlist`, and `clcmdprefixbegin()` always returned null — making the 2.7.2 say/say_team
  separation inert on the fleet. Every say, jointeam, menuselect and class pick from every player
  walked ~200 entries with `stricmp`. Added to extension init, before plugins register anything and
  longest-prefix-first (`findPrefix` matches on the *stored* prefix's length, so a bare `say`
  swallows `say_team`). The reload path needs no counterpart: unlike Metamod, extension mode never
  calls `g_commands.clear()` on map change.
  ⚠️ **This needed a one-word upstream fix to be dispatch-neutral**, flagged per the fork-delta rule.
  `registerCmdPrefix` moves a bucketed command *out* of `clcmdlist` (`CCmd.cpp:196`), and
  `findPrefix` compared with `strncmp` while `matchCommandLine` compares with `stricmp` — so once
  buckets existed, a mixed-case `"SAY .ready"` would miss the bucket, fall back to a `clcmdlist`
  that no longer held the say handlers, and match **nothing**. Extension mode had been accidentally
  immune because it had no buckets. `findPrefix` now uses `strnicmp`, matching the comparison every
  other lookup on this path already used.

- **The vault read empty after the first map change (AX-23).** `KTPAMX_ReloadPlugins` cleared
  `g_vault` but never reloaded it; Metamod pairs its `C_ServerDeactivate_Post` clear with a
  `C_Spawn` `loadVault` every map. So `set_vaultdata` on map 1 persisted to `vault.ini` and
  `get_vaultdata` on map 2 returned `""` with the value still on disk. Latent — no KTP plugin uses
  the vault natives — but it silently broke the documented API for anything that did.

- **`ktp_drop_client` could read an argument that was never pushed (AX-34).** `params[0]` is the
  argument **byte** count, so `params[0] >= 2` is true with a single argument and the optional-reason
  branch read `params[2]` regardless. Now `params[0] / sizeof(cell) >= 2`. Masked today: the shipped
  declaration's `const reason[] = ""` makes the compiler always emit two cells.

- **The double-release tripwire cried wolf on every self-removing task (AX-14).** A `set_task`
  callback that calls `remove_task` on its own id — KTPMatchHandler's countdown tick does exactly
  this — released the forward while `execute()` was still on the stack, so `unregisterSPForward`
  zeroed the refcount and deferred the free via `m_ToDelete`. `executeForwards` then called
  `unregisterSPForward` a **second** time to finish it, and that call hit `m_RefCount == 0 &&
  !m_InExec` and logged "released with refcount already 0 (double release?)". The forward was
  always freed exactly once; only the warning was false — but a tripwire that fires on a routine
  match-path idiom is worse than none, since it buries the real double-release it exists to catch.
  The deferred half now frees the slot directly instead of re-entering the release path. It still
  honours `m_InExec`: with an outer `execute()` of the same forward on the stack, `m_ToDelete` stays
  set and that frame does the free — dropping that check would free a live forward, which is the
  bug the 2.7.25 depth counter was added to prevent.

- **`LogError` lost the error-session header on a dropped write (AX-32).** `KTP_ALogEnqueue` returns
  true on a full ring — deliberately, since falling back to synchronous I/O during a disk stall is
  what the async writer exists to avoid — but `LogError` read that as "written" and latched
  `m_LoggedErrMap`. If the first script error of a map landed while the ring was full, the "Start of
  error session" + map/file header was gone for the rest of the map while every later error line
  logged fine. The enqueue now reports "queued" separately from "handled", and only a real write
  latches. Cosmetic, but it removes exactly the context you want in `error_*.log`.

- **Five ReHLDS hooks lacked the Metamod-mode passthrough (AX-25).** They register from
  `GiveFnptrsToDll`, which Metamod calls *before* `Meta_Attach` — so `g_bRunningWithMetamod` is still
  false and they install anyway. `SV_ClientCommand_RH` and `AlertMessage_RH` would double-dispatch
  every client command, registered `.`-command, logevent and `plugin_log`. `PF_changelevel_I_RH`
  would latch `g_bMapChangeInProgress` with nothing left to clear it (`SV_ActivateServer_RH` passes
  through before reaching the clear), wedging every AMXX path that early-outs on it.
  **`PF_precache_model_I_RH` was the worst of the set** — it fires long after `Meta_Attach` with
  `g_bRehldsExtensionInit` still false, so it would run a *second full AMXX init*: reload plugins,
  re-register every forward and every extension hook. `PF_RegUserMsg_RH` rounds it out (benign —
  duplicates what `C_Spawn`'s `REG_USER_MSG` does). All five now carry the guard the two
  already-correct siblings had. Dormant by configuration; production is extension-only.

- **British/para weapon ids were never remapped in extension mode (AX-16).** `info_doddetect`
  carries `detect_allies_country` / `detect_allies_paras` / `detect_axis_paras`, and Metamod reads
  them through `DispatchKeyValue_Post` — which never fires here. The intended replacement
  (`DODX_DetectMapInfo`) was a stub that was never called and could not have worked anyway: GoldSrc
  does not let you read arbitrary keyvalues back off a spawned entity. So all three stayed 0
  forever, `get_weaponid` never remapped 1→37 (brit_knife), 16→36 (mills_bomb) or 20→33 (folding
  carbine), the TraceLine paths reported grenade id 13 instead of 36, and `dod_get_map_info`
  always answered 0 — silent weapon misattribution into HLStatsX for the whole map.
  Now read straight out of the BSP entity lump, which the CP parser was already doing; the open +
  lump read is factored into `DODX_LoadBSPEntityLump` so both consumers share it. Values are
  assigned on every map (`g_map.Init()` runs once per process in extension mode, so a British
  map would otherwise leave its flag set on the next US map).
  ⚠️ **This changes live behavior — do not read it as dormant.** None of the 12 maps played in the
  last 180 days sets any of the three keys (all 12 confirmed present locally, so that is a real zero
  rather than a missing-file false negative), and none are in the 6-map mapcycle. But **14 installed
  maps do set one**, and six of them — `dod_flash`, `dod_jagd` and `dod_caen` (allies_country),
  `dod_switch`, `dod_zalec`, `dod_sturm` (allies_paras) — are present on **every fleet instance**
  and reachable by `changelevel`. On those maps this cut starts remapping weapon ids into HLStatsX
  and reporting grenade id 36 instead of 13, which `KTPPracticeMode` passes straight to
  `dodx_give_grenade`. `NBase.cpp` handles 36 correctly, so this is right DoD behavior — but it is a
  path that has never executed on the fleet.
  ✅ **Smoke-tested 2026-08-08 on a local HLDS running the md5-pinned binaries**, three maps in one
  process, clean exit 0: `dod_jagd` → `country=1 paras=0 axis_paras=1`, `dod_flash` → `1/0/0`,
  `dod_anzio` → `0/0/0`. The third map is the one that matters — it proves the **per-map reset**
  holds, which is the whole risk given `g_map.Init()` runs once per process. CP parsing came through
  the shared-loader refactor unchanged (3/5/5 control points, all with `point_index`). No cores.
  Remaining untested: the give-grenade leg needs a live client. Its risk is low — the only new value
  reaching `dodx_give_grenade` is 36, which has an explicit case (the trace table's own comment
  reads `{ "grenade", 13, … }, // or 36`), and `dodx_give_grenade` is separately known to fail
  fleet-wide already (~75k `ret=-1`), so this cannot regress a path that currently works.

- **BSP entity-lump tokenizer could walk off the allocation.** With the cursor sitting on the lump's
  terminator, `if (*pos != '"') { pos++; continue; }` stepped past `entData[entLength]` and the
  enclosing `while (*pos && *pos != '}')` then read unmapped heap — a 2 MB lump is served by `mmap`,
  so the next page can be absent and the read is a SIGSEGV at map load. Reachable only from a
  truncated or corrupt BSP that ends inside a `{` block. Guarded at the new `DODX_ReadBSPMapInfo`
  site **and** at the pre-existing twin in `DODX_ReadBSPPointIndices`, which had carried it all
  along. Audited all 97 installed maps with a quote-aware scan (entity values legitimately contain
  `{`/`}`, so a naive brace count reports ~half the pool as malformed — the first pass did exactly
  that): **none can reach it**, so this is hardening against a bad file, not a live crash.

### Removed

- **Dead code, verified unreachable before deletion (AX-11, AX-17, AX-18, AX-19, AX-29).**
  - `C_StartFrame_Post`'s `g_putinserver_mask` block: the mask's only setter lives in
    `SV_Spawn_f_RH`, registered only by extension init, so under Metamod it is never populated and
    under extension mode the `!g_bRehldsExtensionInit` guard is always false. Doubly unreachable.
  - `DODX_OnMessageHandler` (~125 lines): a complete parallel message-dispatch implementation with
    no call site. It encoded guards that diverged from the live `DODX_OnMsgBegin` path, so the real
    cost was a future fix landing in the dead copy.
  - `traceVault.iClassName`: written with `ALLOC_STRING` on every map load, read nowhere. Its
    comments still advertised a "~50µs savings" integer compare that was reverted — following them
    reintroduces the bug that broke `dod_grenade_explosion` and practice-mode grenade refill, so
    both `strcmp` sites now carry that as a tripwire.
  - `CPlayer::CheckShotFired` (~115 lines) plus `oldbuttons`/`lastShotTime`/`nextShotTime`: the only
    call site was commented out, and its embedded weapon fire-rate table was a second encoding of
    `weaponData[]` free to rot. The `dod_client_weapon_fire` forward is unaffected — it fires from
    `saveShot` on the live CurWeapon path, which is exactly why running both double-counted shots.
  - The commented-out `Steam_GSBUpdateUserData_RH` / `ExecuteServerStringCmd_RH` blocks (4 sites).
    The disabling is deliberate and stays; the corpses violated the repo's own no-dead-history rule
    and invited re-litigating hooks already ruled out. One line at the registration site records why.

  **Kept deliberately (AX-28).** `GetRehldsApi`, `GetRehldsFuncs`, `GetRehldsServerData` and
  `Reg`/`UnregModuleFrameFunc` do have zero in-tree consumers — confirmed against a positive control
  (`GetRehldsHookchains`, which dodx does call), after a first probe returned zero for the control
  too and was therefore measuring nothing. They are **not** being removed: they're the entry points
  a future module needs, `REQFUNC_OPT` already tolerates their absence, and withdrawing exported SDK
  surface means a cross-repo churn under the dual-copy rule plus a compat break for anything built
  against it. Marked reserved at the registration site instead. (The review's claim that KTPAmxxCurl
  calls `MF_RegModuleFrameFunc` is wrong — it requests both and calls neither.)

### Changed

- **Log formatting buffers are stack-local, not shared statics (AX-31).** `Log()` and `LogError()`
  formatted into `static` buffers while the enqueue path is explicitly hardened for concurrent
  module-thread producers — the two halves of the async design disagreed about whether
  multi-threaded logging is supported. Two concurrent callers would tear each other's line and hand
  `KTP_ALogEnqueue` a transiently unterminated buffer. No such producer exists today (game thread
  only), so this is latent; ~8KB of stack per call is free on either thread.

- **Writer loop drops the `needFlush` state machine (AX-33).** The log `FILE*` is line-buffered and
  every enqueued text is newline-terminated, so the drain-time `fflush` always ran on an empty
  buffer — ~15 lines that never moved a byte. The one case where it *would* have mattered is
  `setvbuf` failing, whose return was unchecked; that is now checked, with an `_IONBF` fallback,
  which makes the durability guarantee explicit instead of implicit. Shutdown already flushed via
  `fclose`.

- **Corrected the `g_activated` narration and dropped two redundant re-sets (AX-30/AX-38).**
  Extension init set `g_activated = true` unconditionally, then the deferred-precache branch
  claimed "Don't set g_activated yet" and two later sites set it true again. Now set once, where it
  actually happens, with the real reason (it gates teardown/hook paths such as
  `SV_InactivateClients_RH`, not plugin readiness). Separately, `CMisc.h` claimed `IsBot()`/
  `IsAlive()` were out-of-lined "for debug logging" — no such logging exists, and a maintainer
  acting on that comment would re-inline the upstream one-liners and silently drop the
  `pEdict->free` guard, the `authorized` gate before `GETPLAYERAUTHID`, and the NULL-edict check.
  Comments only.

### Documentation

- **`@error` on 21 natives that abort without documenting it.** A native raising `AMX_ERR_NATIVE`
  terminates the calling plugin, which is very different from one that logs and returns 0. Each
  line is written from that native's actual `LogError(...AMX_ERR_NATIVE...)` text rather than a
  guess at what it validates — an `@error` naming the wrong condition is worse than none. Covers
  `amxmodx.inc` (5), `string.inc` (6), `sorting.inc` (3), `textparse_ini/smc.inc` (4),
  `celltrie.inc`, `gameconfig.inc`, `messages.inc`. `clamp`, `hash_string` and `fungetc` are
  deliberately left alone — no condition could be extracted, and approximating one is the failure
  mode above.

- **`dodx_get_round_time` can legitimately exceed `mp_timelimit*60`.** During an
  `mp_clan_restartround` countdown the value is projected from a completion time still in the
  future, so it reports more than the nominal half length. That is correct rather than drift, and a
  consumer that clamps misreports the clock for the whole countdown window.

- **Version stamps.** README header and Version Information said 2.7.22 and the
  Verify-Installation console sample said 2.7.20, against a shipped 2.7.24. Header
  and Version Information now read 2.7.24; the console sample is de-versioned to
  `<version>` so it stops needing a bump every cut (the surrounding format string
  is accurate and unchanged).

- **Three shipped releases were dated "unreleased".** 2.7.22 (2026-07-11),
  2.7.23 (2026-07-16) and 2.7.24 (2026-07-18) now carry their activation dates.
  Added a `## [2.7.21]` stub recording that the core was superseded before it ever
  activated *but its DODX artifact shipped with the 2.7.22 wave* — without a
  header there, a dodx change traced to 2.7.21 had nothing to anchor on.

- **README's DODX native list predated two DODX cuts.** Added
  `dodx_get_round_time` (Score Management), a new Score Persistence line
  (`dodx_set/get_user_deaths`, `dodx_set/get_user_score`,
  `dodx_get_observed_deaths`, `dodx_broadcast_scoreboard`),
  `dodx_set_stats_paused` + `dodx_set_pl_teamname` under HLStatsX Integration,
  and the `dod_client_weapon_fire` per-shot forward next to `dod_damage_pre`
  — with a pointer to the coverage envelope documented in `dodx.inc`, since
  that forward's exclusions (no bots, pause-gated, no dry-fire) are what make
  counts come out wrong.

- **ReHLDS compatibility floor.** The stated `3.22.0.904+` predates the engine
  support 2.7.24 actually needs: extension-mode teardown requires .928+, and the
  `client_infochanged` ordering fix is only reachable on .929+. Noted at the floor
  rather than raising it, since the module still loads below.

- **Build Output table implied five deployed artifacts.** Noted that
  `build_linux.sh` stages only `ktpamx_i386.so` and `dodx_ktp_i386.so`; the other
  three modules build but KTP does not deploy them.

- **`CLAUDE.md`:** dropped a staging note that still described 2.7.19 as
  uncommitted and gated on the ReHLDS .927 canary, and added Chicago to the
  deploy-target table (it was missing entirely — a deploy driven off that table
  would have skipped a whole host).

## [2.7.24] - 2026-07-18

Stack-review cut (`reviews/stack-review-2026-07-15/KTPAMXX.md`): five CONFIRMED P2s, one P3 ride-along, and one ordering fix that `.929` makes reachable. **Core + DODX** delta over 2.7.23 (`ktpamx_i386.so` + `dodx_ktp_i386.so`) — the first cut since 2.7.22 that moves the core, so the console banner (`2.7.24.5551`) matches the shipped artifacts again.

Every item is an instance of one class: **init/teardown that only exists on the Metamod path**. `C_Spawn`, `C_ServerDeactivate_Post` and the DLL-table wrappers never run in extension mode, so the state they own is never reset and the forwards they fire never arrive.

**Deploy notes:** ships alone on its own nightly (never stacked with an engine cut). AX-ORDER must land **before** ReHLDS `.929`, which enables the `SV_ClientUserInfoChanged` call site this fix corrects the ordering for.

### Documentation

#### README gave a wrong path *and* wrong contents for `extensions.ini`

Install step 3 said to create `rehlds/extensions.ini` containing
`ktpamx/dlls/ktpamx_i386.so`. Both halves were wrong. The loader reads
`"%s/addons/extensions.ini"` against `com_gamedir` (`sys_dll.cpp:1067`) with a
CWD-relative fallback (`:1073`) — there is no `rehlds/` path — and each line
resolves as `com_gamedir/<line>` (`:1123`), so the entry needs the `addons/`
prefix. The live fleet file is `dod/addons/extensions.ini` containing
`addons/ktpamx/dlls/ktpamx_i386.so`; verified on an ATL host.

Following the old text failed twice over, and silently both times: the loader
returns without error when no config is found (`:1077`), so the server boots as
vanilla HLDS with no wall-penetration fix, no cvar enforcement and no match
handler. This is the failure mode the `ktp_extension_loaded` sentinel exists to
catch. The same wrong path had propagated to four other docs across the stack
and was corrected in each.

### Fixed

#### AX-01: a failed changelevel wedged all AMXX processing for the rest of the map

`PF_changelevel_I_RH` latched `g_bMapChangeInProgress` unconditionally, but the flag only ever clears in `SV_ActivateServer_RH`. `pfnChangeLevel` merely queues `changelevel <map>` into Cbuf; `Host_Changelevel_f` runs it next frame and rejects an unknown map (`!PF_IsMapValid_I`) *before* `SV_InactivateClients`/`SV_SpawnServer`/`SV_ActivateServer`. The server kept playing, with the flag stuck true — early-outing `SV_Frame_RH` (all `set_task` timers, all module frame callbacks incl. Discord/HTTP), `MessageHook_Handler` (every `register_event`), `AlertMessage_RH` (every logevent) and `SV_ClientCommand_RH` (every `.` chat command) until a later valid map change or a restart. Now latches only when the target map exists, using `IS_MAP_VALID` — literally `PF_IsMapValid_I`, the same predicate `Host_Changelevel_f` rejects on (`sys_dll.cpp:160`), so the guard cannot disagree with the engine and skip a latch on a real map change. A genuine change still latches here, and `SV_InactivateClients_RH` latches again once it is actually under way — as the *first* step inside `Host_Changelevel_f_internal`, before `SV_ServerShutdown`/`SV_SpawnServer`, so even a hypothetical false negative leaves the crash-protection window covered; only the ≤1-frame gap between Cbuf queue and execution goes unlatched, on a live server.

**⚠️ KNOWN RESIDUAL — the wedge is narrowed, not eliminated.** `PF_changelevel_I_internal` consumes its once-per-spawncount queue guard (`last_spawncount`) **regardless of map validity**. So: an invalid game-DLL `pfnChangeLevel` correctly skips the latch here but *still* burns the spawncount; a later valid `pfnChangeLevel` in the same spawncount then latches while the internal silently drops the queue — no changelevel, no `SV_ActivateServer`, flag stuck for the map. Recovery is a console/rcon `changelevel` or the nightly restart. Strictly narrower than the bug fixed (it needs an invalid *then* valid game-DLL changelevel within one map, and DoD gamerules pre-validates with the same `IS_MAP_VALID` before calling `CHANGE_LEVEL`, so the invalid first call is already rare) — but it is the same wedge. **Proper fix is the review's other option: an `SV_Frame_RH` watchdog that clears the flag after N frames with the server still active on the same map. Queued for 2.7.25.** Any watchdog must not fight the deliberate `KTP_ExtensionShutdown` latch (`meta_api.cpp`), which sets the same flag on purpose to suppress plugin code during teardown.

#### AX-02: a mid-map crash-reconnect silently lost authorization and admin flags

ReHLDS's `SV_ConnectClient` reconnect path calls `pfnClientDisconnect` **directly**, and extension mode wraps no DLL table — so `CPlayer::Disconnect()` never ran and `ingame` stayed true from the dead session. `ClientConnected_RH` then re-ran `Connect()` (wiping `flags[]`, clearing `authorized`) while `SV_Spawn_f_RH`'s `initialized && !ingame` gate stayed false: no `client_authorized`, no `client_putinserver`, `is_user_authorized()` permanently 0, and **an admin who crashed lost admin until a full disconnect**. `ClientConnected_RH` now replays the `C_ClientDisconnect` flow (disconnect/disconnected/remove forwards + `Disconnect()` + the `g_players_num` decrement) before `Connect()` when the slot is still `ingame`.

Gated on `ingame` **alone, deliberately not `initialized`** (which the review's fix sketch proposed): `Connect()` sets `initialized = true` (`CMisc.cpp:96`) and `Steam_NotifyClientConnect_RH` re-`Connect()`s every slot on a map-change reconnect, so an `initialized` gate would fire spurious disconnect forwards for every player on every map change — including the `client_disconnected` handlers score-persistence saves from. `ingame` is stale-true only on the crash-reconnect path.

#### AX-03: heap out-of-bounds write on any message with >= 32 parameters

The KTP rewrite of `EventsMngr::NextParam()` pre-allocated a 32-entry parse vault and **removed upstream's growth path**, but every caller writes `m_ParseVault[m_ParsePos]` immediately after `NextParam()` returns. Once `m_ParsePos` reached 32 the guard (`m_ParsePos < m_ParseVaultSize`) went false, nothing grew, and the store went off the end of the heap block. Growth is restored (doubling to fit, copying the old contents) — upstream's initial size was **also** 32, so the pre-allocation optimisation and the growth safety were never in conflict; the fast path still never allocates, and the vault lives for the process, so growth is one-shot rather than per-message churn.

#### AX-04 / AX-05: DODX disconnect cleanup never ran — slot-reuse state inheritance

`DODX_OnSV_DropClient` was fully implemented and documented as live ("replaces FN_ClientDisconnect", and the 2.7.20 entry claims the disconnect-time reset) but **never registered**, so `CPlayer::Disconnect()` never ran in production. A substitute reusing a slot mid-map inherited the leaver's `ingame` flag (so `PutInServer()`/`restartStats()` never ran for them), `weapons[]` stats, `savedScore` (first `ObjScore` computing a negative delta), and `g_observedDeaths` — pushing observed-vs-offset drift past the +1 band KTPMatchHandler 0.10.144 tolerates, silently vetoing the substitute's score saves. LAN substitutes are exactly the trigger. Now registered in `DODX_SetupExtensionHooks`, with the unregister mirrored in `DODX_CleanupExtensionHooks`.

Chain order is load-bearing and left as-is: the handler calls `chain->callNext()` first so the core's `SV_DropClient` hook fires `client_disconnected` while the slot is still `ingame` (plugins save stats there). dodx registers from `OnAmxxAttach` — i.e. during `loadModules()`, before the core registers its own `SV_DropClient` hook — and `addHook` appends equal priorities, so dodx sits outermost and its cleanup runs after the forward. Commented at the registration site.

#### AX-07: previous map's CP index attributed to scores on the next map

`g_lastCapturedCP`/`g_lastCapturedTime` were reset only in the Metamod-only `ServerDeactivate`, so they survived a map change in extension mode. With the new map's clock restarting near 0, `gpGlobals->time - g_lastCapturedTime` went *negative* and stayed under the 2.0s freshness gate for ~30 minutes, letting `dod_score_event` fire with the previous map's CP index (KTPScoreTracker capture logging, match stats). Both globals are now reset in `DODX_OnSV_ActivateServer` beside the `AlliesScore`/`AxisScore` zeroing, along with the custom-weapon `weaponData[].needcheck` flags for parity with the same skipped teardown.

#### AX-15 (P3, ride-along): CP-score correlation window had no negative-delta guard

The pending-CP resolution check `(gpGlobals->time - g_lastCapturedTime) < 2.0f` is trivially true for a negative delta, making the 2-second window effectively infinite after a map change. Now `delta >= 0.0f && delta < 2.0f` at **both** resolution sites (extension and Metamod paths), matching the sibling stale-timestamp guard 2.7.20 already shipped for `g_lastDeathReportTime`. Belt-and-braces with AX-07: AX-07 stops the timestamp going stale, this stops a stale one being trusted.

#### AX-ORDER: `client_infochanged` fired after the name cache was already refreshed

`SV_ClientUserInfoChanged_RH` assigned `pPlayer->name` **before** `executeForwards(FF_ClientInfoChanged)`; the Metamod path (`C_ClientUserInfoChanged_Post`) does the opposite. Plugins detect a rename with the stock idiom — compare cached `get_user_name()` against `get_user_info("name")` — and saw them already equal, so **the rename was undetectable**; `plugins/admin.sma:767-794`'s name-change re-auth is dead in extension mode. The forward now fires before the cache refresh, matching Metamod ordering (`chain->callNext(cl)` still runs first, so the engine has applied the userinfo before we re-read the infobuffer).

Fail-safe and cosmetic today — it can only *suppress* a re-auth, never add one, and fleet admin auth is SteamID-keyed. It ships now purely for ordering: the handler is dead code until `.929` enables the engine call site, and this is the only KTPAMXX nightly before it. Found by the `.929` review, not the stack review.

## [2.7.23] - 2026-07-16

DODX-only delta over 2.7.22 (`dodx_ktp_i386.so` + `dodx.inc`; core unchanged). Groundwork for the closed-loop broadcast half-clock in KTPHudObserver: the overlay clock currently runs on an open-loop `mp_clan_timer` anchor estimate because the engine emits no usable go-live signal (RoundState==1 confirmed never sent at `mp_clan_restartround` completion — prod NY1 4/4 matches + local repro with a real client, 2026-07-11).

### Added

#### dodx: `dodx_get_round_time()` — engine-authoritative half-clock remaining

New native `Float:dodx_get_round_time()` returns the current half's seconds-remaining read straight from CDoDTeamPlay gamerules — the same clock the client HUD renders — replacing KTPHudObserver's open-loop `mp_clan_timer` estimate. Accounting is `mp_timelimit*60 - (gpGlobals->time - m_flDoDMapTime)`, projected from `m_flRestartRoundTime` through the `mp_clan_restartround` countdown so callers get the correct post-go-live value across the whole restart window; `mp_timelimit` is read via a cached cvar pointer resolved in `OnPluginsLoaded` (extension-mode-safe, NULL-checked). Fail-soft `-1.0` on any unavailable/implausible read (no gamerules, unresolved offset, NULL cvar, out-of-range base/remaining); freezes correctly under pause (keys off `gpGlobals->time`). All three gamerules members (`m_flDoDMapTime`, `m_flRestartRoundTime`, `m_bRoundRestarting`) resolve from the shipped `offsets-cdodteamplay.txt` (no new gamedata) and are read only behind the `DODX_HasGameRules()` guard.

#### dodx: round-timer offset resolution + diagnostics (`dodx_test_dump_round_timers()`, `dodx_test_scan_gamerules()`)

Resolves the DoD round-timer field offsets the gamedata has always shipped but nothing consumed — `CDoDTeamPlay::m_flRoundTime`/`m_pParaTimer`, `CDodParaRoundTimer::m_fRoundTimer`/`m_bTimer`, `CDodRoundTimer::m_fRoundTime`/`m_fTimerLength`/`m_bTimer` — at `OnPluginsLoaded` alongside the existing `m_iTeamScores` lookup (all optional, −1 sentinel, fail-soft). Two always-compiled diagnostic natives (house style of the `dodx_test_dispatch_*` set): `dodx_test_dump_round_timers()` logs every candidate field with both derived interpretations (remaining-if-end-time / elapsed-if-start-anchor) plus a timer-suspect entity scan; `dodx_test_scan_gamerules()` is a change-scanner over the documented CDoDTeamPlay extent (576 bytes) that logs each changed dword across the `mp_clan_restartround` completion edge — the tool that located the `m_flDoDMapTime` anchor. Read-only, safe on any map in any state; production plugins must not call them.

## [2.7.22] - 2026-07-11

Supersedes the staged-but-never-activated 2.7.21 (`.new` on the fleet, superseded before its 07-11 nightly). A **core-only** delta over 2.7.21 — the one crash fix below (`ktpamx_i386.so` only). The DODX module and includes are unchanged from 2.7.21 (ship the 2.7.21 `dodx_ktp_i386.so`; no new dodx `.new`). Same platform wave as 2.7.21: core (CForward refcount + CTask re-entry guard + `KTP_ExtensionShutdown`) and DODX/includes (2026-07-06 includes assessment A1/A2/A5/A6 + 07-05 review follow-ups) in one cut.

**Deploy notes:** ship no earlier than ReHLDS .928 activation (the shutdown export is inert until the engine calls it — .928 already live since 07-09); ship the module with or before any plugin written to the new checkable-return contracts (older modules still abort); build the ship artifacts with operator WIP stashed out of the tree (2.7.20 procedure).

### Fixed

#### Core: `hostname` cvar pointer NULL in extension mode — `get_user_name()`/`show_motd()` one deref from a game-thread crash

`cvar_t* hostname` is assigned in exactly one place — `C_Spawn` (the Metamod `pfnSpawn` hook), which never runs as a ReHLDS extension. The extension-mode init path re-does `C_Spawn`'s `mp_timelimit` cache but omitted `hostname`, so it stayed NULL for the whole process lifetime on every ext-mode boot. `get_user_name()` with an out-of-range index (index `< 1` or `> maxClients`, e.g. `get_user_name(0)` for the server name), `show_motd()` with no explicit title, and the per-client `gmsgServerName` write all deref `hostname->string` — NULL+4 → SIGSEGV on the game thread. Version-independent latent bug; surfaced as DAL1's 2026-07-11 mid-match crash (a plugin hit the out-of-range path). Root fix: the ext-init path now caches `hostname = CVAR_GET_POINTER("hostname")` alongside `mp_timelimit`. The three deref sites are also NULL-guarded (degrade to `""`) as defense-in-depth.

#### Core: SP-forward dedup handed out shared handles with no reference counting — live tasks executed the WRONG callback

The 2.6.10 dedup returns the same `CSPForward` handle for identical (amx, function, params) registrations — common under `set_task` (many plugins register the same callback from several sites). `unregisterSPForward` freed the handle when the FIRST holder died, pushing the id onto the free list while other holders lived; the next registration of a *different* function recycled the id, and the surviving holder's timer then executed that other function. Production signature: `task_deferred_discord_fwd` executed 9 times for 8 registrations (the 0.10.137 plugin guard suppressed the extras); the historic 5×-in-1s multi-fire was a *repeating* stale holder; a freed-but-held forward returning 0 silently is the companion lost-timer class (a plausible contributor to the old `.ready` HUD non-persistence reports beyond the 2.7.20 CTask counter fix). `CSPForward` is now reference-counted: `Set()` = 1, each dedup hit increments (and rescues a mid-execute deferred-delete), release decrements, only the last release frees the slot. Non-shared forwards behave byte-identically. An invariant tripwire logs any release observed at refcount 0 on a live, non-executing forward (caller double-release — the class refcounting can't defend against).

#### Core: `CTask::executeIfRequired` re-entry guard

A one-shot task's completion state was only written after its forward returned, so a nested engine frame during the callback could re-enter and double-fire it. Re-entrant calls on the same task object now return immediately; the outer invocation still completes/reschedules it (no starvation).

### Added

#### Core: `KTP_ExtensionShutdown` export — orderly extension-mode shutdown (the CHI1 root fix)

In extension mode nothing ever called the module-detach path at full server shutdown (`Meta_Detach` is Metamod-only; the engine dlclosed extensions cold), so module exit-time destructors ran against an unmapped core — the CHI1:27015 shutdown-segfault class that amxxcurl's atexit guard backstops. ReHLDS .928 dlsym's and calls `KTP_ExtensionShutdown` before its dlclose loop; KTPAMXX now exports it: idempotency latch → extension-init gate (Metamod mode untouched) → teardown-window guard (no Pawn can run) → `PluginsUnloading` forwards → the `Meta_Detach` core-state clear set → `detachModules()` (dodx/reapi/amxxcurl `AMXX_Detach`/`OnAmxxDetach` finally run at shutdown, with core and engine still mapped) → CLog close + async-writer drain (idempotent vs the later `~CLog` at dlclose). Honors the engine contract (post-`Cmd/Cvar/NET_Shutdown`: no cvars, no commands, no engine networking anywhere in the transitive teardown — verified per module). `plugin_end` is deliberately NOT fired here (already fired per map by `SV_InactivateClients`; on direct quit it has never fired in extension mode and running Pawn post-`Cvar_Shutdown` would violate the contract).

#### `dodx`: `weaponData[]` sized to `DODMAX_WEAPONS` — out-of-bounds reads on every DODMAX_WEAPONS-bounded loop (A1+A2)

The table was defined unsized with 42 initializers while its extern declaration (and every loop bound, including hot-path `Client_AmmoX` on every ammo update) used `DODMAX_WEAPONS` = 47. Indices 42-46 read (and, in the deactivate/rank-save clear-loop and `custom_weapon_add`, wrote) past the array into adjacent `.data`. The definition is now `weaponData[DODMAX_WEAPONS]`; the 5 trailing custom-weapon slots zero-initialize (`needcheck=false`) — same state the attach loop was already writing. Pawn side: `dodconst.inc` `DODMAX_WEAPONS` reconciled 46 → 47 to match `xmod_get_maxweapons()` (arrays sized with the old constant and looped to the native overran by one).

#### `dodx`: observed-deaths counter gated per life — structurally exactly-once (defense-in-depth over the 2.7.20 33ms window)

Both death-report paths (Damage hook and `Client_DeathMsg`) shared only the 33ms `g_lastDeathReportTime` window; production still showed `observed = pdata+1` skews when the two reports raced past it. New invariant: a victim dies at most once per life, so `g_observedDeaths[i]` increments at most once between life starts (`g_deathCountedThisLife` flag, re-armed on spawn via PStatus/ResetHUD — both un-gated by stats-pause — plus a PreThink alive-observation backstop, and cleared by the existing lifecycle resets: Init/Connect/Disconnect/`dodx_reset_all_stats`). The 33ms window is retained unchanged as the dedup for `saveKill()` and the `client_death` forward (HLStatsX kill-log semantics untouched).

#### `dodx`: recoverable native failures log + return 0 instead of aborting the calling public (A5 + the filed `dodx_set_team_score` defect)

`MF_LogError(..., AMX_ERR_NATIVE, ...)` raises a runtime error that aborts the caller, so documented "returns 0 on failure" branches were unreachable. Converted to `MF_Log` + `return 0` (10 natives): `dodx_set_team_score` (gamerules missing / invalid team — makes KTPScoreTracker 1.1.2's checked branch live), `dodx_get_team_score` (invalid team), `dodx_broadcast_team_score` (invalid team / message not registered), `dodx_set_scoreboard_team_name` (invalid team / message / empty name), `dodx_broadcast_scoreboard` (ScoreShort not registered), `dodx_set_grenade_ammo` / `dodx_get_grenade_ammo` (invalid grenade type), `dodx_send_ammox` (AmmoX not registered), `dodx_set_user_team` (invalid team), `dodx_flush_all_stats` (forward not registered). Kept as genuine aborts: out-of-range weapon/player ids, `dod_weapon_type`/`get_map_info` bad constants, the `custom_weapon_*` domain checks, and `dodx_give_grenade`/`dodx_strip_grenade` invalid type (contract documents `@error`). dodx.inc return-contract docs updated to match.

### Changed

- `dodx.inc`: `dod_damage_pre` now documents that grenade damage reduction is a post-hoc heal-back gated on the victim being alive — 100% reduction cannot save a raw-lethal hit (the pre-TakeDamage hook is deliberately not built).
- `dodx.inc`: declared the registered-but-undeclared aliases `dod_get_user_team`, `dod_get_wpnname`, `dod_get_wpnlogname`, `dod_is_melee`.
- `reapi.inc` / `reapi_gamedll.inc` / `reapi_gamedll_const.inc` / `reapi_rechecker.inc`: mirrored KTPReAPI `3d88291` contract docs (RegisterHookChain failures now log + return INVALID_HOOKCHAIN, no abort) — copies byte-identical again per the dual-copy rule.
- `reapi_version.inc`: `REAPI_VERSION` 529362 → 529365 (sync with KTPReAPI 5.29.0.365).

## [2.7.21] - superseded by 2.7.22, never activated

Staged fleet-wide as `.new` but superseded before its nightly swap, so no fleet
instance ever ran this core. Its content is described inside the 2.7.22 entry
above (2.7.22 is a core-only delta over it). The **DODX** artifact did ship: the
2.7.22 wave deployed the 2.7.21 `dodx_ktp_i386.so` unchanged, so a dodx change
traced to this cut landed on the fleet even though the core did not.

---

## [2.7.20] - 2026-07-05

Fix wave from the 2026-07-05 full-stack code review (P0 #1/#2/#3, P1 #9, plus Part-1 P2 hygiene). No new natives, no include changes — plugins do not need a recompile.

### Fixed

#### `dodx`: `g_observedDeaths[]` never reset in extension mode — score-persistence validation gate rejected nearly all saves

The only reset lived in `CPlayer::Connect()`, which extension mode deliberately never calls, so the counter was monotonic for the whole server process while scoreboard pdata `m_iDeaths` zeroes every map load. KTPMatchHandler's offset-validation gate (`save_player_score`) compared the two and rejected nearly everything — the feature has been silently no-oping in production even though the `+4` pdata offset is correct (confirmed by the 2026-07-04 fleet log sweep). Four lifecycle sites now keep the counter in step with pdata:

- `dodx_reset_all_stats()` zeroes all slots (match-start baseline);
- `CPlayer::Init()` zeroes per-slot (per-map, matches pdata's map-load zero);
- `CPlayer::Disconnect()` zeroes per-slot so a mid-map substitute joining a recycled slot doesn't inherit the leaver's tally (`Init()` is skipped for slots that already have an edict; safe vs the disconnect-save because the drop-client hook runs the chain — and therefore the plugin's save — before this POST cleanup);
- `dodx_set_user_deaths()` re-baselines the counter to the value it writes, so a restored player's next save doesn't mismatch by exactly their restored deaths (restore→re-disconnect flow).

Companion plugin-side fixes (slot leak, intermission gate, cross-half staleness) land in KTPMatchHandler 0.10.141.

#### `dodx`: death dedup guard made symmetric — DeathMsg-first ordering double-counted deaths

The 33ms `g_lastDeathReportTime` window was only checked in `Client_DeathMsg`; the Damage-hook death branch fired `saveKill()` + the `client_death` forward + the observed-deaths increment unconditionally. When DeathMsg processed first, the same death was reported twice — double kill/death log lines into HLStatsX (silent stats corruption in fleet builds) and a further-inflated observed-deaths counter. The Damage-hook side now checks the same window before reporting. Also fixed on both sides: a negative time delta (server time restarts at map change) is no longer treated as "inside the window" — stale timestamps from the previous map could suppress legitimate death reports early in the next map.

Known limitation (symmetric counterpart of the one already documented in `Client_DeathMsg`): when the guard suppresses the Damage-side fire because DeathMsg won the race, `saveKill()` is skipped — the attacker's DODX weapon-stat kill credit is lost for that death, same as any DeathMsg-only death (world/suicide) today. The forward still fires exactly once.

#### Core: `CTask` active-count double-decrement on task self-removal — all `set_task` timers could silently stall

`removeTasks()` decremented `m_ActiveCount` even when the removed task was the one currently executing; `startFrame()`'s post-execution free check then decremented again for the same task. One `remove_task(own_id)` inside a task callback — a standard AMXX idiom, used by six self-removing tick tasks in KTPMatchHandler alone — skewed the counter toward 0. Once it hit 0 with real tasks pending, `startFrame()`'s fast path stopped iterating and **every timer on the server went dead** until some new `set_task` call bumped the counter. Matches the field-reported ".ready/.confirm HUD not persisting" symptom. `removeTasks()` now skips the decrement for an in-execute task and lets the post-execution check own that transition.

#### Core: `CForward` string-pointer mitigation widened (bounded scan, page-crossing probes, write-back writability)

The 2.7.12 `mincore()` check covered less than its commit message claimed: it probed only the first page (so `strlen` could still walk off a mapped page into an unmapped neighbor and SEGV), and the FP_STRINGEX write-back reused the read check (a readable-but-read-only page — e.g. a string literal passed through a mismatched param type — still SEGV'd on write). Now: the read side does a bounded per-page-probed scan (32KB budget; unterminated-within-budget is treated as garbage, logged, and passed as `""`), the STRINGEX read copy is clamped to the 128-cell allot (an over-long incoming string could overrun the AMX heap block), and the write-back is bounded (replaces `amx_GetStringOld`'s copy-until-zero-cell, which had no output bound) behind a pipe-based writability probe of the exact byte range (`write()`/`read()` round-trip through a pipe EFAULTs instead of faulting; falls back to prior behavior if the pipe can't be created). Documented residual: freed-but-still-mapped heap and PROT_NONE regions still pass the mapped-page check — the mitigation catches wild pointers into unmapped space, which is the class seen in fleet cores.

### Changed

#### Core: async log writer hygiene

- Writer-thread spawn is now double-checked under the queue mutex (the old check-then-act could double-spawn if a module thread ever logged concurrently with the game thread).
- The 4.4MB ring is heap-allocated on first use instead of static BSS (allocation failure falls back to synchronous logging).
- Dequeue copies only the used bytes of each op instead of the full 4.3KB struct while holding the mutex.
- Linux link now passes `-pthread` explicitly (previously resolved only via glibc ≥ 2.34 folding libpthread into libc).

## [2.7.19] - 2026-07-03

### Changed

#### Core: async AMXX log writer — `CLog::Log`/`LogError` no longer touch disk on the game thread

`CLog::Log` (amxx_logging 1/2) and `CLog::LogError` did `fopen("a+")` + `fprintf` + `fclose` per line on the game thread. Each cycle joins an ext4 journal transaction, and on the fleet's consumer SSDs a journal commit in flight blocks that join for up to ~165ms — a whole-server frame freeze. The 2026-07-03 NYC perf/bpftrace investigation proved this was the last remaining 100ms+ spike class (17/17 traced stalls matched AMXX log lines to the second; a live match ate repeated stalls 17:00–18:10 ET), the same disease KTP-ReHLDS 3.22.0.927 cured for engine `Log_Printf`.

The fix mirrors the .927 design: a dedicated writer thread owns the `FILE*`; the game thread only formats the line and enqueues it into a 1024-slot ring (full queue = drop + count, never block). Each op carries its fully resolved target path, so daily rollover (type 1), per-map files (type 2), error logs, and the map-change header/footer lines all flow through one ordered queue — the writer closes/reopens when the path changes. The writer opens files line-buffered (`setvbuf _IOLBF`), so a crash loses at most the in-flight line — the same durability as the old open/write/close cycle. On write error it drops the handle and retries a fresh open on the next line.

- Gate: localinfo `amxx_log_async` — default on; set `localinfo amxx_log_async 0` for the exact legacy synchronous path. Latched per map in `MapChange()`, like the engine's `ktp_log_async` at `Log_Open`.
- `pthread_create`/`CreateThread` with a synchronous fallback if thread creation fails (built with `-fno-exceptions`; `std::thread` can't fail cleanly).
- Shutdown: the `CLog` destructor (which runs inside `dlclose` at `Host_Shutdown` → `ReleaseEntityDlls`) drains the queue and joins the writer, so the final lines — including "Log file closed." — hit disk before the `.so` unmaps. This is the real production path: `Meta_Detach` never fires in extension mode, and `dod_i386.so` owns the `pfnGameShutdown` slot (verified via relocations: `GameShutdown__Fv`), so the loader's only-if-empty merge can't take an extension hook. The writer's mutex/condvar are heap-allocated on first use and deliberately never destroyed, so the destructor path is immune to cross-TU static-destruction order.
- `CreateNewFile` still creates/truncates the file synchronously (one metadata op per map change) — its filename scan probes on-disk existence, so a writer-deferred create would let back-to-back map changes reuse and clobber a name; only the header line is queued. Create failure keeps the legacy behavior (ALERT + `amxx_logging 0`).
- Dropped lines (full queue, or open/write failure on the writer thread) are counted and reported to the server console at the next map change (`[AMXX] async log writer dropped N line(s)…`), mirroring the engine's `logq_drops=`.
- Behavior change (accepted tradeoff): with async on, a log file deleted mid-map is no longer recreated per line — the writer holds its handle until the path changes (next map or day), and a persistently broken log path no longer disables AMXX logging mid-map; it surfaces via the drop counter instead.
- Console echo (`print_srvconsole`) is unchanged and stays synchronous (measured µs).
- amxx_logging 3 (HL logs) is unaffected — it already routes through the engine's async writer.

### Fixed

#### Extension mode never ran `CLog::MapChange()`

The extension port only called `SetLogType()` once at startup — Metamod mode runs `g_log.MapChange()` from `C_Spawn` on every map. Consequences before this fix: per-map log rotation (`amxx_logging 2`) silently never rotated in extension mode, the `-------- Mapchange to <map> --------` marker never appeared in AMXX daily logs, and (new in this release) the `amxx_log_async` latch and drop-counter report would never have run. Extension startup and `KTPAMX_ReloadPlugins()` now call `MapChange()`, matching Metamod behavior exactly. Observable change: AMXX daily logs on the fleet gain the standard per-map `Mapchange` marker lines.

## [2.7.18] - 2026-06-11

### Added

#### `dodx`: `dod_client_weapon_fire(id, weapon, Float:gametime)` per-shot forward

Fires on every primary-attack actuation from `CPlayer::saveShot` — the single shot-accounting chokepoint — so it catches pure-miss shots the `client_damage` hits-stream never reports. Server-side enabler for the fire-cadence clock (wheel-turbo detection) and future per-fire timing detectors.

`saveShot` is shared by the clip-decrement path and the hitscan-trace, grenade, rocket, and melee-gated damage paths, so the forward also fires for grenades/rockets/melee. That is correct for an actuation/input-multiplication clock; a firearm-only consumer must filter by weapon id. `gametime` is `gpGlobals->time` at the fire, passed via `amx_ftoc` like other float cells.

Backward-compatible — plugins that don't register the forward are unaffected. Consumers recompile against the updated `dodx.inc`.

## [2.7.17] - 2026-05-21

### Added

#### `dodx`: Per-player score / deaths native infrastructure for mid-match score persistence (spike branch B+C+broadcast)

Built on the existing 5/11 v1.2 spike (commit `851295e4` — added `dodx_get/set_user_deaths` + `dodx_get/set_user_score` natives writing to `STEAM_PDOFFSET_DEATHS` / `STEAM_PDOFFSET_SCORE` in pvPrivateData). Three composed changes round out the design after 5/21 client-test discoveries:

**1. Independent `g_iScoreDeathsOffsetAdjust` global** (`dodx.h` + `moduleconfig.cpp`). Splits the score/deaths offset adjust from the grenade adjust (`g_iLinuxPdataOffsetAdjust`). Both default to `+4` (Ubuntu 24.04 baremetal fleet), but grenade is auto-detect-promoted to `+5` at first grenade op via the heuristic in `moduleconfig.cpp:1987` — which 5/21 evidence proved is correct for grenades but FALSE-POSITIVE-promotes score/deaths offsets one int past the correct location. Two field families, two different correct adjusts on the same OS build. Score/deaths keep their own adjust and intentionally skip the auto-detect (the heuristic doesn't generalize — non-zero-int-at-offset ≠ correct-field). Override via new `dodx.ini` key `score_deaths_offset = N`. Confirmed offsets via disassembling production `dod_i386.so` md5 `4f4727b2…`: `m_iObjScore` at byte `0x780` (int 480 = base 476 + 4), `m_iDeaths` at byte `0x784` (int 481 = base 477 + 4) — see `KTPMatchHandler/research/OFFSETS_RESEARCH_2026-05-21.md`.

**2. `dodx_get_observed_deaths(id)` native + dedicated `g_observedDeaths[33]` counter** (`NBase.cpp` + `usermsg.cpp` + `CMisc.cpp` + `dodx.inc`). Engine-authoritative ground-truth death counter for the validation gate. Ticks once per death event in `usermsg.cpp`'s death paths — both the Damage hook (deaths via normal frag flow) and `Client_DeathMsg` (suicides via `kill` console + world deaths). The pre-existing 33ms dedup gate against `g_lastDeathReportTime[]` already prevents double-fire, so the new counter stays one-tick-per-death. Reset on `CPlayer::Connect` so reconnects don't inherit the prior session's tally.

Selected over the obvious alternatives:
- `pPlayer->life.deaths` (DODX `CPlayer` stat) — 5/21 verified under-counts: only ticks via `CPlayer::saveKill` from the Damage hook path, so `kill`-console suicides bypass it. Returned 0 against scoreboard=2 in the 5/21 test.
- AMXX core `get_user_deaths(id)` — its `Client_ScoreInfo` hook on the `ScoreInfo` user message doesn't catch DoD's death broadcasts. DoD doesn't even register `gmsgTeamInfo` server-side; its scoreboard updates flow via `gmsgScoreShort` / `gmsgScoreInfoLong` instead, neither handled by AMXX core. Returned 0 with scoreboard=2 in the same 5/21 test.

**3. `dodx_broadcast_scoreboard(id)` native** (`NBase.cpp` + `dodx.inc`). Refreshes a player's scoreboard row immediately by sending a `ScoreShort` message in DoD's exact native format (BYTE id + SHORT m_iObjScore + SHORT frags + SHORT m_iDeaths + BYTE 1). Format derived from disassembling the broadcast site immediately after `inc DWORD PTR [eax+0x784]` in `CDoDTeamPlay::PlayerKilled` (b2774-b27ee in `dod_i386.so`). Uses direct engine-func `MESSAGE_BEGIN`/`WRITE_BYTE`/`WRITE_SHORT`/`MESSAGE_END` — the same pattern as `dodx_broadcast_team_score` (proven safe since v0.10.20 per the historical CLAUDE.md note), explicitly NOT the AMX `message_begin` Pawn native path which crashed ATL:27019 on 5/21 v1.3.1 RESTORE (vtable lookup segfault at `ktpamx_i386.so+0x561c3`).

**Tested 2026-05-21 on ATL:27019 baremetal:** full SAVE → disconnect → reconnect → RESTORE cycle validated end-to-end. Scoreboard refreshed immediately on RESTORE with no crash. Validation gate caught the offset-vs-observed mismatch in earlier iteration (when `life.deaths` was the ground truth) — proving the gate works as designed.

**Branch state:** This commit lands on `feature/dodx-score-persistence-spike-v1.2`; do NOT merge to master until consumer plugin (`KTPMatchHandler`) is also ready + fleet-deploy pipeline (`.new` push to all 25 instances) is wired. The plugin side is on `main` of `KTPMatchHandler` waiting on the same upstream gate.

---

## [2.7.16] - 2026-05-06

### Fixed

#### `client_infochanged` forward + `CPlayer::name` cache stuck at connect-time name in extension mode
`get_user_name()` reads `CPlayer::name`, an internal AMXX cache. That cache is only refreshed by `C_ClientUserInfoChanged_Post` (`amxmodx/meta_api.cpp:1635`), which is wired into `gFunctionTable_Post.pfnClientUserInfoChanged` — the Metamod DLL-export table. In extension mode there is no Metamod, so the engine calls the game DLL's `pfnClientUserInfoChanged` directly and the AMXX post-hook never fires. Two consequences:

1. The `client_infochanged` forward never fires, contradicting the audit note in `KTPAMXX_NATIVE_AUDIT.md` ("Not needed").
2. `CPlayer::name` stays frozen at the connect-time name. Every subsequent `get_user_name()` call — including the one a plugin makes inside its own `dod_client_spawn` / `client_putinserver` handler on respawn — returns the stale name even after the player ran `setinfo "name" "..."` mid-life. The engine's `pEdict->v.netname` is updated correctly (the in-game DeathMsg kill feed reads the new name), creating a visible divergence between AMXX-driven HUDs and the engine's own scoreboard.

Discovered 2026-05-03 against the production fleet: a player connected as `金緑ぎ子供[bc]`, renamed to `incite` mid-match via setinfo, died and respawned multiple times, and AMXX-fed HUDs (DoD HUD Observer overlay) still rendered the original CJK name while the in-game kill feed showed `incite`.

**Fix:** Added extension-mode `SV_ClientUserInfoChanged_RH` hook in `amxmodx/meta_api.cpp` (forward decl beside the other extension-mode hooks; definition next to the disabled `Steam_GSBUpdateUserData_RH` placeholder block, which had been left as a dead pass-through under the wrong assumption that `C_ClientUserInfoChanged_Post` covered the extension-mode path). Registered via `RehldsHookchains->SV_ClientUserInfoChanged()->registerHook(...)` in the extension-mode init block alongside `Steam_NotifyClientConnect`. The handler calls `chain->callNext(cl)` first (post-hook semantics so the engine's userinfo update has applied), then short-circuits in Metamod mode to avoid double-firing, validates the client/edict/index/initialized/ingame, skips fakeclients, re-reads the infobuffer via `GET_INFOKEYBUFFER` + `INFOKEY_VALUE`, and only refreshes `pPlayer->name` + fires `FF_ClientInfoChanged` when both the infobuffer and name key are present (defensive against engine infobuffer corruption / connect-time races, which would otherwise reproduce the same stale-cache bug class). Flipped the `client_infochanged` row in `KTPAMXX_NATIVE_AUDIT.md` from N/A / "Not needed" → OK / `SV_ClientUserInfoChanged` / "Userinfo changes; refreshes `CPlayer::name` (`get_user_name`)".

**Compatibility:** Strictly additive. In Metamod mode the new hook is a no-op pass-through (early `return` after `chain->callNext(cl)` when `g_bRunningWithMetamod` is true), so the existing Metamod path through `C_ClientUserInfoChanged_Post` is untouched. The `client_infochanged` forward signature is unchanged. No plugin source needs recompilation. The ReHLDS hookchain `SV_ClientUserInfoChanged` was already exposed in the API (`public/resdk/engine/rehlds_api.h:383`) — this PR simply consumes it.

**Verification:** `bash build_linux.sh` build succeeds. Pre-push hook compile of the consuming plugin (DoD HUD Observer's `KTPHudObserver.sma`, which calls `get_user_name()` from `dod_client_spawn` / `client_authorized` / roster dump paths) passes. End-to-end behavior reproducible by connecting under one name, calling `setinfo "name" "newname"` mid-life, and observing that the next AMXX-driven HUD update reflects the new name without requiring a respawn cycle.

---

## [2.7.15] - 2026-04-30

### Fixed

#### `ktp_version_reporter.inc` — silent truncation past 2 entries (engine `MAX_KV_LEN=127` cap)
The shared `amx_ktp_versions` rcon command was reporting only the first 2 plugins by load order (KTP Admin Audit, KTP Cvar Checker) instead of all 9 KTP plugins that adopted the include 2026-04-25. Discovered 2026-04-30 09:43 ET against ATL:27015 — a `localinfo` console dump showed `_ktp_v_data` capped at 113 bytes containing exactly two entries.

Root cause was structural to the v1 include design: it accumulated all entries into a single `_ktp_v_data` localinfo string. GoldSrc/ReHLDS `Info_SetValueForStarKey` (which `set_localinfo` resolves to) enforces a per-value cap of `MAX_KV_LEN = 127` chars (see `KTPReHLDS/rehlds/engine/info.h:34` + `info.cpp:611`). The first 2 entries (113 chars) fit; the third would push past 127 → silently rejected at the engine boundary, while AMXX's `set_localinfo` Pawn native returns 1 (success) regardless.

##### Changed
- **`plugins/include/ktp_version_reporter.inc`** — Replaced the localinfo-string accumulator with an AMXX multi-forward (`CreateMultiForward("KTP_OnVersionDump", ET_IGNORE, FP_CELL, FP_ARRAY)`). Each plugin owns its own one-line dump via a `public KTP_OnVersionDump(id, counter[1])` callback; the rcon handler fires `ExecuteForward` and lets every plugin print its own line. The counter uses `PrepareArray` with copyback so each plugin can increment `counter[0]` for the totals footer. Localinfo is now used only for a single-byte `_ktp_v_reg` flag (well under the 127-cap) to gate "first registrant" concmd registration.

##### Compatibility
Plugin source files unchanged — only the include semantics changed. All 9 KTP plugins recompiled cleanly against the new include with no source modifications. Old `_ktp_v_data` localinfo key is no longer written; stale data from prior server runs persists harmlessly until restart.

##### Verification
Activation gated on next 03:00 ET nightly auto-swap (`.new` files staged to all 24 active fleet instances 2026-04-30 ~10:25 ET, md5s verified post-deploy). Post-restart expectation: `amx_ktp_versions` rcon shows all 9 plugins with `Total: 9 KTP plugin(s) loaded`. v1 failure mode captured for forensic record in TODO.md and `info_string_max_kv_len_cap.md` memory.

---

## [2.7.14] - 2026-04-29

### Build system

#### Vendored Metamod headers — drop external `metamod-am` build dependency
KTPAMXX previously required a sibling checkout of `alliedmodders/metamod-hl1` (locally named `metamod-am`) at build time, supplied via the `--metamod` configure flag or the `METAMOD` environment variable. This was confusing — KTP runs in extension mode and never loads Metamod at runtime — and added an external dep that CI workflows had to checkout for every build.

Vendored the actual subset KTPAMXX uses (transitive closure of `<meta_api.h>` and `<sdk_util.h>`: 13 headers, ~126 KB) into `third_party/metamod/`. See `third_party/metamod/README.md` for the file list and update procedure.

##### Changed
- **`AMBuildScript`** — Removed `detectMetamod()` method, the `self.metamod_path` field, and the call to `detectMetamod()` in module init. The conditional `if self.metamod_path: compiler.cxxincludes.append(...)` was replaced with an unconditional `compiler.cxxincludes.append(third_party/metamod)`.
- **`configure.py`** — Removed the `--metamod` CLI option. Build no longer accepts or requires it.
- **`third_party/metamod/`** — New directory containing 13 headers (dllapi.h, engine_api.h, enginecallbacks.h, log_meta.h, meta_api.h, meta_eiface.h, mhook.h, mreg.h, mutil.h, osdep.h, plinfo.h, sdk_util.h, types_meta.h) verbatim from `alliedmodders/metamod-hl1`, plus the upstream GPL `LICENSE.txt` and a `README.md` documenting why and the update procedure.

##### Compatibility
Strictly internal — no runtime behavior change, no plugin-facing API change. The `--metamod` flag and `METAMOD` env var are now silently ignored if passed (option no longer registered). Anything that scripts the build with those vars set will need them removed from the invocation; KTPInfrastructure's `build/amxx/Dockerfile` updated in lockstep (companion KTPInfrastructure 1.5.5).

##### Why now
Identified as cleanup in TODO.md ("Vendor metamod-am headers — drop external build dep"). Cost ~3-4h as estimated. Upstream `metamod-hl1` is essentially abandoned (last meaningful commit ~2018), so vendoring is safe and won't drift.

##### Verification
- `bash build_linux.sh` build succeeds with no compile errors related to the move.
- `obj-linux/packages/base/addons/ktpamx/dlls/ktpamx_i386.so` produced at expected ~6.9 MB.
- Pre-existing warnings (sh_tinyhash.h `Walloc-size-larger-than`, ld text relocation in natives-asm.obj) unchanged — unrelated to this change.

---

## [2.7.13] - 2026-04-23

### Fixed

#### DODX forwards silently stall across map changes — `g_pFirstEdict` per-map re-init blocked by `FNullEnt` check + no recovery path in PreThink
Two chained bugs in `modules/dod/dodx/moduleconfig.cpp` caused every DODX-forward-based event (`kill`, `damage`, `prone_change`, `player_spawn`, `player_team_change`, `flag_captured`, `player_score`) to go silent within hours of server restart on all three production KTP hosts (DEN5, ATL1, NY1), with AMXX core hooks & polling tasks still firing. A full `restart` was the only known workaround.

**Bug #1 — `DODX_OnSV_ActivateServer` used `!FNullEnt(pWorld)`** (line 1160). `FNullEnt(edict 0)` returns TRUE because edict 0 IS the world entity — the same issue that was fixed in `DODX_SetupExtensionHooks` in 2.7.5 (`b95b82c1`) via the comment *"Do NOT use FNullEnt — edict 0 IS the world entity (index 0 is valid)"*. That fix was applied to the attach-time fallback path but missed the per-map hook path, so every map change's init was silently skipped. Replaced the check with a plain `if (pWorld)` and added an `MF_Log` on the `pWorld == NULL` branch so future hook-miss conditions surface in logs instead of stalling silently.

**Bug #2 — `DODX_OnPlayerPreThink` had no recovery path after 2.7.4 removal** (lines 947-948). Commit `096adb70` replaced the `ENTINDEX()`-based fallback with a hard `return`. Once the per-map init failed (for any reason, not just bug #1), forwards stayed silent until plugin re-attach. Restored the fallback with an explicit `tmpIndex >= 1 && tmpIndex <= maxClients` bounds check (addresses the original "unsafe fallback init" concern), extension-mode-only (Metamod builds untouched), and logs via `MF_Log` so recoveries are visible.

**Evidence:** Symptom isolation is mathematical — every silent event type routes through the `g_pFirstEdict` gate; every event type that keeps firing (team_score, time_sync, flags_init, player_connect, user_say) does not. Live `restart` verification on ATL1 confirmed attach-time fallback is what re-enables forwards. Local 4h/589-rotation stress test on vanilla HLDS did not reproduce, ruling out rotation count & wall-clock uptime as standalone triggers — prod-specific (KTP-ReHLDS hook behavior + real HLTV/player churn) is what eventually makes `DODX_OnSV_ActivateServer` skip init on a given map.

**Risk:** Patch #1 is a strict re-application of 2.7.5's sibling-path fix — same reasoning, same happy-path behavior. Patch #2 restores pre-2.7.4 behavior with added bounds check, extension-mode-guarded. Happy-path Metamod builds and correctly-initialized extension-mode servers are untouched.

**Credit:** Diagnosis and patch by @JimmyLockhart65616 (PR [#4](https://github.com/afraznein/KTPAMXX/pull/4)).

---

## [2.7.12] - 2026-04-22

### Fixed

#### Forward execution UAF crash — `CForward::execute` / `CSPForward::execute`
Both forward-execution paths (global forwards in `CForward.cpp:67` and single-plugin forwards at `CForward.cpp:289`) had a defensive check that rejected only `NULL` and pointers `< 0x1000` before calling `strlen()` on `FP_STRING` / `FP_STRINGEX` parameters. In practice, use-after-free on scheduled tasks (e.g. `set_task` with a string param whose backing memory was freed between registration and fire) leaves a high-value garbage integer in the cell slot — a value like `0x3f145406` passes the `< 0x1000` check but points to an unmapped page, and `strlen()`'s SSE-accelerated `movdqu` instruction SEGVs the entire server process.

Replaced the existing check with a new helper `amxx_is_string_ptr_readable(ptr)` that combines the old NULL/low-page rejection with a `mincore()` syscall to verify the containing page is actually mapped. Unmapped pages are rejected and the string defaults to `""` with a diagnostic warning logged instead of crashing. Windows builds fall through to the old behavior (no `mincore()` equivalent and this crash pattern is i386-Linux-specific on our deployment).

Also applied the same check to the `FP_STRINGEX` writeback path (`CForward.cpp:196` and `:414`) which had the identical vulnerability on copy-back — a stale pointer would SEGV inside `amx_GetStringOld`'s `memcpy` after the plugin's param was modified. Both read and write paths now use the helper consistently.

Fleet impact: 15 segfaults captured across Atlanta (2026-04-21) and New York (2026-04-22) — all traced to this path via gdb on `/tmp/core.hlds_linux.*` dumps captured after the core-dump fleet rollout earlier the same day.

---

## [2.7.11] - 2026-04-19

### Added

#### Live CAreaCapture state natives (PR #1 by JimmyLockhart65616)
Resized `pd_dca.unknown_block_16` so `cap_mode` lines up with `m_iCapMode` per `offsets-careacapture.txt` gamedata (Win=492, Linux=512). Renamed `iunk_*` slots to match in-game struct fields: `cap_mode`, `is_capturing`, `capturing_team`, `owning_team`, `cap_time` (was `time_to_cap`), `time_remaining` (was `iunk_128`, now float), `num_allies`, `num_axis`. Exposed via `dodx_area_get_data` with new `CA_*` enum values: `CA_num_allies`, `CA_num_axis`, `CA_is_capturing`, `CA_capturing_team`, `CA_owning_team`, `CA_cap_mode`, `CA_time_remaining`. Plugins (HUD Observer, MatchHandler, HLTV) can now read live cap state directly from the engine instead of reimplementing AABB/radius math.

#### DeathMsg handler — suicide / world-kill path (PR #1)
New `Client_DeathMsg` catches deaths the Damage hook misses — suicides via `kill` console and world deaths (fall, drown, trigger_hurt) where no Damage message is sent. Resolves weapon name → wpnindex and dedups against `g_lastDeathReportTime` so the normal kill flow doesn't double-fire.

#### `scripts/pre-push.sh` and `scripts/install-hooks.sh` (PR #1)
Pre-push hook runs `make build-amxx` + `make build-plugins` from a sibling `KTPInfrastructure/` checkout to catch API-breaking changes against every downstream plugin before the push is accepted. Bypass with `git push --no-verify` or `KTP_SKIP_PREPUSH=1`.

### Fixed

#### InitObj → DLL ordering reorder in extension mode (PR #1)
Entity-scan order during `SV_ActivateServer` isn't guaranteed to match the DLL's `SetObj` id space. The first matching InitObj (`newCount == mObjects.count` and `!g_cpOrderingFinalized`) now snapshots entity-scan entries, clears `mObjects`, parses the InitObj, and re-pairs each CP's `pAreaEdict` by matching edict pointers. Re-fires `iFInitCP` so SMA plugins rebuild their name cache in DLL order. Stale / partial InitObj messages are skipped.

#### pdata origin unreliable — BSP sort used wrong coords (PR #1)
`DODX_InitCPFromEntities` now reads origin from `pEdict->v.origin` instead of `cpd.origin_x/y`. The pdata origin offsets were unreliable (observed as `(0, world_x)` on dod_anzio), which silently broke BSP `point_index` reorder and mapped CP names to the wrong entity.

#### DeathMsg dedup window too wide (PR #2)
`Client_DeathMsg` used a 100ms dedup window (~12 frames at 120Hz) — much wider than needed since the Damage hook and DeathMsg fire in the same `SV_RunCmd` pass (<1ms apart). Tightened to 33ms (~4 frames), well beyond expected engine jitter. Documented residual edge case (FPS dip >33ms could cause TK misreport) in-code for future debugging.

### Changed

#### CP-init diagnostic logs gated behind `DODX_DEBUG_CP_INIT` (PR #2)
BSP sort entity+bsp dump in `DODX_InitCPFromEntities` and the case 0 / per-CP reorder-list logs in `Client_InitObj` fired on every map change across every instance (~12 lines per map load, 60 per server rotation, 300 fleet-wide). Gated behind compile-time `#ifdef DODX_DEBUG_CP_INIT`. Default off in prod; enable with `-DDODX_DEBUG_CP_INIT=1` when investigating. Summary line (`InitObj: reordered N CPs to DLL order`) and error-condition logs remain. `CVAR_REGISTER` crashes in extension mode, so a runtime cvar wasn't viable.

---

## [2.7.10] - 2026-04-16

### Fixed

#### TraceLine iClassName Comparison Broken
Reverted `iClassName` integer comparison back to `strcmp` in both Metamod and extension mode TraceLine hooks. `ALLOC_STRING` does NOT intern/deduplicate strings in GoldSrc — two calls with the same text (e.g. `ALLOC_STRING("grenade")` in DODX vs the game DLL's own allocation) produce different `string_t` offsets, so the integer comparison never matched. This broke the `dod_grenade_explosion` forward, which meant practice mode grenade refill on explosion stopped working.

#### v2.7.8/v2.7.9 Code Changes Restored
All code optimizations from v2.7.8 and v2.7.9 were lost during the AmxxCurl CMake migration rollback (2026-04-14). Restored: g_putinserver bitmask, module frame callback cache, event vault pre-allocation, WeaponsCheck XOR, grenade object pool.

---

## [2.7.9] - 2026-04-02

### Changed

#### Event Vault Pre-allocation
`EventsMngr::NextParam()` no longer dynamically grows the parse vault with `new`/`delete`/`memcpy` on every resize. The vault is now pre-allocated to 32 entries on first use (game messages have at most ~16 parameters). Eliminates allocation churn during high-frequency message parsing at 1000Hz.

#### WeaponsCheck XOR Bitwise Loop
`CPlayer::WeaponsCheck()` no longer iterates through all 42 weapon slots per player per frame. Uses XOR to find only changed bits, then `__builtin_ctz` to iterate only those weapons. Reduces from 42 iterations to ~2-3 on average (typical weapon pickup/drop). Grenade slots masked out with a static bitmask instead of per-iteration if-chain.

#### Grenade Object Pool
`Grenades` class replaced linked list (`new Obj` per throw, `delete` on expiry, O(n) pointer-chasing scan) with a fixed-size 32-entry pool. Zero allocation at runtime, cache-friendly linear scan, automatic expiry marking on find(). Pool size of 32 far exceeds any realistic concurrent grenade count.

---

## [2.7.8] - 2026-04-02

### Changed

#### g_putinserver Bitmask
Replaced `ke::Vector<int> g_putinserver` with a `uint32_t` bitmask for pending `client_putinserver` forwards. The vector was scanned and compacted every frame during player joins. The bitmask has zero cost when empty (single integer compare), and O(maxClients) bit scan when players are joining — no memory allocation, no compaction, no resize.

#### Module Frame Callback Length Cache
`Module_ExecuteFrameCallbacks()` now caches `g_moduleFrameCallbacks.length()` before the loop instead of calling it per iteration.

#### DODX TraceLine String Lookup
Grenade/rocket classname matching in TraceLine hooks now uses pre-cached `ALLOC_STRING` integer comparison instead of `strcmp()` against 6 traceData entries. The `iClassName` field is initialized during `ServerActivate`/`SV_ActivateServer` when precache strings are available. Saves ~50µs per grenade/rocket hit.

---

## [2.7.7] - 2026-04-02

### Changed

#### Compiler Optimizations
- `-O3` replaces `-O2` for aggressive inlining and loop unrolling
- `-march=native -mtune=native` enables SSE4.2, AVX, BMI instructions on server hardware
- `-flto` (link-time optimization) enables cross-translation-unit inlining for hot paths (AMX execution, native dispatch, DODX stats processing)
- `-fno-math-errno` eliminates redundant errno stores after math calls

---

## [2.7.6] - 2026-04-13

### Fixed

#### ktp_discord.inc: 164ms TLS Handshake Spike on First Discord Request
Every Discord request created a new curl handle (`curl_easy_init`), establishing a fresh DNS + TCP + TLS connection each time. On cold connections to the Cloud Run relay, this blocked the main thread for 100-164ms. Added `CURLOPT_CONNECTTIMEOUT` (2s), `CURLOPT_TCP_KEEPALIVE`, and `CURLOPT_DNS_CACHE_TIMEOUT` (5min) to all curl requests. Added `ktp_discord_prewarm()` that fires a lightweight `/health` GET at config load to establish the TLS connection before any admin action needs it.

---

## [2.7.5] - 2026-04-04

### Fixed

#### DODX Extension Mode: CPlayer Uninitialized on First Map (moduleconfig.cpp)
In extension mode, `g_pFirstEdict` and `g_bServerActive` were set by the `SV_ActivateServer` hook. However, this hook was registered during `DODX_SetupExtensionHooks()` (called from `OnAmxxAttach`), which runs AFTER the server has already activated for the first map. The hook only fired on subsequent map changes. On the first map, `g_pFirstEdict` stayed NULL, causing `DODX_OnPlayerPreThink` to bail out before initializing any `CPlayer` structs. All DODX natives (`dodx_set_user_noclip`, `dodx_give_grenade`, `dodx_set_grenade_ammo`, etc.) silently returned 0 because `CHECK_PLAYER` saw `ingame=false`. Added fallback initialization in `DODX_SetupExtensionHooks` that reads `INDEXENT(0)` directly. Also fixed `FNullEnt` check on world edict (index 0 is valid, not null).

#### DODX Extension Mode: Player Init Blocked by Stats Pause (moduleconfig.cpp)
`DODX_OnPlayerPreThink` checked `isModuleActive()` before initializing players. When stats collection was paused (`g_bStatsPaused` or `dodstats_pause` cvar), new players were never marked `ingame`, permanently breaking all DODX natives for them. Moved player initialization before the `isModuleActive()` gate — player tracking must work regardless of stats pause state.

### Added

#### DODX Natives: `dodx_get_user_movetype`, `dodx_debug_player_state` (NBase.cpp, dodx.inc)
New diagnostic natives for extension mode where the engine module is unavailable:
- `dodx_get_user_movetype(id)` — returns player movetype (3=WALK, 8=NOCLIP)
- `dodx_debug_player_state(id)` — returns CPlayer state bitmask (ingame/edict/free/nullent)

---

## [2.7.4] - 2026-03-24

### Fixed

#### Message Hook RemoveHook Wrong Index (messages.h)
`RegisteredMessage::RemoveHook` called `m_Forwards.remove(forward)` where `forward` is the SP forward ID value. `ke::Vector::remove(size_t)` treats the argument as a position index, so this removed at position `forward` (a random index) instead of position `i` (the matched entry). Stale forward IDs accumulated in the hook vector on every map change cycle of `register_message`/`unregister_message`. Fixed to use `m_Forwards.remove(i)`.

#### Client_ObjScore Stale Player Pointer (DODX usermsg.cpp)
`Client_ObjScore` used a `static CPlayer*` across message parse states without revalidation at case 1. If the edict was freed between case 0 and case 1 (possible during extension mode message dispatch), `pPlayer->savedScore` would read corrupt memory. Added validity recheck (`ingame`, `pEdict`, `pEdict->free`) at the top of case 1.

#### PreThink Fallback Init Removed (DODX moduleconfig.cpp)
`DODX_OnPlayerPreThink` had a fallback `g_pFirstEdict` initialization using `ENTINDEX()` that could run if `DODX_OnSV_ActivateServer` failed. `ENTINDEX` is an engine call that may not be safe during early-stage init. Replaced with a hard guard (`if (!g_pFirstEdict) return;`) since `DODX_OnSV_ActivateServer` is the proper init path.

#### CPlayer::Disconnect Missing Edict Free Check (DODX CMisc.cpp)
`Disconnect()` called `ignoreBots(pEdict)` without first checking if the edict was freed. During crash/map-change sequences, `pEdict->free` can be set before `ClientDisconnect` fires, causing `ignoreBots` to dereference freed entity flags. Added `if (!pEdict || pEdict->free) return;` guard.

#### Event/LogEvent Dedup O(n) Reverse-Lookup Eliminated (CEvent.cpp, CLogEvent.cpp)
Event and log event dedup scanned the full `EventHandles`/`LogEventHandles` table to recover the handle ID for a duplicate registration. Added `m_HandleId` field to `ClEvent` and `CLogEvent`, cached at creation time, enabling O(1) handle lookup during dedup.

#### Rank Save Skipped in Extension Mode (DODX moduleconfig.cpp)
`ServerDeactivate` called `g_rank.saveRank()` unconditionally, performing unnecessary file I/O in extension mode where the rank system is unused. Added `if (!g_bExtensionMode)` guard.

#### CTaskMngr::startFrame Use-After-Realloc (CTask.cpp)
`startFrame()` cached `auto &task = m_Tasks[i]` as a reference, then called `task->executeIfRequired()`. If the callback called `set_task()` → `registerTask()` → `m_Tasks.append()`, the vector's internal buffer could reallocate, invalidating the cached reference. Subsequent `task->isFree()` read freed memory. Fixed by re-indexing `m_Tasks[i]` after each callback instead of caching a reference.

### Added

#### `dodx_set_stats_paused` Native (DODX NRank.cpp, Utils.cpp)
New native `dodx_set_stats_paused(bool paused)` allows plugins to pause/unpause DODX stats collection. When paused, `isModuleActive()` returns false — kills, damage, shots, and ObjScore are not tracked. Used by KTPMatchHandler for round-freeze filtering (pause stats during freeze time, unpause on round live).

---

## [2.7.2] - 2026-03-13

### Fixed

#### CLogEvent Second Overload Last-Char Trim (CLogEvent.cpp)
The variadic `setLogString` overload still had the old `logString[--len] = 0` trim that was correctly removed from the first (va_list) overload. Every log event written through `AlertMessage_RH` in extension mode had its last character silently dropped (typically the closing `"` on DoD log events), which could break `register_logevent` filter matching.

#### MessageHook_Handler Null Chain Propagation (meta_api.cpp)
When `msg` was null, the handler called `chain->callNext(null)` which would propagate null into downstream hooks that dereference `msg`. Changed to return immediately without calling the chain.

#### Say/Say_team Prefix List Separation (meta_api.cpp)
Registered `say_team` prefix before `say` to prevent prefix list merging. `findPrefix` uses `strncmp` with the prefix's own length, so `"say"` (len 3) matched `"say_team"` causing both to share one list (~199 entries). Now separated: ~119 `say` + ~80 `say_team`.

---

## [2.7.1] - 2026-03-13

### Critical — SP Forward Null Deref (CForward.cpp)
`CSPForward::execute` crashed if `findPluginFast(m_Amx)` returned null (plugin unloaded while forward handle still live). Added null check before dereference.

### Critical — MessageHook_Handler Null Check Order (meta_api.cpp)
Null check on `msg` occurred after `chain->callNext(msg)` — crash if msg was null inside the chain. Moved null check before callNext.

### Critical — Extension Mode Map-Change Memory Leaks (meta_api.cpp)
`KTPAMX_ReloadPlugins` was missing `g_xvars.clear()`, `g_vault.clear()`, and `ClearPluginLibraries()`. Xvar IDs accumulated without dedup each map change (stale cross-plugin variable access). Plugin-registered native trampolines leaked mmap'd memory per map change.

### Critical — DODX `dod_weaponlist` Array OOB (NBase.cpp)
Bounds check used `WEAPONLIST` (hardcoded 71) but the `weaponlist[]` array only has 42 entries. Indices 42-70 accessed uninitialized memory. Replaced with `WEAPONLIST_SIZE` computed from actual array size.

### Critical — Event `parserInit` Off-By-One (CEvent.cpp)
Guard used `msg_type > MAX_AMX_REG_MSG` (should be `>=`). Allowed access one past end of `m_Events[]` array.

### Fixed

#### DODX Shot Double-Counting Disabled (CMisc.cpp)
`CheckShotFired()` button-based shot detection ran in PreThink alongside `CurWeapon` message handler clip-decrement detection. Every shot was counted twice in extension mode, inflating HLStatsX accuracy stats. Disabled button-based path — CurWeapon handler is authoritative in both modes.

#### `SV_CheckConsistencyResponse_RH` Player Guard (meta_api.cpp)
Added `pPlayer->initialized` check before firing `FF_InconsistentFile` forward. Prevents plugin handlers from accessing uninitialized player state during connection handshake.

#### `dodx_give_grenade` Entity Leak (NBase.cpp)
`oldSolid` was captured before `pfnSpawn` which changes `.solid`. Post-touch comparison was always false, so failed pickups never cleaned up the entity. Moved capture to after spawn.

#### `_FORTIFY_SOURCE=2` Debug Build Fix (AMBuildScript)
Moved `-D_FORTIFY_SOURCE=2` into the optimization block. GCC requires `-O1+` for FORTIFY; without it, `-Werror` fails debug builds.

#### `CLogEvent` Truncation Handling (CLogEvent.cpp)
Used full `sizeof(logString)` instead of hardcoded 255. Fixed POSIX truncation detection (returns would-be length, not -1). Removed unnecessary last-character trim.

#### `C_ClientConnect_Post` Bounds Check (meta_api.cpp)
Added ENTINDEX bounds check before `GET_PLAYER_POINTER`. Prevents crash if Metamod passes entity 0 or out-of-range entity.

#### `DODX_OnMsgBegin` gpGlobals Guard (moduleconfig.cpp)
Added null guard on `gpGlobals` before accessing `->maxClients`. Prevents crash if message fires before DODX extension hooks are initialized.

#### `CALMFromFile` sscanf Width (CPlugin.cpp)
`sscanf("%s")` into `pluginName[256]` changed to `"%255s"`.

#### `srvcmd.cpp` strtol Validation (srvcmd.cpp)
`strtol` end-pointer check was dead code (`!pEnd` — strtol never returns NULL). Fixed to check for no-digits-parsed and trailing garbage.

## [2.7.0] - 2026-03-13

### Critical — JIT/ASM32 Re-Enabled
The Pawn JIT compiler and x86 ASM dispatcher were disabled since the initial KTP fork (`AMBuilder` lines 7-9 commented out with "KTP DEBUG" label). All plugins were executing through the slow C interpreter instead of native x86 JIT-compiled code. Re-enabled `JIT`, `ASM32` defines and `amxexecn.asm`, `amxjitsn.asm` assembly files. Significant performance improvement expected for all plugin callbacks.

### Critical — Security Hardening Flags
Added compiler and linker hardening flags to `configure_linux` in `AMBuildScript`:
- `-fstack-protector-strong` — stack canary protection for local buffers
- `-D_FORTIFY_SOURCE=2` — compile-time and runtime bounds checking on libc functions
- `-Wl,-z,relro -Wl,-z,now` — full RELRO (GOT read-only after dynamic linking)

### Critical — Module SDK `rewriteNativeLists` Double-Free (CModule.cpp)
`MNF_OverrideNatives` called more than once (multiple modules across map loads) appended the same index to `m_DestroyableIndexes` without dedup. On `clear()`, the destructor loop called `delete[]` on the same index twice. Added dedup check before appending.

### Critical — `detachReloadModules` Stale Pointers (modules.cpp)
After `detachReloadModules()`, `g_moduleFrameCallbacks` and the three message handler arrays (`g_ModuleMsgBeginHandlers`, `g_ModuleMsgHandlers`, `g_ModuleMsgEndHandlers`) retained function pointers into unmapped `.so` memory. Added `Module_ClearFrameCallbacks()` and `Module_ClearMsgHandlers()` cleanup functions called from `detachReloadModules`.

### Critical — DODX `saveKill`/`saveHit` Bounds Checks (CMisc.cpp)
`wweapon` used as index into `weapons[]`, `weaponsLife[]`, `weaponsRnd[]`, `weaponData[]` without validation. `bbody` (hitgroup) indexed `bodyHits[8]` without bounds check. Added clamping: `wweapon` to `[0, DODMAX_WEAPONS)`, `bbody` to `[0, 7]`.

### Critical — DODX `Client_CurWeapon` Bounds Check (usermsg.cpp)
`iId` from network message used as direct array index into `weaponData[]` and `weapons[]` without range check. Added `break` guard for out-of-range values.

### Critical — `C_ClientCvarChanged` Missing Guard (meta_api.cpp)
`pfnClientCvarChanged` fired `client_cvar_changed` forward without checking `pPlayer->initialized` or `pPlayer->ingame`. Cvar responses during reconnect/map-change could crash plugin handlers. Added `initialized && ingame` guard before `executeForwards`.

### Fixed

#### Pass-Through Hooks Disabled (meta_api.cpp)
`ExecuteServerStringCmd_RH` and `Steam_GSBUpdateUserData_RH` were registered unconditionally but did nothing except call the chain. Every client command went through extra function call overhead for zero functionality. Commented out registration and function definitions.

#### `sscanf` Unbounded Writes (CCmd.cpp, CPlugin.cpp)
`Command::Command()` used `sscanf("%s %s")` into `char[64]` buffers — changed to `"%63s %63s"`. `loadPluginsFromFile` used `"%s"` into `char[256]` — changed to `"%255s"`.

#### `C_StartFrame_Post` Dead Code Guard (meta_api.cpp)
The `g_putinserver` processing block in `C_StartFrame_Post` was dead code in extension mode. Replaced `#if 0` with runtime guard `!g_bRehldsExtensionInit` so it only runs under Metamod.

#### `g_szPreviousMap` Dead Variable Removed (meta_api.cpp)
Written in three places, read nowhere. Removed declaration and all write sites.

#### Module SDK Null/Free Edict Guards (modules.cpp)
`MNF_GetPlayerFrags`, `MNF_GetPlayerHealth`, `MNF_GetPlayerArmor`, `MNF_IsPlayerHLTV` accessed `pPlayer->pEdict->v` without null/free checks. Added guards returning 0 on null or freed edicts.

#### `sprintf` Overflow in `CheckModules` (modules.cpp)
`sprintf(error, ...)` into `char[128]` with no length limit. Changed to `snprintf(error, 128, ...)`.

#### DODX `get_user_wrstats` Wrong Guard (NRank.cpp)
Guard checked `pPlayer->weaponsLife[weapon].shots` (life stats) but function copies from `pPlayer->weaponsRnd[weapon]` (round stats). Changed guard to `weaponsRnd`.

#### DODX `Scoping()` Global vs `this` (CMisc.cpp)
Member function `CPlayer::Scoping()` accessed `mPlayer->current` (global message state pointer) instead of `this->current`. Fixed all three references.

#### DODX `cwpn_dmg` Dead Null Check (NBase.cpp)
`pAtt` dereferenced before null check — guard was unreachable. Removed dead `if(!pAtt) pAtt = pVic` block.

#### Forward Dedup Missing Param Types (CForward.cpp, CForward.h)
Multi-forward dedup matched on name + execType + numParams only. Two forwards with same name/count but different param types would collide. Added `memcmp` on `m_ParamTypes` array to match SP forward dedup behavior.

#### Dual `g_putinserver` Processing Guard (meta_api.cpp)
Both `C_StartFrame_Post` and `SV_Frame_RH` could process the `g_putinserver` queue in extension mode. `C_StartFrame_Post` now guarded to only run under Metamod.

### Build System
- 32-bit architecture check changed from warning to hard `exit 1` (`build_linux.sh`)
- Removed dead deploy blocks for fun/engine/fakemeta modules (`build_linux.sh`)
- Removed fun/engine/fakemeta from CLAUDE.md build output table

## [2.6.18] - 2026-03-12

### Fixed

#### DODX Module - Pdata Detection Log Spam
`DODX_PdataWriteBoth` and `DODX_DetectPdataOffset` called `MF_Log` (synchronous `fprintf`) on every grenade set call during the detection phase. On map load with KTPPracticeMode active and players spawning, this generated dozens of synchronous log writes during an already high-load window. Changed Phase 1 to log once via `MF_PrintSrvConsole`, removed per-call Phase 2 probe logging, and kept only the final detection result output.

#### DODX Module - Stickgrenade-First Detection Failure
`DODX_DetectPdataOffset` only probed handgrenade offsets, but Phase 1 writes go to whichever grenade family was requested. If the first `dodx_set_grenade_ammo` call was for stickgrenades (Axis players), the handgrenade offsets were never written, causing detection to read uninitialized data and defer indefinitely. Now probes both handgrenade and stickgrenade families, scoring across all 6 locations.

#### DODX Module - Tied Score Tiebreaker Simplification
The old consistency tiebreaker compared per-location value equality, but those local variables no longer exist after the multi-family probe rewrite. Simplified to default to +4 on tied scores, which matches the historical Ubuntu 22.04 behavior.

#### Stats Logging - Flush Task Registration in Hookchain Context
`stats_logging.sma` registered its repeating buffer flush task in `plugin_init`, which fires from within a ReHLDS hookchain handler in extension mode. `set_task` with repeating flag intermittently fails to register in this context (~10% failure rate). Moved task registration to `plugin_cfg`, which fires later and outside the hookchain. Without this fix, headshot kill events could accumulate in the buffer and never flush until the next map change.

#### Core AMXX - Dead Code Removal
Removed `KTPAMX_ServerDeactivate()` and `KTPAMX_ServerDeactivatePost()` — superseded by `SV_InactivateClients_RH` hook in v2.6.15 but left in the file. The old functions contained an older disconnect loop that manually zeroed player fields instead of calling `pPlayer->Disconnect()`, creating a maintenance hazard.

## [2.6.17] - 2026-03-11

### Fixed

#### DODX Module - Team Score Zeroed Before Halftime Save

`DODX_OnChangelevel` reset `AlliesScore` and `AxisScore` to 0 before KTPMatchHandler's changelevel hook could read them. This caused `save_first_half_scores()` to always save 0-0, breaking score carry-forward into the second half. Every match since v2.6.15 reported 0-0 at halftime regardless of actual score.

Moved score zeroing from `DODX_OnChangelevel` (pre-changelevel) to `DODX_OnSV_ActivateServer` (post-map-load), so plugin hooks can read the actual scores during the changelevel transition before they're cleared for the new map.

## [2.6.16] - 2026-03-11

### Fixed

#### DODX Module - Pdata Offset Auto-Detection Rewrite

The pdata offset auto-detection for grenade ammo operations failed on first player spawn because the game DLL hadn't initialized the player's private data yet. The old logic required all 3 grenade memory locations to contain matching valid values (1-10), but at spawn time they were uninitialized (all zeros), causing detection to fail silently and lock in the wrong default (+4).

Replaced with a two-phase write-then-verify approach:
- **Phase 1** (first grenade set call): Writes the requested count to BOTH +4 and +5 offsets, ensuring the correct one gets the right value regardless of which is correct
- **Phase 2** (second grenade set call): Reads back from both offsets and scores each by how many of the 3 locations contain valid values (1-10). The higher-scoring offset wins, with a minimum threshold of 2/3
- If neither offset has sufficient data, detection defers and retries on the next grenade operation instead of locking in a wrong answer

This eliminates the need for manual `pdata_offset` configuration in `dodx.ini` — the correct offset is determined automatically based on the actual game DLL binary and server environment.

## [2.6.15] - 2026-03-11

### Fixed

#### Core AMXX - Extension Mode Lifecycle Gaps

`plugin_end` and `client_disconnect` forwards were not firing before map transitions in extension mode. This caused memory leaks and missing cleanup on every map change:
- `modules_callPluginsUnloading()` now called before plugin re-initialization, allowing modules (ReAPI) to clear hookchain vectors that are 100% plugin-owned
- Data handles (Arrays, Tries, DataPacks) properly freed between maps
- HUD sync objects, dynamic admins, and cvar manager state cleaned up
- Grenade and auth caches cleared on reload

#### Core AMXX - Plugin Re-initialization Deduplication

Subsystem registrations accumulated on every map change because `plugin_init` re-ran without clearing prior state. Clearing was unsafe (segfaults) because C++ modules register state during `AMXX_Attach` that must persist. Fixed with dedup-at-registration for all subsystems:
- Commands, SP forwards, multi-forwards, events, log events, messages, menus
- `setCmdType()` changed to return bool, preventing secondary list accumulation
- Result: `plugin_init` flat at ~0.9ms regardless of map changes (was growing to 107ms+)

## [2.6.14] - 2026-03-05

### Fixed

#### Core AMXX - amx_ExecPerf Hot Path Optimization

`amx_ExecPerf()` called `g_plugins.findPluginFast(amx)` on every AMX execution call, even when performance logging was disabled (the default). Moved the plugin lookup inside the `amxmodx_perflog->value > 0.0f` branch so the common path (profiling off) goes straight to `amx_Exec()` with only a single float comparison.

#### Core AMXX - g_putinserver O(n) Removal Replaced with Compact Pattern

`SV_Frame_RH` and `C_StartFrame_Post` both used `g_putinserver.remove(i)` which shifts all remaining elements on every removal — O(n) per removal, O(n²) worst case during peak player joins. Replaced with a single-pass compact pattern: valid entries are written to `writeIdx`, then the vector is resized once at the end. All removals are now O(1).

#### Core AMXX - Extension Mode Missing ClearMenus and FrameAction Cleanup

`KTPAMX_ReloadPlugins()` (extension mode map change) did not call `ClearMenus()` or `g_frameActionMngr.clear()`, causing menu state and frame actions from the previous map to persist. This could lead to stale menu handler references across map changes. Both are now cleared before plugin re-initialization, matching the Metamod mode cleanup path.

#### DODX Module - DODX_OnMsgBegin Missing Bounds Check

`DODX_OnMsgBegin()` indexed into `modMsgs[msg_id]` and `modMsgsEnd[msg_id]` without validating `msg_id` against `MAX_REG_MSGS`. A corrupt or out-of-range message ID could cause an out-of-bounds array read. Added bounds check matching the existing validation in `DODX_OnMessageHandler`.

#### DODX Module - dod_weaponlist Missing wpnID Bounds Check

`dod_weaponlist` native used `wpnID` (params[2]) to index into `weaponlist[]` array without validating it was in range `[0, WEAPONLIST)`. Also added bounds check on `params[1]` before its use as a `weaponlist[]` index. Prevents out-of-bounds reads from plugin calls with invalid weapon IDs.

#### DODX Module - Grenade Tracking Leak on Map Change

`g_grenades` linked list was not cleared in `DODX_OnChangelevel()`. Grenade entries holding edict pointers from the previous map survived the map change, creating stale pointer references. Added `g_grenades.clear()` to the changelevel handler.

#### DODX Module - Team Scores Not Reset on Map Change

`AlliesScore` and `AxisScore` were not reset in `DODX_OnChangelevel()`. Plugins reading `dod_get_team_score()` early on a new map (before the first TeamScore message) would get stale values from the previous map. Both are now zeroed on map change.

#### DODX Module - Buffer Overflow Protection (6 strcpy → strncpy)

Multiple `strcpy` calls in DODX wrote to fixed-size buffers without length validation:
- `CPlayer::Connect()` — `ip[32]` buffer (CMisc.cpp)
- `CPlayer::initModel()` — `modelclass[64]` buffer (CMisc.cpp)
- `register_cwpn` — `name[32]` and `logname[16]` buffers (NBase.cpp)
- `dodx_objective_set_data` — `cap_message[256]`, `model_neutral[256]`, `model_allies[256]`, `model_axis[256]` fields (NCP.cpp)
- `dodx_area_set_data` — `hud_sprite[256]` field (NCP.cpp)

All replaced with `strncpy` + explicit null termination.

#### Core AMXX - Forward String Parameter Double-strlen Eliminated

`CForward::execute()` and `CSPForward::execute()` called `strlen(str)` for `amx_Allot` sizing, then `amx_SetStringOld()` called `strlen()` again internally to copy the string. Replaced `amx_SetStringOld` with an inlined unpacked char-to-cell copy loop using the pre-computed length, eliminating the redundant `strlen` on every string parameter in every forward call.

#### Core AMXX - Task Manager Free-Slot Scan and startFrame Optimizations

Three improvements to `CTaskMngr`:
- **Active count tracking:** Added `m_ActiveCount` to skip `startFrame()` iteration entirely when no tasks are registered (common during warmup/idle periods)
- **Free-slot hint index:** Added `m_FirstFreeHint` so `registerTask()` starts scanning for free slots from the last-known position instead of index 0, reducing O(n) to amortized O(1) for sequential registrations
- **Self-clear tracking:** `startFrame()` now decrements `m_ActiveCount` and updates the free hint when tasks complete and self-clear, keeping the counters accurate without requiring CTask to call back into the manager

#### DODX Module - sendScore Float Precision Fix

`sendScore` was declared as `int` but used as a time comparison against `gpGlobals->time` (float). The `(int)` cast at assignment truncated the target time, causing the score forward to fire with 0–1.25s delay instead of a consistent 0.25s. Changed `sendScore` from `int` to `float` and removed the truncating cast.

#### DODX Module - CHECK_PLAYERRANGE Rejects Index 0

`CHECK_PLAYERRANGE` macro allowed player index 0 (the world entity), which would access `players[0]` — a summary/unused slot, not a real player. Changed the lower bound check from `x < 0` to `x < 1`.

#### DODX Module - loadRank Buffer Overflow Protection

`RankSystem::loadRank()` read player name and unique ID from the rank file into fixed 64-byte buffers without clamping the length read from file. A corrupted rank file with a length field ≥64 would overflow the buffer. Added length clamping to `sizeof(buffer) - 1` and explicit null termination after each read.

## [2.6.13] - 2026-03-04

### Fixed

#### Core AMXX - CTaskMngr Use-After-Free on Changelevel During Task Callback

`CTaskMngr::startFrame()` iterates tasks and calls `executeIfRequired()`, which runs plugin callbacks. If a plugin callback triggers a synchronous changelevel (via `server_cmd` + `server_exec`), `KTPAMX_ReloadPlugins()` calls `g_tasksMngr.clear()` which destroys the `m_Tasks` vector — while the executing task's `CTask::executeIfRequired()` is still on the call stack. When execution returns, `CTask::clear()` at line 148 calls `delete[]` on a dangling `m_pParams` pointer, crashing with `free(): invalid pointer`.

- Added `m_bInStartFrame` / `m_bDeferredClear` flags to `CTaskMngr`
- When `clear()` is called during `startFrame()`, tasks are individually cleared (params freed, marked free) but the vector is NOT destroyed
- `startFrame()` breaks iteration on deferred clear, then destroys the vector after the loop exits safely
- The executing task returns to `executeIfRequired()`, sees `isFree() == true` at line 135, and returns without double-freeing
- Confirmed via gdb analysis of New York 2 core dump (2026-03-04): crash at `CTask.cpp:66` during `startFrame` → `executeIfRequired` → `clear`

#### Core AMXX - New Menu Handler Use-After-Free

In both Metamod and extension mode `ClientCommand` handlers, the `pMenu` pointer was used after `executeForwards()` without re-validation. If a plugin's menu callback destroyed the menu (via `menu_destroy`), `pMenu` became a dangling pointer.

- **MENU_BACK/MENU_MORE paths:** After `executeForwards(pMenu->pageCallback)`, re-validate with `get_menu_by_id(menu)` before calling `pMenu->Display()`
- **Normal item path:** Capture `pMenu->func` into local `menuFunc` before `executeForwards()`, preventing access to potentially freed memory
- Applied to both Metamod (`C_ClientCommand`) and extension mode (`SV_ClientCommand_RH`) code paths

## [2.6.12] - 2026-03-03

### Fixed

#### Core AMXX - Forward Execute Invalid Pointer Crash Prevention

`CForward::execute()` and `CSPForward::execute()` in `CForward.cpp` only checked for NULL string pointers before calling `strlen()`. When a forward parameter type mismatch caused a cell value (e.g., player index `1`) to be reinterpreted as `const char*`, the resulting pointer `0x1` passed the NULL check but crashed on `strlen()`.

- Both execute methods now reject pointers below `0x1000` (first page, always unmapped on Linux)
- Logs a WARNING with forward name, parameter index, function ID, and the bad pointer value for diagnosis
- Defaults to empty string instead of crashing
- STRINGEX cleanup paths also guarded to prevent crash when copying back to an invalid address
- Confirmed via gdb analysis of 2 Atlanta core dumps (2026-03-02): both crashed at `CForward.cpp:282` with `str = 0x1`

#### Core AMXX - SP Forward Free List Reuse Bug

Both `registerSPForward()` overloads in `CForward.cpp` had a bug where the free list entry was popped AFTER an early-return check. When `pForward->Set()` succeeded but `getFuncsNum() == 0`, the function returned `-1` without popping from the free list. The slot was left with `isFree = false` (set by `Set()`) but still queued in `m_FreeSPForwards`, causing the slot to be reused later with potentially stale parameter types.

- Moved `m_FreeSPForwards.pop()` before the early-return check
- On failed registration, properly re-marks the slot as free and pushes it back to the free list

## [2.6.11] - 2026-02-25

### Fixed

#### DODX Module - Missing pvPrivateData Null Checks

`dodx_set_user_class` and `dodx_set_user_team` natives accessed `pEdict->pvPrivateData` without null-checking it first, inconsistent with other KTP natives (grenade, noclip, teamname) that properly validate before access.

- `dodx_set_user_class` — Added `|| !pPlayer->pEdict->pvPrivateData` guard
- `dodx_set_user_team` — Added `|| !pPlayer->pEdict->pvPrivateData` guard

#### DODX Module - CRank IP Lookup Infinite Loop (Stock AMXX Bug)

`CRank.cpp:findEntryInRank()` with IP-based ranking had `a = a->prev` inside the `strncmp` match block only. When `strncmp` didn't match (common case), the loop variable never advanced, causing an infinite loop. Moved `a = a->prev` outside the conditional so the linked list always advances.

This is a stock AMX Mod X bug. Does not affect KTP production (rank system is skipped in extension mode) but fixed for correctness.

#### Core AMXX - SP Forward Dedup Parameter Type Mismatch (Crash Fix)

Both `registerSPForward` overloads in `CForward.cpp` matched on `amx + func/name` only, ignoring `numParams` and `paramTypes`. When the same Pawn function was registered as both a menu callback (FP_CELL params) and a curl/discord callback (FP_STRING params), the dedup returned the wrong forward handle. Integer values (e.g., menu selection `1`) were then cast to `const char*` and passed to `strlen()`, causing a segfault at address `0x1` in `CSPForward::execute`.

- Both overloads now compare `numParams` and `paramTypes` via `memcmp` in addition to `amx + func/name`
- Fixes 4 confirmed production crashes (3x NY, 1x ATL) on 2026-02-27

#### Core AMXX - C_ClientCvarChanged Null Guard

`C_ClientCvarChanged` called `GET_PLAYER_POINTER(pEntity)` without validating `pEntity`. Added null check, `FNullEnt` check, and player index range validation before accessing the player array. Uses `GET_PLAYER_POINTER_I` with validated index instead of raw `GET_PLAYER_POINTER` macro.

#### DODX Module - Debug Ammo Dump Out-of-Bounds Read

`dodx_debug_dump_ammo` scanned pvPrivateData offsets 0–400 (1600 bytes), well beyond the ~700 byte DoD player private data structure. Reduced scan range to 0–175 (700 bytes) to stay within safe bounds.

### Changed

#### Shared Include - ktp_discord.inc v1.3.2

- Fixed duplicate audit messages when `discord_channel_id_admin` matches an audit channel ID — added `_ktp_discord_audit_add()` dedup helper
- Removed `g_ktpDiscordTempFile` dead code (unused since v1.1.0, 128 cells freed per plugin)
- Changed `containi` to `contain` for audit key matching (keys already lowercased)

---

## [2.6.10] - 2026-02-17

### Fixed

#### Extension Mode Subsystem Re-Registration Leak

In extension mode, `KTPAMX_ReloadPlugins()` fires `plugin_init` on every map change without clearing subsystem registrations. Each map change re-registered all commands, forwards, events, log events, messages, and menu commands — causing linear growth in plugin_init time (~2ms/map, reaching 100ms+ after 50 maps).

**Two-pronged fix:**

1. **Module cleanup callback** — Call `modules_callPluginsUnloading()` before `plugin_init` in `KTPAMX_ReloadPlugins()`. This notifies modules (e.g., KTP-ReAPI) to clear plugin-owned state like hookchain vectors before plugins re-register them.

2. **Registration-time deduplication** — All AMXX subsystems now detect duplicate registrations and return existing handles instead of allocating new entries:

| Subsystem | Dedup Key | File |
|-----------|-----------|------|
| Commands (`CmdMngr`) | plugin + command line | `CCmd.cpp` |
| SP Forwards (`CForwardMngr`) | AMX + function index/name | `CForward.cpp` |
| Multi-Forwards (`CForwardMngr`) | function name + exec type + param count | `CForward.cpp` |
| Events (`EventsMngr`) | plugin + function + message ID | `CEvent.cpp` |
| Log Events (`LogEventsMngr`) | plugin + function | `CLogEvent.cpp` |
| Messages (`MessageHooks`) | message ID + function | `messages.h` |
| Menu Commands (`MenuMngr`) | plugin + function + menu keys | `CMenu.cpp` |

3. **`setCmdType()` guard** — Changed return type from `void` to `bool`. Returns `false` if command type bits are unchanged, preventing duplicate entries in secondary command lists and redundant `REG_SVR_COMMAND` engine calls.

**Result:** plugin_init time is now flat at ~0.9ms regardless of map change count (was 107ms+ at 55 map changes — 120x improvement).

**Technical note:** Subsystem clearing (e.g., `g_commands.clear()`) is NOT safe because C++ modules register state once during `AMXX_Attach` — clearing subsystems destroys module state and causes delayed segfaults. Dedup-at-registration is the correct approach.

---

## [2.6.9] - 2026-02-01

### Added

#### DODX Module - Runtime Pdata Offset Detection

Auto-detection of Linux pdata offsets for grenade ammo manipulation:

- **Ubuntu 22.04 and older** - Uses +5 offset adjustment
- **Ubuntu 24.04 and newer** - Uses +4 offset adjustment
- **Auto-detection on first spawn** - Probes memory at both offsets to find valid grenade count
- **No recompilation needed** - Works across Ubuntu versions automatically

**New Globals:**
- `g_iLinuxPdataOffsetAdjust` - Current offset adjustment (4 or 5)
- `g_bPdataOffsetDetected` - Whether detection has run

**New Function:**
- `DODX_DetectPdataOffset(edict_t*)` - Probes player pdata to detect correct offset

**Detection Strategy:**
1. Look for value 1-10 at expected grenade offset
2. Check if same value appears at all 3 redundant offsets (they should match)
3. If +4 matches, use +4; if +5 matches, use +5; otherwise default to +4

**Use Case:** Denver bare-metal runs Ubuntu 24.04, Atlanta runs Ubuntu 22.04. This eliminates the need for separate binaries.

#### DODX Module - New Grenade Natives

- **`dodx_strip_grenade(id, grenade_type)`** - Remove grenade from player and clear ammo slots
  - Clears all 3 ammo slots for the specified grenade type
  - Returns 1 on success, 0 on failure

- **`dodx_debug_dump_ammo(id)`** - Debug utility to dump pdata ammo offsets
  - Scans player pdata for values 1-10 (potential grenade counts)
  - Shows current offset adjustment and expected offsets
  - Useful for debugging offset issues on new OS versions

---

## [2.6.8] - 2026-01-31

### Added

#### Extension Mode Header Stubs
Complete Metamod-free compilation support for third-party modules:

**amxmodx/amxmodx.h:**
- Added Metamod enum stubs (`PLUG_LOADTIME`, `PL_UNLOAD_REASON`)
- Added `hudtextparms_t` struct definition
- Added engine function macros (`INDEXENT`, `VARS`, `IS_DEDICATED_SERVER`, etc.)
- Added cvar macros (`CVAR_GET_POINTER`, `CVAR_REGISTER`, etc.)
- Added info key macros (`GET_INFO_KEY_BUFFER`, `INFO_KEY_VALUE`, etc.)
- Added game DLL function wrapper stubs (`MDLL_Spawn`, etc.)

**public/sdk/amxxmodule.h:**
- Mirror stub definitions for third-party module compilation
- Enables modules like amxxcurl to compile without Metamod SDK headers

**amxmodx/fakemeta.cpp:**
- Added `#ifndef USE_METAMOD` guards for extension mode
- Returns early when Metamod unavailable (no-op instead of crash)

#### Docker Build Support
- `Dockerfile` - Ubuntu 22.04 build environment for glibc 2.35 compatibility
- `docker-build.sh` - Automated Docker build script

### Fixed

- **Module compilation without Metamod** - Third-party modules no longer require Metamod SDK headers when `USE_METAMOD` is not defined

---

## [2.6.7] - 2026-01-24

### Added

#### DODX Module - Pre-Damage Forward
New forward for modifying damage before it's applied:

- **`dod_damage_pre(attacker, victim, damage, wpnindex, hitplace, TA)`** - Fires before `client_damage`
  - Return a lower damage value (0 to damage-1) to reduce damage taken
  - Return 0 to completely block the damage
  - Return original damage (or higher) for no modification
  - **Health message sync** - Automatically sends Health message to victim's HUD after heal-back
  - Stats tracking uses the effective (modified) damage value

**Use Case:** KTPPracticeMode uses this to reduce teammate grenade damage for practice sessions.

#### DODX Module - Give Grenade Native
New native for giving grenades to players (extension mode compatible):

- **`dodx_give_grenade(id, grenade_type)`** - Give a grenade to a player
  - `DODW_HANDGRENADE` (13) - US hand grenade
  - `DODW_STICKGRENADE` (14) - German stick grenade
  - `DODW_MILLS_BOMB` (36) - British Mills bomb
  - Creates weapon entity and touches it to the player
  - Returns 1 on success, 0 on failure

**Use Case:** KTPPracticeMode uses this to give grenades during practice mode.

#### DODX Module - Player Manipulation Natives
New natives ported from dodfun module for extension mode compatibility:

**Class/Team:**
- **`dodx_set_user_class(id, classId)`** - Set player class (1-6, or 0 for random)
- **`dodx_set_user_team(id, teamId, refresh=1)`** - Set player team (1=Allies, 2=Axis, 3=Spectators)
  - Kills player, sets random class
  - `refresh=1` broadcasts team change to all clients

**Position/Angles:**
- **`dodx_get_user_origin(id, Float:origin[3])`** - Get player position
- **`dodx_set_user_origin(id, Float:origin[3])`** - Teleport player
- **`dodx_get_user_angles(id, Float:angles[3])`** - Get player view angles
- **`dodx_set_user_angles(id, Float:angles[3])`** - Set player view angles (includes fixangle)

**Use Case:** These enable player state save/restore during hostname broadcast operations where the server briefly respawns players.

**New Private Data Offsets (CMisc.h):**
- `STEAM_PDOFFSET_CLASS` (367 + Linux offset) - Player class
- `STEAM_PDOFFSET_RCLASS` (368 + Linux offset) - Random class flag

### Fixed

#### Multi-Victim Grenade Damage
- **Freed entity handling** - Grenade entities can be freed after damaging the first victim but before subsequent victims are processed
- **Solution** - Check `enemy->free` flag and fall back to grenade lookup table when entity is freed
- **Result** - Grenade damage now correctly attributes to the thrower for all victims

---

## [2.6.6] - 2026-01-23

### Added

#### DODX Module - AmmoX HUD Sync Native
New native for updating client HUD ammo display after modifying grenade ammo:

- **`dodx_send_ammox(id, ammo_slot, count)`** - Send AmmoX message to update client HUD
  - `ammo_slot=9` for hand grenade / Mills bomb
  - `ammo_slot=11` for stick grenade
  - `count` clamped to 0-254 range
  - Returns 1 on success, 0 on failure

**Use Case:** After calling `dodx_set_grenade_ammo()`, the server-side ammo is updated but the client HUD still shows the old value. Call `dodx_send_ammox()` to sync the client's ammo display.

**Why a native?** AMX Mod X `message_begin()` / `emessage_begin()` crash in extension mode for certain message types. This native uses the engine's `MESSAGE_BEGIN` directly from C++ which works correctly.

**Plugins using this native:**
- KTPGrenadeLoadout - HUD sync after setting spawn grenades
- KTPPracticeMode - HUD sync after refilling grenades

---

## [2.6.5] - 2026-01-23

### Added

#### DODX Module - Noclip Native
New native for player noclip control (ported from fun module for extension mode compatibility):

- **`dodx_set_user_noclip(id, noclip)`** - Set player noclip mode
  - `noclip=0` disables noclip (MOVETYPE_WALK)
  - `noclip=1` enables noclip (MOVETYPE_NOCLIP)
  - Returns 1 on success, 0 on failure

**Use Case:** KTPPracticeMode uses this for the `.noclip` command without requiring the fun module (which needs Metamod).

---

## [2.6.4] - 2026-01-22

### Added

#### DODX Module - Grenade Ammo Natives
New natives for grenade ammo manipulation (extension mode compatible, no Metamod/dodfun required):

- **`dodx_set_grenade_ammo(id, grenade_type, count)`** - Set grenade count for a player
  - Grenade types: `DODW_HANDGRENADE` (13), `DODW_STICKGRENADE` (14), `DODW_MILLS_BOMB` (36)
  - Count clamped to 0-10 range
  - Hand grenade and Mills bomb share the same ammo pool
- **`dodx_get_grenade_ammo(id, grenade_type)`** - Get current grenade count
  - Returns current count or 0 on failure

**New Defines (dodx.h):**
- `PDOFFSET_AMMO_HANDGRENADE_1/2/3` - Private data offsets for hand grenade/Mills bomb ammo
- `PDOFFSET_AMMO_STICKGRENADE_1/2/3` - Private data offsets for stick grenade ammo
- Linux offsets have +5 adjustment per DoD convention

**Use Case:** KTPGrenadeLoadout and KTPPracticeMode plugins use these for grenade customization.

### Fixed

#### SV_CheckConsistencyResponse Hook (Extension Mode)
- **Inverted condition bug** - Hook was triggering forward for CONSISTENT files instead of INCONSISTENT
  - Bug: `if (!result && ...)` fired when `result=false` (file matched)
  - Fix: `if (result && ...)` fires when `result=true` (file mismatched)
- **Return value semantics** - Clarified and fixed return values
  - Hook returns `FALSE` = file OK (allow player)
  - Hook returns `TRUE` = file bad (kick player)
  - Forward returns `1` (PLUGIN_HANDLED) = allow player to stay
- **Model checking now works** - `fc_checkmodels 1` correctly kicks players with modified models

#### force_unmodified() Timing (Extension Mode)
- **Root cause** - `ENGINE_FORCE_UNMODIFIED` only works during spawn/precache phase
- **Solution** - Added `PF_precache_model_I` hook to initialize AMXX during precache
  - Full AMXX init (modules, plugins, hooks) runs on first precache call
  - `plugin_precache` forward executes so plugins can call `force_unmodified()`
  - Force lists processed with `ENGINE_FORCE_UNMODIFIED` while still in precache phase
  - `plugin_init`/`plugin_cfg` deferred to `SV_ActivateServer` (game state not ready during precache)
- **Map change fix** - Reset `g_bExtPrecacheProcessed` and clear force lists in `SV_InactivateClients`
  - Without this, `plugin_precache` wouldn't fire on subsequent maps

### Technical

**New Global Variables:**
- `g_bExtPrecacheProcessed` - Tracks if precache hooks processed force lists this map
- `g_bInitDuringPrecache` - Tracks if AMXX init was called during precache phase

**New Hook:**
- `PF_precache_model_I` - Fires during engine precache, triggers early AMXX initialization

---

## [2.6.3] - 2026-01-06

### Added

#### ktp_discord.inc v1.2.0 - Draft Channel Support

- **`KTP_DISCORD_CHANNEL_DRAFT`** - New channel type constant (value 5) for draft match Discord posts
- **`discord_channel_id_draft`** - New config key in discord.ini for draft channel ID
- **`g_ktpDiscordChannelDraft`** - Storage variable for draft channel
- **Draft channel getter** - `ktp_discord_get_channel(KTP_DISCORD_CHANNEL_DRAFT, ...)` returns draft channel (no fallback)

---

## [2.6.2] - 2025-12-31

### Added

#### DODX Module - New Natives for Score Broadcasting

Two new natives for scoreboard manipulation:

- **`dodx_broadcast_team_score(team, score)`** - Broadcast TeamScore message to all clients
  - Sets gamerules score AND sends TeamScore message in one operation
  - Avoids server crashes that occurred with AMX message natives
  - Used by KTPMatchHandler for 2nd half score restoration
  - Returns 1 on success, 0 on failure

- **`dodx_set_scoreboard_team_name(team, const name[])`** - Set custom team name on scoreboard
  - Sends TeamInfo message to all clients for players on specified team
  - May override hardcoded "Allies"/"Axis" display on client scoreboard
  - Returns number of players updated

#### ktp_discord.inc Cleanup
- Removed unused `g_ktpDiscordConfigLoaded` variable

---

## [2.6.1] - 2025-12-26

### Changed

#### ktp_discord.inc v1.1.0
Major rewrite of Discord integration include:

- **AMXX curl module** - Switched from `server_cmd("curl...")` to proper AMXX curl module
  - `server_cmd()` cannot execute shell commands on Linux servers
  - Now uses `curl_easy_init()`, `curl_easy_perform()` with async callbacks
- **Fixed JSON field names** - Match Discord Relay API expectations
  - `channel_id` → `channelId`
  - `payload.embeds` → `embeds` (top level)
- **Added curl cleanup** - Proper `curl_easy_cleanup()` and `curl_slist_free_all()` in callback
- **Debug logging** - Added `log_amx()` calls for troubleshooting Discord issues

#### reapi_engine_const.inc
- **RH_SV_Rcon hook** - Added enum constant for new KTP-ReHLDS RCON audit hook
  - Parameters: `(const command[], const from_ip[], bool:is_valid)`
  - Used by KTPAdminAudit v2.2.0 for RCON command logging

---

## [2.6.0] - 2025-12-21

### Added

#### New Native: ktp_drop_client
New native for dropping clients via ReHLDS API, bypassing blocked kick console command:

**New Native:**
- **`ktp_drop_client(id, const reason[] = "")`** - Drop client using ReHLDS DropClient API
  - Bypasses blocked kick command in KTP ReHLDS
  - Works in extension mode (no Metamod required)
  - Requires ReHLDS API to be available
  - Returns 1 on success, 0 on failure

**Use Case:**
- KTPAdminAudit uses this to execute kicks after menu-based admin approval
- Allows kick functionality when console `kick` command is blocked at engine level

#### New Include: ktp_discord.inc
Shared Discord integration include for KTP plugins:

**Features:**
- Common Discord configuration loading from `discord.ini`
- Audit channel ID retrieval
- Shared webhook/relay integration pattern

**Plugins using this include:**
- KTPAdminAudit
- KTPCvarChecker
- KTPFileChecker
- KTPMatchHandler

### Technical Details
- Native implemented in `amxmodx.cpp`
- Uses `g_RehldsApi->GetFuncs()->DropClient()` for direct client drop
- Include file location: `plugins/include/ktp_discord.inc`

---

## [2.5.1] - 2025-12-20

### Added

#### DODX Module - Player Team Name Native
New native for setting player team names in private data (extension mode compatible):

**New Native:**
- **`dodx_set_pl_teamname(id, szName[])`** - Set player's team name in private data
  - Affects server-side logging (team name in kill logs, etc.)
  - Works in extension mode (no Metamod required)
  - Max 15 characters + null terminator
  - Note: Does NOT affect scoreboard (DoD client hardcodes "Allies"/"Axis")

**New Defines (dodx.h):**
- `STEAM_PDOFFSET_TEAMNAME` - Player private data offset for team name (1400 Windows, 1405 Linux)
- `STEAM_PDOFFSET_SCORE` - Player score offset
- `STEAM_PDOFFSET_DEATHS` - Player deaths offset

**New Message Registration:**
- `gmsgTeamInfo` - For potential future scoreboard refresh functionality

### Technical Details
- Native implemented in `NBase.cpp`
- Uses same offsets as dodfun module for compatibility
- 16-byte null-padded copy to match engine expectations

---

## [2.5.0] - 2025-12-19

### Added

#### HLStatsX Integration
New DODX natives for match-based statistics tracking with KTPMatchHandler:

**New Natives:**
- **`dodx_flush_all_stats()`** - Fire `dod_stats_flush` forward for all connected players
  - Allows flushing warmup stats before match starts
  - Returns number of players flushed
- **`dodx_reset_all_stats()`** - Reset all accumulated stats for all players
  - Clears weapons[], attackers[], victims[], weaponsLife[], weaponsRnd[], life, round
  - Call after flushing to start fresh for match
- **`dodx_set_match_id(matchId[])`** - Set match ID for stats correlation
  - When set, weaponstats log lines include `(matchid "xxx")` property
  - Pass empty string to clear match context
- **`dodx_get_match_id(output[], maxlen)`** - Get current match ID

**New Forward:**
- **`dod_stats_flush(id)`** - Called by `dodx_flush_all_stats()` for each player
  - stats_logging.sma registers for this to log pending weaponstats

#### stats_logging.sma Updates
- **Match ID support** - All log lines include `(matchid "xxx")` when match ID is set
- **`dod_stats_flush` handler** - Logs weaponstats on demand (for warmup flush)
- **`log_player_stats()` stock** - Refactored from client_disconnected for reuse

### Technical Details

**Intended workflow for KTPMatchHandler:**
1. During warmup: stats accumulate normally
2. Match start: `dodx_flush_all_stats()` → logs warmup stats without match ID
3. Match start: `dodx_reset_all_stats()` → clears stats for fresh match
4. Match start: `dodx_set_match_id("KTP-1234567890-dod_charlie")` → sets context
5. During match: stats accumulate with match ID
6. Match end: Stats logged on disconnect include match ID
7. Match end: `dodx_set_match_id("")` → clears context for warmup

---

## [2.4.0] - 2025-12-16

### Added

#### DODX Extension Mode - Complete Rewrite
The DODX module has been extensively rewritten for full extension mode support:

**New ReHLDS Hook Handlers:**
- `DODX_OnPlayerPreThink` - Main stats tracking loop (replaces `FN_PlayerPreThink_Post`)
- `DODX_OnClientConnected` - Player connection handling (replaces `FN_ClientConnect_Post`)
- `DODX_OnSV_Spawn_f` - Player spawn handling (replaces `FN_ClientPutInServer_Post`)
- `DODX_OnSV_DropClient` - Player disconnect handling (replaces `FN_ClientDisconnect`)
- `DODX_OnChangelevel` - Pre-changelevel cleanup to prevent stale pointer crashes
- `DODX_OnTraceLine` - Hit detection and aiming (replaces `TraceLine_Post`)
- `IMessageManager` hooks for 16 game message types

**Shot Tracking via Button State:**
- Tracks weapon shots via IN_ATTACK button monitoring in PreThink
- Detects rising edge (new shots) and held attack (automatic weapons)
- Per-weapon fire rate delays for accurate shot counting:
  - MG42: 0.05s | .30 cal, MG34, Bren: 0.08s
  - SMGs (Thompson, MP40, MP44, Sten): 0.1s
  - Semi-auto/bolt rifles: 0.5s (rising edge only)
  - Pistols: 0.3s (rising edge only)
- New CPlayer fields: `oldbuttons`, `lastShotTime`, `nextShotTime`

#### ENTINDEX_SAFE Implementation
- **New inline function** `ENTINDEX_SAFE(edict_t*)` uses pointer arithmetic instead of engine calls
- **New global** `g_pFirstEdict` cached in `ServerActivate_Post` for safe entity index calculation
- **Prevents crashes** from calling engine functions during ReHLDS hooks
- `GET_PLAYER_POINTER` macro updated to use `ENTINDEX_SAFE`

#### Server Active Flag
- **New global** `g_bServerActive` tracks whether server is in valid state for processing
- Set to `true` in `ServerActivate_Post`, `false` in `ServerDeactivate` and `OnChangelevel`
- Prevents message hooks from using stale pointers during map changes

#### Module SDK Extensions
New functions for modules to access engine resources in extension mode:
- **`MF_GetEngineFuncs()`** - Returns pointer to engine function table
- **`MF_GetGlobalVars()`** - Returns pointer to gpGlobals
- **`MF_GetUserMsgId(name)`** - Look up message ID by name (works in extension mode)
- **`MF_RegModuleMsgHandler()`** - Register module message handler callbacks
- **`MF_UnregModuleMsgHandler()`** - Unregister module message handler callbacks
- **`MF_RegModuleMsgBeginHandler()`** - Register message begin handler

#### DODX Deferred Initialization
- Cvar registration moved from `OnAmxxAttach` to `OnPluginsLoaded` (engine not ready earlier)
- Message ID lookup via `MF_GetUserMsgId` instead of engine calls
- Player initialization via PreThink hook (lazy initialization on first frame)

### Fixed

#### Stats Native Safety Hardening
All DODX stats natives now have comprehensive safety checks:
- `gpGlobals` NULL check (can be NULL during map change)
- Player index range validation
- `pEdict` and `pEdict->free` checks before access
- `pPlayer->rank` NULL checks (rank system not used in extension mode)

**Hardened natives:**
- `get_user_astats`, `get_user_vstats`
- `get_user_wstats`, `get_user_wlstats`, `get_user_wrstats`
- `get_user_stats`, `get_user_lstats`, `get_user_rstats`
- `reset_user_wstats`

#### CHECK_PLAYER Macro Rewrite
- Now uses `players[]` array directly instead of `MF_IsPlayerIngame`/`MF_GetPlayerEdict`
- Checks `pEdict->free` before calling `FNullEnt()`
- Prevents crashes when player edict is freed during disconnect

#### TraceLine Hook Safety
- Added `g_bServerActive` and `g_pFirstEdict` checks
- Added `ptr` NULL validation
- Added `pEdict->free` checks for all edict accesses
- Uses `ENTINDEX_SAFE` for all index calculations

#### ServerDeactivate Safety
- Clears `g_bServerActive` and `g_pFirstEdict` at start of function
- Added `gpGlobals` NULL check
- Added `maxClients` range validation with fallback

#### Log File Handling
- Removed `log on` call from stats_logging.sma that caused log rotation
- Logging should be enabled via `sv_logfile 1` in server.cfg only

### Changed

#### Debug Logging Cleanup
- Removed all `[DODX DEBUG]` and `[KTPAMX DEBUG]` statements
- Removed debug counters and tracking variables
- Cleaned up verbose initialization logging

#### Startup Message Cleanup
Removed verbose messages, kept only essential operational output:
- Kept: `[KTP AMX] ReHLDS extension mode detected...`
- Kept: `[DODX] Running in ReHLDS extension mode.`
- Kept: `KTP AMX initialized as ReHLDS extension (no Metamod)`
- Kept: `[KTP AMX] Loaded X plugin(s).`

---

## [2.3.0] - 2025-12-14

### Added

#### DODX Extension Mode Fully Functional
- **PF_TraceLine hook** - Hit detection and aiming statistics now work in extension mode
  - POST hook only - reads trace results without affecting gameplay
  - Safe for wallpen (doesn't interfere with wallbang detection)
- **All 4 DODX hooks now active** in extension mode:
  - `SV_PlayerRunPreThink` - Stats tracking loop
  - `PF_changelevel_I` - Pre-changelevel cleanup
  - `PF_TraceLine` - Hit detection/aiming
  - `IMessageManager` - 16 message hooks for game stats

### Fixed

#### stats_logging.sma Disconnect Crash
- **Root cause**: `get_user_wstats` called during `client_disconnected` crashed because the player's edict was already marked as free
- **Solution**: Hardened `CHECK_PLAYER` macro in `dodx.h` to check `edict->free` before calling `FNullEnt()`
- **Result**: stats_logging.sma now works correctly for end-of-round logging

#### stats_logging.sma Verified Working
- **Tested and confirmed**: Plugin logs `weaponstats`, `weaponstats2`, `time`, and `latency` on disconnect
- **Log output**: Correctly written to HLDS log files for stats parsers
- **Startup fix**: Added `set_task(1.0, "enable_logging")` to force `log on` after server startup

#### DODX Safety Hardening
- **ENTINDEX_SAFE conversion** - All raw `ENTINDEX()` calls converted to `ENTINDEX_SAFE()` using pointer arithmetic
- **pEdict access hardening** - All pEdict accesses now have `if (!pEdict || pEdict->free)` guards
- **Prevents crashes** from stale or invalid edict pointers during map changes and player disconnects

### Technical Details

#### CHECK_PLAYER Macro Fix (dodx.h)
Before:
```cpp
if (!MF_IsPlayerIngame(x) || FNullEnt(MF_GetPlayerEdict(x)))
```
After:
```cpp
edict_t* _pEdict = MF_GetPlayerEdict(x);
if (!MF_IsPlayerIngame(x) || !_pEdict || _pEdict->free || FNullEnt(_pEdict))
```

#### ENTINDEX_SAFE Implementation (dodx.h)
```cpp
inline int ENTINDEX_SAFE(const edict_t *pEdict) {
    if (!pEdict || !g_pFirstEdict)
        return 0;
    return static_cast<int>(pEdict - g_pFirstEdict);
}
```

---

## [2.2.0] - 2025-12-08

### Added

#### Extension Mode Event Support
- **`register_event` in extension mode** - Events now work via KTPReHLDS IMessageManager integration
  - `MessageHook_Handler` parses message parameters and fires AMXX event callbacks
  - Hooks installed on-demand when plugins call `register_event`
- **`register_logevent` in extension mode** - Log events work via AlertMessage hookchain
  - Filters `at_logged` messages and fires log event handlers
  - Also triggers `plugin_log` forward

#### Module API for Extension Mode
- `MF_IsExtensionMode()` - Check if running without Metamod
- `MF_GetRehldsApi()` - Access ReHLDS API from modules
- `MF_GetRehldsHookchains()` - Access ReHLDS hookchains
- `MF_GetRehldsFuncs()` - Access ReHLDS functions
- `MF_GetRehldsServerData()` - Access ReHLDS server data
- `MF_GetRehldsMessageManager()` - Access IMessageManager
- `MF_GetGameDllFuncs()` - Access game DLL functions

#### Module Compatibility Testing
- **amxxcurl**: Confirmed working in extension mode (uses `MF_RegModuleFrameFunc`)
- **ReAPI**: Confirmed working in extension mode (has dedicated extension support)
- **DODX**: Extension hooks added but disabled due to crashes (`#if 0`)
- **SQLite**: Crashes in extension mode (Metamod hooks incompatible)

### Changed
- Default module suffix changed from `_amxx` to `_ktp`
- Module loader now recognizes both `_amxx` and `_ktp` suffixes

### Fixed
- `getEventId()` now works in extension mode using `REG_USER_MSG` lookup

### Technical Details

#### IMessageManager Integration
KTPReHLDS 3.16+ provides `IMessageManager` for intercepting network messages without Metamod. KTPAMXX now:
1. Calls `RehldsMessageManager->registerHook(msg_id, handler)` per message type
2. `MessageHook_Handler` parses `IMessage` parameters (byte, short, long, float, string)
3. Calls `g_events.parseValue()` and executes registered event handlers

#### AlertMessage Hookchain
New `AlertMessage` hookchain in KTPReHLDS provides pre-formatted log strings:
1. Hook fires with `ALERT_TYPE` and formatted message
2. KTPAMXX filters for `at_logged` type
3. Passes to `g_logevents` for parsing and execution

---

## [2.1.0] - 2025-12-06

### Added

#### New ReHLDS Hooks for Extension Mode
- **SV_ClientCommand hookchain** - Enables `register_clcmd`, menu systems, and `client_command` forward in extension mode
- **SV_InactivateClients hookchain** - Proper map change deactivation with `plugin_end` and `client_disconnect` forwards
- **SV_Spawn_f hookchain** - Client reinitialization after map change for `client_connect` and `client_putinserver` forwards

#### Map Change Support (Extension Mode)
- Clients now persist through map changes without disconnection
- All AMXX forwards (`plugin_init`, `plugin_cfg`, `client_connect`, `client_putinserver`) fire correctly on new maps
- Proper `plugin_end` and `client_disconnect` forwards during map transition

#### Client Command Processing (Extension Mode)
- Chat commands (`/start`, `.start`, etc.) now work in extension mode
- Menu selections (`menuselect 1-9`) properly handled
- `register_clcmd` and `register_menucmd` fully functional

### Fixed

- **SV_Spawn_f hook registration** - Function existed but was never registered, causing map change reconnect issues
- **Vtable alignment** - Fixed mismatch between KTPAMXX and KTPReHLDS headers (added 20+ missing virtual methods to IRehldsHookchains)
- **Debug logging cleanup** - Removed all debug `AMXXLOG_Log` statements from production code

### Changed

- **rehlds_api.h** - Updated to match KTPReHLDS vtable layout with all hookchain methods
- **mod_rehlds_api.cpp** - Updated for new API structure
- Removed deprecated debug logging from CForward.cpp, CMisc.cpp, CTask.cpp, amxmodx.cpp, cvars.cpp, meta_api.cpp

### Technical Details

#### New Hook Registrations (Extension Mode)
- `SV_ClientCommand` - Client command processing for chat commands and menus
- `SV_InactivateClients` - Map change deactivation sequence
- `SV_Spawn_f` - Client spawn command after map change reconnect

#### Map Change Sequence
The extension mode now properly handles the map change sequence:
1. `SV_InactivateClients()` → Fire disconnect forwards, clear player state
2. `SV_ActivateServer()` → Fire `plugin_init`, `plugin_cfg`
3. `SV_Spawn_f()` → Reinitialize reconnecting clients, fire `client_connect`, `client_putinserver`

---

## [2.0.0] - 2025-12-04

### Added

#### ReHLDS Extension Mode (Metamod-Free Operation)
- **Standalone ReHLDS extension support** - KTP AMX can now run as a direct ReHLDS extension without requiring Metamod
- **Dual-mode operation** - Automatically detects and adapts to both Metamod and extension modes
- **ReHLDS API integration** - Full integration with ReHLDS hooks and callbacks
- **Game DLL function wrappers** (`KTPAMX_*` macros) - Unified API that works in both operating modes
- **Module frame callbacks** - `MNF_RegModuleFrameFunc()` / `MNF_UnregModuleFrameFunc()` for modules requiring per-frame processing (replaces Metamod's pfnStartFrame for modules)

#### New Forward: `client_cvar_changed`
- **Real-time cvar monitoring** - Instant notification when clients respond to ANY cvar query
- **Event-driven architecture** - Zero polling overhead, 100% callback-based
- **KTP-ReHLDS integration** - Receives `pfnClientCvarChanged` callbacks from modified engine
- **Simple plugin API** - Single forward handler receives all cvar responses

#### Build System Improvements
- **Windows build support** - `build_windows.bat` for native Windows builds
- **Linux build support** - `build_linux.sh` with WSL compatibility
- **Cross-platform packaging** - `collect_builds.bat` to gather builds from both platforms
- **WSL build integration** - `build_linux_wsl.ps1` and `setup_wsl_build.ps1` for building Linux binaries from Windows
- **Plugin compilation toggle** - `--disable-plugins` configure flag to skip plugin compilation

#### Binary Renaming (KTP Branding)
- Main binary renamed from `amxmodx_mm` to `ktpamx` (Windows: `ktpamx.dll`, Linux: `ktpamx_i386.so`)
- Module binaries renamed from `*_amxx` suffix to `*_ktp` suffix
- Updated all default paths from `addons/amxmodx/` to `addons/ktpamx/`

### Changed

#### Core Architecture
- **Hybrid initialization** - Supports both `Meta_Attach()` (Metamod) and `AMXX_RehldsExtensionInit()` (ReHLDS extension)
- **Game DLL interface abstraction** - `g_pGameEntityInterface` pointer works in both modes
- **Forward execution** - All standard forwards now work in extension mode
- **Server command handling** - `amx` prefix commands work without Metamod

#### Path Defaults Updated
- `amxx_configsdir` default: `addons/ktpamx/configs`
- `amxx_pluginsdir` default: `addons/ktpamx/plugins`
- `amxx_datadir` default: `addons/ktpamx/data`

#### Build System
- `AMBuildScript` - Updated for KTP binary naming and optional plugin compilation
- `PackageScript` - Reorganized packaging for KTP AMX distribution
- `configure.py` - Added `--disable-plugins` option

#### Code Quality
- Added extensive debug logging throughout core systems (controlled by `AMXXLOG_Log`)
- Improved CRLF handling in config file parsing (CVault.cpp)
- Fixed cvar registration to properly handle pre-existing config values
- Added null pointer checks throughout player handling code

### Fixed

- **CVault CRLF handling** - Fixed "Can't use values with ASCII control characters" errors when config files have Windows line endings
- **Cvar registration race condition** - Properly handle cvars set by configs before plugin registration
- **Extension mode game DLL access** - Properly resolve server library base address in non-Metamod mode
- **Module loading in extension mode** - Graceful handling of Metamod-dependent modules

### Technical Details

#### New Global Variables
- `g_bRunningWithMetamod` - Boolean flag indicating Metamod presence
- `g_bRehldsExtensionInit` - Boolean flag indicating ReHLDS extension initialization
- `g_pGameEntityInterface` - Pointer to game DLL functions (works in both modes)

#### New Exported Functions (Extension Mode)
- `AMXX_RehldsExtensionInit()` - Entry point for ReHLDS extension loading
- `AMXX_RehldsExtensionShutdown()` - Cleanup for extension unloading

#### Hook Registrations (Extension Mode)
- `SV_DropClient` - Client disconnect handling
- `SV_ActivateServer` - Server activation (map load)
- `Cvar_DirectSet` - Cvar change monitoring
- `SV_WriteFullClientUpdate` - Client info updates
- `ED_Alloc` / `ED_Free` - Entity allocation tracking
- `SV_StartSound` - Sound emission hook
- `PF_Remove_I` - Entity removal hook
- `ClientConnected` / `SV_ConnectClient` - Client connection handling

### Compatibility Notes

- **Requires KTP-ReHLDS** for `client_cvar_changed` forward functionality
- **Backwards compatible** - All existing AMX Mod X plugins work unchanged
- **Standard ReHLDS compatible** - Extension mode works with standard ReHLDS (without `client_cvar_changed`)
- **Metamod compatible** - Can still run as traditional Metamod plugin

## [1.10.0] - Upstream

Base version forked from AMX Mod X 1.10.0.5468-dev.

See [AMX Mod X releases](https://github.com/alliedmodders/amxmodx/releases) for upstream changelog.

---

## Version History Summary

| Version | Date | Description |
|---------|------|-------------|
| 2.6.13 | 2026-03-04 | CTaskMngr use-after-free on changelevel during task callback, new menu handler use-after-free |
| 2.6.12 | 2026-03-03 | Forward execute invalid pointer crash prevention, SP forward free list reuse bug fix |
| 2.6.11 | 2026-02-25 | SP forward dedup crash fix, pvPrivateData null checks, CRank infinite loop fix, cvar null guard |
| 2.6.10 | 2026-02-17 | Extension mode subsystem dedup: flat plugin_init time across map changes |
| 2.6.9 | 2026-02-01 | DODX runtime pdata offset detection for Ubuntu 22.04/24.04 |
| 2.6.8 | 2026-01-31 | Extension mode header stubs, Docker build support |
| 2.6.7 | 2026-01-24 | DODX dod_damage_pre forward, dodx_give_grenade + player manipulation natives, grenade fix |
| 2.6.6 | 2026-01-23 | DODX dodx_send_ammox native for HUD ammo sync |
| 2.6.5 | 2026-01-23 | DODX dodx_set_user_noclip native |
| 2.6.4 | 2026-01-22 | DODX grenade ammo natives, consistency hook fix, precache timing fix |
| 2.6.3 | 2026-01-06 | ktp_discord.inc v1.2.0: Draft channel support |
| 2.6.2 | 2025-12-31 | DODX score broadcasting natives, ktp_discord.inc cleanup |
| 2.6.1 | 2025-12-26 | ktp_discord.inc v1.1.0 (curl module), RH_SV_Rcon hook constant |
| 2.6.0 | 2025-12-21 | ktp_drop_client native, ktp_discord.inc shared include |
| 2.5.1 | 2025-12-20 | DODX dodx_set_pl_teamname native for player team names |
| 2.5.0 | 2025-12-19 | HLStatsX integration: match ID, stats flush/reset natives |
| 2.4.0 | 2025-12-16 | DODX shot tracking, module SDK extensions, log file fix, debug cleanup |
| 2.3.0 | 2025-12-14 | DODX extension mode complete, TraceLine hook, stats_logging crash fix |
| 2.2.0 | 2025-12-08 | register_event/register_logevent extension mode, module API |
| 2.1.0 | 2025-12-06 | Map change support, client commands, menu systems in extension mode |
| 2.0.0 | 2025-12-04 | Major release: ReHLDS extension mode, KTP branding, client_cvar_changed |
| 1.10.0 | - | Base fork from AMX Mod X |

[2.6.13]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.13
[2.6.12]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.12
[2.6.11]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.11
[2.6.10]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.10
[2.6.9]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.9
[2.6.8]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.8
[2.6.7]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.7
[2.6.6]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.6
[2.6.5]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.5
[2.6.4]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.4
[2.6.3]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.3
[2.6.2]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.2
[2.6.1]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.1
[2.6.0]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.6.0
[2.5.1]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.5.1
[2.5.0]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.5.0
[2.4.0]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.4.0
[2.3.0]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.3.0
[2.2.0]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.2.0
[2.1.0]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.1.0
[2.0.0]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.0.0
[1.10.0]: https://github.com/alliedmodders/amxmodx/releases/tag/1.10.0
