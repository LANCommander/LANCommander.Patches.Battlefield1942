/* [Voice]: voice chat with the other players on the server, through LANCommander Comms.
 *
 * The voice library (LANCommander.Comms.dll, x86, from LANCommander.Voice) is loaded from the bf1942patch folder
 * next to BF1942.exe; without it the game is left as it is. The game server runs the LANCommander Comms server
 * plugin, which mirrors its players, teams and positions into a voice server and advertises that server's port in
 * its query replies (voice_net.c). This client:
 * - finds the server the game plays on and connects to its voice server as the local player (the server knows
 *   the game's players and only lets each be claimed by its own name / id: the bridge's ids are engine id + 1);
 * - talks to the players nearby while the proximity key is held (or always while the microphone hears speech,
 *   with VoiceActivation=1), and on the team radio while the radio key is held: the server shows each player
 *   their own team's group only, which becomes the radio's target;
 * - keeps what the HUD shows up to date (voice_hud.c), and applies the keys and volumes of the menus
 *   (voice_menu.c).
 * Everything runs on the game thread, after each step of the main loop (0x44abc0, detoured; before each Present if
 * another plugin hooked it first). The voice client of the LANCommander Comms Interposer plugin stands down while
 * this one runs (the mutex Local\bf1942patch_voice_<pid>). The local player:
 * IPlayerManager [0x97d76c], vtable +0x18 player by id, +0x20 local player; IPlayer vtable +0x18 name (std::string *),
 * +0x44 id, field +0xac team. */
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice.h"
#include "d3d.h"
#include "comms/client.h"

#define FRAME_FN        0x44abc0u    /* the main loop step: thiscall(app) -> keep running */
#define PLAYER_MANAGER  0x97d76cu
#define PM_BY_ID        0x18
#define PM_LOCAL        0x20
#define PLAYER_NAME     0x18
#define PLAYER_ID       0x44
#define PLAYER_TEAM     0xac         /* int: 1 Axis, 2 Allies */

#define UPDATE_MS       100
#define RECONNECT_MS    5000
#define LEAVE_MS        5000         /* no local player this long: left the server */
#define SAVE_MS         1000         /* a slider stopped moving this long: saved */
#define TAIL_MS         1000         /* after a key is let go, the voice still sent is its (the release delay) */
#define SLOT_NEARBY     0
#define SLOT_RADIO      1

/* ---------------------------------------------------------------- the library */

#define COMMS_FUNCTIONS(X) \
    X(uint32_t, comms_get_api_version, (void)) \
    X(const char *, comms_get_version_string, (void)) \
    X(const char *, comms_result_to_string, (comms_result)) \
    X(void, comms_set_log_handler, (comms_log_fn, comms_log_level, void *)) \
    X(void, comms_client_config_init, (comms_client_config *)) \
    X(comms_result, comms_client_create, (const comms_client_config *, comms_client **)) \
    X(void, comms_connect_info_init, (comms_connect_info *)) \
    X(comms_result, comms_client_connect, (comms_client *, const comms_connect_info *)) \
    X(comms_result, comms_client_disconnect, (comms_client *)) \
    X(comms_connection_state, comms_client_get_state, (comms_client *)) \
    X(comms_result, comms_client_poll_event, (comms_client *, comms_client_event *)) \
    X(comms_result, comms_client_set_talk_targets, (comms_client *, const comms_talk_target *, uint32_t)) \
    X(comms_result, comms_client_set_ptt_targets, (comms_client *, uint32_t, const comms_talk_target *, uint32_t)) \
    X(comms_result, comms_client_set_push_to_talk, (comms_client *, uint32_t, int)) \
    X(comms_result, comms_client_get_group_info, (comms_client *, comms_group_id, comms_client_group_info *)) \
    X(comms_result, comms_client_get_player_info, (comms_client *, comms_player_id, comms_client_player_info *)) \
    X(comms_result, comms_client_get_active_speakers, (comms_client *, comms_speaker_info *, uint32_t, uint32_t *)) \
    X(comms_result, comms_client_get_transmit_state, (comms_client *, comms_transmit_state *)) \
    X(comms_result, comms_client_set_input_gain, (comms_client *, float)) \
    X(comms_result, comms_client_set_master_volume, (comms_client *, float)) \
    X(comms_result, comms_client_set_transmit_mode, (comms_client *, comms_transmit_mode))

