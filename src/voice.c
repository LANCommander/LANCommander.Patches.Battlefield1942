/* bf1942patch.voiceChat: voice chat with the other players on the server, through the LANCommander Interposer's voice API.
 *
 * The voice library is the Interposer, whose interposer_comms_* exports are the former LANCommander Comms: the one
 * that loaded the Interposer plugin build, else the one embedded in the dinput8.dll / ASI builds (the patch's own
 * image exports them, bf1942patch.c). Without it the game is left as it is. The game server runs the
 * LANCommander Comms server plugin, which mirrors its players, teams and positions into a voice server and
 * advertises that server's port in its query replies (voice_net.c). This client:
 * - finds the server the game plays on and connects to its voice server as the local player (the server knows
 *   the game's players and only lets each be claimed by its own name / id: the bridge's ids are engine id + 1);
 * - talks to the players nearby while the proximity key is held (or always while the microphone hears speech,
 *   with bf1942patch.voiceActivation 1), and on the team radio while the radio key is held: the server shows each player
 *   their own team's group only, which becomes the radio's target;
 * - keeps what the HUD shows up to date (voice_hud.c), and applies the keys and volumes of the menus
 *   (voice_menu.c);
 * - mutes the players muted from the scoreboard (voice_scoreboard.c), by name, whenever they are in voice;
 * - moves the mouths of the soldiers talking with their voice (voice_lips.c).
 * Everything runs on the game thread, after each step of the main loop (0x44abc0, detoured; before each Present if
 * another plugin hooked it first). The voice client of LANCommander.Voice's BF1942 Interposer plugin stands down
 * while this one runs (the mutex Local\bf1942patch_voice_<pid>). The local player:
 * IPlayerManager [0x97d76c], vtable +0x18 player by id, +0x20 local player; IPlayer vtable +0x18 name (std::string *),
 * +0x44 id, field +0xac team. */
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice.h"
#include "d3d.h"
#include "interposer/LANCommander.Interposer.Comms.Client.h"

#define FRAME_FN        0x44abc0u    /* the main loop step: thiscall(app) -> keep running */
#define PLAYER_MANAGER  0x97d76cu
#define PM_BY_ID        0x18
#define PM_LOCAL        0x20
#define PLAYER_NAME     0x18
#define PLAYER_ID       0x44
#define PLAYER_IS_AI    0x58         /* bool: the scoreboard's votes leave these players alone (0x6dfcb0) */
#define PLAYER_TEAM     0xac         /* int: 1 Axis, 2 Allies */

#define UPDATE_MS       100
#define RECONNECT_MS    5000
#define LEAVE_MS        5000         /* no local player this long: left the server */
#define SAVE_MS         1000         /* a slider stopped moving this long: saved */
#define TAIL_MS         1000         /* after a key is let go, the voice still sent is its (the release delay) */
#define SLOT_NEARBY     0
#define SLOT_RADIO      1

/* ---------------------------------------------------------------- the library */

