/**
 * KTP Discord Integration - Shared Include File
 *
 * Provides standardized Discord webhook functionality for all KTP plugins.
 * Uses discord.ini for configuration and AMXX curl module for HTTP requests.
 *
 * AUTHOR: Nein_
 * VERSION: 1.1.0
 * DATE: 2025-12-21
 * GITHUB: https://github.com/KTP-Community
 *
 * CHANGELOG:
 * v1.1.0 (2025-12-21)
 *   * FIXED: Use AMXX curl module instead of server_cmd() which can't execute shell commands
 *   * REQUIRES: curl module (curl_amxx.so / curl_amxx.dll)
 *
 * ========== USAGE ==========
 * #include <ktp_discord>
 *
 * In plugin_cfg():
 *   ktp_discord_load_config();
 *
 * To send messages:
 *   ktp_discord_send("Hello World");  // Default channel
 *   ktp_discord_send_to(KTP_DISCORD_CHANNEL_AUDIT, "Audit message");
 *   ktp_discord_send_embed(title, description, color);
 *
 * ========== CONFIGURATION ==========
 * File: <configsdir>/discord.ini
 *
 * Required keys:
 *   discord_relay_url=https://your-relay.example.com
 *   discord_auth_secret=your_secret_here
 *   discord_channel_id=default_channel_id
 *
 * Optional channel keys:
 *   discord_channel_id_audit=audit_channel_id
 *   discord_channel_id_audit_competitive=competitive_audit_channel
 *   discord_channel_id_audit_12man=12man_audit_channel
 *   discord_channel_id_audit_scrim=scrim_audit_channel
 *   discord_channel_id_12man=12man_channel
 *   discord_channel_id_scrim=scrim_channel
 *   discord_channel_id_admin=admin_channel
 */

#if defined _ktp_discord_included
    #endinput
#endif
#define _ktp_discord_included

// Requires amxmisc for get_configsdir()
#if !defined _amxmisc_included
    #include <amxmisc>
#endif

// Requires curl module for HTTP requests
#if !defined _curl_included
    #include <curl>
#endif

// Note: Not using #pragma semicolon 1 to maintain compatibility with plugins
// that don't use semicolons. The include itself uses semicolons consistently.

// ============================================================================
// CONSTANTS
// ============================================================================

#define KTP_DISCORD_VERSION "1.1.0"

// Channel types
#define KTP_DISCORD_CHANNEL_DEFAULT     0   // Main channel (discord_channel_id)
#define KTP_DISCORD_CHANNEL_AUDIT       1   // Audit channel(s)
#define KTP_DISCORD_CHANNEL_12MAN       2   // 12-man match channel
#define KTP_DISCORD_CHANNEL_SCRIM       3   // Scrim channel
#define KTP_DISCORD_CHANNEL_ADMIN       4   // Admin channel

// Maximum audit channels (for multi-channel audit support)
#define KTP_DISCORD_MAX_AUDIT_CHANNELS  10

// Embed colors (decimal values)
#define KTP_DISCORD_COLOR_RED       16711680    // 0xFF0000 - Errors, bans
#define KTP_DISCORD_COLOR_ORANGE    16750848    // 0xFFA500 - Warnings, kicks
#define KTP_DISCORD_COLOR_GREEN     65280       // 0x00FF00 - Success
#define KTP_DISCORD_COLOR_BLUE      3447003     // 0x3498DB - Info
#define KTP_DISCORD_COLOR_PURPLE    10181046    // 0x9B59B6 - Special

// ============================================================================
// GLOBAL STORAGE
// ============================================================================

// Discord configuration (loaded from INI)
new g_ktpDiscordRelayUrl[256];
new g_ktpDiscordAuthSecret[128];
new g_ktpDiscordChannelDefault[64];
new g_ktpDiscordChannel12man[64];
new g_ktpDiscordChannelScrim[64];
new g_ktpDiscordChannelAdmin[64];

// Audit channels array (supports multiple)
new g_ktpDiscordAuditChannels[KTP_DISCORD_MAX_AUDIT_CHANNELS][64];
new g_ktpDiscordAuditChannelCount = 0;