#define DECLARE(ret, name, args) static ret (COMMS_CALL *p_##name) args;
COMMS_FUNCTIONS(DECLARE)

static HMODULE g_dll;

static int resolve(void)
{
#define RESOLVE(ret, name, args) \
    if (!(p_##name = (ret (COMMS_CALL *) args)(void (*)(void))GetProcAddress(g_dll, #name))) { \
        bf_log("voice: %s is missing from the voice library", #name); \
        return 0; \
    }
    COMMS_FUNCTIONS(RESOLVE)
    return 1;
}

/* [Voice] VoiceLibrary (relative to the game folder), else bf1942patch\LANCommander.Comms.dll, else next to this
 * DLL or BF1942.exe */
static int load_library(void)
{
    char custom[MAX_PATH], path[MAX_PATH * 2], self[MAX_PATH], *slash;
    const char *tries[4];
    int n = 0, i;
    if (bf_ini_str("Voice", "VoiceLibrary", custom, sizeof custom)) {
        tries[n++] = custom;
    }
    tries[n++] = "bf1942patch\\LANCommander.Comms.dll";
    if (bf_module() && GetModuleFileNameA(bf_module(), self, MAX_PATH) && (slash = strrchr(self, '\\')) != NULL) {
        strcpy(slash + 1, "LANCommander.Comms.dll");
        tries[n++] = self;
    }
    tries[n++] = "LANCommander.Comms.dll";
    for (i = 0; i < n && !g_dll; i++) {
        if (tries[i][0] && tries[i][1] == ':') {
            lstrcpynA(path, tries[i], MAX_PATH);
        } else {
            snprintf(path, sizeof path, "%s%s", bf_game_dir(), tries[i]);
        }
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            g_dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!g_dll) {
                bf_log("voice: cannot load %s (%lu)", path, GetLastError());
            }
        }
    }
    if (!g_dll) {
        bf_log("voice: no LANCommander.Comms.dll in %sbf1942patch, no voice chat", bf_game_dir());
        return 0;
    }
    GetModuleFileNameA(g_dll, path, MAX_PATH);
    if (!resolve()) {
        return 0;
    }
    if (p_comms_get_api_version() != COMMS_API_VERSION) {
        bf_log("voice: %s has API version %u, the patch needs %u; no voice chat", path, p_comms_get_api_version(),
               COMMS_API_VERSION);
        return 0;
    }
    bf_log("voice: %s %s", path, p_comms_get_version_string());
    return 1;
}

/* any of the library's threads (bf_log is) */
static void COMMS_CALL comms_log(comms_log_level level, const char *message, void *user)
{
    static const char *const names[] = {"trace", "debug", "info", "warn", "error"};
    (void)user;
    bf_log("voice: [comms %s] %s", level <= COMMS_LOG_ERROR ? names[level] : "?", message);
}

/* ---------------------------------------------------------------- keys */

/* the game's key names (the Controls menu, 0x6c67d0) and the virtual key held for each. The punctuation keys
 * depend on the keyboard layout: DirectInput codes, which are where the key is, turned into the layout's key */
