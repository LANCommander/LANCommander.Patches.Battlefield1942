/* Profiles, campaign saves, settings and screenshots in the user's Saved Games folder instead of the install folder.
 *
 * Folder: <Saved Games>\DICE\Battlefield 1942 (paths.c). The game names its files relative to the install folder
 * (its working directory), mostly with forward slashes; these move, keeping their path below the save folder:
 *     Mods\<mod>\Settings\...      the current profile (Profile.con), the profiles (Profiles\<name>\: options,
 *                                  controls, server list, buddies), the last used options and the server set-up
 *                                  (ServerSettings.con, MapList.con, banlist.con) of every mod
 *     Mods\<mod>\save\...          single player campaign progress, per profile
 *     ScreenShots\...              screenshots
 *     cache\...                    the desktop mode the game restores (orgDisplayMode.dat) and similar
 * Mods\<mod>\Settings\Default\ (the templates new profiles are made from) stays where it is. Reading a file that
 * only exists in the install folder falls back to it, so the shipped defaults (Default.con, AliasedCommands.con,
 * ...) are still found; anything written goes to the save folder. Listings (the profiles menu, the campaign
 * slots) come from the save folder only: the first time a mod is used, its existing profiles and saves are
 * copied over. A Profile.con missing from both folders is written again (restore_profile_con), naming the
 * profile changed last, instead of the game stopping with "Failed to load profile".
 *
 * Hooks: the exe's KERNEL32 file / folder / find imports and its MSVCR70 fopen, rename, remove, _mkdir, _chmod and
 * _findfirst, plus bfcprt.dll's MSVCR70 fopen (behind the std::ifstream the exe uses). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <io.h>
#include "bf1942patch.h"

static const char *g_dir;           /* save folder, with trailing backslash */
static char g_cwd[MAX_PATH];        /* working directory at startup, with trailing backslash */
static int g_on;
static int g_log_access;              /* bf1942patch.logFiles 1: every redirected path (2: every file) */
static CRITICAL_SECTION g_cs;

/* ---------------------------------------------------------------- path mapping */

static int has_prefix_i(const char *s, const char *prefix)
{
    return _strnicmp(s, prefix, strlen(prefix)) == 0;
}

/* "<word>\" at the start of s: the length of word, 0 if there is none */
static size_t segment(const char *s)
{
    const char *slash = strchr(s, '\\');
    return slash && slash > s ? (size_t)(slash - s) : 0;
}

static void migrate_mod(const char *mod, size_t len);

/* the part of rel (a path below the install folder) that moves: 1 if it does */
static int moved(const char *rel)
{
    size_t n;
    if (has_prefix_i(rel, "ScreenShots\\") || !_stricmp(rel, "ScreenShots") || has_prefix_i(rel, "cache\\")) {
        return 1;
    }
    if (!has_prefix_i(rel, "Mods\\") || !(n = segment(rel + 5))) {
        return 0;
    }
    rel += 5;
    if (has_prefix_i(rel + n + 1, "Settings\\Default\\") || !_stricmp(rel + n + 1, "Settings\\Default")) {
        return 0;
    }
    if (has_prefix_i(rel + n + 1, "Settings\\") || !_stricmp(rel + n + 1, "Settings")
        || has_prefix_i(rel + n + 1, "save\\") || !_stricmp(rel + n + 1, "save")) {
        migrate_mod(rel, n);
        return 1;
    }
    return 0;
}

/* game path -> place in the save folder (dest) and in the install (orig). 0 if the path is not one we move. */
static int map_path(const char *path, char *dest, char *orig)
{
    char full[MAX_PATH];
    const char *rel, *game = bf_game_dir();
    if (!g_on || !path || !path[0] || (path[0] == '\\' && path[1] == '\\')) {
        return 0;
    }
    if (g_log_access > 1) {
        bf_log("saves: access %s", path);
    }
    if (!GetFullPathNameA(path, MAX_PATH, full, NULL)) {   /* also turns the game's / into \ */
        return 0;
    }
    if (has_prefix_i(full, game)) {
        rel = full + strlen(game);
    } else if (has_prefix_i(full, g_cwd)) {
        rel = full + strlen(g_cwd);
    } else {
        return 0;
    }
    if (!moved(rel)) {
        return 0;
    }
    snprintf(dest, MAX_PATH, "%s%s", g_dir, rel);
    snprintf(orig, MAX_PATH, "%s%s", game, rel);
    if (g_log_access) {
        bf_log("saves: %s -> save folder (%s there, %s in the install)", path, GetFileAttributesA(dest) != INVALID_FILE_ATTRIBUTES ? "exists" : "missing",
               GetFileAttributesA(orig) != INVALID_FILE_ATTRIBUTES ? "exists" : "missing");
    }
    return 1;
}

