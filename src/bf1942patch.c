/* Core of the patch: verifies the exe, reads the options, installs the patch modules.
 * The options are console variables bf1942patch.<name> (settings.h, settings_game.c): read from bf1942patch.con next
 * to the DLL (dinput8.dll proxy and ASI builds; the plugin build adds the Interposer's Config.yml, see
 * interposer_plugin.c), then from the player's bf1942patch.con in the save folder, and set in the console or by the
 * game's .con files. The log goes to bf1942patch.log next to the DLL, or the Interposer's session log. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bf1942patch.h"

#define BF_TIMESTAMP 0x417564c4u    /* BF1942.exe 1.61 (version string "BF1942 (Ver: Tue, 19 Oct 2004 14:...)") */
#define BF_ENTRY     0x404c1au

static char g_log[MAX_PATH];
static void (*g_log_line)(const char *line);
static CRITICAL_SECTION g_log_cs;
static int g_log_cs_ready;
static HMODULE g_module;
static int g_server;                 /* in BF1942_w32ded.exe (voice_server_win.c) */

void bf_set_log_backend(void (*log_line)(const char *line))
{
    g_log_line = log_line;
}

HMODULE bf_module(void)
{
    return g_module;
}

void bf_set_module(HMODULE dll)
{
    g_module = dll;
}

void bf_log(const char *fmt, ...)
{
    va_list ap;
    char line[1024];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_log_line) {
        g_log_line(line);
        return;
    }
    if (!g_log_cs_ready) {
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_ready = 1;
    }
    EnterCriticalSection(&g_log_cs);
    {
        FILE *f = fopen(g_log, "a");
        if (f) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(f, "%02d:%02d:%02d.%03d [%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                    GetCurrentProcessId(), line);
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_log_cs);
}

int bf_write(uint32_t addr, const void *bytes, size_t len)
{
    DWORD old;
    if (!VirtualProtect(BF_ADDR(addr), len, PAGE_EXECUTE_READWRITE, &old)) {
        bf_log("VirtualProtect %08x failed (%lu)", addr, GetLastError());
        return 0;
    }
    memcpy(BF_ADDR(addr), bytes, len);
    VirtualProtect(BF_ADDR(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), BF_ADDR(addr), len);
    return 1;
}

int bf_expect(uint32_t addr, const void *bytes, size_t len)
{
    return memcmp(BF_ADDR(addr), bytes, len) == 0;
}

int bf_patch(uint32_t addr, const void *expect, const void *bytes, size_t len)
{
    if (!bf_expect(addr, expect, len)) {
        bf_log("patch %08x: original bytes differ, skipped", addr);
        return 0;
    }
    return bf_write(addr, bytes, len);
}

/* op rel32 at from, then NOPs up to len bytes */
static int rel32(uint32_t from, uint8_t op, const void *to, size_t len)
{
    uint8_t b[32];
    int32_t rel = (int32_t)((uintptr_t)to - (from + 5));
    if (len < 5 || len > sizeof b) {
        return 0;
    }
    b[0] = op;
    memcpy(b + 1, &rel, 4);
    memset(b + 5, 0x90, len - 5);
    return bf_write(from, b, len);
}

int bf_jmp(uint32_t from, const void *to) { return rel32(from, 0xe9, to, 5); }
int bf_call(uint32_t from, const void *to) { return rel32(from, 0xe8, to, 5); }

int bf_replace_call(uint32_t site, const void *expect, size_t len, const void *to)
{
    return bf_expect(site, expect, len) && rel32(site, 0xe8, to, len);
}