/* the Interposer's voice exports: p_comms_<name> is interposer_comms_<name> */
#define COMMS_FUNCTIONS(X) \
    X(uint32_t, comms_get_api_version, (void)) \
    X(const char *, comms_get_version_string, (void)) \
    X(const char *, comms_result_to_string, (interposer_comms_result)) \
    X(void, comms_set_log_handler, (interposer_comms_log_fn, interposer_comms_log_level, void *)) \
    X(void, comms_client_config_init, (interposer_comms_client_config *)) \
    X(interposer_comms_result, comms_client_create, \
      (const interposer_comms_client_config *, interposer_comms_client **)) \
    X(void, comms_connect_info_init, (interposer_comms_connect_info *)) \
    X(interposer_comms_result, comms_client_connect, \
      (interposer_comms_client *, const interposer_comms_connect_info *)) \
    X(interposer_comms_result, comms_client_disconnect, (interposer_comms_client *)) \
    X(interposer_comms_connection_state, comms_client_get_state, (interposer_comms_client *)) \
    X(interposer_comms_result, comms_client_poll_event, (interposer_comms_client *, interposer_comms_client_event *)) \
    X(interposer_comms_result, comms_client_set_talk_targets, \
      (interposer_comms_client *, const interposer_comms_talk_target *, uint32_t)) \
    X(interposer_comms_result, comms_client_set_ptt_targets, \
      (interposer_comms_client *, uint32_t, const interposer_comms_talk_target *, uint32_t)) \
    X(interposer_comms_result, comms_client_set_push_to_talk, (interposer_comms_client *, uint32_t, int)) \
    X(interposer_comms_result, comms_client_get_group_info, \
      (interposer_comms_client *, interposer_comms_group_id, interposer_comms_client_group_info *)) \
    X(interposer_comms_result, comms_client_get_player_info, \
      (interposer_comms_client *, interposer_comms_player_id, interposer_comms_client_player_info *)) \
    X(interposer_comms_result, comms_client_list_players, \
      (interposer_comms_client *, interposer_comms_player_id *, uint32_t, uint32_t *)) \
    X(interposer_comms_result, comms_client_set_player_muted, \
      (interposer_comms_client *, interposer_comms_player_id, int)) \
    X(interposer_comms_result, comms_client_get_active_speakers, \
      (interposer_comms_client *, interposer_comms_speaker_info *, uint32_t, uint32_t *)) \
    X(interposer_comms_result, comms_client_get_transmit_state, \
      (interposer_comms_client *, interposer_comms_transmit_state *)) \
    X(interposer_comms_result, comms_client_set_input_gain, (interposer_comms_client *, float)) \
    X(interposer_comms_result, comms_client_set_master_volume, (interposer_comms_client *, float)) \
    X(interposer_comms_result, comms_client_set_transmit_mode, \
      (interposer_comms_client *, interposer_comms_transmit_mode)) \
    X(interposer_comms_result, comms_list_audio_devices, \
      (interposer_comms_device_type, interposer_comms_audio_device *, uint32_t, uint32_t *)) \
    X(interposer_comms_result, comms_client_set_capture_device, (interposer_comms_client *, const char *))

#define DECLARE(ret, name, args) static ret (INTERPOSER_CALL *p_##name) args;
COMMS_FUNCTIONS(DECLARE)

static HMODULE g_interposer;          /* the Interposer: the one that loaded the plugin build, else the patch itself */
static HMODULE g_dll;
static char g_dll_path[MAX_PATH];

void bf_voice_set_interposer(HMODULE interposer)
{
    g_interposer = interposer;
}

HMODULE bf_interposer(void)
{
    return g_interposer;
}

static int resolve(void)
{
#define RESOLVE(ret, name, args)     if (!(p_##name = (ret (INTERPOSER_CALL *) args)(void (*)(void))GetProcAddress(g_dll, "interposer_" #name))) {         bf_log("voice: interposer_%s is missing from %s", #name, g_dll_path);         return 0;     }
    COMMS_FUNCTIONS(RESOLVE)
    return 1;
}

/* the Interposer with the voice API this patch was built for; NULL without one. The game and the dedicated server
 * (voice_server_win.c) both use it. */
HMODULE bf_voice_library(void)
{
    static int tried;
    uint32_t (INTERPOSER_CALL *version)(void);
    const char *(INTERPOSER_CALL *version_string)(void);
    if (tried) {
        return g_dll;
    }
    tried = 1;
    if (!g_interposer) {
        bf_log("voice: no LANCommander Interposer, no voice chat");
        return NULL;
    }
    GetModuleFileNameA(g_interposer, g_dll_path, MAX_PATH);
    version = (uint32_t (INTERPOSER_CALL *)(void))(void (*)(void))GetProcAddress(g_interposer,
                                                                                 "interposer_comms_get_api_version");
    version_string = (const char *(INTERPOSER_CALL *)(void))(void (*)(void))GetProcAddress(g_interposer,
                                                                                          "interposer_comms_get_version_string");
    if (!version || !version_string || version() != INTERPOSER_COMMS_API_VERSION) {
        bf_log("voice: %s has voice API version %u, the patch needs %u; no voice chat", g_dll_path,
               version ? version() : 0, INTERPOSER_COMMS_API_VERSION);
        return NULL;
    }
    bf_log("voice: %s (Interposer %s)", g_dll_path, version_string());
    return g_dll = g_interposer;
}

static int load_library(void)
{
    return bf_voice_library() && resolve();
}