// Config state
new bool:g_ktpDiscordConfigLoaded = false;
new bool:g_ktpDiscordEnabled = false;

// Temp file for curl responses (no longer used, kept for compatibility)
new g_ktpDiscordTempFile[128];

// Curl state
new curl_slist:g_ktpDiscordCurlHeaders = SList_Empty;
new g_ktpDiscordAuthHeader[192];

// Reusable buffers (to avoid stack overflow)
new g_ktpDiscordEscapedBuf[512];
new g_ktpDiscordPayloadBuf[1024];

// ============================================================================
// CONFIGURATION LOADING
// ============================================================================

/**
 * Load Discord configuration from discord.ini
 * Call this in plugin_cfg() after configs are loaded
 *
 * @return      true if configuration loaded successfully, false otherwise
 */
stock bool:ktp_discord_load_config()
{
    // Reset state
    g_ktpDiscordRelayUrl[0] = EOS;
    g_ktpDiscordAuthSecret[0] = EOS;
    g_ktpDiscordChannelDefault[0] = EOS;
    g_ktpDiscordChannel12man[0] = EOS;
    g_ktpDiscordChannelScrim[0] = EOS;
    g_ktpDiscordChannelAdmin[0] = EOS;
    g_ktpDiscordAuditChannelCount = 0;
    g_ktpDiscordEnabled = false;
    g_ktpDiscordConfigLoaded = false;

    for (new i = 0; i < KTP_DISCORD_MAX_AUDIT_CHANNELS; i++) {
        g_ktpDiscordAuditChannels[i][0] = EOS;
    }

    // Build path to discord.ini
    new configsDir[128], path[192];
    get_configsdir(configsDir, charsmax(configsDir));
    formatex(path, charsmax(path), "%s/discord.ini", configsDir);

    // Set up temp file path
    formatex(g_ktpDiscordTempFile, charsmax(g_ktpDiscordTempFile), "%s/ktp_discord_resp.tmp", configsDir);

    // Open config file
    new fp = fopen(path, "rt");
    if (!fp) {
        log_amx("[KTP Discord] Config not found: %s", path);
        return false;
    }

    new line[256], key[64], val[192];
    new loadedCount = 0;

    while (!feof(fp)) {
        fgets(fp, line, charsmax(line));
        trim(line);

        // Skip empty lines and comments
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '/' && line[1] == '/') continue;

        // Find equals sign
        new eq = contain(line, "=");
        if (eq <= 0) continue;

        // Extract key and value
        copy(key, min(eq, charsmax(key)), line);
        trim(key);

        // Convert key to lowercase for consistent matching
        for (new k = 0; key[k]; k++) {
            key[k] = tolower(key[k]);
        }

        copy(val, charsmax(val), line[eq + 1]);
        trim(val);

        if (!key[0] || !val[0]) continue;

        // Parse known keys
        if (equal(key, "discord_relay_url")) {
            copy(g_ktpDiscordRelayUrl, charsmax(g_ktpDiscordRelayUrl), val);
            loadedCount++;
        }
        else if (equal(key, "discord_auth_secret")) {
            copy(g_ktpDiscordAuthSecret, charsmax(g_ktpDiscordAuthSecret), val);
            loadedCount++;
        }
        else if (equal(key, "discord_channel_id")) {
            copy(g_ktpDiscordChannelDefault, charsmax(g_ktpDiscordChannelDefault), val);
            loadedCount++;
        }
        else if (equal(key, "discord_channel_id_12man")) {
            copy(g_ktpDiscordChannel12man, charsmax(g_ktpDiscordChannel12man), val);
            loadedCount++;
        }
        else if (equal(key, "discord_channel_id_scrim")) {
            copy(g_ktpDiscordChannelScrim, charsmax(g_ktpDiscordChannelScrim), val);
            loadedCount++;
        }
        else if (equal(key, "discord_channel_id_admin")) {
            copy(g_ktpDiscordChannelAdmin, charsmax(g_ktpDiscordChannelAdmin), val);
            // Also add to audit channels
            if (g_ktpDiscordAuditChannelCount < KTP_DISCORD_MAX_AUDIT_CHANNELS) {
                copy(g_ktpDiscordAuditChannels[g_ktpDiscordAuditChannelCount], 63, val);
                g_ktpDiscordAuditChannelCount++;
            }
            loadedCount++;
        }
        // Collect all audit channel variants
        else if (containi(key, "discord_channel_id_audit") != -1) {
            if (g_ktpDiscordAuditChannelCount < KTP_DISCORD_MAX_AUDIT_CHANNELS) {
                copy(g_ktpDiscordAuditChannels[g_ktpDiscordAuditChannelCount], 63, val);
                g_ktpDiscordAuditChannelCount++;
                loadedCount++;
            }
        }
    }
    fclose(fp);

    // Check if we have minimum required config
    if (g_ktpDiscordRelayUrl[0] && g_ktpDiscordAuthSecret[0] &&
        (g_ktpDiscordChannelDefault[0] || g_ktpDiscordAuditChannelCount > 0)) {
        g_ktpDiscordEnabled = true;
        g_ktpDiscordConfigLoaded = true;
        log_amx("[KTP Discord] Loaded %d keys, %d audit channels from %s",
                loadedCount, g_ktpDiscordAuditChannelCount, path);
        return true;
    }

    log_amx("[KTP Discord] Missing required config (url=%d, auth=%d, channel=%d)",
            g_ktpDiscordRelayUrl[0] ? 1 : 0,
            g_ktpDiscordAuthSecret[0] ? 1 : 0,
            (g_ktpDiscordChannelDefault[0] || g_ktpDiscordAuditChannelCount > 0) ? 1 : 0);
    return false;
}

