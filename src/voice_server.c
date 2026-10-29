/* Voice on the dedicated server (BF1942_w32ded.exe 1.61, voice_server_win.c; bf1942_lnxded 1.61, linux/
 * server_linux.c): hosts the voice server of the LANCommander Interposer's voice API and mirrors the game into it, so
 * the game clients (voice.c) find it and talk to whom the game says.
 * - Every player is registered under game id engine id + 1 (the voice API keeps 0 for none); clients log in by
 *   claiming their game player (INTERPOSER_COMMS_AUTH_CLAIM), so voice always matches who is on the game server. Bots
 *   only with bf1942patch.voiceIncludeBots 1 (testing).
 * - The teams are the server groups "Axis" and "Allies": each player sees their own team's only, its team radio.
 * - The live players' positions drive proximity chat 20 times a second; the dead drop out of proximity but keep
 *   the radio.
 * - The voice port is advertised as \commsport\ in the game's GameSpy query replies (the platform's sendto hook calls
 *   bf_server_query_reply), so clients do not need it set.
 * - So is the FastDL server the maps and mods are downloaded from (bf1942patch.fastDlPort, fastDlHost, fastDlPath), as
 *   \fastdlport\, \fastdlhost\ and \fastdlpath\: the clients hand it to their Interposer (fastdl.c). That needs no
 *   voice server.
 * Everything but bf_server_query_reply runs on the game thread, from the platform's frame hook. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice_server.h"
#include "interposer/LANCommander.Interposer.Comms.Server.h"

#define SYNC_MS         50
#define STATUS_MS       30000
#define MAX_PLAYERS     256
#define TEAM_AXIS       1
#define TEAM_ALLIES     2

/* the Interposer's voice server exports: p_<name> is interposer_comms_<name> */
#define SERVER_FUNCTIONS(X) \
    X(uint32_t, get_api_version, (void)) \
    X(const char *, result_to_string, (interposer_comms_result)) \
    X(void, set_log_handler, (interposer_comms_log_fn, interposer_comms_log_level, void *)) \
    X(void, server_config_init, (interposer_comms_server_config *)) \
    X(interposer_comms_result, server_create, (const interposer_comms_server_config *, interposer_comms_server **)) \
    X(interposer_comms_result, server_start, (interposer_comms_server *)) \
    X(interposer_comms_result, server_stop, (interposer_comms_server *)) \
    X(void, server_destroy, (interposer_comms_server *)) \
    X(interposer_comms_result, server_poll_event, (interposer_comms_server *, interposer_comms_server_event *)) \
    X(interposer_comms_result, server_add_player, \
      (interposer_comms_server *, uint64_t, const char *, interposer_comms_player_id *)) \
    X(interposer_comms_result, server_remove_player, (interposer_comms_server *, interposer_comms_player_id)) \
    X(void, zone_config_init, (interposer_comms_zone_config *)) \
    X(interposer_comms_result, server_configure_zone, \
      (interposer_comms_server *, interposer_comms_zone_id, const interposer_comms_zone_config *)) \
    X(interposer_comms_result, server_update_player_position, \
      (interposer_comms_server *, interposer_comms_player_id, interposer_comms_zone_id, interposer_comms_vec3, \
       const interposer_comms_vec3 *)) \
    X(interposer_comms_result, server_clear_player_position, (interposer_comms_server *, interposer_comms_player_id)) \
    X(void, group_config_init, (interposer_comms_group_config *)) \
    X(interposer_comms_result, server_create_group, \
      (interposer_comms_server *, const interposer_comms_group_config *, interposer_comms_group_id *)) \
    X(interposer_comms_result, server_add_to_group, \
      (interposer_comms_server *, interposer_comms_group_id, interposer_comms_player_id)) \
    X(interposer_comms_result, server_remove_from_group, \
      (interposer_comms_server *, interposer_comms_group_id, interposer_comms_player_id))

#define DECLARE(ret, name, args) static ret (INTERPOSER_CALL *p_##name) args;
SERVER_FUNCTIONS(DECLARE)

static struct {
    int tracked;
    interposer_comms_player_id voice;
    char name[64];
    int team;
    int positioned;
} g_players[MAX_PLAYERS];

