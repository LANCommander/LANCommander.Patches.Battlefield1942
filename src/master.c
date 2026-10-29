/* The server list and server reporting through OpenSpy (https://openspy.net), now that GameSpy is gone.
 *
 * 1.61 talks to the master server with the GameSpy SDK of its day: servers send heartbeats (UDP 27900) to the host
 * named at 0x957c30 (bound and resolved in 0x477e00 / 0x477da0), the server browser connects (TCP 28900) to the
 * host named at 0x957df8 (0x47f460), and the version check fetches http://motd.gamespy.com/... (0x476fa9). The
 * retail exe names master.gamespy.com; the community builds patched in master.bf1942.sk or master.bf1942.org.
 * Every name goes through WSOCK32!gethostbyname (the exe's IAT slot 0x8c3680, also behind the thunk at 0x8050f6),
 * so that one import is hooked:
 *   master.gamespy.com, master.bf1942.sk, master.bf1942.org   -> bf1942patch.masterServer (master.openspy.net)
 *   anything else under gamespy.com                           -> the same name under openspy.net
 * OpenSpy speaks the old GameSpy protocols and knows the game (bfield1942), so nothing else changes. */
#include <stdio.h>
#include <string.h>
#include <winsock.h>
#include "bf1942patch.h"

#define LIST_HOST   0x957df8u        /* the exe's master server names: the server list, server reports */
#define REPORT_HOST 0x957c30u

static struct hostent *(WINAPI *real_gethostbyname)(const char *name);
static char g_master[128];

static const char *const MASTERS[] = {"master.gamespy.com", "master.bf1942.sk", "master.bf1942.org"};

/* name with ".gamespy.com" at its end replaced by ".openspy.net" into out; 0 if it is not a GameSpy name */
static int openspy_name(const char *name, char *out, size_t size)
{
    size_t n = strlen(name), tail = strlen("gamespy.com");
    if (n < tail || _stricmp(name + n - tail, "gamespy.com") || (n > tail && name[n - tail - 1] != '.')) {
        return 0;
    }
    if (n >= size) {
        return 0;
    }
    memcpy(out, name, n - tail);
    strcpy(out + n - tail, "openspy.net");
    return 1;
}

static struct hostent *WINAPI hk_gethostbyname(const char *name)
{
    char mapped[256];
    const char *use = name;
    size_t i;
    if (name) {
        for (i = 0; i < sizeof MASTERS / sizeof MASTERS[0]; i++) {
            if (!_stricmp(name, MASTERS[i])) {
                use = g_master;
            }
        }
        if (use == name && openspy_name(name, mapped, sizeof mapped)) {
            use = mapped;
        }
        if (use != name) {
            struct hostent *h = real_gethostbyname(use);
            bf_log("master: %s -> %s (%s)", name, use, h ? inet_ntoa(*(struct in_addr *)h->h_addr_list[0]) : "not found");
            return h;
        }
    }
    return real_gethostbyname(name);
}

void bf_master_install(void)
{
    if (!bf_setting_int("openSpy")) {
        bf_log("master: openSpy 0, the game's own master server names are used");
        return;
    }
    lstrcpynA(g_master, bf_setting_str("masterServer"), sizeof g_master);
    if (!g_master[0]) {
        strcpy(g_master, "master.openspy.net");
    }
    real_gethostbyname = bf_hook_import("WSOCK32.dll", "gethostbyname", hk_gethostbyname);
    if (!real_gethostbyname) {
        bf_log("master: gethostbyname is not imported, the master server stays as it is");
        return;
    }
    bf_log("master: server list and server reports go to %s (the exe names %s / %s)", g_master,
           (const char *)BF_ADDR(LIST_HOST), (const char *)BF_ADDR(REPORT_HOST));
}