// ============================================================================
// CHANNEL SELECTION
// ============================================================================

/**
 * Get channel ID for a specific channel type
 *
 * @param channelType   Channel type constant (KTP_DISCORD_CHANNEL_*)
 * @param output        Buffer to store channel ID
 * @param maxlen        Maximum buffer length
 * @return              true if channel found, false otherwise
 */
stock bool:ktp_discord_get_channel(channelType, output[], maxlen)
{
    output[0] = EOS;

    switch (channelType) {
        case KTP_DISCORD_CHANNEL_DEFAULT: {
            if (g_ktpDiscordChannelDefault[0]) {
                copy(output, maxlen, g_ktpDiscordChannelDefault);
                return true;
            }
        }
        case KTP_DISCORD_CHANNEL_12MAN: {
            if (g_ktpDiscordChannel12man[0]) {
                copy(output, maxlen, g_ktpDiscordChannel12man);
                return true;
            }
            // Fall back to default
            if (g_ktpDiscordChannelDefault[0]) {
                copy(output, maxlen, g_ktpDiscordChannelDefault);
                return true;
            }
        }
        case KTP_DISCORD_CHANNEL_SCRIM: {
            if (g_ktpDiscordChannelScrim[0]) {
                copy(output, maxlen, g_ktpDiscordChannelScrim);
                return true;
            }
            // Fall back to default
            if (g_ktpDiscordChannelDefault[0]) {
                copy(output, maxlen, g_ktpDiscordChannelDefault);
                return true;
            }
        }
        case KTP_DISCORD_CHANNEL_ADMIN: {
            if (g_ktpDiscordChannelAdmin[0]) {
                copy(output, maxlen, g_ktpDiscordChannelAdmin);
                return true;
            }
        }
        case KTP_DISCORD_CHANNEL_AUDIT: {
            // Return first audit channel (use ktp_discord_send_audit for all)
            if (g_ktpDiscordAuditChannelCount > 0) {
                copy(output, maxlen, g_ktpDiscordAuditChannels[0]);
                return true;
            }
        }
    }

    return false;
}

// ============================================================================
// JSON ESCAPING
// ============================================================================

/**
 * Escape a string for JSON embedding
 *
 * @param input     Input string to escape
 * @param output    Buffer for escaped output
 * @param maxlen    Maximum output buffer length
 */