static interposer_comms_server *g_server;
static interposer_comms_group_id g_teams[3];
static int g_failed, g_bots;
static uint64_t g_last_sync, g_last_status;
static volatile uint16_t g_port;     /* advertised once the voice server runs */
static bf_server_player g_read[MAX_PLAYERS];

static int resolve(void)
{
#define RESOLVE(ret, name, args) \
    if (!(p_##name = (ret (INTERPOSER_CALL *) args)bf_server_symbol("interposer_comms_" #name))) { \
        bf_log("voice server: the Interposer has no interposer_comms_%s, no voice", #name); \
        return 0; \
    }
    SERVER_FUNCTIONS(RESOLVE)
    if (p_get_api_version() != INTERPOSER_COMMS_API_VERSION) {
        bf_log("voice server: the Interposer has voice API version %u, the patch needs %u; no voice",
               p_get_api_version(), INTERPOSER_COMMS_API_VERSION);
        return 0;
    }
    return 1;
}

/* any of the library's threads (bf_log is) */
static void INTERPOSER_CALL voice_log(interposer_comms_log_level level, const char *message, void *user)
{
    static const char *const names[] = {"trace", "debug", "info", "warn", "error"};
    (void)user;
    bf_log("voice server: [interposer %s] %s", level <= INTERPOSER_COMMS_LOG_ERROR ? names[level] : "?", message);
}

static float setting_float(const char *name, float def)
{
    const char *text = bf_setting_str(name);
    return text[0] ? (float)atof(text) : def;
}

static int start(void)
{
    char password[128], admin[128], bind[64];
    interposer_comms_server_config config;
    interposer_comms_zone_config zone;
    interposer_comms_group_config team;
    interposer_comms_result r;
    if (!resolve()) {
        return 0;
    }
    p_set_log_handler(voice_log, INTERPOSER_COMMS_LOG_INFO, NULL);
    snprintf(password, sizeof password, "%s", bf_setting_str("voicePassword"));
    snprintf(admin, sizeof admin, "%s", bf_setting_str("voiceAdminKey"));
    snprintf(bind, sizeof bind, "%s", bf_setting_str("voiceBindAddress"));
    g_bots = bf_setting_int("voiceIncludeBots") != 0;
    p_server_config_init(&config);
    config.bind_address = bind[0] ? bind : NULL;
    config.port = (uint16_t)bf_setting_int("voicePort");
    config.name = "Battlefield 1942";
    config.auth_mode = INTERPOSER_COMMS_AUTH_CLAIM;
    config.password = password[0] ? password : NULL;
    config.admin_key = admin[0] ? admin : NULL;
    config.max_players = (uint32_t)bf_setting_int("voiceMaxPlayers");
    config.max_private_groups_per_player = 1;
    config.opus_max_bitrate = (uint32_t)bf_setting_int("voiceMaxBitrate");
    config.position_authority = INTERPOSER_COMMS_POSITION_SERVER;
    r = p_server_create(&config, &g_server);
    if (r == INTERPOSER_COMMS_OK) {
        r = p_server_start(g_server);
    }
    if (r != INTERPOSER_COMMS_OK) {
        bf_log("voice server: cannot start on port %u: %s", config.port, p_result_to_string(r));
        if (g_server) {
            p_server_destroy(g_server);
            g_server = NULL;
        }
        return 0;
    }
    /* Refractor is Y up and left-handed, in metres */
    p_zone_config_init(&zone);
    zone.max_distance = setting_float("proximityRange", 50.0f);
    zone.min_distance = setting_float("proximityFullVolumeRange", 4.0f);
    zone.up.x = 0.0f;
    zone.up.y = 1.0f;
    zone.up.z = 0.0f;
    zone.left_handed = 1;
    if (p_server_configure_zone(g_server, 0, &zone) != INTERPOSER_COMMS_OK) {
        bf_log("voice server: proximityRange / proximityFullVolumeRange are not valid, using the defaults");
    }
    p_group_config_init(&team);
    team.type = INTERPOSER_COMMS_GROUP_SERVER;
    team.name = "Axis";
    p_server_create_group(g_server, &team, &g_teams[TEAM_AXIS]);
    team.name = "Allies";
    p_server_create_group(g_server, &team, &g_teams[TEAM_ALLIES]);
    g_port = config.port;
    bf_log("voice server: listening on port %u (bots %s)", config.port, g_bots ? "included" : "left out");
    return 1;
}

static void forget(int id)
{
    p_server_remove_player(g_server, g_players[id].voice);
    g_players[id].tracked = 0;
}

static void sync_players(uint64_t now)
{
    static unsigned char seen[MAX_PLAYERS];
    int n = bf_server_read_players(g_read, MAX_PLAYERS), i, tracked = 0;
    if (n < 0) {
        return;                      /* between rounds, or not hosting: the last known state stays */
    }
    memset(seen, 0, sizeof seen);
    for (i = 0; i < n; i++) {
        const bf_server_player *p = &g_read[i];
        int positioned;
        if (p->id < 0 || p->id >= MAX_PLAYERS || (p->ai && !g_bots) || !p->name[0]) {
            continue;
        }
        seen[p->id] = 1;
        if (g_players[p->id].tracked && strcmp(g_players[p->id].name, p->name)) {
            forget(p->id);           /* the id went to someone else (or a rename): start over */
        }
        if (!g_players[p->id].tracked) {
            interposer_comms_result r = p_server_add_player(g_server, (uint64_t)p->id + 1, p->name,
                                                            &g_players[p->id].voice);
            if (r != INTERPOSER_COMMS_OK) {
                bf_log("voice server: cannot add player %d '%s': %s", p->id, p->name, p_result_to_string(r));
                continue;
            }
            g_players[p->id].tracked = 1;
            g_players[p->id].team = -1;
            g_players[p->id].positioned = 0;
            snprintf(g_players[p->id].name, sizeof g_players[p->id].name, "%s", p->name);
            bf_log("voice server: player %d '%s' joined (team %d%s)", p->id, p->name, p->team, p->ai ? ", a bot" : "");
        }
        if (g_players[p->id].team != p->team) {
            int old = g_players[p->id].team;
            if (old == TEAM_AXIS || old == TEAM_ALLIES) {
                p_server_remove_from_group(g_server, g_teams[old], g_players[p->id].voice);
            }
            if (p->team == TEAM_AXIS || p->team == TEAM_ALLIES) {
                p_server_add_to_group(g_server, g_teams[p->team], g_players[p->id].voice);
            }
            g_players[p->id].team = p->team;
        }
        positioned = p->alive && p->has_position;
        if (positioned) {
            interposer_comms_vec3 at;
            at.x = p->position[0];
            at.y = p->position[1];
            at.z = p->position[2];
            p_server_update_player_position(g_server, g_players[p->id].voice, 0, at, NULL);
        } else if (g_players[p->id].positioned) {
            p_server_clear_player_position(g_server, g_players[p->id].voice);
        }
        g_players[p->id].positioned = positioned;
    }
    for (i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].tracked && !seen[i]) {
            bf_log("voice server: player %d '%s' left", i, g_players[i].name);
            forget(i);
        }
        tracked += g_players[i].tracked;
    }
    if (now - g_last_status >= STATUS_MS) {
        g_last_status = now;
        bf_log("voice server: status: %d game players, %d in voice", n, tracked);
    }
}