/* any of the library's threads (bf_log is) */
static void INTERPOSER_CALL voice_log(interposer_comms_log_level level, const char *message, void *user)
{
    static const char *const names[] = {"trace", "debug", "info", "warn", "error"};
    (void)user;
    bf_log("voice: [interposer %s] %s", level <= INTERPOSER_COMMS_LOG_ERROR ? names[level] : "?", message);
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
    bf_setting_keep_str(which == BF_VOICE_RADIO ? "radioKey" : "proximityKey", name[0] ? name : "none");
    bf_log("voice: %s key %s", which == BF_VOICE_RADIO ? "radio" : "proximity", name[0] ? name : "none");
}

/* a key option: empty = the default, "none" = no key */
static void load_key(int which, const char *name)
{
    lstrcpynA(g_keys[which], bf_setting_str(name), sizeof g_keys[0]);
    if (!g_keys[which][0]) {
        lstrcpynA(g_keys[which], bf_options[bf_option_find(name)].def_str, sizeof g_keys[0]);
    }
    if (!_stricmp(g_keys[which], "none")) {
        g_keys[which][0] = '\0';
    }
    g_vk[which] = g_keys[which][0] ? bf_voice_vk(g_keys[which]) : 0;
    if (g_keys[which][0] && !g_vk[which]) {
        bf_log("voice: bf1942patch.%s %s is not a keyboard key or mouse button", name, g_keys[which]);
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

int bf_voice_player(int id, char *name, int size)
{
    void *pm = *(void **)BF_ADDR(PLAYER_MANAGER), *player;
    const void *s;
    if (id < 0 || id > 255 || !object(pm)) {
        return 0;
    }
    player = ((void *(__attribute__((thiscall)) *)(void *, int))(*(void ***)pm)[PM_BY_ID / 4])(pm, id);
    if (!object(player) || player == vcall(pm, PM_LOCAL) || (BYTE)(uintptr_t)vcall(player, PLAYER_IS_AI)) {
        return 0;
    }
    s = vcall(player, PLAYER_NAME);
    if (!readable(s, BF_STRING_SIZE) || !readable(bf_string_chars(s), 1)) {
        return 0;
    }
    lstrcpynA(name, bf_string_chars(s), size);
    return name[0] != '\0';
}

/* ---------------------------------------------------------------- the client */

int bf_voice_volume = 100, bf_voice_mic = 100;

static interposer_comms_client *g_client;
static int g_failed;
static WORD g_port = INTERPOSER_COMMS_DEFAULT_PORT;     /* if the game server does not say */
static char g_password[128], g_fixed_host[256], g_join_host[256];
static WORD g_join_port;
static WORD g_query_ports[8];
static int g_n_query_ports;
unsigned char bf_voice_va[2];                /* voice activation: nearby, radio */
static int g_applied_va = -1, g_saved_va;
static int g_radio_tail;                     /* the voice being sent goes to the team */
static ULONGLONG g_key_up;                   /* a key was last held */
static interposer_comms_group_id g_team;
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

/* ---------------------------------------------------------------- the microphone */

/* bf1942patch.inputDevice: the capture device's id as the library lists it, "" = the system's default. The Controls
 * options' MICROPHONE row (voice_menu.c) steps through the default and every device listed at the time */
#define MAX_MICS        32
static char g_mic[256];
static interposer_comms_audio_device g_mics[MAX_MICS];

/* the capture devices now (none before the library is loaded) */
static uint32_t list_mics(void)
{
    uint32_t n = 0;
    if (!p_comms_list_audio_devices
        || p_comms_list_audio_devices(INTERPOSER_COMMS_DEVICE_CAPTURE, g_mics, MAX_MICS, &n) != INTERPOSER_COMMS_OK) {
        return 0;
    }
    return n < MAX_MICS ? n : MAX_MICS;
}

/* the chosen device's index in g_mics; -1 for the default (also when the chosen one is not plugged in) */
static int mic_index(uint32_t n)
{
    uint32_t i;
    for (i = 0; g_mic[0] && i < n; i++) {
        if (!strcmp(g_mics[i].id, g_mic)) {
            return (int)i;
        }
    }
    return -1;
}

void bf_voice_mic_text(wchar_t *out, int size)
{
    const int max = 30;              /* what the row's box holds */
    int i = mic_index(list_mics()), n;
    if (i < 0 || !MultiByteToWideChar(CP_UTF8, 0, g_mics[i].name, -1, out, size)) {
        lstrcpynW(out, L"DEFAULT", size);
        return;
    }
    n = lstrlenW(out);
    if (n > max && size > max) {
        lstrcpyW(out + max - 3, L"...");
        n = max;
    }
    CharUpperBuffW(out, (DWORD)n);
}

void bf_voice_next_mic(void)
{
    uint32_t n = list_mics();
    int i = mic_index(n) + 1;
    lstrcpynA(g_mic, i < (int)n ? g_mics[i].id : "", sizeof g_mic);
    bf_setting_keep_str("inputDevice", g_mic);
    if (g_client) {
        p_comms_client_set_capture_device(g_client, g_mic[0] ? g_mic : NULL);
    }
    bf_log("voice: microphone %s", g_mic[0] ? g_mics[i].name : "the default");
}

static int create(void)
{
    interposer_comms_client_config config;
    interposer_comms_talk_target nearby = {INTERPOSER_COMMS_TARGET_PROXIMITY, 0};
    int mic = mic_index(list_mics());
    p_comms_set_log_handler(voice_log, INTERPOSER_COMMS_LOG_INFO, NULL);
    p_comms_client_config_init(&config);
    config.transmit_mode = INTERPOSER_COMMS_TRANSMIT_PUSH_TO_TALK;   /* update_activation sets it */
    config.voice_activation.auto_threshold = 1;
    if (g_mic[0] && mic < 0) {
        bf_log("voice: the microphone of bf1942patch.inputDevice is not there, using the default");
    }
    config.capture_device_id = mic >= 0 ? g_mic : NULL;
    if (p_comms_client_create(&config, &g_client) != INTERPOSER_COMMS_OK) {
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
    interposer_comms_connect_info info;
    interposer_comms_result r;
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
    interposer_comms_connection_state state = p_comms_client_get_state(g_client);
    WORD game_port, voice_port;
    DWORD addr;
    int r;
    if (in_game) {
        g_seen = now;
    } else {
        bf_voice_query_cancel();
        if (state != INTERPOSER_COMMS_STATE_DISCONNECTED && now - g_seen > LEAVE_MS) {
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
    if (state != INTERPOSER_COMMS_STATE_DISCONNECTED || now - g_last_attempt < RECONNECT_MS) {
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

static void apply_mutes(void);

static void drain_events(void)
{
    interposer_comms_client_event e;
    while (p_comms_client_poll_event(g_client, &e) == INTERPOSER_COMMS_OK) {
        switch (e.type) {
        case INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_JOINED:
            apply_mutes();
            break;
        case INTERPOSER_COMMS_CLIENT_EVENT_STATE_CHANGED:
            bf_log("voice: %s%s%s", e.state == INTERPOSER_COMMS_STATE_CONNECTED ? "connected"
                                    : e.state == INTERPOSER_COMMS_STATE_DISCONNECTED ? "disconnected" : "connecting",
                   e.reason ? ": " : "", e.reason ? p_comms_result_to_string((interposer_comms_result)e.reason) : "");
            if (e.state == INTERPOSER_COMMS_STATE_CONNECTED) {
                apply_mutes();
            }
            if (e.state == INTERPOSER_COMMS_STATE_DISCONNECTED) {
                g_team = INTERPOSER_COMMS_INVALID_ID;
                if (e.reason == INTERPOSER_COMMS_ERR_AUTH_FAILED) {
                    g_claim_by_name = !g_claim_by_name;   /* no player with our id: try by name, and back */
                    g_last_attempt = 0;
                }
            }
            break;
        case INTERPOSER_COMMS_CLIENT_EVENT_GROUP_ADDED: {
            /* the server shows us our own team's group only: the radio talks to it */
            interposer_comms_client_group_info info;
            if (p_comms_client_get_group_info(g_client, e.group, &info) == INTERPOSER_COMMS_OK
                && info.type == INTERPOSER_COMMS_GROUP_SERVER && info.is_member) {
                interposer_comms_talk_target radio;
                radio.kind = INTERPOSER_COMMS_TARGET_GROUP;
                radio.id = e.group;
                g_team = e.group;
                p_comms_client_set_ptt_targets(g_client, SLOT_RADIO, &radio, 1);
                g_applied_va = -1;   /* the radio's voice activation needs the group */
                bf_log("voice: team radio %s", info.name);
            }
            break;
        }
        case INTERPOSER_COMMS_CLIENT_EVENT_GROUP_REMOVED:
            if (e.group == g_team) {
                g_team = INTERPOSER_COMMS_INVALID_ID;
                g_applied_va = -1;
            }
            break;
        case INTERPOSER_COMMS_CLIENT_EVENT_AUDIO_DEVICE_LOST:
            bf_log("voice: audio device lost: %s", e.text);
            break;
        case INTERPOSER_COMMS_CLIENT_EVENT_KICKED:
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

/* ---------------------------------------------------------------- muted players */

/* By the game's name (Windows-1252), kept with the profile as the buddies are. The buddy list is the console script
 * Settings/Profiles/<profile>/BuddyList.con, a game.addPlayerToBuddyListByName "<name>" line per buddy: rewritten
 * whole on every change (0x468740: the path through the app's write path 0x445c40, FileHandle open 0x583a20, write
 * 0x5839f0, close 0x583870), and run through the console whenever a profile loads (0x467c10, called at 0x48fc2d and
 * 0x4939f3: the read path 0x445ad0, the console [0x9a9420] vtable +0x20 run(path, 8 arguments, 0x9a9428)). The mutes
 * are Settings/Profiles/<profile>/VoiceMuteList.con next to it, a bf1942patch.voiceMute "<name>" line per player,
 * written and run the same way (both calls to 0x467c10 are redirected to run it after the buddies). saves.c moves
 * both to the save folder. */
#define MAX_MUTED       64
#define MUTE_FILE       "VoiceMuteList.con"
#define APP_READ_PATH   0x445ad0u    /* thiscall(app, std::string *out, std::string path) -> out */
#define APP_WRITE_PATH  0x445c40u
#define PROFILES        0xa5c630u    /* the profile manager: 0x69b790 thiscall -> the current profile's name */
#define PROFILE_NAME    0x69b790u
#define CONSOLE         0x9a9420u
#define CONSOLE_RUN     0x20
#define CONSOLE_CONTEXT 0x9a9428u
#define FH_CTOR         0x583840u
#define FH_DTOR         0x583850u
#define FH_CLOSE        0x583870u
#define FH_OPEN         0x583a20u    /* thiscall(fh, const std::string *path, 0, 1, 0) -> opened (to write) */
#define FH_WRITE        0x5839f0u
#define BUDDY_LOAD      0x467c10u
#define APP_PTR         0x971eacu
static const uint32_t BUDDY_LOAD_CALLS[] = {0x48fc2du, 0x4939f3u};

static char g_muted[MAX_MUTED][64];
static int g_n_muted;
static int g_loading;                /* the profile's file runs: its lines do not save it again */

typedef void *(__attribute__((thiscall)) *path_fn)(void *app, void *out, bf_msvc_string path);

/* where the profile's VoiceMuteList.con is read (or written): a std::string in out; 0 without a profile */
static int mute_path(uint32_t resolve, void *out)
{
    void *profiles = *(void **)BF_ADDR(PROFILES), *app = *(void **)BF_ADDR(APP_PTR);
    const void *profile;
    char rel[MAX_PATH];
    bf_msvc_string s;
    if (!profiles || !app) {
        return 0;
    }
    profile = ((const void *(__attribute__((thiscall)) *)(void *))BF_ADDR(PROFILE_NAME))(profiles);
    if (!readable(profile, BF_STRING_SIZE) || !bf_string_chars(profile)[0]) {
        return 0;
    }
    snprintf(rel, sizeof rel, "Settings/Profiles/%s/" MUTE_FILE, bf_string_chars(profile));
    if (!bf_string_init(&s, rel)) {
        return 0;
    }
    ((path_fn)BF_ADDR(resolve))(app, out, s);   /* destroys s */
    return 1;
}

static void save_mutes(void)
{
    typedef void (__attribute__((thiscall)) *fh_fn)(void *fh);
    typedef char (__attribute__((thiscall)) *open_fn)(void *fh, const void *path, int a, int b, int c);
    typedef void (__attribute__((thiscall)) *write_fn)(void *fh, const void *line);
    BYTE fh[64], path[BF_STRING_SIZE], line[BF_STRING_SIZE];
    char text[96];
    int i;
    if (g_loading || !mute_path(APP_WRITE_PATH, path)) {
        return;
    }
    ((fh_fn)BF_ADDR(FH_CTOR))(fh);
    if (((open_fn)BF_ADDR(FH_OPEN))(fh, path, 0, 1, 0)) {
        for (i = 0; i < g_n_muted; i++) {
            snprintf(text, sizeof text, "bf1942patch.voiceMute \"%s\"\n", g_muted[i]);
            if (bf_string_init(line, text)) {
                ((write_fn)BF_ADDR(FH_WRITE))(fh, line);
                bf_string_free(line);
            }
        }
        ((fh_fn)BF_ADDR(FH_CLOSE))(fh);
    } else {
        bf_log("voice: cannot write %s", bf_string_chars(path));
    }
    ((fh_fn)BF_ADDR(FH_DTOR))(fh);
    bf_string_free(path);
}

/* the profile's mutes, after its buddies */
static void __attribute__((thiscall)) hk_buddy_load(void *buddies)
{
    typedef void (__attribute__((thiscall)) *run_fn)(void *con, const void *path, void *a1, void *a2, void *a3,
                                                     void *a4, void *a5, void *a6, void *a7, void *a8, void *context);
    void *con = *(void **)BF_ADDR(CONSOLE);
    BYTE path[BF_STRING_SIZE], a[8][BF_STRING_SIZE];
    int i, n = 0;
    ((void (__attribute__((thiscall)) *)(void *))BF_ADDR(BUDDY_LOAD))(buddies);
    g_n_muted = 0;
    if (!object(con) || !mute_path(APP_READ_PATH, path)) {
        return;
    }
    while (n < 8 && bf_string_init(a[n], "")) {
        n++;
    }
    if (n == 8) {
        g_loading = 1;
        ((run_fn)(*(void ***)con)[CONSOLE_RUN / 4])(con, path, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                                                    BF_ADDR(CONSOLE_CONTEXT));
        g_loading = 0;
    }
    for (i = 0; i < n; i++) {
        bf_string_free(a[i]);
    }
    bf_string_free(path);
    bf_log("voice: %d muted player%s in the profile", g_n_muted, g_n_muted == 1 ? "" : "s");
    if (g_client) {
        apply_mutes();
    }
}

static int muted_index(const char *name)
{
    int i;
    for (i = 0; i < g_n_muted; i++) {
        if (!strcmp(g_muted[i], name)) {
            return i;
        }
    }
    return -1;
}

int bf_voice_muted(const char *name)
{
    return muted_index(name) >= 0;
}

/* every voice player muted or not as the list says */
static void apply_mutes(void)
{
    interposer_comms_player_id ids[64];
    interposer_comms_client_player_info info;
    char name[64];
    uint32_t n = 0, i;
    int m;
    if (p_comms_client_get_state(g_client) != INTERPOSER_COMMS_STATE_CONNECTED
        || p_comms_client_list_players(g_client, ids, 64, &n) != INTERPOSER_COMMS_OK) {
        return;
    }
    for (i = 0; i < n && i < 64; i++) {
        if (p_comms_client_get_player_info(g_client, ids[i], &info) != INTERPOSER_COMMS_OK) {
            continue;
        }
        hud_name(name, sizeof name, info.name);
        m = bf_voice_muted(name);
        if (m != (info.locally_muted != 0)) {
            p_comms_client_set_player_muted(g_client, ids[i], m);
            bf_log("voice: %s %s", name, m ? "muted" : "unmuted");
        }
    }
}

void bf_voice_set_muted(const char *name, int muted)
{
    int i = muted_index(name);
    if (!name[0] || muted == (i >= 0) || (muted && g_n_muted == MAX_MUTED)) {
        return;
    }
    if (muted) {
        lstrcpynA(g_muted[g_n_muted++], name, sizeof g_muted[0]);
    } else {
        memmove(g_muted[i], g_muted[i + 1], (size_t)(g_n_muted - i - 1) * sizeof g_muted[0]);
        g_n_muted--;
    }
    save_mutes();
    if (!g_loading) {
        bf_log("voice: %s %s", name, muted ? "muted" : "unmuted");
    }
    if (g_client && !g_loading) {
        apply_mutes();
    }
}

/* bf1942patch.voiceMute "<name>" / voiceUnmute "<name>" (the profile's VoiceMuteList.con, or typed) */
static void mute_cmd(const char *cmd, int argc, const char *const *argv, char *out, int size)
{
    char name[64];
    int i;
    name[0] = '\0';
    for (i = 0; i < argc; i++) {     /* an unquoted name with spaces comes in pieces */
        if (i) {
            strncat(name, " ", sizeof name - strlen(name) - 1);
        }
        strncat(name, argv[i], sizeof name - strlen(name) - 1);
    }
    bf_voice_set_muted(name, !strcmp(cmd, "voiceMute"));
    (void)out;
    (void)size;
}

static void install_mutes(void)
{
    size_t i;
    int ok = bf_console_add("voiceMute", BF_CONSOLE_METHOD, mute_cmd)
             && bf_console_add("voiceUnmute", BF_CONSOLE_METHOD, mute_cmd);
    for (i = 0; i < sizeof BUDDY_LOAD_CALLS / sizeof BUDDY_LOAD_CALLS[0]; i++) {
        ok &= bf_redirect_call(BUDDY_LOAD_CALLS[i], BUDDY_LOAD, hk_buddy_load);
    }
    if (!ok) {
        bf_log("voice: the buddy list code differs, muted players are not kept with the profile");
    }
}

int bf_voice_running(void)
{
    return g_client != NULL;
}

static void update_state(void)
{
    interposer_comms_speaker_info speakers[BF_VOICE_MAX_TALKERS];
    interposer_comms_transmit_state tx;
    bf_voice_mouth mouths[BF_VOICE_MAX_TALKERS + 1];
    uint32_t n = 0, i;
    int id = -1, n_mouths = 0;
    bf_voice_state *s = &g_state;
    s->connected = p_comms_client_get_state(g_client) == INTERPOSER_COMMS_STATE_CONNECTED;
    s->n = 0;
    s->talking = s->radio = 0;
    if (s->connected && !local_player(s->me, sizeof s->me, &id, &s->my_team)) {
        s->me[0] = 0;
        id = -1;
    }
    if (s->connected && p_comms_client_get_transmit_state(g_client, &tx) == INTERPOSER_COMMS_OK) {
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
        if (s->talking && id >= 0) {
            mouths[n_mouths].id = id;
            mouths[n_mouths++].level_db = tx.input_level_db;
        }
    }
    if (s->connected
        && p_comms_client_get_active_speakers(g_client, speakers, BF_VOICE_MAX_TALKERS, &n) == INTERPOSER_COMMS_OK) {
        for (i = 0; i < n && i < BF_VOICE_MAX_TALKERS; i++) {
            interposer_comms_client_player_info info;
            bf_voice_talker *t = &s->talkers[s->n];
            if (speakers[i].locally_muted
                || p_comms_client_get_player_info(g_client, speakers[i].player, &info) != INTERPOSER_COMMS_OK) {
                continue;
            }
            hud_name(t->name, sizeof t->name, info.name);
            t->team = info.game_player_id ? team_of((int)info.game_player_id - 1) : 0;   /* the bridge's ids are engine id + 1 */
            t->radio = speakers[i].kind == INTERPOSER_COMMS_TARGET_GROUP;
            s->n++;
            if (info.game_player_id) {
                mouths[n_mouths].id = (int)info.game_player_id - 1;
                mouths[n_mouths++].level_db = speakers[i].level_db;
            }
        }
    }
    bf_voice_hud_update(s);
    bf_voice_lips_update(mouths, n_mouths);
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
            bf_setting_keep_int("voiceVolume", g_saved_volume = v);
        }
        if (m != g_saved_mic) {
            bf_setting_keep_int("micSensitivity", g_saved_mic = m);
        }
        bf_log("voice: voice volume %d%%, microphone %d%%", v, m);
    }
}

/* the voice activation check boxes: voice activation sends to the default targets (nearby, the team or both) whenever
 * the microphone hears speech; the keys still send to their own targets. Applied at once, kept in the player's bf1942patch.con */
static void update_activation(void)
{
    interposer_comms_talk_target targets[2];
    int va = (bf_voice_va[0] ? 1 : 0) | (bf_voice_va[1] ? 2 : 0), n = 0;
    if (va == g_applied_va) {
        return;
    }
    if (va & 1) {
        targets[n].kind = INTERPOSER_COMMS_TARGET_PROXIMITY;
        targets[n++].id = 0;
    }
    if ((va & 2) && g_team) {
        targets[n].kind = INTERPOSER_COMMS_TARGET_GROUP;
        targets[n++].id = g_team;
    }
    if (n) {
        p_comms_client_set_talk_targets(g_client, targets, (uint32_t)n);
    }
    p_comms_client_set_transmit_mode(g_client, n ? INTERPOSER_COMMS_TRANSMIT_VOICE_ACTIVITY
                                                 : INTERPOSER_COMMS_TRANSMIT_PUSH_TO_TALK);
    if (va != g_saved_va) {
        bf_setting_keep_int("voiceActivation", va & 1);
        bf_setting_keep_int("radioActivation", (va & 2) >> 1);
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
    if (!g_client && !(load_library() && create())) {   /* the first step */
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

/* ---------------------------------------------------------------- the options set in the console */

/* the volumes (bound to bf_voice_volume / bf_voice_mic): update_volumes applies them with its next update */
static void volumes_changed(void)
{
}

static void activation_changed(void)
{
    bf_voice_va[0] = (unsigned char)(bf_setting_int("voiceActivation") != 0);
    bf_voice_va[1] = (unsigned char)(bf_setting_int("radioActivation") != 0);
}

static void keys_changed(void)
{
    load_key(BF_VOICE_NEARBY, "proximityKey");
    load_key(BF_VOICE_RADIO, "radioKey");
}

static void mic_changed(void)
{
    lstrcpynA(g_mic, bf_setting_str("inputDevice"), sizeof g_mic);
    if (g_client) {
        p_comms_client_set_capture_device(g_client, g_mic[0] ? g_mic : NULL);
    }
}

/* ---------------------------------------------------------------- hooks */

static DWORD (__attribute__((thiscall)) *g_frame)(void *app);

static DWORD __attribute__((thiscall)) hk_frame(void *app)
{
    DWORD r = g_frame(app);
    tick();
    return r;
}

/* when another plugin hooked the main loop step first (LANCommander.Voice's BF1942 plugin does): each rendered frame */
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
    if (!bf_setting_int("voiceChat")) {
        bf_log("voice: voiceChat 0, no voice chat");
        return;
    }
    if (!bf_voice_library()) {
        return;
    }
    g_port = (WORD)bf_setting_int("voicePort");
    lstrcpynA(g_password, bf_setting_str("voicePassword"), sizeof g_password);
    lstrcpynA(g_fixed_host, bf_setting_str("voiceServer"), sizeof g_fixed_host);
    lstrcpynA(text, bf_setting_str("voiceQueryPorts"), sizeof text);
    g_n_query_ports = bf_voice_ports(text, g_query_ports, 8);
    activation_changed();
    g_saved_va = bf_voice_va[0] | bf_voice_va[1] << 1;
    bf_setting_bind("voiceVolume", &bf_voice_volume);   /* the Sound sliders and the console share them */
    bf_setting_bind("micSensitivity", &bf_voice_mic);
    g_saved_volume = bf_voice_volume;
    g_saved_mic = bf_voice_mic;
    mic_changed();
    keys_changed();
    bf_setting_watch("voiceActivation", activation_changed);
    bf_setting_watch("radioActivation", activation_changed);
    bf_setting_watch("voiceVolume", volumes_changed);
    bf_setting_watch("micSensitivity", volumes_changed);
    bf_setting_watch("proximityKey", keys_changed);
    bf_setting_watch("radioKey", keys_changed);
    bf_setting_watch("inputDevice", mic_changed);
    install_mutes();
    bf_voice_join_server(GetCommandLineA(), g_join_host, sizeof g_join_host, &g_join_port);

    g_frame = (DWORD (__attribute__((thiscall)) *)(void *))bf_detour(FRAME_FN, frame_head, sizeof frame_head, hk_frame);
    if (!g_frame) {
        bf_log("voice: the main loop step (%08x) is hooked already, voice runs with each rendered frame", FRAME_FN);
        bf_d3d_install();
        bf_d3d_on_present(present_tick);
    }
    real_sendto = bf_hook_import("WSOCK32.dll", "sendto", hk_sendto);
    if (!real_sendto) {
        bf_log("voice: the game's sendto is not hooked; set bf1942patch.voiceServer if the game joins without restarting");
    }
    /* LANCommander.Voice's BF1942 Interposer plugin stands down while this one runs the voice client */
    snprintf(name, sizeof name, "Local\\bf1942patch_voice_%lu", GetCurrentProcessId());
    CreateMutexA(NULL, FALSE, name);
    bf_voice_hud_install();
    bf_voice_menu_install();
    bf_voice_scoreboard_install();
    bf_voice_lips_install();
}
