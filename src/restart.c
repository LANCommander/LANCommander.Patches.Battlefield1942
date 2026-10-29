/* Seamless restarts. BF1942 starts a new copy of itself for every change of state: hosting (+hostServer 1), joining
 * (+joinServer ...), going back to the menu (+goToInterface <n>), the next campaign map (+continueCampaign ...),
 * reconnecting. All of them end in 0x454dd0, which tears the engine down and then
 *   full screen (app+0x38 set):  CreateProcessA("BlackScreen.exe <pid> +game <mod> +restart 1 <args>") (0x45512a);
 *                                BlackScreen.exe changes the display mode back, shows a black window, waits for this
 *                                process to end, starts BF1942.exe and waits for it to create
 *                                HKCU\SOFTWARE\Battlefield 1942\Initiated
 *   windowed:                    Sleep(2000) (0x455137), _spawnl(P_NOWAIT, "BF1942.exe", "BF1942.exe", args)
 *                                (0x4552ec)
 * then Sleep(1000) (0x4552ff) and ExitProcess. The engine depends on starting from a fresh process (it has no way
 * to unload a mod or a map), so the restart itself stays; what changes is what the player sees:
 *   - from the moment 0x454dd0 starts, a window with the mod's loading picture (Mods\<mod>\00000000.256) covers the
 *     game's monitor;
 *   - instead of BlackScreen.exe / the direct start, a helper (rundll32 running bf_restart_helper in this DLL) puts
 *     up the same picture, waits for this process to end, starts BF1942.exe with the game's arguments and keeps the
 *     picture up until the new copy has shown its first frames (signalled through the event named in the
 *     environment variable BF1942PATCH_READY), then hands it the foreground;
 *   - nothing waits on fixed sleeps any more, nothing touches the display mode (with window.c) or the registry.
 * game.seamlessRestart 0 turns this off. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bf1942patch.h"
#include "d3d.h"

#define RESTART_FN    0x454dd0u    /* thiscall on the application object, arguments by value (std::string) */
#define SLEEP_2000    0x455137u    /* push 0x7d0 before the windowed restart's Sleep */
#define SLEEP_1000    0x4552ffu    /* push 0x3e8 before the last Sleep */
#define READY_VAR     "BF1942PATCH_READY"
#define COVER_CLASS   "bf1942patch_cover"

static BOOL (WINAPI *real_CreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                          LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
static intptr_t (__cdecl *real__spawnv)(int, const char *, const char *const *);
static RECT g_cover_rect;
static char g_cover_mod[64] = "bf1942";
static int g_restarting;

/* ---------------------------------------------------------------- the cover window */

static HBITMAP g_picture;

static void load_picture(const char *mod)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sMods\\%s\\00000000.256", bf_game_dir(), mod);
    g_picture = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (!g_picture && _stricmp(mod, "bf1942")) {
        load_picture("bf1942");
    }
}

static LRESULT CALLBACK cover_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(wnd, &ps);
        GetClientRect(wnd, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (g_picture) {
            BITMAP bm;
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, g_picture);
            int w, h;
            GetObjectA(g_picture, sizeof bm, &bm);
            /* the picture at its own shape, as large as fits */
            h = rc.bottom;
            w = MulDiv(h, bm.bmWidth, bm.bmHeight);
            if (w > rc.right) {
                w = rc.right;
                h = MulDiv(w, bm.bmHeight, bm.bmWidth);
            }
            SetStretchBltMode(dc, HALFTONE);
            StretchBlt(dc, (rc.right - w) / 2, (rc.bottom - h) / 2, w, h, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        EndPaint(wnd, &ps);
        return 0;
    }
    if (msg == WM_SETCURSOR) {
        SetCursor(NULL);
        return TRUE;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

static HWND make_cover(const RECT *r, const char *mod)
{
    WNDCLASSA wc;
    HWND wnd;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = cover_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = COVER_CLASS;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    load_picture(mod);
    wnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, COVER_CLASS, "Battlefield 1942", WS_POPUP | WS_VISIBLE,
                          r->left, r->top, r->right - r->left, r->bottom - r->top, NULL, NULL, wc.hInstance, NULL);
    if (wnd) {
        SetForegroundWindow(wnd);
        UpdateWindow(wnd);
    }
    return wnd;
}

/* in the game: the cover goes up on a thread of its own, and stays until the process ends */
static DWORD WINAPI cover_thread(LPVOID unused)
{
    MSG msg;
    (void)unused;
    if (!make_cover(&g_cover_rect, g_cover_mod)) {
        return 0;
    }
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        DispatchMessageA(&msg);
    }
    return 0;
}

