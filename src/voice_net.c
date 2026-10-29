/* Voice chat: finding the voice server of the game server the game plays on.
 *
 * - The game server: "+joinServer <host>[:<port>]" in the command line (joining from the server browser relaunches
 *   the game with it, built at 0x4933d0 / 0x69f8d0 / 0x6b4130), else the destination of most of the game's own
 *   packets (WSOCK32!sendto, see voice.c) when it joined without relaunching.
 * - Its voice port: the LANCommander Comms server plugin adds \commsport\<port> to the game server's GameSpy
 *   query replies. "\info\" goes to each query port (22000 LAN, 23000 internet); a reply whose \hostport\ is not
 *   the game port is another server on the same host. Host names are resolved on a thread of their own, so the
 *   game never waits on DNS.
 * - bf_query_info is the same query, blocking (fastdl.c asks it as the game joins, while it loads anyway). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice.h"

/* ---------------------------------------------------------------- the command line, port lists */

int bf_voice_join_server(const char *cmdline, char *host, int size, WORD *port)
{
    static const char key[] = "+joinserver";
    const char *p = cmdline, *colon;
    char token[256];
    int n = 0, quote = 0;
    for (;;) {
        p = strstr(p, "+");
        if (!p) {
            return 0;
        }
        if (!_strnicmp(p, key, sizeof key - 1) && (p[sizeof key - 1] == ' ' || p[sizeof key - 1] == '\t')) {
            break;
        }
        p++;
    }
    p += sizeof key - 1;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '"' || *p == '\'') {
        quote = *p++;
    }
    while (*p && n < (int)sizeof token - 1 && (quote ? *p != quote : *p != ' ' && *p != '\t')) {
        token[n++] = *p++;
    }
    token[n] = '\0';
    if (!n || token[0] == '+') {
        return 0;
    }
    *port = 0;
    if ((colon = strrchr(token, ':')) != NULL) {
        int v = atoi(colon + 1);
        if (v <= 0 || v > 65535) {
            return 0;
        }
        *port = (WORD)v;
        token[colon - token] = '\0';
    }
    if (!token[0]) {
        return 0;
    }
    lstrcpynA(host, token, size);
    return 1;
}