static int exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static int ends_with_i(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !_stricmp(s + n - m, suffix);
}

/* Settings\Profile.con (game.setProfile "<name>") is missing from both folders: the game would stop with "Failed to
 * load profile" (0x6e7c60). Write one naming the most recently changed profile in Settings\Profiles, the ones the
 * player made before Custom and Default. Nothing is written if there is no profile at all. */
static void restore_profile_con(const char *dest)
{
    char pat[MAX_PATH], best[MAX_PATH] = "";
    WIN32_FIND_DATAA fd;
    FILETIME best_time = {0, 0};
    int best_rank = -1;
    HANDLE h;
    FILE *f;
    size_t dir = strlen(dest) - strlen("Profile.con");
    snprintf(pat, sizeof pat, "%.*sProfiles\\*", (int)dir, dest);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        int rank;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') {
            continue;
        }
        rank = !_stricmp(fd.cFileName, "Default") ? 0 : !_stricmp(fd.cFileName, "Custom") ? 1 : 2;
        if (rank > best_rank || (rank == best_rank && CompareFileTime(&fd.ftLastWriteTime, &best_time) > 0)) {
            best_rank = rank;
            best_time = fd.ftLastWriteTime;
            lstrcpynA(best, fd.cFileName, sizeof best);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (!best[0] || !(f = fopen(dest, "wb"))) {
        return;
    }
    fprintf(f, "rem ** %s **\ngame.setProfile \"%s\"", best, best);   /* as the game writes it */
    fclose(f);
    bf_log("saves: %s was missing; written with profile \"%s\"", dest + strlen(g_dir), best);
}

/* a path to read: the save folder's copy, or the install's if only that one exists */
static const char *read_path(const char *path, char *dest, char *orig)
{
    if (!map_path(path, dest, orig)) {
        return path;
    }
    if (!exists(dest) && !exists(orig) && ends_with_i(dest, "\\Settings\\Profile.con")) {
        restore_profile_con(dest);
    }
    return exists(dest) || !exists(orig) ? dest : orig;
}

/* a write into the install folder that stays there (logged, up to 40 of them: anything user data among them is a
 * path this module should move) */
static void note_unmoved(const char *path)
{
    static int count;
    char full[MAX_PATH];
    if (count < 40 && g_on && path && GetFullPathNameA(path, MAX_PATH, full, NULL) && has_prefix_i(full, bf_game_dir())) {
        count++;
        bf_log("saves: written in the install folder: %s", full + strlen(bf_game_dir()));
    }
}

/* a path to write: the save folder's, with its folders created */
static const char *write_path(const char *path, char *dest, char *orig)
{
    if (!map_path(path, dest, orig)) {
        note_unmoved(path);
        return path;
    }
    bf_make_dirs(dest);
    return dest;
}

/* a path to update in place (open for writing without truncating it): an install-only file is brought over first */
static const char *update_path(const char *path, char *dest, char *orig)
{
    if (!map_path(path, dest, orig)) {
        note_unmoved(path);
        return path;
    }
    bf_make_dirs(dest);
    if (!exists(dest) && exists(orig) && CopyFileA(orig, dest, TRUE)) {
        bf_log("saves: copied %s over", orig + strlen(bf_game_dir()));
    }
    return dest;
}

/* ---------------------------------------------------------------- migration */