static const struct { const char *name; int vk; int dik; } KEYS[] = {
    {"IDKey_ArrowLeft", VK_LEFT, 0}, {"IDKey_ArrowRight", VK_RIGHT, 0}, {"IDKey_ArrowUp", VK_UP, 0},
    {"IDKey_ArrowDown", VK_DOWN, 0}, {"IDKey_LeftAlt", VK_LMENU, 0}, {"IDKey_RightAlt", VK_RMENU, 0},
    {"IDKey_LeftShift", VK_LSHIFT, 0}, {"IDKey_RightShift", VK_RSHIFT, 0}, {"IDKey_LeftCtrl", VK_LCONTROL, 0},
    {"IDKey_RightCtrl", VK_RCONTROL, 0}, {"IDKey_Tab", VK_TAB, 0}, {"IDKey_PageUp", VK_PRIOR, 0},
    {"IDKey_PageDown", VK_NEXT, 0}, {"IDKey_Insert", VK_INSERT, 0}, {"IDKey_Delete", VK_DELETE, 0},
    {"IDKey_Home", VK_HOME, 0}, {"IDKey_End", VK_END, 0}, {"IDKey_Multiply", VK_MULTIPLY, 0},
    {"IDKey_Add", VK_ADD, 0}, {"IDKey_Subtract", VK_SUBTRACT, 0}, {"IDKey_Divide", VK_DIVIDE, 0},
    {"IDKey_Decimal", VK_DECIMAL, 0}, {"IDKey_NumpadComma", VK_SEPARATOR, 0}, {"IDKey_NumpadEnter", VK_RETURN, 0},
    {"IDKey_Enter", VK_RETURN, 0}, {"IDKey_Space", VK_SPACE, 0}, {"IDKey_Pause", VK_PAUSE, 0},
    {"IDKey_Backspace", VK_BACK, 0}, {"IDKey_PrintScreen", VK_SNAPSHOT, 0}, {"IDKey_Capital", VK_CAPITAL, 0},
    {"IDKey_Numlock", VK_NUMLOCK, 0}, {"IDKey_ScrollLock", VK_SCROLL, 0},
    {"IDKey_Comma", 0, 0x33}, {"IDKey_Period", 0, 0x34}, {"IDKey_Slash", 0, 0x35}, {"IDKey_Backslash", 0, 0x2b},
    {"IDKey_LeftBracket", 0, 0x1a}, {"IDKey_RightBracket", 0, 0x1b}, {"IDKey_Semicolon", 0, 0x27},
    {"IDKey_Equals", 0, 0x0d}, {"IDKey_Minus", 0, 0x0c}, {"IDKey_Apostrophe", 0, 0x28}, {"IDKey_OEM_102", 0, 0x56},
    {"Mouse_IDButton_0", VK_LBUTTON, 0}, {"Mouse_IDButton_1", VK_RBUTTON, 0}, {"Mouse_IDButton_2", VK_MBUTTON, 0},
    {"Mouse_IDButton_3", VK_XBUTTON1, 0}, {"Mouse_IDButton_4", VK_XBUTTON2, 0},
};