/* the mod a restart is for: "+game <mod>" in the arguments, else the one running */
static void mod_of(const char *args, char *mod, size_t size)
{
    const char *p = args ? strstr(args, "+game ") : NULL;
    size_t n = 0;
    if (!p) {
        return;
    }
    p += 6;
    while (*p == ' ') {
        p++;
    }
    while (p[n] && p[n] != ' ' && n + 1 < size) {
        n++;
    }
    if (n) {
        memcpy(mod, p, n);
        mod[n] = '\0';
    }
}

/* 0x454dd0 is starting: put the cover up before the engine (and its window) goes away */
void __cdecl bf_restart_begin(void)
{
    if (g_restarting) {
        return;
    }
    g_restarting = 1;
    mod_of(GetCommandLineA(), g_cover_mod, sizeof g_cover_mod);
    bf_game_monitor(&g_cover_rect);
    AllowSetForegroundWindow(ASFW_ANY);
    CloseHandle(CreateThread(NULL, 0, cover_thread, NULL, 0, NULL));
    bf_log("restart: the game is restarting itself, covering the screen");
}

/* the hook at 0x454dd0: note the restart, then run the original (its arguments are std::strings by value, so the
 * stack is passed on untouched) */
void *bf_restart_orig;               /* bf_detour trampoline */
extern void bf_restart_entry(void);
__asm__(".text\n"
        ".globl _bf_restart_entry\n"
        "_bf_restart_entry:\n"
        "    pushal\n"
        "    call _bf_restart_begin\n"
        "    popal\n"
        "    jmp *_bf_restart_orig\n");

/* ---------------------------------------------------------------- starting the helper */

static int launch_helper(const char *args)
{
    char dll[MAX_PATH], rundll[MAX_PATH], cmd[4096], event[64];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE up;
    HMODULE self = NULL;
    char mod[64];
    strcpy(mod, g_cover_mod);
    mod_of(args, mod, sizeof mod);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)bf_restart_helper, &self);
    GetModuleFileNameA(self, dll, MAX_PATH);
    GetSystemDirectoryA(rundll, MAX_PATH);   /* SysWOW64 for a 32-bit process: the 32-bit rundll32 */
    strcat(rundll, "\\rundll32.exe");
    snprintf(event, sizeof event, "Local\\bf1942patch_cover_%lu", GetCurrentProcessId());
    up = CreateEventA(NULL, TRUE, FALSE, event);
    snprintf(cmd, sizeof cmd, "\"%s\" \"%s\",bf_restart_helper %lu %ld %ld %ld %ld %s \"%s\" \"%s\" | %s", rundll, dll,
             GetCurrentProcessId(), g_cover_rect.left, g_cover_rect.top, g_cover_rect.right, g_cover_rect.bottom, mod,
             bf_game_dir(), bf_log_file()[0] ? bf_log_file() : "-", args);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!real_CreateProcessA(rundll, cmd, NULL, NULL, FALSE, 0, NULL, bf_game_dir(), &si, &pi)) {
        bf_log("restart: cannot start the helper (%lu): %s", GetLastError(), cmd);
        CloseHandle(up);
        return 0;
    }
    bf_log("restart: helper %lu started for: %s", pi.dwProcessId, args);
    AllowSetForegroundWindow(pi.dwProcessId);
    /* this process ends right after: wait until the helper's picture is up, so the screen stays covered */
    {
        HANDLE both[2] = {up, pi.hProcess};
        WaitForMultipleObjects(2, both, FALSE, 5000);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(up);
    return 1;
}

static int prefix_i(const char *s, const char *p)
{
    return !_strnicmp(s, p, strlen(p));
}

