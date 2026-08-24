// vim: set ts=4 sw=4 tw=99 noet:
//
// AMX Mod X, based on AMX Mod by Aleksander Naszko ("OLO").
// Copyright (C) The AMX Mod X Development Team.
// Copyright (C) 2003 JustinHoMi.
//
// This software is licensed under the GNU General Public License, version 3 or higher.
// Additional exceptions apply. For full license details, see LICENSE.txt or visit:
//     https://alliedmods.net/amxmodx-license

//
// DoD Stats Logging Plugin
//

#include <amxmodx>
#include <dodx>

// KTP: extra per-match stat capture for HLStatsX (assists, cap breaks,
// positions and frag context today; the per-hit damage ledger in a later
// phase). Self-contained -- it shares no state with this file. See its
// header for why.
#include "ktp_stats_capture"

#define PLUGIN_VERSION "1.15.8"

// KTP: Buffered logging to avoid synchronous file I/O during postthink.
// client_death fires inside SV_RunCmd postthink phase — a single log_message()
// call can stall the frame 26-122ms due to synchronous fwrite. Instead, we
// append to a memory buffer and flush periodically from a set_task callback.
#define LOG_BUFFER_MAX_ENTRIES 32
#define LOG_BUFFER_LINE_LEN 512
#define LOG_BUFFER_FLUSH_INTERVAL 5.0

new g_logBuffer[LOG_BUFFER_MAX_ENTRIES][LOG_BUFFER_LINE_LEN]
new g_logBufferCount = 0

new g_pingSum[MAX_PLAYERS + 1]
new g_pingCount[MAX_PLAYERS + 1]

// KTP: Match ID for HLStatsX integration
// Set via dodx_set_match_id() native, retrieved via dodx_get_match_id()
new g_matchId[64]

public plugin_init() {
  register_plugin("Stats Logging",PLUGIN_VERSION,"AMXX Dev Team")

  // KTP: Don't call "log on" - it causes log rotation
  // Logging should be enabled via sv_logfile 1 in server.cfg
  // The "log on" command will close and reopen the log file, breaking file writes

  ksc_init()
}

public plugin_cfg() {
  // KTP: Start periodic flush for buffered log entries.
  // Registered in plugin_cfg (not plugin_init) because plugin_init fires from
  // within a hookchain handler in extension mode, where set_task with repeating
  // flag intermittently fails to register (~10% failure rate).
  set_task(LOG_BUFFER_FLUSH_INTERVAL, "flush_log_buffer_task", .flags="b")

  ksc_cfg()
}

public plugin_end() {
  // KTP: Flush any remaining buffered log entries before map change
  flush_log_buffer()
  ksc_flush()
}

// KTP: Append a formatted log line to the memory buffer instead of writing to disk.
// If the buffer is full, flush immediately to avoid data loss.
stock buffer_log(const line[]) {
  if (g_logBufferCount >= LOG_BUFFER_MAX_ENTRIES)
    flush_log_buffer()

  copy(g_logBuffer[g_logBufferCount], LOG_BUFFER_LINE_LEN - 1, line)
  g_logBufferCount++
}

// KTP: Flush all buffered log entries to disk. Called from set_task and plugin_end.
public flush_log_buffer_task() {
  flush_log_buffer()
}

stock flush_log_buffer() {
  if (g_logBufferCount <= 0)
    return

  for (new i = 0; i < g_logBufferCount; i++)
    log_message("%s", g_logBuffer[i])

  g_logBufferCount = 0
}

// KTP: Forward handler for dod_stats_flush(id)
// Called by dodx_flush_all_stats() native to log pending stats for a player
// This allows flushing warmup stats before match starts
public dod_stats_flush(id) {
  if ( !is_user_connected(id) || !isDSMActive() )
    return PLUGIN_CONTINUE

  // Production has always excluded bots from StatsMe weapon summaries. Lane B
  // needs to exercise that ingestion path with its all-bot roster, so its
  // separately compiled artifact may opt in. Keep two runtime containment
  // gates as defence if a test binary is ever copied outside the harness.
  if ( is_user_bot(id) ) {
#if defined KTP_LANE_B_BOT_WEAPONSTATS
    if ( get_cvar_num("sv_lan") != 1 || get_cvar_num("ktp_testmatch_enabled") != 1 )
      return PLUGIN_CONTINUE
#else
    return PLUGIN_CONTINUE
#endif
  }

  // KTP: Get current match ID for HLStatsX integration
  dodx_get_match_id(g_matchId, charsmax(g_matchId))

  // Log weaponstats for this player
  log_player_stats(id)

  return PLUGIN_CONTINUE
}