int bf_voice_vk(const char *name)
{
    size_t i;
    if (strncmp(name, "IDKey_", 6) == 0) {
        const char *k = name + 6;
        if (((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9')) && !k[1]) {
            return k[0];
        }
        if (k[0] == 'F' && atoi(k + 1) >= 1 && atoi(k + 1) <= 12) {
            return VK_F1 + atoi(k + 1) - 1;
        }
        if (!strncmp(k, "Numpad", 6) && k[6] >= '0' && k[6] <= '9' && !k[7]) {
            return VK_NUMPAD0 + k[6] - '0';
        }
    }
    for (i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
        if (!strcmp(name, KEYS[i].name)) {
            return KEYS[i].vk ? KEYS[i].vk : (int)MapVirtualKeyA(KEYS[i].dik, MAPVK_VSC_TO_VK_EX);
        }
    }
    return 0;
}

static char g_keys[2][48];
static int g_vk[2];

const char *bf_voice_key(int which)
{
    return g_keys[which];
}

void bf_voice_set_key(int which, const char *name)
{
    lstrcpynA(g_keys[which], name, sizeof g_keys[0]);
    g_vk[which] = name[0] ? bf_voice_vk(name) : 0;
    bf_ini_set_str("Voice", which == BF_VOICE_RADIO ? "RadioKey" : "ProximityKey", name[0] ? name : "none");
    bf_log("voice: %s key %s", which == BF_VOICE_RADIO ? "radio" : "proximity", name[0] ? name : "none");
}

/* a key setting: missing = the default, "none" = no key */
static void load_key(int which, const char *key, const char *def)
{
    if (!bf_ini_str("Voice", key, g_keys[which], sizeof g_keys[0])) {
        strcpy(g_keys[which], def);
    }
    if (!_stricmp(g_keys[which], "none")) {
        g_keys[which][0] = '\0';
    }
    g_vk[which] = g_keys[which][0] ? bf_voice_vk(g_keys[which]) : 0;
    if (g_keys[which][0] && !g_vk[which]) {
        bf_log("voice: [Voice] %s=%s is not a keyboard key or mouse button", key, g_keys[which]);
    }
}

/* ---------------------------------------------------------------- the local player */

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY
                   | PAGE_EXECUTE_WRITECOPY;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !(mbi.Protect & ok)
        || (mbi.Protect & PAGE_GUARD)) {
        return 0;
    }
    return (const BYTE *)p + n <= (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
}

/* an engine object: readable, with a vtable in the exe */
static int object(const void *p)
{
    uintptr_t vt;
    if (!readable(p, 8)) {
        return 0;
    }
    vt = *(const uintptr_t *)p;
    return vt >= 0x8c0000 && vt < 0xa00000 && readable((const void *)vt, 0x60);
}

static void *vcall(void *self, int offset)
{
    return ((void *(__attribute__((thiscall)) *)(void *))(*(void ***)self)[offset / 4])(self);
}

/* the player on this machine; 0 while not in a game */
static int local_player(char *name, int size, int *id, int *team)
{
    void *pm = *(void **)BF_ADDR(PLAYER_MANAGER), *player;
    const void *s;
    if (!object(pm) || !object(player = vcall(pm, PM_LOCAL))) {
        return 0;
    }
    s = vcall(player, PLAYER_NAME);
    if (!readable(s, BF_STRING_SIZE) || !readable(bf_string_chars(s), 1)) {
        return 0;
    }
    lstrcpynA(name, bf_string_chars(s), size);
    *id = (int)(intptr_t)vcall(player, PLAYER_ID);
    *team = readable((BYTE *)player + PLAYER_TEAM, 4) ? *(int *)((BYTE *)player + PLAYER_TEAM) : 0;
    return name[0] != '\0';
}

/* the team of the game's player id (IPlayerManager vtable +0x18, the player by id) */
static int team_of(int id)
{
    void *pm = *(void **)BF_ADDR(PLAYER_MANAGER), *player;
    if (id < 0 || id > 255 || !object(pm)) {
        return 0;
    }
    player = ((void *(__attribute__((thiscall)) *)(void *, int))(*(void ***)pm)[PM_BY_ID / 4])(pm, id);
    return object(player) && readable((BYTE *)player + PLAYER_TEAM, 4) ? *(int *)((BYTE *)player + PLAYER_TEAM) : 0;
}

/* ---------------------------------------------------------------- the client */

int bf_voice_volume = 100, bf_voice_mic = 100;

static comms_client *g_client;
static int g_failed;
static WORD g_port = COMMS_DEFAULT_PORT;     /* if the game server does not say */
static char g_password[128], g_fixed_host[256], g_join_host[256];
static WORD g_join_port;
static WORD g_query_ports[8];
static int g_n_query_ports;
unsigned char bf_voice_va[2];                /* voice activation: nearby, radio */
static int g_applied_va = -1, g_saved_va;
static int g_radio_tail;                     /* the voice being sent goes to the team */
static ULONGLONG g_key_up;                   /* a key was last held */
static comms_group_id g_team;
static int g_held[2];
static int g_claim_by_name;                  /* the server did not know our id: claim by name */
static ULONGLONG g_last_update, g_last_attempt, g_seen;
static int g_applied_volume = -1, g_applied_mic = -1, g_saved_volume, g_saved_mic;
static ULONGLONG g_changed;
static bf_voice_state g_state;

const bf_voice_state *bf_voice_now(void)
{
    return &g_state;
}

static int create(void)
{
    comms_client_config config;
    comms_talk_target nearby = {COMMS_TARGET_PROXIMITY, 0};
    p_comms_set_log_handler(comms_log, COMMS_LOG_INFO, NULL);
    p_comms_client_config_init(&config);
    config.transmit_mode = COMMS_TRANSMIT_PUSH_TO_TALK;   /* update_activation sets it */
    config.voice_activation.auto_threshold = 1;
    if (p_comms_client_create(&config, &g_client) != COMMS_OK) {
        bf_log("voice: the voice client could not be created");
        return 0;
    }
    p_comms_client_set_talk_targets(g_client, &nearby, 1);
    p_comms_client_set_ptt_targets(g_client, SLOT_NEARBY, &nearby, 1);
    bf_log("voice: ready (nearby %s, radio %s; proximity key %s, radio key %s%s%s)",
           bf_voice_va[0] ? "voice activation" : "push to talk", bf_voice_va[1] ? "voice activation" : "push to talk",
           g_keys[0][0] ? g_keys[0] : "none", g_keys[1][0] ? g_keys[1] : "none", g_join_host[0] ? ", joining " : "",
           g_join_host);
    return 1;
}

static void connect_to(const char *host, WORD port, const char *name, int id)
{
    comms_connect_info info;
    comms_result r;
    p_comms_connect_info_init(&info);
    info.host = host;
    info.port = port;
    info.game_player_id = g_claim_by_name ? 0 : (uint64_t)id + 1;   /* the server bridge's ids are engine id + 1 */
    info.player_name = name;
    info.password = g_password[0] ? g_password : NULL;
    r = p_comms_client_connect(g_client, &info);
    bf_log("voice: connecting to %s:%u as '%s' (id %d): %s", host, port, name, id, p_comms_result_to_string(r));
}

static void update_connection(ULONGLONG now)
{
    char name[64], host[256];
    int id, team, in_game = local_player(name, sizeof name, &id, &team);
    comms_connection_state state = p_comms_client_get_state(g_client);
    WORD game_port, voice_port;
    DWORD addr;
    int r;
    if (in_game) {
        g_seen = now;
    } else {
        bf_voice_query_cancel();
        if (state != COMMS_STATE_DISCONNECTED && now - g_seen > LEAVE_MS) {
            bf_log("voice: left the game, disconnecting");
            p_comms_client_disconnect(g_client);
        }
        return;
    }
    /* waiting for the game server to say which port its voice server is on */
    if (bf_voice_query_active()) {
        if (!(r = bf_voice_query_poll(&voice_port))) {
            return;
        }
        if (!bf_voice_query_address()[0]) {
            bf_log("voice: cannot resolve the game server's name; trying again");
            return;
        }
        if (r < 0) {
            bf_log("voice: the game server at %s does not say where its voice server is; trying port %u",
                   bf_voice_query_address(), g_port);
        }
        connect_to(bf_voice_query_address(), r > 0 ? voice_port : g_port, name, id);
        return;
    }
    if (state != COMMS_STATE_DISCONNECTED || now - g_last_attempt < RECONNECT_MS) {
        return;
    }
    /* VoiceServer, else the +joinServer the game was started with, else the game's traffic */
    g_last_attempt = now;
    if (g_fixed_host[0]) {
        connect_to(g_fixed_host, g_port, name, id);
        return;
    }
    lstrcpynA(host, g_join_host, sizeof host);
    game_port = g_join_port;
    if (!host[0]) {
        if (!bf_voice_traffic_server(&addr, &game_port)) {
            g_last_attempt = 0;
            return;
        }
        snprintf(host, sizeof host, "%lu.%lu.%lu.%lu", addr >> 24, (addr >> 16) & 0xff, (addr >> 8) & 0xff,
                 addr & 0xff);
    }
    if (!bf_voice_query_start(host, g_query_ports, g_n_query_ports, game_port)) {
        connect_to(host, g_port, name, id);  /* no QueryPorts (or no network) */
    }
}

static int game_has_focus(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}

static void update_keys(void)
{
    int focus = game_has_focus(), i;
    int held[2];
    held[0] = focus && g_vk[0] && (GetAsyncKeyState(g_vk[0]) & 0x8000);
    held[1] = focus && g_vk[1] && g_team && (GetAsyncKeyState(g_vk[1]) & 0x8000);
    for (i = 0; i < 2; i++) {
        if (held[i] != g_held[i]) {
            g_held[i] = held[i];
            p_comms_client_set_push_to_talk(g_client, i == 0 ? SLOT_NEARBY : SLOT_RADIO, held[i]);
        }
    }
}

static void drain_events(void)
{
    comms_client_event e;
    while (p_comms_client_poll_event(g_client, &e) == COMMS_OK) {
        switch (e.type) {
        case COMMS_CLIENT_EVENT_STATE_CHANGED:
            bf_log("voice: %s%s%s", e.state == COMMS_STATE_CONNECTED ? "connected"
                                    : e.state == COMMS_STATE_DISCONNECTED ? "disconnected" : "connecting",
                   e.reason ? ": " : "", e.reason ? p_comms_result_to_string((comms_result)e.reason) : "");
            if (e.state == COMMS_STATE_DISCONNECTED) {
                g_team = COMMS_INVALID_ID;
                if (e.reason == COMMS_ERR_AUTH_FAILED) {
                    g_claim_by_name = !g_claim_by_name;   /* no player with our id: try by name, and back */
                    g_last_attempt = 0;
                }
            }
            break;
        case COMMS_CLIENT_EVENT_GROUP_ADDED: {
            /* the server shows us our own team's group only: the radio talks to it */
            comms_client_group_info info;
            if (p_comms_client_get_group_info(g_client, e.group, &info) == COMMS_OK && info.type == COMMS_GROUP_SERVER
                && info.is_member) {
                comms_talk_target radio;
                radio.kind = COMMS_TARGET_GROUP;
                radio.id = e.group;
                g_team = e.group;
                p_comms_client_set_ptt_targets(g_client, SLOT_RADIO, &radio, 1);
                g_applied_va = -1;   /* the radio's voice activation needs the group */
                bf_log("voice: team radio %s", info.name);
            }
            break;
        }
        case COMMS_CLIENT_EVENT_GROUP_REMOVED:
            if (e.group == g_team) {
                g_team = COMMS_INVALID_ID;
                g_applied_va = -1;
            }
            break;
        case COMMS_CLIENT_EVENT_AUDIO_DEVICE_LOST:
            bf_log("voice: audio device lost: %s", e.text);
            break;
        case COMMS_CLIENT_EVENT_KICKED:
            bf_log("voice: removed from voice: %s", e.text);
            break;
        default:
            break;
        }
    }
}

/* a player's name for the HUD (Windows-1252): the game's names are, but a name in UTF-8 is converted */
static void hud_name(char *out, int size, const char *in)
{
    wchar_t w[64];
    const char *p;
    for (p = in; *p && !(*p & 0x80); p++) {
    }
    if (*p && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, w, 64) > 0
        && WideCharToMultiByte(1252, 0, w, -1, out, size, "?", NULL) > 0) {
        return;
    }
    lstrcpynA(out, in, size);
}

