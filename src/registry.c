/* No registry: BF1942.exe's registry imports answer from a small in-memory registry, so the game runs from any
 * folder without an installer.
 *
 * What the exe uses the registry for (every ADVAPI32 call site, 1.61):
 *   HKLM\SOFTWARE\Electronic Arts\EA Games\Battlefield 1942\ergc, default value     the CD key: read through
 *        0x443f10 (key, value name, buffer, size) when hosting / joining (0x40cd57, 0x495103), checked against a
 *        banned prefix at 0x441d80
 *   HKLM\...\EA Games\Battlefield 1942 The Road to Rome\ergc, ...\EA GAMES\Battlefield 1942 Secret Weapons of
 *        WWII\ergc                                                                  the expansions' CD keys (0x459e20)
 *   HKLM\SOFTWARE\EA GAMES\Battlefield 1942, LANGUAGE (0x442cc0), REGSTR (0x445fa0)  install language / region
 *   HKCU\SOFTWARE\Battlefield 1942, PatchAvailable (0x443ec0), nAId and AId0<n> (0x447790)   written, never read
 *   HKCU\SOFTWARE\Battlefield 1942\Initiated (created when the window is up 0x462f50, deleted before a restart
 *        0x447790)       the "new process is up" signal for BlackScreen.exe, which restart.c no longer uses
 *   HKLM\Software\Microsoft\Direct3D (0x80a4c0)                                     a D3DX debug switch
 *
 * The CD keys come from, first match wins:
 *   bf1942patch.cdKey / cdKeyXPack1 / cdKeyXPack2 (bf1942patch.con, the console; Interposer: Config.yml)
 *   a file named cdkey (or cdkey.txt) in the save folder, then in the game folder: the key on the first line;
 *        lines XPack1=<key> / XPack2=<key> give the expansions their own keys
 * An expansion without a key of its own gets the main key. Everything the game writes stays in memory for the
 * session (bf1942patch.noRegistry 0 hands the registry back to the game). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bf1942patch.h"

#define ERGC_MAIN   "HKLM\\SOFTWARE\\Electronic Arts\\EA Games\\Battlefield 1942\\ergc"
#define ERGC_XPACK1 "HKLM\\SOFTWARE\\Electronic Arts\\EA Games\\Battlefield 1942 The Road to Rome\\ergc"
#define ERGC_XPACK2 "HKLM\\SOFTWARE\\Electronic Arts\\EA GAMES\\Battlefield 1942 Secret Weapons of WWII\\ergc"
#define INSTALL_KEY "HKLM\\SOFTWARE\\EA GAMES\\Battlefield 1942"
#define HANDLE_BASE 0x42460000u    /* fake HKEYs: 'BF' << 16 | slot */
#define MAX_HANDLES 64

typedef struct {
    char name[64];
    DWORD type;
    BYTE *data;
    DWORD size;
} value_t;

typedef struct {
    char path[MAX_PATH];
    value_t *vals;
    int nvals, cap;
} rkey_t;

static rkey_t *g_keys;
static int g_nkeys, g_capkeys;
static char g_handles[MAX_HANDLES][MAX_PATH];
static int g_used[MAX_HANDLES];
static CRITICAL_SECTION g_cs;
static int g_log_calls;

/* ---------------------------------------------------------------- store */

static rkey_t *find_key(const char *path)
{
    int i;
    for (i = 0; i < g_nkeys; i++) {
        if (!_stricmp(g_keys[i].path, path)) {
            return &g_keys[i];
        }
    }
    return NULL;
}

/* a key exists if it was created, or is the parent of one that was */
static int key_exists(const char *path)
{
    size_t n = strlen(path);
    int i;
    if (!strchr(path, '\\')) {
        return 1;                       /* HKLM, HKCU, ... */
    }
    for (i = 0; i < g_nkeys; i++) {
        if (!_strnicmp(g_keys[i].path, path, n) && (g_keys[i].path[n] == '\0' || g_keys[i].path[n] == '\\')) {
            return 1;
        }
    }
    return 0;
}