static void drain_events(void)
{
    interposer_comms_server_event e;
    while (p_server_poll_event(g_server, &e) == INTERPOSER_COMMS_OK) {
        switch (e.type) {
        case INTERPOSER_COMMS_SERVER_EVENT_PLAYER_CONNECTED:
            bf_log("voice server: voice player %u connected", e.player);
            break;
        case INTERPOSER_COMMS_SERVER_EVENT_PLAYER_DISCONNECTED:
            bf_log("voice server: voice player %u disconnected %s", e.player, e.text);
            break;
        case INTERPOSER_COMMS_SERVER_EVENT_AUTH_FAILED:
            bf_log("voice server: a login was refused: %s", e.text);
            break;
        case INTERPOSER_COMMS_SERVER_EVENT_ADMIN_COMMAND:
            bf_log("voice server: admin command: %s", e.text);
            break;
        default:
            break;
        }
    }
}

void bf_server_tick(void)
{
    uint64_t now;
    if (g_failed) {
        return;
    }
    if (!g_server && !start()) {
        g_failed = 1;
        return;
    }
    now = bf_server_now_ms();
    if (now - g_last_sync >= SYNC_MS) {
        g_last_sync = now;
        sync_players(now);
    }
    drain_events();
}

void bf_server_stop(void)
{
    if (g_server) {
        g_port = 0;
        p_server_stop(g_server);
        p_server_destroy(g_server);
        g_server = NULL;
        bf_log("voice server: stopped");
    }
    g_failed = 1;
}