int bf_voice_ports(const char *text, WORD *ports, int max)
{
    int n = 0;
    while (*text && n < max) {
        char *end;
        long v = strtol(text, &end, 10);
        if (end != text && v > 0 && v <= 65535 && (!*end || *end == ',' || *end == ' ' || *end == '\t')) {
            ports[n++] = (WORD)v;
        }
        text = end != text ? end : text + 1;
        while (*text == ',' || *text == ' ' || *text == '\t') {
            text++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------- the game's own traffic */

#define SLOTS      8
#define WINDOW_MS  5000
#define MIN_PACKETS 20

static struct { DWORD addr; WORD port; DWORD packets; ULONGLONG start, last; } g_slots[SLOTS];
static CRITICAL_SECTION g_traffic_cs;
static LONG g_traffic_init;

static void traffic_lock(void)
{
    if (InterlockedCompareExchange(&g_traffic_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_traffic_cs);
        InterlockedExchange(&g_traffic_init, 2);
    }
    while (g_traffic_init != 2) {
        Sleep(0);
    }
    EnterCriticalSection(&g_traffic_cs);
}

/* any thread (the game sends from more than one) */
void bf_voice_traffic(DWORD addr, WORD port)
{
    ULONGLONG now = GetTickCount64();
    int i, hit = -1, oldest = 0;
    if (!addr || addr == 0xffffffffu || (addr >> 28) == 0xe) {
        return;                      /* broadcasts (LAN server search) and multicast */
    }
    traffic_lock();
    for (i = 0; i < SLOTS; i++) {
        if (g_slots[i].packets && g_slots[i].addr == addr && g_slots[i].port == port) {
            hit = i;
        }
        if (g_slots[i].last < g_slots[oldest].last) {
            oldest = i;
        }
    }
    if (hit < 0) {
        hit = oldest;
        g_slots[hit].addr = addr;
        g_slots[hit].port = port;
        g_slots[hit].packets = 0;
        g_slots[hit].start = now;
    }
    if (now - g_slots[hit].start > WINDOW_MS) {
        g_slots[hit].start = now;
        g_slots[hit].packets = 0;
    }
    g_slots[hit].packets++;
    g_slots[hit].last = now;
    LeaveCriticalSection(&g_traffic_cs);
}

/* the destination of most packets lately, if it clearly is the one (well ahead of server browser pings) */
int bf_voice_traffic_server(DWORD *addr, WORD *port)
{
    ULONGLONG now = GetTickCount64();
    int i, best = -1;
    DWORD second = 0;
    traffic_lock();
    for (i = 0; i < SLOTS; i++) {
        if (!g_slots[i].packets || now - g_slots[i].last > WINDOW_MS) {
            continue;
        }
        if (best < 0 || g_slots[i].packets > g_slots[best].packets) {
            if (best >= 0 && g_slots[best].packets > second) {
                second = g_slots[best].packets;
            }
            best = i;
        } else if (g_slots[i].packets > second) {
            second = g_slots[i].packets;
        }
    }
    if (best >= 0 && g_slots[best].packets >= MIN_PACKETS && g_slots[best].packets >= second * 3) {
        *addr = g_slots[best].addr;
        *port = g_slots[best].port;
    } else {
        best = -1;
    }
    LeaveCriticalSection(&g_traffic_cs);
    return best >= 0;
}

/* ---------------------------------------------------------------- the voice port query */

#define QUERY_TIMEOUT_MS   1500
#define RESOLVE_TIMEOUT_MS 5000
#define MAX_QUERY_PORTS    8

/* a host name lookup, shared with its thread: a cancelled query just lets go of it */
typedef struct {
    LONG refs;
    volatile LONG done;
    int ok;
    DWORD addr;                      /* network byte order */
    char host[256];
} resolution;

static struct {
    SOCKET s;
    int wsa;
    resolution *r;
    WORD ports[MAX_QUERY_PORTS];
    struct { WORD host_port, voice_port; int final; } replies[MAX_QUERY_PORTS];
    int n;
    WORD game_port;
    DWORD addr;                      /* network byte order */
    char addr_text[16];
    ULONGLONG deadline;
} g_q = {.s = INVALID_SOCKET};

static void release(resolution *r)
{
    if (r && InterlockedDecrement(&r->refs) == 0) {
        free(r);
    }
}

static DWORD WINAPI resolve_thread(void *param)
{
    resolution *r = param;
    struct addrinfo hints, *res = NULL;
    WSADATA wsa;
    HMODULE self;
    int started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    r->ok = started && getaddrinfo(r->host, NULL, &hints, &res) == 0 && res;
    r->addr = r->ok ? ((const struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr : 0;
    if (res) {
        freeaddrinfo(res);
    }
    if (started) {
        WSACleanup();
    }
    InterlockedExchange(&r->done, 1);
    release(r);
    /* the thread holds a reference on this module (taken when it started) */
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)resolve_thread, &self)) {
        FreeLibraryAndExitThread(self, 0);
    }
    return 0;
}

static void close_query(void)
{
    if (g_q.s != INVALID_SOCKET) {
        closesocket(g_q.s);
        g_q.s = INVALID_SOCKET;
    }
    if (g_q.wsa) {
        WSACleanup();
        g_q.wsa = 0;
    }
    g_q.n = 0;
}

void bf_voice_query_cancel(void)
{
    release(g_q.r);
    g_q.r = NULL;
    close_query();
}

int bf_voice_query_active(void)
{
    return g_q.s != INVALID_SOCKET || g_q.r != NULL;
}

const char *bf_voice_query_address(void)
{
    return g_q.addr_text;
}

static int send_queries(DWORD addr)
{
    static const char info[] = "\\info\\";
    WORD ports[MAX_QUERY_PORTS];
    WSADATA wsa;
    u_long nonblocking = 1;
    struct in_addr in;
    int i, n = g_q.n;
    in.s_addr = addr;
    lstrcpynA(g_q.addr_text, inet_ntoa(in), sizeof g_q.addr_text);
    memcpy(ports, g_q.ports, sizeof ports);
    g_q.n = 0;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 0;
    }
    g_q.wsa = 1;
    if ((g_q.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        close_query();
        return 0;
    }
    ioctlsocket(g_q.s, FIONBIO, &nonblocking);
    g_q.addr = addr;
    g_q.deadline = GetTickCount64() + QUERY_TIMEOUT_MS;
    for (i = 0; i < n; i++) {
        struct sockaddr_in to;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_addr = in;
        to.sin_port = htons(ports[i]);
        if (sendto(g_q.s, info, sizeof info - 1, 0, (const struct sockaddr *)&to, sizeof to) != SOCKET_ERROR) {
            g_q.ports[g_q.n] = ports[i];
            memset(&g_q.replies[g_q.n], 0, sizeof g_q.replies[0]);
            g_q.n++;
        }
    }
    if (!g_q.n) {
        close_query();
        return 0;
    }
    return 1;
}

int bf_voice_query_start(const char *host, const WORD *ports, int n, WORD game_port)
{
    unsigned long addr;
    resolution *r;
    HMODULE self;
    HANDLE thread;
    bf_voice_query_cancel();
    g_q.addr_text[0] = '\0';
    if (n <= 0 || !host[0]) {
        return 0;
    }
    g_q.n = n < MAX_QUERY_PORTS ? n : MAX_QUERY_PORTS;
    memcpy(g_q.ports, ports, g_q.n * sizeof ports[0]);
    g_q.game_port = game_port;
    if ((addr = inet_addr(host)) != INADDR_NONE) {
        return send_queries((DWORD)addr);
    }
    /* a host name: resolved on a thread, the queries go out from a later poll */
    if (!(r = calloc(1, sizeof *r))) {
        return 0;
    }
    lstrcpynA(r->host, host, sizeof r->host);
    r->refs = 2;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)resolve_thread, &self)) {
        free(r);
        return 0;
    }
    if (!(thread = CreateThread(NULL, 0, resolve_thread, r, 0, NULL))) {
        FreeLibrary(self);
        free(r);
        return 0;
    }
    CloseHandle(thread);
    g_q.r = r;
    g_q.deadline = GetTickCount64() + RESOLVE_TIMEOUT_MS;
    return 1;
}

