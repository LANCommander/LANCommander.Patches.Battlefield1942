/* The Windows dedicated server (BF1942_w32ded.exe 1.61): voice_server.c's voice server, run by the same patch DLLs as
 * the game. The server imports no DirectInput, so it loads the Interposer plugin build (the Interposer in the server)
 * or the ASI (with an ASI loader the server loads, e.g. Ultimate ASI Loader as winmm.dll); bf1942patch.c sees the
 * server's exe and installs only this.
 *
 * (integrations/bf1942 of LANCommander.Comms, docs/integrations/bf1942.md)
 * - Game::frame (0x432410, thiscall, returns "keep running") is detoured: the voice server runs after each frame,
 *   between engine updates. It returns 0 when the server quits, and the voice server stops then.
 * - IPlayerManager [0x74c534], vtable +0x2c: std::list<IPlayer *> * of every player (bots too), its head node at +4,
 *   nodes {next, prev, value}. IPlayer: vtable +0x18 name (std::string *), +0x38 the soldier or vehicle controlled,
 *   +0x44 id, +0x58 is AI; +0xa9 alive (bool), +0xac team. IObject vtable +0x34: its position (const float[3] *).
 * - The GameSpy query replies leave through the exe's WSOCK32 sendto (FUN_004444b0): hooked to add \commsport\.
 * - A dedicated server runs unattended, so its error dialogs would hang it for good: MessageBoxA/W are answered OK and
 *   logged (bf1942patch.suppressDialogs 1), and the server then exits as it would after the click. */
#include <winsock2.h>
#include <stdio.h>
#include <string.h>
#include "voice.h"
#include "voice_server.h"

#define SERVER_TIMESTAMP 0x417565a6u /* BF1942_w32ded.exe 1.61 */
#define SERVER_ENTRY    0x29a707u
#define FRAME_FN        0x432410u
#define PLAYER_MANAGER  0x74c534u
#define PLAYER_MANAGER_REF 0x4538a5u /* mov ecx,[0074c534] */
#define PM_PLAYERS      0x2c
#define PLAYER_NAME     0x18
#define PLAYER_VEHICLE  0x38
#define PLAYER_ID       0x44
#define PLAYER_IS_AI    0x58
#define PLAYER_ALIVE    0xa9
#define PLAYER_TEAM     0xac
#define OBJECT_POSITION 0x34
#define MAX_WALK        512

static uintptr_t g_image_end;

int bf_server_exe(void)
{
    static const BYTE frame[14] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec, 0x1c, 0x56, 0x8b, 0xf1, 0x8a, 0x86};
    static const BYTE pm_ref[6] = {0x8b, 0x0d, 0x34, 0xc5, 0x74, 0x00};
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)dos + dos->e_lfanew);
    if ((uintptr_t)dos != 0x400000 || nt->FileHeader.TimeDateStamp != SERVER_TIMESTAMP
        || nt->OptionalHeader.AddressOfEntryPoint != SERVER_ENTRY || !bf_expect(FRAME_FN, frame, sizeof frame)
        || !bf_expect(PLAYER_MANAGER_REF, pm_ref, sizeof pm_ref)) {
        return 0;
    }
    g_image_end = 0x400000 + nt->OptionalHeader.SizeOfImage;
    return 1;
}

/* ---------------------------------------------------------------- reading the game */

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
    return vt >= 0x400000 && vt < g_image_end && readable((const void *)vt, 0x60);
}

static void *vcall(void *self, int offset)
{
    return ((void *(__attribute__((thiscall)) *)(void *))(*(void ***)self)[offset / 4])(self);
}

static int read_player(void *player, bf_server_player *out)
{
    const BYTE *b = player;
    const void *name, *vehicle;
    const float *at;
    if (!object(player) || !readable(b + PLAYER_TEAM, 4)) {
        return 0;
    }
    memset(out, 0, sizeof *out);
    out->id = (int)(intptr_t)vcall(player, PLAYER_ID);
    name = vcall(player, PLAYER_NAME);
    if (readable(name, BF_STRING_SIZE) && readable(bf_string_chars(name), 1)) {
        lstrcpynA(out->name, bf_string_chars(name), sizeof out->name);
    }
    out->team = *(const int *)(b + PLAYER_TEAM);
    out->alive = b[PLAYER_ALIVE] != 0;
    out->ai = (BYTE)(uintptr_t)vcall(player, PLAYER_IS_AI) != 0;
    vehicle = vcall(player, PLAYER_VEHICLE);
    if (object(vehicle) && readable(at = vcall((void *)vehicle, OBJECT_POSITION), 12)) {
        memcpy(out->position, at, 12);
        out->has_position = 1;
    }
    return 1;
}