/* room for the trampolines, a page at a time */
static BYTE *trampoline(size_t len)
{
    static BYTE *page;
    static size_t used;
    BYTE *t;
    len = (len + 15) & ~(size_t)15;
    if (!page || used + len > 4096) {
        if (!(page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
            bf_log("VirtualAlloc for a trampoline failed (%lu)", GetLastError());
            return NULL;
        }
        used = 0;
    }
    t = page + used;
    used += len;
    return t;
}

void *bf_detour(uint32_t fn, const void *prologue, size_t len, const void *hook)
{
    BYTE *t;
    int32_t rel;
    if (len < 5 || !bf_expect(fn, prologue, len) || !(t = trampoline(len + 5))) {
        return NULL;
    }
    memcpy(t, prologue, len);
    t[len] = 0xe9;
    rel = (int32_t)((fn + len) - ((uintptr_t)t + len + 5));
    memcpy(t + len + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, len + 5);
    return rel32(fn, 0xe9, hook, len) ? t : NULL;
}

int bf_hook_slot(uint32_t slot, uint32_t expect, const void *hook)
{
    if (*(const uint32_t *)BF_ADDR(slot) != expect) {
        return 0;
    }
    return bf_write(slot, &hook, sizeof hook);
}

int bf_redirect_call(uint32_t site, uint32_t old_target, const void *to)
{
    uint8_t old[5];
    int32_t rel = (int32_t)(old_target - (site + 5));
    old[0] = 0xe8;
    memcpy(old + 1, &rel, 4);
    if (!bf_expect(site, old, 5)) {
        bf_log("call at %08x does not go to %08x, left alone", site, old_target);
        return 0;
    }
    return bf_call(site, to);
}

static int exe_is_supported(void)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)dos + dos->e_lfanew);
    return (uintptr_t)dos == 0x400000 && nt->FileHeader.TimeDateStamp == BF_TIMESTAMP
        && nt->OptionalHeader.AddressOfEntryPoint == BF_ENTRY;
}

int bf_patch_start(void)
{
    if (!exe_is_supported()) {
        char exe[MAX_PATH];
        if (bf_server_exe()) {
            g_server = 1;
        } else {
            GetModuleFileNameA(NULL, exe, MAX_PATH);
            bf_log("bf1942patch: %s is neither the supported BF1942.exe 1.61 nor BF1942_w32ded.exe 1.61; no patches "
                   "applied", exe);
            return 0;
        }
    }
    bf_settings_load(g_server);
    if (!bf_setting_int("enabled")) {
        bf_log("bf1942patch: disabled (bf1942patch.enabled 0)");
        return 0;
    }
    {
        /* the dinput8.dll proxy, the ASI and the Interposer plugin carry the same patches: only the first one loaded applies them */
        char name[48];
        snprintf(name, sizeof name, "Local\\bf1942patch_%lu", GetCurrentProcessId());
        if (CreateMutexA(NULL, FALSE, name) && GetLastError() == ERROR_ALREADY_EXISTS) {
            bf_log("bf1942patch: another copy of the patch is already loaded; this one does nothing");
            return 0;
        }
    }
    bf_log("bf1942patch " BF_VERSION ": %s 1.61 recognised, installing (command line: %s)",
           g_server ? "BF1942_w32ded.exe" : "BF1942.exe", GetCommandLineA());
    return 1;
}

void bf_install(void)
{
    if (g_server) {
        bf_server_install();         /* the dedicated server: its voice server only */
        return;
    }
    bf_crashlog_install();
    bf_saves_install();
    bf_registry_install();
    bf_master_install();
    bf_window_install();
    bf_restart_install();
    bf_widescreen_install();
    bf_menu_install();
    bf_settings_console_install();
    bf_gamepad_install();
    bf_framerate_install();
    bf_detail_install();
    bf_voice_install();
}

const char *bf_log_file(void)
{
    return g_log_line ? "" : g_log;
}

void bf_set_files(const char *con, const char *log)
{
    bf_settings_set_file(con);
    lstrcpynA(g_log, log, MAX_PATH);
}

/* the dinput8.dll proxy and the ASI: bf1942patch.con / bf1942patch.log next to the DLL, then the patches. The log
 * is started afresh at a normal start; the copies the game starts of itself (+restart 1) add to it. */