static void update_state(void)
{
    comms_speaker_info speakers[BF_VOICE_MAX_TALKERS];
    comms_transmit_state tx;
    uint32_t n = 0, i;
    int id;
    bf_voice_state *s = &g_state;
    s->connected = p_comms_client_get_state(g_client) == COMMS_STATE_CONNECTED;
    s->n = 0;
    s->talking = s->radio = 0;
    if (s->connected && !local_player(s->me, sizeof s->me, &id, &s->my_team)) {
        s->me[0] = 0;
    }
    if (s->connected && p_comms_client_get_transmit_state(g_client, &tx) == COMMS_OK) {
        s->talking = tx.transmitting;
        /* to the team while its key is held, and for the release delay after (which sends with no key held); else by
         * voice activation, to the team when only the radio has it */
        ULONGLONG now = GetTickCount64();
        if (tx.ptt_slots_held) {
            g_radio_tail = (tx.ptt_slots_held & (1u << SLOT_RADIO)) != 0;
            g_key_up = now;
        } else if (now - g_key_up > TAIL_MS) {
            g_radio_tail = bf_voice_va[1] && !bf_voice_va[0];
        }
        s->radio = g_radio_tail;
    }
    if (s->connected && p_comms_client_get_active_speakers(g_client, speakers, BF_VOICE_MAX_TALKERS, &n) == COMMS_OK) {
        for (i = 0; i < n && i < BF_VOICE_MAX_TALKERS; i++) {
            comms_client_player_info info;
            bf_voice_talker *t = &s->talkers[s->n];
            if (speakers[i].locally_muted
                || p_comms_client_get_player_info(g_client, speakers[i].player, &info) != COMMS_OK) {
                continue;
            }
            hud_name(t->name, sizeof t->name, info.name);
            t->team = info.game_player_id ? team_of((int)info.game_player_id - 1) : 0;   /* the bridge's ids are engine id + 1 */
            t->radio = speakers[i].kind == COMMS_TARGET_GROUP;
            s->n++;
        }
    }
    bf_voice_hud_update(s);
}

