/* game.fastDl: the maps the game lacks, downloaded from the game server's FastDL server as the game joins it,
 * by the LANCommander Interposer's FastDL.
 *
 * - The game server (running this patch, voice_server.c) says where its FastDL server is in its GameSpy query
 *   replies: \fastdlport\, with \fastdlhost\ (else the game server's own host) and \fastdlpath\ (else "/").
 * - Joining, the game gets the server's map (message 0x36 to the client's message handler 0x4933d0, its case at
 *   0x49478e by the jump table 0x4956c8) and looks for the map's archive in each mod's Archives folder (the file
 *   manager [0x9a2380] vtable +0x10) before it loads the map, or before it restarts itself for the server's mod. The
 *   browser, +joinServer and the console all join that way. The handler's std::string at +0x22c is the server
 *   ("ip:port", what +joinServer gets). Before that look, this module asks the server's query ports
 *   (game.queryPorts) for its FastDL server and hands the URL to the Interposer
 *   (InterposerSetFastDLBaseUrl); the Interposer's file hooks then fetch an archive the game does not have. The
 *   server's FastDL server always wins: over game.fastDlUrl (used only when the server names none) and over
 *   the Interposer's own FastDL.BaseUrl.
 * - The Interposer's Config.yml decides the rest (FastDL.Enabled, FastDL.Paths). For the Interposer embedded in the
 *   dinput8.dll / ASI builds, .interposer\Config.yml next to the patch, a Config.yml with FastDL on for the .rfa
 *   files under Mods is written before it starts when there is none (or an empty one): one already there is the
 *   player's, or LANCommander's. */
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice.h"

#define MAP_CASE_SLOT   0x49574cu    /* the jump table 0x4956c8, entry 33: message 0x36 (the server's map) */
#define MAP_CASE        0x49478eu    /* edi = the handler */
#define JOIN_ADDRESS    0x22c        /* the handler's std::string "ip:port" */

typedef BOOL (*set_url_fn)(const wchar_t *base_url);

static char g_server[64];            /* the game server whose FastDL server the Interposer was given */
static WORD g_ports[8];
static int g_n_ports;

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

/* the Interposer's InterposerSetFastDLBaseUrl (the embedded one's goes to the Interposer that installed the hooks,
 * when that is another one) */
static set_url_fn interposer_set_url(void)
{
    static int tried;
    static set_url_fn set;
    if (tried) {
        return set;
    }
    tried = 1;
    if (!bf_interposer()) {
        bf_log("fastdl: no LANCommander Interposer, no FastDL");
        return NULL;
    }
    if (!(set = (set_url_fn)(void (*)(void))GetProcAddress(bf_interposer(), "InterposerSetFastDLBaseUrl"))) {
        bf_log("fastdl: the LANCommander Interposer in the game has no InterposerSetFastDLBaseUrl; update it for "
               "FastDL");
    }
    return set;
}

/* the FastDL server the game server's query reply names, as a URL in url ("" for none) */
static void server_url(const char *reply, const char *game_host, char *url, int size)
{
    char port[16], host[BF_OPT_STR_SIZE], path[BF_OPT_STR_SIZE];
    int len = (int)strlen(reply), p;
    url[0] = '\0';
    if (!bf_query_value(reply, len, "fastdlport", port, sizeof port) || (p = atoi(port)) <= 0 || p > 65535) {
        return;
    }
    if (!bf_query_value(reply, len, "fastdlhost", host, sizeof host) || !host[0]) {
        lstrcpynA(host, game_host, sizeof host);
    }
    if (!bf_query_value(reply, len, "fastdlpath", path, sizeof path) || !path[0]) {
        strcpy(path, "/");
    }
    snprintf(url, size, "http://%s:%d%s%s", host, p, path[0] == '/' ? "" : "/", path);
}

