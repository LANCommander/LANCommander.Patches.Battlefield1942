/* Voice chat: what voice.c, voice_net.c, voice_hud.c and voice_menu.c share. */
#ifndef BF_VOICE_H
#define BF_VOICE_H
#include "bf1942patch.h"

/* ---------------------------------------------------------------- voice.c */

/* what the HUD shows, updated on the game thread every frame */
#define BF_VOICE_MAX_TALKERS 12
typedef struct {
    char name[64];                   /* Windows-1252, as the game's */
    int team;                        /* 1 Axis, 2 Allies, 0 not known */
    int radio;                       /* on the team radio (else heard nearby) */
} bf_voice_talker;

typedef struct {
    int connected;
    int talking;                     /* the local player's voice is being sent */
    int radio;                       /* to the team */
    char me[64];                     /* the local player, and their team */
    int my_team;
    int n;
    bf_voice_talker talkers[BF_VOICE_MAX_TALKERS];
} bf_voice_state;

const bf_voice_state *bf_voice_now(void);

/* the Interposer with the voice API, loaded (on the game thread) the first time; NULL without one */
HMODULE bf_voice_library(void);

/* the push-to-talk keys, as the game names them (IDKey_Capital, Mouse_IDButton_3) */
enum { BF_VOICE_NEARBY, BF_VOICE_RADIO };
const char *bf_voice_key(int which);
void bf_voice_set_key(int which, const char *name);   /* "" = none; kept in the player's bf1942patch.con */
int bf_voice_vk(const char *name);   /* the virtual key that is held for a game key name, 0 if there is none */

/* the Sound options sliders, in percent, and the Controls options' voice activation check boxes (nearby, radio):
 * bound to the menus, read every frame */
extern int bf_voice_volume, bf_voice_mic;
extern unsigned char bf_voice_va[2];

/* the microphone (bf1942patch.inputDevice): its name as the Controls options show it, and the next one (after the last
 * one the default), applied at once and kept in the player's bf1942patch.con; on the game thread */
void bf_voice_mic_text(wchar_t *out, int size);
void bf_voice_next_mic(void);

/* muting other players, by the game's name (kept with the profile, VoiceMuteList.con) */
int bf_voice_running(void);          /* the voice client is up */
int bf_voice_player(int id, char *name, int size);   /* the name of the game's player id, if one that can be muted */
int bf_voice_muted(const char *name);
void bf_voice_set_muted(const char *name, int muted);

/* ---------------------------------------------------------------- voice_net.c */

/* "+joinServer <host>[:<port>]" in the command line (how the game relaunches itself to join from the browser) */
int bf_voice_join_server(const char *cmdline, char *host, int size, WORD *port);
int bf_voice_ports(const char *text, WORD *ports, int max);   /* "22000,23000" */

/* where the game sends its own packets (the game server, when it joined without relaunching); host byte order */
void bf_voice_traffic(DWORD addr, WORD port);
int bf_voice_traffic_server(DWORD *addr, WORD *port);

/* asks a game server for the voice port it advertises (\commsport\ in its \info\ reply) without blocking: the host
 * name is resolved on a thread of its own */
int bf_voice_query_start(const char *host, const WORD *ports, int n, WORD game_port);
int bf_voice_query_poll(WORD *voice_port);    /* 0 pending, 1 found, -1 not (the query is over) */
void bf_voice_query_cancel(void);
int bf_voice_query_active(void);
const char *bf_voice_query_address(void);     /* the host's address once resolved, else "" */

/* the same query, blocking (up to 1.5 s, and the host name's lookup): the reply (\k\v pairs) of the query port whose
 * \hostport\ is game_port (any, for 0) in reply. 1 found, 0 not, -1 the host cannot be resolved */
int bf_query_info(const char *host, const WORD *ports, int n, WORD game_port, char *reply, int size);
int bf_query_value(const char *packet, int len, const char *key, char *value, int size);   /* \key\'s value */

/* ---------------------------------------------------------------- voice.c, for fastdl.c */

HMODULE bf_interposer_in_process(void);   /* any LANCommander Interposer already in the process (any version), or NULL */
const char *bf_interposer_file(void);     /* the file bf_voice_library loads when none is in the process, else "" */

/* ---------------------------------------------------------------- voice_hud.c, voice_menu.c, voice_scoreboard.c */

void bf_voice_hud_install(void);
void bf_voice_hud_update(const bf_voice_state *s);   /* every frame, on the game thread */
void bf_voice_menu_install(void);
void bf_voice_menu_tick(void);
void bf_voice_scoreboard_install(void);
#endif