/* the sliders of the Sound options: applied as they move, saved once they stop */
static void update_volumes(ULONGLONG now)
{
    int v = bf_voice_volume < 0 ? 0 : bf_voice_volume > 200 ? 200 : bf_voice_volume;
    int m = bf_voice_mic < 0 ? 0 : bf_voice_mic > 200 ? 200 : bf_voice_mic;
    if (v != g_applied_volume || m != g_applied_mic) {
        if (v != g_applied_volume) {
            p_comms_client_set_master_volume(g_client, v / 100.0f);
        }
        if (m != g_applied_mic) {
            p_comms_client_set_input_gain(g_client, m / 100.0f);
        }
        if (g_applied_volume >= 0) {
            g_changed = now;
        }
        g_applied_volume = v;
        g_applied_mic = m;
    }
    if (g_changed && now - g_changed >= SAVE_MS) {
        g_changed = 0;
        if (v != g_saved_volume) {
            bf_ini_set_int("Voice", "VoiceVolume", g_saved_volume = v);
        }
        if (m != g_saved_mic) {
            bf_ini_set_int("Voice", "MicSensitivity", g_saved_mic = m);
        }
        bf_log("voice: voice volume %d%%, microphone %d%%", v, m);
    }
}

/* the voice activation check boxes: voice activation sends to the default targets (nearby, the team or both) whenever
 * the microphone hears speech; the keys still send to their own targets. Applied at once, kept in Settings.ini */