static rkey_t *create_key(const char *path)
{
    rkey_t *k = find_key(path);
    if (k) {
        return k;
    }
    if (g_nkeys == g_capkeys) {
        g_capkeys = g_capkeys ? g_capkeys * 2 : 16;
        g_keys = realloc(g_keys, g_capkeys * sizeof *g_keys);
    }
    k = &g_keys[g_nkeys++];
    memset(k, 0, sizeof *k);
    lstrcpynA(k->path, path, MAX_PATH);
    return k;
}

static value_t *find_value(rkey_t *k, const char *name)
{
    int i;
    for (i = 0; k && i < k->nvals; i++) {
        if (!_stricmp(k->vals[i].name, name)) {
            return &k->vals[i];
        }
    }
    return NULL;
}

static void set_value(rkey_t *k, const char *name, DWORD type, const void *data, DWORD size)
{
    value_t *v = find_value(k, name);
    if (!v) {
        if (k->nvals == k->cap) {
            k->cap = k->cap ? k->cap * 2 : 8;
            k->vals = realloc(k->vals, k->cap * sizeof *k->vals);
        }
        v = &k->vals[k->nvals++];
        memset(v, 0, sizeof *v);
        lstrcpynA(v->name, name, sizeof v->name);
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = malloc(size ? size : 1);
    if (size) {
        memcpy(v->data, data, size);
    }
}

static void set_string(rkey_t *k, const char *name, const char *s)
{
    set_value(k, name, REG_SZ, s, (DWORD)strlen(s) + 1);
}

static void delete_tree(const char *path)
{
    size_t n = strlen(path);
    int i, j;
    for (i = g_nkeys - 1; i >= 0; i--) {
        if (!_strnicmp(g_keys[i].path, path, n) && (g_keys[i].path[n] == '\0' || g_keys[i].path[n] == '\\')) {
            for (j = 0; j < g_keys[i].nvals; j++) {
                free(g_keys[i].vals[j].data);
            }
            free(g_keys[i].vals);
            g_keys[i] = g_keys[--g_nkeys];
        }
    }
}

/* ---------------------------------------------------------------- CD keys */

static void trim(char *s)
{
    char *p = s;
    size_t n;
    while (*p == ' ' || *p == '\t' || *p == '"') {
        p++;
    }
    memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '"')) {
        s[--n] = '\0';
    }
}

/* the key for pack ("" = the main game) from a cdkey file: the first plain line, or a "<pack>=" line */
static int key_from_file(const char *dir, const char *pack, char *out, int size)
{
    static const char *const names[] = {"cdkey", "cdkey.txt"};
    char path[MAX_PATH], line[256];
    int i, found = 0;
    for (i = 0; i < 2 && !found; i++) {
        FILE *f;
        snprintf(path, sizeof path, "%s%s", dir, names[i]);
        if (!(f = fopen(path, "r"))) {
            continue;
        }
        while (!found && fgets(line, sizeof line, f)) {
            char *eq;
            if (!strncmp(line, "\xef\xbb\xbf", 3)) {
                memmove(line, line + 3, strlen(line + 3) + 1);
            }
            trim(line);
            if (!line[0] || line[0] == ';' || line[0] == '#') {
                continue;
            }
            eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                trim(line);
                if (pack[0] && !_stricmp(line, pack)) {
                    trim(eq + 1);
                    lstrcpynA(out, eq + 1, size);
                    found = out[0] != '\0';
                }
            } else if (!pack[0]) {
                lstrcpynA(out, line, size);
                found = 1;
            }
        }
        fclose(f);
    }
    return found;
}

static int find_cdkey(const char *pack, char *out, int size, const char **from)
{
    const char *save = bf_save_dir();
    out[0] = '\0';
    lstrcpynA(out, bf_setting_str(!pack[0] ? "cdKey" : !strcmp(pack, "XPack1") ? "cdKeyXPack1" : "cdKeyXPack2"), size);
    if (out[0]) {
        trim(out);
        *from = "the settings";
        return out[0] != '\0';
    }
    if (save[0] && key_from_file(save, pack, out, size)) {
        *from = "cdkey in the save folder";
        return 1;
    }
    if (key_from_file(bf_game_dir(), pack, out, size)) {
        *from = "cdkey in the game folder";
        return 1;
    }
    return 0;
}

int bf_cdkey(const char *pack, char *out, int size)
{
    const char *from;
    return find_cdkey(pack, out, size, &from) || (pack[0] && find_cdkey("", out, size, &from));
}