stock ktp_discord_escape_json(const input[], output[], maxlen)
{
    new outPos = 0;

    for (new i = 0; input[i] && outPos < maxlen - 2; i++) {
        switch (input[i]) {
            case '"': {
                if (outPos < maxlen - 2) {
                    output[outPos++] = 92;  // backslash
                    output[outPos++] = '"';
                }
            }
            case 92: {  // backslash
                if (outPos < maxlen - 2) {
                    output[outPos++] = 92;
                    output[outPos++] = 92;
                }
            }
            case 10: {  // newline
                if (outPos < maxlen - 2) {
                    output[outPos++] = 92;
                    output[outPos++] = 'n';
                }
            }
            case 13: {  // carriage return
                if (outPos < maxlen - 2) {
                    output[outPos++] = 92;
                    output[outPos++] = 'r';
                }
            }
            case 9: {   // tab
                if (outPos < maxlen - 2) {
                    output[outPos++] = 92;
                    output[outPos++] = 't';
                }
            }
            default: {
                // Only include printable ASCII characters
                if (input[i] >= 32 && input[i] < 127) {
                    output[outPos++] = input[i];
                }
            }
        }
    }

    output[outPos] = EOS;
}

// ============================================================================
// MESSAGE SENDING
// ============================================================================

/**
 * Check if Discord is enabled and configured
 *
 * @return      true if Discord is ready to send messages
 */
stock bool:ktp_discord_is_enabled()
{
    return g_ktpDiscordEnabled;
}

/**
 * Send a simple text message to the default channel
 *
 * @param message   Message content (will be escaped automatically)
 * @return          true if message was sent, false otherwise
 */
stock bool:ktp_discord_send(const message[])
{
    return ktp_discord_send_to(KTP_DISCORD_CHANNEL_DEFAULT, message);
}

/**
 * Send a simple text message to a specific channel type
 *
 * @param channelType   Channel type (KTP_DISCORD_CHANNEL_*)
 * @param message       Message content (will be escaped automatically)
 * @return              true if message was sent, false otherwise
 */
stock bool:ktp_discord_send_to(channelType, const message[])
{
    if (!g_ktpDiscordEnabled) return false;

    new channelId[64];
    if (!ktp_discord_get_channel(channelType, channelId, charsmax(channelId))) {
        return false;
    }

    // Escape message for JSON
    ktp_discord_escape_json(message, g_ktpDiscordEscapedBuf, charsmax(g_ktpDiscordEscapedBuf));

    // Build payload (use channelId camelCase to match relay API)
    formatex(g_ktpDiscordPayloadBuf, charsmax(g_ktpDiscordPayloadBuf),
        "{^"channelId^":^"%s^",^"content^":^"%s^"}",
        channelId, g_ktpDiscordEscapedBuf);

    // Send via curl
    ktp_discord_send_raw(g_ktpDiscordPayloadBuf);
    return true;
}

/**
 * Send a message to ALL configured audit channels
 *
 * @param message   Message content (will be escaped automatically)
 * @return          Number of channels message was sent to
 */
stock ktp_discord_send_audit(const message[])
{
    log_amx("[KTP Discord] send_audit called, enabled=%d, audit_channels=%d", g_ktpDiscordEnabled, g_ktpDiscordAuditChannelCount);
    if (!g_ktpDiscordEnabled || g_ktpDiscordAuditChannelCount == 0) return 0;

    // Escape message once
    ktp_discord_escape_json(message, g_ktpDiscordEscapedBuf, charsmax(g_ktpDiscordEscapedBuf));

    new sent = 0;
    for (new i = 0; i < g_ktpDiscordAuditChannelCount; i++) {
        if (!g_ktpDiscordAuditChannels[i][0]) continue;

        // Build payload for this channel (use channelId camelCase to match relay API)
        formatex(g_ktpDiscordPayloadBuf, charsmax(g_ktpDiscordPayloadBuf),
            "{^"channelId^":^"%s^",^"content^":^"%s^"}",
            g_ktpDiscordAuditChannels[i], g_ktpDiscordEscapedBuf);

        ktp_discord_send_raw(g_ktpDiscordPayloadBuf);
        sent++;
    }

    return sent;
}

/**
 * Send a Discord embed to the default channel
 *
 * @param title         Embed title
 * @param description   Embed description (supports Discord markdown)
 * @param color         Embed color (use KTP_DISCORD_COLOR_* constants)
 * @return              true if embed was sent, false otherwise
 */