// KTP: Log all weapon stats for a player
// Extracted from client_disconnected to allow reuse in dod_stats_flush
stock log_player_stats(id) {
  new szTeam[16], szName[MAX_NAME_LENGTH], szAuthid[32], iStats[DODX_MAX_STATS], iHits[MAX_BODYHITS], szWeapon[16]
  new iUserid = get_user_userid(id)

  get_user_info(id, "team", szTeam, charsmax(szTeam))
  if (szTeam[0])
    szTeam[0] -= 32

  get_user_name(id, szName, charsmax(szName))
  get_user_authid(id, szAuthid, charsmax(szAuthid))

  for(new i = 1; i < DODMAX_WEAPONS; ++i) {
    if( get_user_wstats(id, i, iStats, iHits) ) {
      xmod_get_wpnlogname(i, szWeapon, charsmax(szWeapon))

      // KTP: Include match ID if set (for HLStatsX match tracking)
      if (g_matchId[0]) {
        log_message("^"%s<%d><%s><%s>^" triggered ^"weaponstats^" (weapon ^"%s^") (shots ^"%d^") (hits ^"%d^") (kills ^"%d^") (headshots ^"%d^") (tks ^"%d^") (damage ^"%d^") (deaths ^"%d^") (score ^"%d^") (matchid ^"%s^")",
          szName, iUserid, szAuthid, szTeam, szWeapon, iStats[DODX_SHOTS], iStats[DODX_HITS], iStats[DODX_KILLS], iStats[DODX_HEADSHOTS], iStats[DODX_TEAMKILLS], iStats[DODX_DAMAGE], iStats[DODX_DEATHS], iStats[DODX_POINTS], g_matchId)
        log_message("^"%s<%d><%s><%s>^" triggered ^"weaponstats2^" (weapon ^"%s^") (head ^"%d^") (chest ^"%d^") (stomach ^"%d^") (leftarm ^"%d^") (rightarm ^"%d^") (leftleg ^"%d^") (rightleg ^"%d^") (matchid ^"%s^")",
          szName, iUserid, szAuthid, szTeam, szWeapon, iHits[HIT_HEAD], iHits[HIT_CHEST], iHits[HIT_STOMACH], iHits[HIT_LEFTARM], iHits[HIT_RIGHTARM], iHits[HIT_LEFTLEG], iHits[HIT_RIGHTLEG], g_matchId)
      } else {
        // No match ID - warmup/practice stats
        log_message("^"%s<%d><%s><%s>^" triggered ^"weaponstats^" (weapon ^"%s^") (shots ^"%d^") (hits ^"%d^") (kills ^"%d^") (headshots ^"%d^") (tks ^"%d^") (damage ^"%d^") (deaths ^"%d^") (score ^"%d^")",
          szName, iUserid, szAuthid, szTeam, szWeapon, iStats[DODX_SHOTS], iStats[DODX_HITS], iStats[DODX_KILLS], iStats[DODX_HEADSHOTS], iStats[DODX_TEAMKILLS], iStats[DODX_DAMAGE], iStats[DODX_DEATHS], iStats[DODX_POINTS])
        log_message("^"%s<%d><%s><%s>^" triggered ^"weaponstats2^" (weapon ^"%s^") (head ^"%d^") (chest ^"%d^") (stomach ^"%d^") (leftarm ^"%d^") (rightarm ^"%d^") (leftleg ^"%d^") (rightleg ^"%d^")",
          szName, iUserid, szAuthid, szTeam, szWeapon, iHits[HIT_HEAD], iHits[HIT_CHEST], iHits[HIT_STOMACH], iHits[HIT_LEFTARM], iHits[HIT_RIGHTARM], iHits[HIT_LEFTLEG], iHits[HIT_RIGHTLEG])
      }
    }
  }
}