static void install_values(void)
{
    static const struct { const char *key, *pack, *label; } packs[] = {
        {ERGC_MAIN, "", "Battlefield 1942"},
        {ERGC_XPACK1, "XPack1", "The Road to Rome"},
        {ERGC_XPACK2, "XPack2", "Secret Weapons of WWII"},
    };
    char key[128], lang[64];
    size_t i;
    const char *from;
    for (i = 0; i < sizeof packs / sizeof packs[0]; i++) {
        if (find_cdkey(packs[i].pack, key, sizeof key, &from)) {
            bf_log("registry: %s CD key from %s", packs[i].label, from);
        } else if (packs[i].pack[0] && find_cdkey("", key, sizeof key, &from)) {
            bf_log("registry: %s uses the Battlefield 1942 CD key", packs[i].label);
        } else {
            bf_log("registry: no %s CD key (put it in a file named cdkey in the game or save folder, or in bf1942patch.cdKey)",
                   packs[i].label);
            continue;
        }
        set_string(create_key(packs[i].key), "", key);
    }
    lstrcpynA(lang, bf_setting_str("language"), sizeof lang);
    if (!lang[0]) {
        strcpy(lang, "English");
    }
    set_string(create_key(INSTALL_KEY), "LANGUAGE", lang);
}

/* ---------------------------------------------------------------- handles */

static const char *root_name(HKEY h)
{
    switch ((uintptr_t)h) {
    case (uintptr_t)HKEY_CLASSES_ROOT: return "HKCR";
    case (uintptr_t)HKEY_CURRENT_USER: return "HKCU";
    case (uintptr_t)HKEY_LOCAL_MACHINE: return "HKLM";
    case (uintptr_t)HKEY_USERS: return "HKU";
    case (uintptr_t)HKEY_CURRENT_CONFIG: return "HKCC";
    }
    return NULL;
}

/* the key path of handle h + sub; 0 if h is not a key we know */
static int resolve(HKEY h, const char *sub, char *out)
{
    const char *base = root_name(h);
    uintptr_t slot = (uintptr_t)h - HANDLE_BASE;
    size_t n;
    if (!base) {
        if (slot >= MAX_HANDLES || !g_used[slot]) {
            return 0;
        }
        base = g_handles[slot];
    }
    lstrcpynA(out, base, MAX_PATH);
    while (sub && *sub == '\\') {
        sub++;
    }
    if (sub && *sub) {
        n = strlen(out);
        snprintf(out + n, MAX_PATH - n, "\\%s", sub);
    }
    n = strlen(out);
    while (n && out[n - 1] == '\\') {
        out[--n] = '\0';
    }
    return 1;
}

static HKEY new_handle(const char *path)
{
    int i;
    for (i = 0; i < MAX_HANDLES; i++) {
        if (!g_used[i]) {
            g_used[i] = 1;
            lstrcpynA(g_handles[i], path, MAX_PATH);
            return (HKEY)(uintptr_t)(HANDLE_BASE + i);
        }
    }
    return NULL;
}

/* "(Default)" is how 0x441d80 names the default value */
static const char *value_name(const char *name)
{
    return !name || !_stricmp(name, "(Default)") ? "" : name;
}

/* ---------------------------------------------------------------- the API */

static LONG WINAPI hk_RegOpenKeyExA(HKEY h, LPCSTR sub, DWORD opts, REGSAM sam, PHKEY out)
{
    char path[MAX_PATH];
    LONG r = ERROR_FILE_NOT_FOUND;
    (void)opts;
    (void)sam;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, sub, path)) {
        r = ERROR_INVALID_HANDLE;
    } else if (key_exists(path)) {
        *out = new_handle(path);
        r = *out ? ERROR_SUCCESS : ERROR_OUTOFMEMORY;
    }
    LeaveCriticalSection(&g_cs);
    if (g_log_calls) {
        bf_log("registry: open %s -> %ld", path, r);
    }
    return r;
}

static LONG WINAPI hk_RegOpenKeyA(HKEY h, LPCSTR sub, PHKEY out)
{
    return hk_RegOpenKeyExA(h, sub, 0, KEY_ALL_ACCESS, out);
}