static void update_activation(void)
{
    comms_talk_target targets[2];
    int va = (bf_voice_va[0] ? 1 : 0) | (bf_voice_va[1] ? 2 : 0), n = 0;
    if (va == g_applied_va) {
        return;
    }
    if (va & 1) {
        targets[n].kind = COMMS_TARGET_PROXIMITY;
        targets[n++].id = 0;
    }
    if ((va & 2) && g_team) {
        targets[n].kind = COMMS_TARGET_GROUP;
        targets[n++].id = g_team;
    }
    if (n) {
        p_comms_client_set_talk_targets(g_client, targets, (uint32_t)n);
    }
    p_comms_client_set_transmit_mode(g_client, n ? COMMS_TRANSMIT_VOICE_ACTIVITY : COMMS_TRANSMIT_PUSH_TO_TALK);
    if (va != g_saved_va) {
        bf_ini_set_int("Voice", "VoiceActivation", va & 1);
        bf_ini_set_int("Voice", "RadioActivation", (va & 2) >> 1);
        g_saved_va = va;
        bf_log("voice: nearby %s, radio %s", va & 1 ? "voice activation" : "push to talk",
               va & 2 ? "voice activation" : "push to talk");
    }
    g_applied_va = va;
}

static void tick(void)
{
    ULONGLONG now;
    if (g_failed) {
        return;
    }
    if (!g_client && !create()) {
        g_failed = 1;
        return;
    }
    update_keys();                   /* every frame, so the keys feel immediate */
    now = GetTickCount64();
    if (now - g_last_update >= UPDATE_MS) {
        g_last_update = now;
        update_connection(now);
        update_volumes(now);
        update_activation();
    }
    drain_events();
    update_state();
    bf_voice_menu_tick();
}