static int copy_tree(const char *from, const char *to)
{
    char pat[MAX_PATH], a[2 * MAX_PATH], b[2 * MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;
    snprintf(pat, sizeof pat, "%s\\*", from);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    bf_make_dirs(to);
    CreateDirectoryA(to, NULL);
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) {
            continue;
        }
        snprintf(a, sizeof a, "%s\\%s", from, fd.cFileName);
        snprintf(b, sizeof b, "%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            n += copy_tree(a, b);
        } else if (CopyFileA(a, b, TRUE)) {
            n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* the first time a mod's settings are touched: its profiles, its current profile and its saves come over */
static void migrate_mod(const char *mod, size_t len)
{
    static char done[32][64];
    static int ndone;
    char name[64], from[MAX_PATH], to[MAX_PATH], marker[MAX_PATH];
    int i, n = 0;
    if (len >= sizeof name) {
        return;
    }
    memcpy(name, mod, len);
    name[len] = '\0';
    EnterCriticalSection(&g_cs);
    for (i = 0; i < ndone; i++) {
        if (!_stricmp(done[i], name)) {
            LeaveCriticalSection(&g_cs);
            return;
        }
    }
    if (ndone < 32) {
        strcpy(done[ndone++], name);
    }
    snprintf(marker, sizeof marker, "%sMods\\%s\\Settings\\Profiles", g_dir, name);
    if (!exists(marker)) {
        snprintf(from, sizeof from, "%sMods\\%s\\Settings\\Profiles", bf_game_dir(), name);
        n += copy_tree(from, marker);
        bf_make_dirs(marker);
        CreateDirectoryA(marker, NULL);
        {
            /* the files right in Settings: the current profile and the last used options and server set-up */
            WIN32_FIND_DATAA fd;
            HANDLE h;
            snprintf(from, sizeof from, "%sMods\\%s\\Settings\\*", bf_game_dir(), name);
            if ((h = FindFirstFileA(from, &fd)) != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        snprintf(from, sizeof from, "%sMods\\%s\\Settings\\%s", bf_game_dir(), name, fd.cFileName);
                        snprintf(to, sizeof to, "%sMods\\%s\\Settings\\%s", g_dir, name, fd.cFileName);
                        n += CopyFileA(from, to, TRUE) != 0;
                    }
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
        snprintf(from, sizeof from, "%sMods\\%s\\save", bf_game_dir(), name);
        snprintf(to, sizeof to, "%sMods\\%s\\save", g_dir, name);
        n += copy_tree(from, to);
        bf_log("saves: first use of mod %s, copied %d files (profiles, campaign saves) from the install folder", name, n);
    }
    LeaveCriticalSection(&g_cs);
}

/* ---------------------------------------------------------------- KERNEL32 hooks */

static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *real_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static BOOL (WINAPI *real_CreateDirectoryA)(LPCSTR, LPSECURITY_ATTRIBUTES);
static BOOL (WINAPI *real_RemoveDirectoryA)(LPCSTR);
static BOOL (WINAPI *real_DeleteFileA)(LPCSTR);
static BOOL (WINAPI *real_CopyFileA)(LPCSTR, LPCSTR, BOOL);
static BOOL (WINAPI *real_MoveFileA)(LPCSTR, LPCSTR);

static HANDLE WINAPI hk_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                    DWORD flags, HANDLE tmpl)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    int write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) != 0
                || disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == TRUNCATE_EXISTING || disp == OPEN_ALWAYS;
    const char *p;
    if (!write) {
        p = read_path(path, dest, orig);
    } else if (disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == TRUNCATE_EXISTING) {
        p = write_path(path, dest, orig);
    } else {
        p = update_path(path, dest, orig);
    }
    return real_CreateFileA(p, access, share, sa, disp, flags, tmpl);
}

/* a find: listings (profiles, campaign slots) come from the save folder only, the first use copied the old ones
 * over; a find of one file (the game tests whether its settings exist that way) falls back to the install like a
 * read, or the game takes a shipped file for missing and writes its defaults over the settings */
static const char *find_path(const char *pattern, char *dest, char *orig)
{
    if (pattern && !strpbrk(pattern, "*?")) {
        return read_path(pattern, dest, orig);
    }
    return map_path(pattern, dest, orig) ? dest : pattern;
}

static HANDLE WINAPI hk_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real_FindFirstFileA(find_path(pattern, dest, orig), fd);
}

static BOOL WINAPI hk_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    if (map_path(path, dest, orig)) {
        bf_make_dirs(dest);
        return real_CreateDirectoryA(dest, sa);
    }
    return real_CreateDirectoryA(path, sa);
}

static BOOL WINAPI hk_RemoveDirectoryA(LPCSTR path)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real_RemoveDirectoryA(map_path(path, dest, orig) ? dest : path);
}

static BOOL WINAPI hk_DeleteFileA(LPCSTR path)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real_DeleteFileA(map_path(path, dest, orig) ? dest : path);
}

static BOOL WINAPI hk_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists)
{
    char d1[MAX_PATH], o1[MAX_PATH], d2[MAX_PATH], o2[MAX_PATH];
    return real_CopyFileA(read_path(from, d1, o1), write_path(to, d2, o2), fail_if_exists);
}

static BOOL WINAPI hk_MoveFileA(LPCSTR from, LPCSTR to)
{
    char d1[MAX_PATH], o1[MAX_PATH], d2[MAX_PATH], o2[MAX_PATH];
    const char *src = read_path(from, d1, o1), *dst = write_path(to, d2, o2);
    if (src == o1 && dst == d2) {   /* an install-only file moving into the save folder: copy, leave the install as is */
        return real_CopyFileA(src, dst, TRUE);
    }
    return real_MoveFileA(src, dst);
}

/* ---------------------------------------------------------------- MSVCR70 hooks */