/* full screen restart: "BlackScreen.exe <pid> <args>" */
static BOOL WINAPI hk_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                     BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA si,
                                     LPPROCESS_INFORMATION pi)
{
    if (!app && cmd && prefix_i(cmd, "BlackScreen.exe ")) {
        const char *args = cmd + strlen("BlackScreen.exe ");
        strtoul(args, (char **)&args, 10);   /* this process's id */
        while (*args == ' ') {
            args++;
        }
        if (launch_helper(args)) {
            memset(pi, 0, sizeof *pi);
            return TRUE;
        }
    }
    return real_CreateProcessA(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}

/* windowed restart: _spawnl(P_NOWAIT, "BF1942.exe", "BF1942.exe", args, NULL) */
static intptr_t __cdecl hk__spawnl(int mode, const char *path, const char *arg0, ...)
{
    const char *argv[64];
    va_list ap;
    int n = 0;
    const char *base = path ? strrchr(path, '\\') : NULL;
    base = base ? base + 1 : path;
    va_start(ap, arg0);
    for (argv[0] = arg0; argv[n] && n < 63; argv[++n] = va_arg(ap, const char *)) {
    }
    va_end(ap);
    argv[n] = NULL;
    if (base && !_stricmp(base, "BF1942.exe") && n >= 2 && launch_helper(argv[1])) {
        return 1;
    }
    return real__spawnv ? real__spawnv(mode, path, argv) : -1;
}

/* ---------------------------------------------------------------- the new copy: telling the helper it is up */

static char g_ready[64];

static void ready_present(void *device, int after)
{
    static int frames;
    (void)device;
    if (after && g_ready[0] && ++frames == 3) {
        HANDLE e = OpenEventA(EVENT_MODIFY_STATE, FALSE, g_ready);
        if (e) {
            SetEvent(e);
            CloseHandle(e);
        }
        bf_log("restart: first frames shown, the helper lets go of the screen");
        g_ready[0] = '\0';
    }
}

/* ---------------------------------------------------------------- the helper (rundll32) */


static BOOL CALLBACK find_dialog(HWND wnd, LPARAM lp)
{
    DWORD pid = 0;
    char cls[16];
    GetWindowThreadProcessId(wnd, &pid);
    if (pid == (DWORD)lp && IsWindowVisible(wnd) && GetClassNameA(wnd, cls, sizeof cls) && !strcmp(cls, "#32770")) {
        return FALSE;   /* a message box of the game (an error): it must be seen */
    }
    return TRUE;
}

static BOOL CALLBACK find_main(HWND wnd, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid == ((DWORD *)lp)[0] && IsWindowVisible(wnd) && !GetWindow(wnd, GW_OWNER)) {
        ((DWORD *)lp)[1] = (DWORD)(uintptr_t)wnd;
        return FALSE;
    }
    return TRUE;
}

/* the next "quoted" word of *p into out */
static void quoted(char **p, char *out, size_t size)
{
    char *s = strchr(*p, '"'), *e = s ? strchr(s + 1, '"') : NULL;
    out[0] = '\0';
    if (e && (size_t)(e - s) <= size) {
        memcpy(out, s + 1, e - s - 1);
        out[e - s - 1] = '\0';
        *p = e + 1;
    }
}

