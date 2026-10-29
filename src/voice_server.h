/* The dedicated server's voice (voice_server.c): what it needs from the platform and what it gives it. Plain C with
 * no Windows headers, so the Linux server build (linux/server_linux.c) compiles it too. */
#ifndef BF_VOICE_SERVER_H
#define BF_VOICE_SERVER_H
#include <stdint.h>
#include "settings.h"                /* the options: bf_setting_int / bf_setting_str */

/* a player of the game server, as the platform reads it */
typedef struct {
    int id;                          /* engine id (0..255); the voice server's game id is id + 1 */
    char name[64];
    int team;                        /* 1 Axis, 2 Allies, 0 none */
    int ai;
    int alive;
    int has_position;
    float position[3];               /* world space, metres; Y up, left-handed */
} bf_server_player;

/* ---------------------------------------------------------------- the platform's (voice_server_win.c, server_linux.c) */

int bf_server_read_players(bf_server_player *out, int capacity);   /* the count, -1 if the game cannot be read now */
void *bf_server_symbol(const char *name);   /* an export of the Interposer (its voice API), loaded the first time */
uint64_t bf_server_now_ms(void);
void bf_log(const char *fmt, ...);

/* ---------------------------------------------------------------- voice_server.c */

void bf_server_tick(void);           /* on the game thread, once a frame */
void bf_server_stop(void);           /* the game quits: the voice server stops before exit runs */
/* the FastDL server the query replies advertise, from the options (at start, before the replies are sent): 1 if there
 * is one */
int bf_server_fastdl_init(void);
/* the game's query reply with \commsport\<voice port> and the FastDL server's \fastdlport\ (\fastdlhost\,
 * \fastdlpath\) before its \final\ (out holds size bytes): its length, 0 if it is not a reply to change (not one, has
 * the keys already, nothing to add, no room). Any thread. */
int bf_server_query_reply(const char *packet, int length, char *out, int size);
#endif