// KTP: client_death dispatches into ktp_stats_capture.inc for everything
// this fork adds on top of the stock plugin: assists, cap-break candidate
// queuing, and frag_context (headshot/prone/scope/ammo on every kill).
//
// frag_context replaced a dedicated "headshot_kill" marker that used to live
// here directly (headshot-only, its own buffered log line). Both use the same
// technique -- log a marker after the kill, daemon flushes and UPDATEs the
// just-inserted Frags row by killerId/victimId/weapon -- so folding headshot
// into frag_context as one more property means one queued line and one
// daemon UPDATE per kill instead of two. See ktp_stats_capture.inc's header.
public client_death(killer, victim, wpnindex, hitplace, TK) {
  ksc_on_death(killer, victim, wpnindex, hitplace, TK)

  return PLUGIN_CONTINUE
}

public client_disconnected(id) {
  // Always remove the ping task, regardless of connection state or stats pause.
  // The repeating task must be cleaned up to prevent orphaned tasks that spam
  // errors when the slot is reused by a new player.
  remove_task( id )
  g_pingSum[ id ] = g_pingCount[ id ] = 0

  // KTP: drop this slot's assist state so the next player to occupy it can't
  // inherit pending credit.
  ksc_clear_player(id)

  if ( is_user_bot(id) || !isDSMActive() )
    return PLUGIN_CONTINUE

  // KTP: Get current match ID for HLStatsX integration
  dodx_get_match_id(g_matchId, charsmax(g_matchId))

  // KTP: Log weapon stats using shared function
  log_player_stats(id)

  // Log time and latency
  new szTeam[16], szName[MAX_NAME_LENGTH], szAuthid[32]
  new iUserid = get_user_userid(id)

  get_user_info(id, "team", szTeam, charsmax(szTeam))
  if (szTeam[0])
    szTeam[0] -= 32

  get_user_name(id, szName, charsmax(szName))
  get_user_authid(id, szAuthid, charsmax(szAuthid))

  new iTime = get_user_time(id, 1)

  // KTP: Include match ID in time/latency logs if set
  if (g_matchId[0]) {
    log_message("^"%s<%d><%s><%s>^" triggered ^"time^" (time ^"%d:%02d^") (matchid ^"%s^")",
      szName, iUserid, szAuthid, szTeam, (iTime / 60), (iTime % 60), g_matchId)
    log_message("^"%s<%d><%s><%s>^" triggered ^"latency^" (ping ^"%d^") (matchid ^"%s^")",
      szName, iUserid, szAuthid, szTeam, (g_pingSum[id] / (g_pingCount[id] ? g_pingCount[id] : 1)), g_matchId)
  } else {
    log_message("^"%s<%d><%s><%s>^" triggered ^"time^" (time ^"%d:%02d^")",
      szName, iUserid, szAuthid, szTeam, (iTime / 60), (iTime % 60))
    log_message("^"%s<%d><%s><%s>^" triggered ^"latency^" (ping ^"%d^")",
      szName, iUserid, szAuthid, szTeam, (g_pingSum[id] / (g_pingCount[id] ? g_pingCount[id] : 1)))
  }

  return PLUGIN_CONTINUE
}

public client_putinserver(id) {
  if ( !is_user_bot( id ) ){
    // Remove any stale task from a previous connection on this slot
    remove_task( id )
    g_pingSum[ id ] = g_pingCount[ id ] = 0
    set_task( 19.5 , "getPing" , id , "" , 0 , "b" )
  }
}

public getPing( id ) {
  if ( !is_user_connected( id ) ) {
    remove_task( id )
    return
  }
  new iPing, iLoss
  get_user_ping( id , iPing, iLoss)
  g_pingSum[ id ] += iPing
  ++g_pingCount[ id ]
}

isDSMActive(){
  if ( get_cvar_num("dodstats_pause") )
    return 0
  return 1
}