stock bool:ktp_discord_send_embed(const title[], const description[], color = KTP_DISCORD_COLOR_BLUE)
{
    return ktp_discord_send_embed_to(KTP_DISCORD_CHANNEL_DEFAULT, title, description, color);
}

/**
 * Send a Discord embed to a specific channel type
 *
 * @param channelType   Channel type (KTP_DISCORD_CHANNEL_*)
 * @param title         Embed title
 * @param description   Embed description
 * @param color         Embed color
 * @return              true if embed was sent, false otherwise
 */
stock bool:ktp_discord_send_embed_to(channelType, const title[], const description[], color = KTP_DISCORD_COLOR_BLUE)
{
    if (!g_ktpDiscordEnabled) return false;

    new channelId[64];
    if (!ktp_discord_get_channel(channelType, channelId, charsmax(channelId))) {
        return false;
    }

    // Escape strings
    new escapedTitle[128], escapedDesc[384];
    ktp_discord_escape_json(title, escapedTitle, charsmax(escapedTitle));
    ktp_discord_escape_json(description, escapedDesc, charsmax(escapedDesc));

    // Get server info for footer
    new serverName[64], mapName[32];
    get_cvar_string("hostname", serverName, charsmax(serverName));
    get_mapname(mapName, charsmax(mapName));

    new escapedServer[128];
    ktp_discord_escape_json(serverName, escapedServer, charsmax(escapedServer));

    // Build embed payload (use channelId camelCase, embeds at top level to match relay API)
    formatex(g_ktpDiscordPayloadBuf, charsmax(g_ktpDiscordPayloadBuf),
        "{^"channelId^":^"%s^",^"embeds^":[{^"title^":^"%s^",^"description^":^"%s^",^"color^":%d,^"footer^":{^"text^":^"%s - %s^"}}]}",
        channelId, escapedTitle, escapedDesc, color, escapedServer, mapName);

    ktp_discord_send_raw(g_ktpDiscordPayloadBuf);
    return true;
}

/**
 * Send an embed to ALL configured audit channels
 *
 * @param title         Embed title
 * @param description   Embed description
 * @param color         Embed color
 * @return              Number of channels message was sent to
 */
stock ktp_discord_send_embed_audit(const title[], const description[], color = KTP_DISCORD_COLOR_BLUE)
{
    log_amx("[KTP Discord] send_embed_audit called, title='%s', enabled=%d, audit_channels=%d", title, g_ktpDiscordEnabled, g_ktpDiscordAuditChannelCount);
    if (!g_ktpDiscordEnabled || g_ktpDiscordAuditChannelCount == 0) return 0;

    // Escape strings once
    new escapedTitle[128], escapedDesc[384];
    ktp_discord_escape_json(title, escapedTitle, charsmax(escapedTitle));
    ktp_discord_escape_json(description, escapedDesc, charsmax(escapedDesc));

    // Get server info
    new serverName[64], mapName[32];
    get_cvar_string("hostname", serverName, charsmax(serverName));
    get_mapname(mapName, charsmax(mapName));

    new escapedServer[128];
    ktp_discord_escape_json(serverName, escapedServer, charsmax(escapedServer));

    new sent = 0;
    for (new i = 0; i < g_ktpDiscordAuditChannelCount; i++) {
        if (!g_ktpDiscordAuditChannels[i][0]) continue;

        // Build embed payload for this channel (use channelId camelCase, embeds at top level)
        formatex(g_ktpDiscordPayloadBuf, charsmax(g_ktpDiscordPayloadBuf),
            "{^"channelId^":^"%s^",^"embeds^":[{^"title^":^"%s^",^"description^":^"%s^",^"color^":%d,^"footer^":{^"text^":^"%s - %s^"}}]}",
            g_ktpDiscordAuditChannels[i], escapedTitle, escapedDesc, color, escapedServer, mapName);

        log_amx("[KTP Discord] Sending to audit channel[%d]='%s'", i, g_ktpDiscordAuditChannels[i]);
        log_amx("[KTP Discord] Payload: %s", g_ktpDiscordPayloadBuf);
        ktp_discord_send_raw(g_ktpDiscordPayloadBuf);
        sent++;
    }

    return sent;
}

// ============================================================================
// RAW SENDING (INTERNAL)
// ============================================================================