void CALLBACK bf_restart_helper(HWND hwnd, HINSTANCE inst, LPSTR cmdline, int show)
{
    char game[MAX_PATH], log[MAX_PATH + 16], ini[MAX_PATH + 16], exe[MAX_PATH + 16], cmd[4096], event[64], mod[64] = "bf1942", *p;
    unsigned long old_pid;
    RECT r;
    HWND cover;
    HANDLE old, ready, up;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD start;
    (void)hwnd; (void)inst; (void)show;

    /* <old pid> <left> <top> <right> <bottom> <mod> "<game folder\>" "<log file>" | <arguments for BF1942.exe> */
    old_pid = strtoul(cmdline, &p, 10);
    r.left = strtol(p, &p, 10);
    r.top = strtol(p, &p, 10);
    r.right = strtol(p, &p, 10);
    r.bottom = strtol(p, &p, 10);
    while (*p == ' ') {
        p++;
    }
    if (sscanf(p, "%63s", mod) == 1) {
        p += strlen(mod);
    }
    quoted(&p, game, sizeof game);
    quoted(&p, log, sizeof log);
    p = strstr(p, " | ");
    p = p ? p + 3 : (char *)"";
    if (!game[0]) {
        return;
    }
    bf_set_game_dir(game);
    snprintf(ini, sizeof ini, "%sbf1942patch.con", game);
    if (!strcmp(log, "-")) {
        snprintf(log, sizeof log, "%sbf1942patch.log", game);
    }
    bf_set_files(ini, log);
    bf_dpi_aware();   /* the rectangle is in the game's real pixels */
    if (r.right <= r.left || r.bottom <= r.top) {
        SetRect(&r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }

    cover = make_cover(&r, mod);
    snprintf(event, sizeof event, "Local\\bf1942patch_cover_%lu", old_pid);
    if ((up = OpenEventA(EVENT_MODIFY_STATE, FALSE, event)) != NULL) {
        SetEvent(up);
        CloseHandle(up);
    }

    /* the old copy has to be gone first (its network ports, its files) */
    if ((old = OpenProcess(SYNCHRONIZE, FALSE, old_pid)) != NULL) {
        start = GetTickCount();
        while (MsgWaitForMultipleObjects(1, &old, FALSE, 100, QS_ALLINPUT) != WAIT_OBJECT_0 && GetTickCount() - start < 20000) {
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                DispatchMessageA(&msg);
            }
        }
        CloseHandle(old);
    }

    snprintf(event, sizeof event, "Local\\bf1942patch_ready_%lu", GetCurrentProcessId());
    ready = CreateEventA(NULL, TRUE, FALSE, event);
    SetEnvironmentVariableA(READY_VAR, event);
    snprintf(exe, sizeof exe, "%sBF1942.exe", game);
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, p);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, game, &si, &pi)) {
        bf_log("restart helper: cannot start %s (%lu)", cmd, GetLastError());
        DestroyWindow(cover);
        return;
    }
    bf_log("restart helper: started %lu: %s", pi.dwProcessId, cmd);
    AllowSetForegroundWindow(pi.dwProcessId);

    /* until the new copy shows its first frames (or ends, or shows a message box): at most two minutes */
    start = GetTickCount();
    for (;;) {
        HANDLE both[2] = {ready, pi.hProcess};
        DWORD w = MsgWaitForMultipleObjects(2, both, FALSE, 250, QS_ALLINPUT);
        MSG msg;
        if (w == WAIT_OBJECT_0 || w == WAIT_OBJECT_0 + 1 || GetTickCount() - start > 120000) {
            break;
        }
        if (!EnumWindows(find_dialog, (LPARAM)pi.dwProcessId)) {
            break;
        }
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            DispatchMessageA(&msg);
        }
    }
    {
        DWORD found[2] = {pi.dwProcessId, 0};
        EnumWindows(find_main, (LPARAM)found);
        if (found[1]) {
            SetForegroundWindow((HWND)(uintptr_t)found[1]);
        }
    }
    DestroyWindow(cover);
    CloseHandle(ready);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/* ---------------------------------------------------------------- install */

void bf_restart_install(void)
{
    static const BYTE entry[6] = {0x81, 0xec, 0xa0, 0x0d, 0x00, 0x00};   /* sub esp, 0xda0 */
    static const BYTE sleep2000[5] = {0x68, 0xd0, 0x07, 0x00, 0x00}, sleep1000[5] = {0x68, 0xe8, 0x03, 0x00, 0x00};
    static const BYTE no_sleep[5] = {0x68, 0x00, 0x00, 0x00, 0x00};
    DWORD n;

    /* the new copy of a seamless restart: tell the helper when the first frames are on the screen */
    n = GetEnvironmentVariableA(READY_VAR, g_ready, sizeof g_ready);
    if (n && n < sizeof g_ready) {
        SetEnvironmentVariableA(READY_VAR, NULL);
        bf_d3d_install();
        bf_d3d_on_present(ready_present);
    } else {
        g_ready[0] = '\0';
    }

    if (!bf_setting_int("seamlessRestart")) {
        bf_log("restart: seamlessRestart 0, the game restarts itself the original way");
        return;
    }
    if (!bf_expect(RESTART_FN, entry, sizeof entry)) {
        bf_log("restart: %08x is not the restart function, seamless restarts are off", RESTART_FN);
        return;
    }
    real_CreateProcessA = bf_hook_import("KERNEL32.dll", "CreateProcessA", hk_CreateProcessA);
    {
        HMODULE crt = GetModuleHandleA("MSVCR70.dll");
        real__spawnv = crt ? (intptr_t (__cdecl *)(int, const char *, const char *const *))(void (*)(void))GetProcAddress(crt, "_spawnv") : NULL;
        bf_hook_import("MSVCR70.dll", "_spawnl", hk__spawnl);
    }
    if (!real_CreateProcessA) {
        bf_log("restart: CreateProcessA is not imported, seamless restarts are off");
        return;
    }
    if (!(bf_restart_orig = bf_detour(RESTART_FN, entry, sizeof entry, bf_restart_entry))) {
        bf_log("restart: cannot hook %08x, seamless restarts are off", RESTART_FN);
        return;
    }
    bf_patch(SLEEP_2000, sleep2000, no_sleep, sizeof no_sleep);
    bf_patch(SLEEP_1000, sleep1000, no_sleep, sizeof no_sleep);
    bf_log("restart: seamless restarts on");
}