/* the map case of the message handler, before the game looks for the map: on the game thread */
void bf_fastdl_join(void *handler)
{
    const void *s = (const BYTE *)handler + JOIN_ADDRESS;
    char address[64], host[64], reply[2048], url[2 * BF_OPT_STR_SIZE], *colon;
    wchar_t wide[2 * BF_OPT_STR_SIZE];
    WORD port = 0;
    set_url_fn set;
    int r;
    if (!readable(s, BF_STRING_SIZE) || !readable(bf_string_chars(s), 1)) {
        return;
    }
    lstrcpynA(address, bf_string_chars(s), sizeof address);
    if (!address[0] || !strcmp(address, g_server)) {
        return;                      /* the next map on the same server: the Interposer has its FastDL server */
    }
    if (!(set = interposer_set_url())) {
        return;
    }
    lstrcpynA(host, address, sizeof host);
    if ((colon = strrchr(host, ':')) != NULL) {
        port = (WORD)atoi(colon + 1);
        *colon = '\0';
    }
    /* the game server's own FastDL server, always; a server that does not answer (yet) is asked again with its next
     * map */
    url[0] = '\0';
    g_server[0] = '\0';
    if ((r = bf_query_info(host, g_ports, g_n_ports, port, reply, sizeof reply)) < 0) {
        bf_log("fastdl: cannot resolve the game server %s", host);
    } else if (!r) {
        bf_log("fastdl: the game server %s does not answer its queries", address);
    } else {
        lstrcpynA(g_server, address, sizeof g_server);
        server_url(reply, host, url, sizeof url);
        if (!url[0]) {
            bf_log("fastdl: the game server %s has no FastDL server", address);
        }
    }
    /* else game.fastDlUrl, if set */
    if (!url[0] && bf_setting_str("fastDlUrl")[0]) {
        lstrcpynA(url, bf_setting_str("fastDlUrl"), sizeof url);
        bf_log("fastdl: using game.fastDlUrl instead");
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wide, sizeof wide / sizeof wide[0])) {
        wide[0] = L'\0';
    }
    if (set(wide)) {
        bf_log(url[0] ? "fastdl: joining %s, missing maps come from %s"
                      : "fastdl: joining %s, missing maps come from the Interposer's own FastDL server, if any%s",
               address, url);
    } else if (url[0]) {
        bf_log("fastdl: the Interposer did not take %s: FastDL is off in its Config.yml (FastDL.Enabled, "
               "FastDL.Paths), or it is not an http:// URL", url);
    }
}

void *bf_fastdl_case = (void *)MAP_CASE;
extern void bf_fastdl_case_entry(void);
__asm__(".text\n"
        ".globl _bf_fastdl_case_entry\n"
        "_bf_fastdl_case_entry:\n"
        "    pushal\n"
        "    pushl %edi\n"            /* the handler */
        "    call _bf_fastdl_join\n"
        "    addl $4, %esp\n"
        "    popal\n"
        "    jmp *_bf_fastdl_case\n");

/* the embedded Interposer's .interposer\Config.yml, next to the patch (dll): FastDL on for the maps under Mods, unless
 * there is one already that is not empty. Before the Interposer starts, which reads it then */
void bf_fastdl_write_config(HMODULE dll)
{
    static const char yaml[] =
        "# Written by bf1942patch: FastDL downloads the maps (and mod archives) the game lacks from the FastDL server\n"
        "# of the game server it joins. Each joined server tells the patch where that is.\n"
        "FastDL:\n"
        "  Enabled: true\n"
        "  AllowedExtensions: [.rfa]\n"
        "  Paths:\n"
        "    - Local: '%GAMEDIR%\\Mods'\n"
        "      Remote: Mods\n";
    char path[MAX_PATH + 32], *slash;
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILE *f;
    if (!bf_setting_int("fastDl") || !GetModuleFileNameA(dll, path, MAX_PATH) || !(slash = strrchr(path, '\\'))) {
        return;
    }
    strcpy(slash + 1, ".interposer\\Config.yml");
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &a) && (a.nFileSizeLow || a.nFileSizeHigh)) {
        return;
    }
    bf_make_dirs(path);
    if (!(f = fopen(path, "w"))) {
        bf_log("fastdl: cannot write %s, FastDL stays off", path);
        return;
    }
    fputs(yaml, f);
    fclose(f);
    bf_log("fastdl: wrote %s (FastDL on for the .rfa files under Mods)", path);
}

void bf_fastdl_install(void)
{
    char text[256];
    if (!bf_setting_int("fastDl")) {
        bf_log("fastdl: fastDl 0, no FastDL");
        return;
    }
    lstrcpynA(text, bf_setting_str("voiceQueryPorts"), sizeof text);
    g_n_ports = bf_voice_ports(text, g_ports, 8);
    if (!bf_hook_slot(MAP_CASE_SLOT, MAP_CASE, bf_fastdl_case_entry)) {
        bf_log("fastdl: the client's message handler differs (%08x), no FastDL", MAP_CASE_SLOT);
        return;
    }
}