/* ---------------------------------------------------------------- hooks */

static DWORD (__attribute__((thiscall)) *g_frame)(void *app);

static DWORD __attribute__((thiscall)) hk_frame(void *app)
{
    DWORD r = g_frame(app);
    tick();
    return r;
}

/* when another plugin hooked the main loop step first (the LANCommander Comms plugin does): every rendered frame */
static void present_tick(void *device, int after)
{
    (void)device;
    if (!after) {
        tick();
    }
}

static int (WINAPI *real_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);

static int WINAPI hk_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    if (to && tolen >= (int)sizeof(struct sockaddr_in) && to->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)to;
        if (ntohs(in->sin_port) != g_port) {
            bf_voice_traffic(ntohl(in->sin_addr.s_addr), ntohs(in->sin_port));
        }
    }
    return real_sendto(s, buf, len, flags, to, tolen);
}

void bf_voice_install(void)
{
    static const BYTE frame_head[6] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};   /* push ebp; mov ebp,esp; and esp,-8 */
    char text[256], name[48];
    if (!bf_ini_int("Voice", "VoiceChat", 1)) {
        bf_log("voice: VoiceChat=0, no voice chat");
        return;
    }
    if (!load_library()) {
        return;
    }
    g_port = (WORD)bf_ini_int("Voice", "VoicePort", COMMS_DEFAULT_PORT);
    bf_ini_str("Voice", "VoicePassword", g_password, sizeof g_password);
    bf_ini_str("Voice", "VoiceServer", g_fixed_host, sizeof g_fixed_host);
    if (!bf_ini_str("Voice", "VoiceQueryPorts", text, sizeof text)) {
        strcpy(text, "22000,23000");
    }
    g_n_query_ports = bf_voice_ports(text, g_query_ports, 8);
    bf_voice_va[0] = (unsigned char)(bf_ini_int("Voice", "VoiceActivation", 0) != 0);
    bf_voice_va[1] = (unsigned char)(bf_ini_int("Voice", "RadioActivation", 0) != 0);
    g_saved_va = bf_voice_va[0] | bf_voice_va[1] << 1;
    bf_voice_volume = g_saved_volume = bf_ini_int("Voice", "VoiceVolume", 100);
    bf_voice_mic = g_saved_mic = bf_ini_int("Voice", "MicSensitivity", 100);
    load_key(BF_VOICE_NEARBY, "ProximityKey", "IDKey_Capital");
    load_key(BF_VOICE_RADIO, "RadioKey", "Mouse_IDButton_3");
    bf_voice_join_server(GetCommandLineA(), g_join_host, sizeof g_join_host, &g_join_port);

    g_frame = (DWORD (__attribute__((thiscall)) *)(void *))bf_detour(FRAME_FN, frame_head, sizeof frame_head, hk_frame);
    if (!g_frame) {
        bf_log("voice: the main loop step (%08x) is hooked already, voice runs with each rendered frame", FRAME_FN);
        bf_d3d_install();
        bf_d3d_on_present(present_tick);
    }
    real_sendto = bf_hook_import("WSOCK32.dll", "sendto", hk_sendto);
    if (!real_sendto) {
        bf_log("voice: the game's sendto is not hooked; set VoiceServer if the game joins without restarting");
    }
    /* the LANCommander Comms Interposer plugin stands down while this one runs the voice client */
    snprintf(name, sizeof name, "Local\\bf1942patch_voice_%lu", GetCurrentProcessId());
    CreateMutexA(NULL, FALSE, name);
    bf_voice_hud_install();
    bf_voice_menu_install();
}