void bf_standalone_init(HINSTANCE dll)
{
    char con[MAX_PATH], log[MAX_PATH + 16], exe[MAX_PATH], *slash;
    WIN32_FILE_ATTRIBUTE_DATA a;
    bf_set_module(dll);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    slash = strrchr(exe, '\\');
    if (!_stricmp(slash ? slash + 1 : exe, "rundll32.exe")) {
        return;   /* the restart helper: bf_restart_helper sets everything up itself */
    }
    GetModuleFileNameA(dll, con, MAX_PATH);
    slash = strrchr(con, '\\');
    if (slash) {
        slash[1] = '\0';
    }
    strcpy(log, con);
    strcat(con, "bf1942patch.con");
    strcat(log, "bf1942patch.log");
    if (!strstr(GetCommandLineA(), "+restart 1")
        || (GetFileAttributesExA(log, GetFileExInfoStandard, &a) && a.nFileSizeLow > 8u << 20)) {
        DeleteFileA(log);
    }
    bf_set_files(con, log);
    if (bf_patch_start()) {
        bf_install();
    }
}
/* ---------------------------------------------------------------- the game's std::string (MSVC 7, bfcprt.dll) */

typedef void (__attribute__((thiscall)) *string_ctor_fn)(void *self, const char *s);
typedef void (__attribute__((thiscall)) *string_dtor_fn)(void *self);
static string_ctor_fn g_string_ctor;
static string_dtor_fn g_string_dtor;

static int string_fns(void)
{
    if (!g_string_ctor) {
        HMODULE cprt = GetModuleHandleA("bfcprt.dll");
        g_string_ctor = (string_ctor_fn)(void (*)(void))GetProcAddress(cprt, "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z");
        g_string_dtor = (string_dtor_fn)(void (*)(void))GetProcAddress(cprt, "??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ");
    }
    return g_string_ctor && g_string_dtor;
}

int bf_string_init(void *mem, const char *s)
{
    if (!string_fns()) {
        return 0;
    }
    g_string_ctor(mem, s);
    return 1;
}

void bf_string_free(void *mem)
{
    if (string_fns()) {
        g_string_dtor(mem);
    }
}

/* layout: +4 the characters (inline while the capacity at +0x18 is below 16, else a pointer), +0x14 the length */
const char *bf_string_chars(const void *s)
{
    const BYTE *p = s;
    return *(const DWORD *)(p + 0x18) < 16 ? (const char *)(p + 4) : *(const char *const *)(p + 4);
}

/* ---------------------------------------------------------------- the game's std::wstring (basic_string<unsigned short>) */

typedef void (__attribute__((thiscall)) *wstring_ctor_fn)(void *self, const wchar_t *s);
static wstring_ctor_fn g_wstring_ctor;
static string_dtor_fn g_wstring_dtor;

static int wstring_fns(void)
{
    if (!g_wstring_ctor) {
        HMODULE cprt = GetModuleHandleA("bfcprt.dll");
        g_wstring_ctor = (wstring_ctor_fn)(void (*)(void))GetProcAddress(cprt, "??0?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QAE@PBG@Z");
        g_wstring_dtor = (string_dtor_fn)(void (*)(void))GetProcAddress(cprt, "??1?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@2@@std@@QAE@XZ");
    }
    return g_wstring_ctor && g_wstring_dtor;
}

int bf_wstring_init(void *mem, const wchar_t *s)
{
    if (!wstring_fns()) {
        return 0;
    }
    g_wstring_ctor(mem, s);
    return 1;
}

void bf_wstring_free(void *mem)
{
    if (wstring_fns()) {
        g_wstring_dtor(mem);
    }
}

/* the same layout as std::string, with room for 8 characters inline */
const wchar_t *bf_wstring_chars(const void *s)
{
    const BYTE *p = s;
    return *(const DWORD *)(p + 0x18) < 8 ? (const wchar_t *)(p + 4) : *(const wchar_t *const *)(p + 4);
}