/* the value of \key\ in a "\k\v\k\v..." reply */
int bf_query_value(const char *packet, int len, const char *key, char *value, int size)
{
    int pos = 0, kl = (int)strlen(key);
    if (len <= 0 || packet[0] != '\\') {
        return 0;
    }
    while (pos < len) {
        const char *k = packet + pos + 1, *v, *e;
        const char *key_end = memchr(k, '\\', len - pos - 1);
        int vl;
        if (!key_end) {
            return 0;
        }
        v = key_end + 1;
        e = memchr(v, '\\', packet + len - v);
        if (!e) {
            e = packet + len;
        }
        if (key_end - k == kl && !memcmp(k, key, kl)) {
            vl = (int)(e - v) < size - 1 ? (int)(e - v) : size - 1;
            memcpy(value, v, vl);
            value[vl] = '\0';
            return 1;
        }
        pos = (int)(e - packet);
    }
    return 0;
}

static WORD parse_port(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    return end != s && !*end && v > 0 && v <= 65535 ? (WORD)v : 0;
}

int bf_voice_query_poll(WORD *voice_port)
{
    ULONGLONG now = GetTickCount64();
    char buf[2048], value[64];
    int i, k, all = 1;
    if (g_q.r) {
        resolution *r = g_q.r;
        if (!r->done) {
            if (now < g_q.deadline) {
                return 0;
            }
            bf_voice_query_cancel();
            return -1;
        }
        g_q.r = NULL;
        if (!r->ok || !send_queries(r->addr)) {
            release(r);
            bf_voice_query_cancel();
            return -1;
        }
        release(r);
        return 0;                    /* the replies come on a later poll */
    }
    if (!bf_voice_query_active()) {
        return -1;
    }
    for (i = 0; i < 64; i++) {       /* bounded: a flood cannot stall the frame */
        struct sockaddr_in from;
        int flen = sizeof from, n = recvfrom(g_q.s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &flen);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) {
                continue;            /* a query port nobody listens on (ICMP port unreachable) */
            }
            break;
        }
        buf[n] = '\0';
        if (from.sin_addr.s_addr != g_q.addr) {
            continue;
        }
        for (k = 0; k < g_q.n; k++) {
            if (g_q.ports[k] != ntohs(from.sin_port)) {
                continue;
            }
            if (bf_query_value(buf, n, "hostport", value, sizeof value)) {
                g_q.replies[k].host_port = parse_port(value);
            }
            if (bf_query_value(buf, n, "commsport", value, sizeof value)) {
                g_q.replies[k].voice_port = parse_port(value);
            }
            if (bf_query_value(buf, n, "final", value, sizeof value)) {
                g_q.replies[k].final = 1;
            }
        }
    }
    for (k = 0; k < g_q.n; k++) {
        if (!g_q.replies[k].final) {
            all = 0;
            continue;
        }
        /* a reply without \hostport\ cannot be checked: taken rather than ignored */
        if (g_q.replies[k].voice_port && (!g_q.game_port || !g_q.replies[k].host_port
                                          || g_q.replies[k].host_port == g_q.game_port)) {
            *voice_port = g_q.replies[k].voice_port;
            close_query();
            return 1;
        }
    }
    if (all || now >= g_q.deadline) {
        close_query();
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the blocking query (fastdl.c) */

int bf_query_info(const char *host, const WORD *ports, int n, WORD game_port, char *reply, int size)
{
    static const char info[] = "\\info\\";
    struct { char text[2048]; int len, final; } *got;
    struct addrinfo hints, *res = NULL;
    struct in_addr in;
    WSADATA wsa;
    SOCKET s;
    ULONGLONG deadline;
    char buf[2048], value[16];
    int i, k, sent = 0, found = 0, done = 0;
    if (n <= 0 || !host[0] || WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return -1;
    }
    n = n < MAX_QUERY_PORTS ? n : MAX_QUERY_PORTS;
    if ((in.s_addr = inet_addr(host)) == INADDR_NONE) {
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            WSACleanup();
            return -1;
        }
        in = ((const struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    if (!(got = calloc(n, sizeof *got)) || (s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        free(got);
        WSACleanup();
        return -1;
    }
    for (i = 0; i < n; i++) {
        struct sockaddr_in to;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_addr = in;
        to.sin_port = htons(ports[i]);
        if (sendto(s, info, sizeof info - 1, 0, (const struct sockaddr *)&to, sizeof to) != SOCKET_ERROR) {
            sent++;
        } else {
            got[i].final = 1;            /* nothing to wait for */
            done++;
        }
    }
    deadline = GetTickCount64() + QUERY_TIMEOUT_MS;
    while (sent && !found && done < n) {
        ULONGLONG now = GetTickCount64();
        struct timeval tv;
        struct sockaddr_in from;
        fd_set readable;
        int flen = sizeof from, len;
        if (now >= deadline) {
            break;
        }
        tv.tv_sec = (long)((deadline - now) / 1000);
        tv.tv_usec = (long)((deadline - now) % 1000) * 1000;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        if (select(0, &readable, NULL, NULL, &tv) <= 0) {
            break;
        }
        if ((len = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &flen)) == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) {
                continue;                /* a query port nobody listens on (ICMP port unreachable) */
            }
            break;
        }
        buf[len] = '\0';
        if (from.sin_addr.s_addr != in.s_addr) {
            continue;
        }
        for (k = 0; k < n; k++) {
            if (ports[k] != ntohs(from.sin_port) || got[k].final) {
                continue;
            }
            /* a reply may come in more than one packet: they add up to the \final\ */
            if (got[k].len + len < (int)sizeof got[k].text) {
                memcpy(got[k].text + got[k].len, buf, (size_t)len + 1);
                got[k].len += len;
            }
            if (!bf_query_value(buf, len, "final", value, sizeof value)) {
                continue;
            }
            got[k].final = 1;
            done++;
            /* a reply without \hostport\ cannot be checked: taken rather than ignored */
            if (!game_port || !bf_query_value(got[k].text, got[k].len, "hostport", value, sizeof value)
                || parse_port(value) == game_port) {
                lstrcpynA(reply, got[k].text, size);
                found = 1;
            }
        }
    }
    closesocket(s);
    free(got);
    WSACleanup();
    return found;
}