/**
 * Send a raw JSON payload to the Discord relay
 * Used internally by other send functions
 *
 * @param payload   Complete JSON payload string
 */
stock ktp_discord_send_raw(const payload[])
{
    if (!g_ktpDiscordRelayUrl[0] || !g_ktpDiscordAuthSecret[0]) return;

    // Use AMXX curl module for proper HTTP requests
    ktp_discord_curl_post(g_ktpDiscordRelayUrl, payload);
}

/**
 * Send raw JSON to a specific endpoint (for edit operations)
 *
 * @param endpoint  Full URL endpoint
 * @param payload   Complete JSON payload string
 */
stock ktp_discord_send_raw_to(const endpoint[], const payload[])
{
    if (!g_ktpDiscordAuthSecret[0]) return;

    // Use AMXX curl module for proper HTTP requests
    ktp_discord_curl_post(endpoint, payload);
}

/**
 * Internal: Perform a POST request using AMXX curl module
 *
 * @param url       URL to POST to
 * @param payload   JSON payload string
 */
stock ktp_discord_curl_post(const url[], const payload[])
{
    log_amx("[KTP Discord] curl_post called, url=%s", url);

    new CURL:curl = curl_easy_init();
    if (!curl) {
        log_amx("[KTP Discord] Failed to initialize curl");
        return;
    }

    // Free any previous headers
    if (g_ktpDiscordCurlHeaders != SList_Empty) {
        curl_slist_free_all(g_ktpDiscordCurlHeaders);
        g_ktpDiscordCurlHeaders = SList_Empty;
    }

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url);

    // Disable SSL verification (server may not have CA bundle)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);

    // Build headers (X-Relay-Auth matches the relay API expectation)
    g_ktpDiscordCurlHeaders = curl_slist_append(SList_Empty, "Content-Type: application/json");
    formatex(g_ktpDiscordAuthHeader, charsmax(g_ktpDiscordAuthHeader), "X-Relay-Auth: %s", g_ktpDiscordAuthSecret);
    g_ktpDiscordCurlHeaders = curl_slist_append(g_ktpDiscordCurlHeaders, g_ktpDiscordAuthHeader);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, g_ktpDiscordCurlHeaders);

    // Set POST data
    curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, payload);

    // Set timeout (5 seconds)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5);

    // Perform request asynchronously
    log_amx("[KTP Discord] Performing curl request...");
    curl_easy_perform(curl, "ktp_discord_curl_callback");
}

/**
 * Callback for curl requests
 */
public ktp_discord_curl_callback(CURL:curl, CURLcode:code)
{
    if (code != CURLE_OK) {
        new error[128];
        curl_easy_strerror(code, error, charsmax(error));
        log_amx("[KTP Discord] curl error: %s (code %d)", error, code);
    } else {
        log_amx("[KTP Discord] curl request successful");
    }

    // Cleanup curl handle
    curl_easy_cleanup(curl);

    // Free headers
    if (g_ktpDiscordCurlHeaders != SList_Empty) {
        curl_slist_free_all(g_ktpDiscordCurlHeaders);
        g_ktpDiscordCurlHeaders = SList_Empty;
    }
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * Get the configured relay URL
 *
 * @param output    Buffer to store URL
 * @param maxlen    Maximum buffer length
 */
stock ktp_discord_get_relay_url(output[], maxlen)
{
    copy(output, maxlen, g_ktpDiscordRelayUrl);
}

/**
 * Get the number of configured audit channels
 *
 * @return          Number of audit channels
 */
stock ktp_discord_get_audit_channel_count()
{
    return g_ktpDiscordAuditChannelCount;
}

/**
 * Get a specific audit channel ID by index
 *
 * @param index     Channel index (0-based)
 * @param output    Buffer to store channel ID
 * @param maxlen    Maximum buffer length
 * @return          true if channel exists at index, false otherwise
 */
stock bool:ktp_discord_get_audit_channel(index, output[], maxlen)
{
    if (index < 0 || index >= g_ktpDiscordAuditChannelCount) {
        output[0] = EOS;
        return false;
    }

    copy(output, maxlen, g_ktpDiscordAuditChannels[index]);
    return true;
}