/* ---------------------------------------------------------------- the query replies */

/* the next '\' at or after at, else length */
static int next_slash(const char *p, int at, int length)
{
    while (at < length && p[at] != '\\') {
        at++;
    }
    return at;
}

/* \fastdlport\<port>, then \fastdlhost\<host> and \fastdlpath\<path> if they are set; "" without FastDL */
static char g_fastdl[2 * BF_OPT_STR_SIZE + 64];

int bf_server_fastdl_init(void)
{
    const char *host = bf_setting_str("fastDlHost"), *path = bf_setting_str("fastDlPath");
    int port = bf_setting_int("fastDlPort"), n;
    g_fastdl[0] = '\0';
    if (port <= 0 || port > 65535) {
        if (port) {
            bf_log("fastdl: fastDlPort %d is not a port, FastDL is not advertised", port);
        }
        return 0;
    }
    /* the replies are \key\value: a backslash would end the value */
    if (strchr(host, '\\') || strchr(path, '\\')) {
        bf_log("fastdl: fastDlHost and fastDlPath cannot hold a backslash, FastDL is not advertised");
        return 0;
    }
    n = snprintf(g_fastdl, sizeof g_fastdl, "\\fastdlport\\%d", port);
    if (host[0]) {
        n += snprintf(g_fastdl + n, sizeof g_fastdl - n, "\\fastdlhost\\%s", host);
    }
    if (path[0]) {
        snprintf(g_fastdl + n, sizeof g_fastdl - n, "\\fastdlpath\\%s%s", path[0] == '/' ? "" : "/", path);
    }
    bf_log("fastdl: the query replies say the FastDL server is http://%s:%d%s%s", host[0] ? host : "<this server>",
           port, path[0] == '/' ? "" : "/", path);
    return 1;
}

int bf_server_query_reply(const char *packet, int length, char *out, int size)
{
    static const char key[] = "commsport", fastdl_key[] = "fastdlport";
    char pairs[sizeof g_fastdl + 32];
    int pos = 0, final_at = -1, key_end, value_end, n, i;
    uint16_t port = g_port;
    if ((!port && !g_fastdl[0]) || !packet || length < 7 || packet[0] != '\\') {
        return 0;
    }
    /* cheap rejection first: this sees every packet the game server sends */
    for (i = 0; i + 7 <= length && memcmp(packet + i, "\\final\\", 7); i++) {
    }
    if (i + 7 > length) {
        return 0;
    }
    /* \key\value pairs; the new one goes before the first \final\ */
    while (pos < length) {
        key_end = next_slash(packet, pos + 1, length);
        if (key_end == length) {
            return 0;                /* a key without a value: not a reply */
        }
        value_end = next_slash(packet, key_end + 1, length);
        n = key_end - pos - 1;
        if ((n == (int)sizeof key - 1 && !memcmp(packet + pos + 1, key, (size_t)n))
            || (n == (int)sizeof fastdl_key - 1 && !memcmp(packet + pos + 1, fastdl_key, (size_t)n))) {
            return 0;
        }
        if (n == 5 && !memcmp(packet + pos + 1, "final", 5) && final_at < 0) {
            final_at = pos;
        }
        pos = value_end;
    }
    n = port ? snprintf(pairs, sizeof pairs, "\\%s\\%u", key, port) : 0;
    n += snprintf(pairs + n, sizeof pairs - n, "%s", g_fastdl);
    if (final_at < 0 || length + n > size) {
        return 0;
    }
    memcpy(out, packet, (size_t)final_at);
    memcpy(out + final_at, pairs, (size_t)n);
    memcpy(out + final_at + n, packet + final_at, (size_t)(length - final_at));
    return length + n;
}