static FILE *(__cdecl *real_fopen)(const char *, const char *);
static FILE *(__cdecl *real_bfcprt_fopen)(const char *, const char *);
static int (__cdecl *real_rename)(const char *, const char *);
static int (__cdecl *real_remove)(const char *);
static int (__cdecl *real__mkdir)(const char *);
static int (__cdecl *real__chmod)(const char *, int);
static intptr_t (__cdecl *real__findfirst)(const char *, void *);      /* struct _finddata_t of MSVCR70 */

static const char *fopen_path(const char *path, const char *mode, char *dest, char *orig)
{
    if (!mode || (!strchr(mode, 'w') && !strchr(mode, 'a') && !strchr(mode, '+'))) {
        return read_path(path, dest, orig);
    }
    return strchr(mode, 'w') ? write_path(path, dest, orig) : update_path(path, dest, orig);
}

static FILE *__cdecl hk_fopen(const char *path, const char *mode)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real_fopen(fopen_path(path, mode, dest, orig), mode);
}

static FILE *__cdecl hk_bfcprt_fopen(const char *path, const char *mode)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real_bfcprt_fopen(fopen_path(path, mode, dest, orig), mode);
}

static int __cdecl hk_rename(const char *from, const char *to)
{
    char d1[MAX_PATH], o1[MAX_PATH], d2[MAX_PATH], o2[MAX_PATH];
    const char *src = read_path(from, d1, o1), *dst = write_path(to, d2, o2);
    if (src == o1 && dst == d2) {
        return real_CopyFileA(src, dst, TRUE) ? 0 : -1;
    }
    return real_rename(src, dst);
}

static int __cdecl hk_remove(const char *path)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real_remove(map_path(path, dest, orig) ? dest : path);
}

static int __cdecl hk__mkdir(const char *path)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    if (map_path(path, dest, orig)) {
        bf_make_dirs(dest);
        return real__mkdir(dest);
    }
    return real__mkdir(path);
}

static int __cdecl hk__chmod(const char *path, int mode)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real__chmod(read_path(path, dest, orig), mode);
}

static intptr_t __cdecl hk__findfirst(const char *pattern, void *fd)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    return real__findfirst(find_path(pattern, dest, orig), fd);
}

/* ---------------------------------------------------------------- install */

#define HOOK(dll, name) real_##name = bf_hook_import(dll, #name, hk_##name)

void bf_saves_install(void)
{
    size_t n;
    HMODULE bfcprt;
    g_dir = bf_save_dir();
    if (!g_dir[0]) {
        bf_log("saves: profiles and saves stay in the install folder");
        return;
    }
    InitializeCriticalSection(&g_cs);
    bf_setting_bind("logFiles", &g_log_access);
    GetCurrentDirectoryA(MAX_PATH - 1, g_cwd);
    n = strlen(g_cwd);
    if (n && g_cwd[n - 1] != '\\') {
        strcat(g_cwd, "\\");
    }
    HOOK("KERNEL32.dll", CreateFileA);
    HOOK("KERNEL32.dll", FindFirstFileA);
    HOOK("KERNEL32.dll", CreateDirectoryA);
    HOOK("KERNEL32.dll", RemoveDirectoryA);
    HOOK("KERNEL32.dll", DeleteFileA);
    HOOK("KERNEL32.dll", CopyFileA);
    HOOK("KERNEL32.dll", MoveFileA);
    HOOK("MSVCR70.dll", fopen);
    HOOK("MSVCR70.dll", rename);
    HOOK("MSVCR70.dll", remove);
    HOOK("MSVCR70.dll", _mkdir);
    HOOK("MSVCR70.dll", _chmod);
    HOOK("MSVCR70.dll", _findfirst);
    if ((bfcprt = GetModuleHandleA("bfcprt.dll")) != NULL) {
        real_bfcprt_fopen = bf_hook_import_in(bfcprt, "MSVCR70.dll", "fopen", hk_bfcprt_fopen);
    }
    if (!real_CreateFileA || !real_FindFirstFileA || !real_fopen || !real__findfirst) {
        bf_log("saves: file imports missing, redirection is off");
        return;
    }
    if (!real_CreateDirectoryA) real_CreateDirectoryA = CreateDirectoryA;
    if (!real_RemoveDirectoryA) real_RemoveDirectoryA = RemoveDirectoryA;
    if (!real_DeleteFileA) real_DeleteFileA = DeleteFileA;
    if (!real_CopyFileA) real_CopyFileA = CopyFileA;
    if (!real_MoveFileA) real_MoveFileA = MoveFileA;
    if (!real_rename) real_rename = rename;
    if (!real_remove) real_remove = remove;
    if (!real__mkdir) real__mkdir = _mkdir;
    if (!real__chmod) real__chmod = _chmod;
    if (!real_bfcprt_fopen) real_bfcprt_fopen = real_fopen;
    g_on = 1;
    bf_log("saves: folder %s", g_dir);
}