static LONG WINAPI hk_RegCreateKeyA(HKEY h, LPCSTR sub, PHKEY out)
{
    char path[MAX_PATH];
    LONG r = ERROR_SUCCESS;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, sub, path)) {
        r = ERROR_INVALID_HANDLE;
    } else {
        create_key(path);
        *out = new_handle(path);
        if (!*out) {
            r = ERROR_OUTOFMEMORY;
        }
    }
    LeaveCriticalSection(&g_cs);
    if (g_log_calls) {
        bf_log("registry: create %s -> %ld", path, r);
    }
    return r;
}

static LONG WINAPI hk_RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    char path[MAX_PATH];
    LONG r = ERROR_FILE_NOT_FOUND;
    value_t *v;
    (void)res;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, NULL, path)) {
        r = ERROR_INVALID_HANDLE;
    } else if ((v = find_value(find_key(path), value_name(name))) != NULL) {
        if (type) {
            *type = v->type;
        }
        if (data && cb && *cb < v->size) {
            r = ERROR_MORE_DATA;
        } else {
            if (data) {
                memcpy(data, v->data, v->size);
            }
            r = ERROR_SUCCESS;
        }
        if (cb) {
            *cb = v->size;
        }
    }
    LeaveCriticalSection(&g_cs);
    if (g_log_calls) {
        bf_log("registry: query %s\\%s -> %ld", path, value_name(name), r);
    }
    return r;
}

static LONG WINAPI hk_RegSetValueExA(HKEY h, LPCSTR name, DWORD res, DWORD type, const BYTE *data, DWORD cb)
{
    char path[MAX_PATH];
    LONG r = ERROR_SUCCESS;
    (void)res;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, NULL, path)) {
        r = ERROR_INVALID_HANDLE;
    } else {
        set_value(create_key(path), value_name(name), type, data, data ? cb : 0);
    }
    LeaveCriticalSection(&g_cs);
    if (g_log_calls) {
        bf_log("registry: set %s\\%s (type %lu, %lu bytes) -> %ld", path, value_name(name), type, cb, r);
    }
    return r;
}

static LONG WINAPI hk_RegDeleteKeyA(HKEY h, LPCSTR sub)
{
    char path[MAX_PATH];
    LONG r = ERROR_FILE_NOT_FOUND;
    EnterCriticalSection(&g_cs);
    if (!resolve(h, sub, path)) {
        r = ERROR_INVALID_HANDLE;
    } else if (strchr(path, '\\') && key_exists(path)) {
        delete_tree(path);
        r = ERROR_SUCCESS;
    }
    LeaveCriticalSection(&g_cs);
    if (g_log_calls) {
        bf_log("registry: delete %s -> %ld", path, r);
    }
    return r;
}

static LONG WINAPI hk_RegCloseKey(HKEY h)
{
    uintptr_t slot = (uintptr_t)h - HANDLE_BASE;
    LONG r = ERROR_SUCCESS;
    EnterCriticalSection(&g_cs);
    if (slot < MAX_HANDLES && g_used[slot]) {
        g_used[slot] = 0;
    } else if (!root_name(h)) {
        r = ERROR_INVALID_HANDLE;
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

/* ---------------------------------------------------------------- install */

void bf_registry_install(void)
{
    if (!bf_setting_int("noRegistry")) {
        bf_log("registry: noRegistry 0, the game uses the registry");
        return;
    }
    InitializeCriticalSection(&g_cs);
    bf_setting_bind("logRegistry", &g_log_calls);
    install_values();
    bf_hook_import("ADVAPI32.dll", "RegOpenKeyA", hk_RegOpenKeyA);
    bf_hook_import("ADVAPI32.dll", "RegOpenKeyExA", hk_RegOpenKeyExA);
    bf_hook_import("ADVAPI32.dll", "RegCreateKeyA", hk_RegCreateKeyA);
    bf_hook_import("ADVAPI32.dll", "RegQueryValueExA", hk_RegQueryValueExA);
    bf_hook_import("ADVAPI32.dll", "RegSetValueExA", hk_RegSetValueExA);
    bf_hook_import("ADVAPI32.dll", "RegDeleteKeyA", hk_RegDeleteKeyA);
    bf_hook_import("ADVAPI32.dll", "RegCloseKey", hk_RegCloseKey);
    bf_log("registry: hooks installed, the game no longer uses the registry");
}