int bf_server_read_players(bf_server_player *out, int capacity)
{
    struct node { struct node *next, *prev; void *value; } *head, *n;
    void *pm = *(void **)BF_ADDR(PLAYER_MANAGER);
    const BYTE *list;
    int count = 0, walked = 0;
    if (!object(pm) || !readable(list = vcall(pm, PM_PLAYERS), 8)) {
        return -1;
    }
    if (!(head = *(struct node *const *)(list + 4))) {
        return 0;
    }
    if (!readable(head, sizeof *head)) {
        return -1;
    }
    for (n = head->next; n && n != head && walked < MAX_WALK; n = n->next, walked++) {
        if (!readable(n, sizeof *n)) {
            return -1;
        }
        if (n->value && count < capacity && read_player(n->value, &out[count])) {
            count++;
        }
    }
    return count;
}

/* ---------------------------------------------------------------- the platform */

void *bf_server_symbol(const char *name)
{
    HMODULE m = bf_voice_library();
    return m ? (void *)GetProcAddress(m, name) : NULL;
}

uint64_t bf_server_now_ms(void)
{
    return GetTickCount64();
}

/* ---------------------------------------------------------------- hooks */

static DWORD (__attribute__((thiscall)) *g_frame)(void *game);

static DWORD __attribute__((thiscall)) hk_frame(void *game)
{
    DWORD keep = g_frame(game);
    if (keep & 0xff) {
        bf_server_tick();
    } else {
        bf_server_stop();            /* the server quits: voice stops before exit runs under its threads */
    }
    return keep;
}

static int (WINAPI *real_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);

static int WINAPI hk_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    char reply[2048];
    int n = bf_server_query_reply(buf, len, reply, sizeof reply), sent;
    if (n) {
        sent = real_sendto(s, reply, n, flags, to, tolen);
        return sent == SOCKET_ERROR ? sent : len;
    }
    return real_sendto(s, buf, len, flags, to, tolen);
}

static int WINAPI hk_message_box_a(HWND w, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)w;
    (void)type;
    bf_log("server: dialog '%s' answered OK: %s", caption ? caption : "", text ? text : "");
    return IDOK;
}

static int WINAPI hk_message_box_w(HWND w, LPCWSTR text, LPCWSTR caption, UINT type)
{
    (void)w;
    (void)type;
    bf_log("server: dialog '%ls' answered OK: %ls", caption ? caption : L"", text ? text : L"");
    return IDOK;
}

/* every caller's (the runtime's too, which finds them by name): a jump at the start of user32's own, which begin
 * with the hot-patch prologue; else only the exe's import */
static void suppress_dialogs(void)
{
    static const BYTE hotpatch[5] = {0x8b, 0xff, 0x55, 0x8b, 0xec};   /* mov edi,edi; push ebp; mov ebp,esp */
    HMODULE user32 = LoadLibraryA("user32.dll");
    static const char *const names[2] = {"MessageBoxA", "MessageBoxW"};
    const void *hooks[2] = {(const void *)hk_message_box_a, (const void *)hk_message_box_w};
    int i, all = 1;
    for (i = 0; i < 2; i++) {
        FARPROC f = GetProcAddress(user32, names[i]);
        if (!f || !bf_detour((uint32_t)(uintptr_t)f, hotpatch, sizeof hotpatch, hooks[i])) {
            all = 0;
            bf_hook_import("USER32.dll", names[i], (void *)hooks[i]);
        }
    }
    bf_log("server: error dialogs are logged and answered OK%s", all ? "" : " (the server's own only)");
}

void bf_server_install(void)
{
    static const BYTE frame_head[6] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};   /* push ebp; mov ebp,esp; and esp,-8 */
    int fastdl, voice;
    if (bf_setting_int("suppressDialogs")) {
        suppress_dialogs();
    }
    fastdl = bf_server_fastdl_init();
    if (!(voice = bf_setting_int("voiceChat"))) {
        bf_log("voice server: voiceChat 0, no voice");
    } else if (!(g_frame = (DWORD (__attribute__((thiscall)) *)(void *))bf_detour(FRAME_FN, frame_head,
                                                                                  sizeof frame_head, hk_frame))) {
        bf_log("voice server: the server's frame (%08x) is hooked already, no voice", FRAME_FN);
        voice = 0;
    }
    if (!voice && !fastdl) {
        return;
    }
    if (!(real_sendto = bf_hook_import("WSOCK32.dll", "sendto", hk_sendto))) {
        bf_log("server: the server's sendto is not hooked, so its queries do not say the voice port or the FastDL "
               "server; clients need bf1942patch.voicePort and fastDlUrl");
    }
    if (voice) {
        bf_log("voice server: installed (the Interposer loads with the first frame)");
    }
}
