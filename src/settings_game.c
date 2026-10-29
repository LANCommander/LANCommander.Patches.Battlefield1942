/* Where the options come from in the game and the Windows dedicated server, and the console commands (settings.h).
 *
 * In this order, each one winning over the ones before it:
 * - the default of the option (settings.c);
 * - bf1942patch.con next to the DLL (the proxy and the ASI), or next to BF1942.exe (the Interposer plugin): the
 *   install's options, shipped with every one of them described. A bf1942patch.ini there (before the options became
 *   console variables) is read instead when there is no bf1942patch.con;
 * - Plugins.Battlefield1942 in the Interposer's Config.yml (the plugin build, interposer_plugin.c);
 * - bf1942patch.con in the save folder (Saved Games\DICE\Battlefield 1942): the player's. The options the patch adds
 *   to the game's menus, and every option set in the console or by the game's .con files, are kept here, so they last
 *   when the game restarts itself (for every map). A Settings.ini there is turned into one the first time. The
 *   dedicated server has none;
 * - the console and the game's .con files: <console name> <value> (game.maxFps 144), from the time the game
 *   registers its own console commands (before any .con file runs). Most options take effect when the game next
 *   starts (which it does for every map); the ones a module can apply at once (bf_setting_watch, or bound to the
 *   variable it reads, bf_setting_bind) do so. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bf1942patch.h"

static char g_file[MAX_PATH];
static void (*g_source)(void);
static void (*g_watch[128])(void);

void bf_settings_set_file(const char *path)
{
    lstrcpynA(g_file, path, MAX_PATH);
}

const char *bf_settings_file(void)
{
    if (!g_file[0]) {   /* Interposer plugin: next to BF1942.exe */
        snprintf(g_file, sizeof g_file, "%sbf1942patch.con", bf_game_dir());
    }
    return g_file;
}

/* bf1942patch.con in the save folder (the install folder without one); NULL while the save folder is being worked out */
const char *bf_user_settings_file(void)
{
    static char path[MAX_PATH];
    const char *dir;
    if (!path[0]) {
        if (!(dir = bf_save_dir_ready())) {
            return NULL;
        }
        snprintf(path, sizeof path, "%sbf1942patch.con", dir[0] ? dir : bf_game_dir());
    }
    return path;
}

void bf_settings_set_source(void (*load)(void))
{
    g_source = load;
}

/* the same folder and name with another extension */
static void sibling(const char *path, const char *ext, char *out, int size)
{
    char *dot;
    lstrcpynA(out, path, size);
    if ((dot = strrchr(out, '.')) != NULL && !strchr(dot, '\\') && (int)(dot - out) + (int)strlen(ext) < size) {
        strcpy(dot, ext);
    }
}

static const char USER_HEADER[] =
    "rem Battlefield 1942 options of this player (bf1942patch). Every option of the bf1942patch.con next to the game\n"
    "rem can be set here too and wins over that file (but game.redirectSaves and game.saveFolder). The options the\n"
    "rem patch adds to the game's menus, and the ones set in the console (game.maxFps 144), are kept here.\n"
    "rem For example:\n"
    "rem game.cdKey \"XXXX-XXXX-XXXX-XXXX-XXXX\"\n"
    "rem voiceChat.proximityKey IDKey_Capital\n";

/* a new player's file: the header, then the options its Settings.ini had */
static void write_user_file(const char *path, int from_ini)
{
    char text[BF_OPT_STR_SIZE + 4];
    int i;
    FILE *f = fopen(path, "w");
    if (!f) {
        bf_log("settings: cannot create %s", path);
        return;
    }
    fputs(USER_HEADER, f);
    for (i = 0; from_ini && i < bf_n_options; i++) {
        if (bf_option_from_user(i)) {
            bf_option_text(i, text, sizeof text, 1);
            fprintf(f, "%s %s\n", bf_options[i].console, text);
        }
    }
    fclose(f);
}

void bf_settings_load(int server)
{
    const char *user;
    char old[MAX_PATH];
    int n = bf_settings_load_con(bf_settings_file(), 0);
    if (n >= 0) {
        bf_log("settings: %d options from %s", n, bf_settings_file());
    } else {
        sibling(bf_settings_file(), ".ini", old, sizeof old);
        if ((n = bf_settings_load_ini(old, 0)) >= 0) {
            bf_log("settings: %d options from %s (the options are console variables in bf1942patch.con now; see the "
                   "one the patch ships)", n, old);
        } else {
            bf_log("settings: no %s, the defaults", bf_settings_file());
        }
    }
    if (g_source) {
        g_source();
    }
    if (server || !(user = bf_user_settings_file()) || !_stricmp(user, bf_settings_file())) {
        return;
    }
    if ((n = bf_settings_load_con(user, 1)) >= 0) {
        bf_log("settings: %d options from %s", n, user);
        return;
    }
    lstrcpynA(old, user, sizeof old);
    strcpy(strrchr(old, '\\') + 1, "Settings.ini");
    n = bf_settings_load_ini(old, 1);
    write_user_file(user, n > 0);
    if (n >= 0) {
        bf_log("settings: %d options from %s, kept in %s from now on", n, old, user);
    } else {
        bf_log("settings: created %s", user);
    }
}

/* ---------------------------------------------------------------- keeping a value in the player's file */

/* the option's line in the player's file is replaced (or added): the rest of the file stays as it is */
static void keep(int i)
{
    const char *path = bf_user_settings_file();
    char tmp[MAX_PATH + 8], line[512], copy[512], text[BF_OPT_STR_SIZE + 4], *value;
    int done = 0;
    FILE *in, *out;
    if (bf_options[i].flags & BF_OPT_PROFILE) {
        return;
    }
    if (!path) {
        bf_log("settings: no player's file yet, %s is not kept", bf_options[i].console);
        return;
    }
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(out = fopen(tmp, "w"))) {
        bf_log("settings: cannot write %s", tmp);
        return;
    }
    bf_option_text(i, text, sizeof text, 1);
    if ((in = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof line, in)) {
            lstrcpynA(copy, line, sizeof copy);
            if (bf_settings_parse_line(copy, &value) == i) {
                if (!done) {
                    fprintf(out, "%s %s\n", bf_options[i].console, text);
                }
                done = 1;
                continue;
            }
            fputs(line, out);
            if (!strchr(line, '\n')) {
                fputc('\n', out);
            }
        }
        fclose(in);
    } else {
        fputs(USER_HEADER, out);
    }
    if (!done) {
        fprintf(out, "%s %s\n", bf_options[i].console, text);
    }
    fclose(out);
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        bf_log("settings: cannot replace %s (%lu)", path, GetLastError());
        DeleteFileA(tmp);
    }
}

void bf_setting_keep_int(const char *name, int value)
{
    int i = bf_option_find(name);
    if (i >= 0 && bf_setting_set_int(name, value) >= 0) {
        keep(i);
    }
}

void bf_setting_keep_str(const char *name, const char *value)
{
    int i = bf_option_find(name);
    if (i >= 0 && bf_setting_set_str(name, value) >= 0) {
        keep(i);
    }
}

void bf_setting_watch(const char *name, void (*fn)(void))
{
    int i = bf_option_find(name);
    if (i >= 0 && i < (int)(sizeof g_watch / sizeof g_watch[0])) {
        g_watch[i] = fn;
    }
}

/* ---------------------------------------------------------------- the console */

/* <console name>: prints the value; <console name> <value> sets it and keeps it in the player's file */
static void option_cmd(const char *name, int argc, const char *const *argv, char *out, int size)
{
    int i = bf_option_find(name), r;
    if (i < 0) {
        return;
    }
    if (argc < 1) {
        bf_option_text(i, out, size, 0);
        return;
    }
    if (bf_options[i].flags & BF_OPT_READONLY) {
        snprintf(out, size, "%s is set in %s", name, bf_settings_file());
        return;
    }
    if ((r = bf_option_set(i, argv[0])) < 0) {
        snprintf(out, size, "%s takes a number", name);
        return;
    }
    if (r == 0) {
        return;
    }
    bf_log("settings: %s %s", name, argv[0]);
    keep(i);
    if (i < (int)(sizeof g_watch / sizeof g_watch[0]) && g_watch[i]) {
        g_watch[i]();
    } else if (!bf_option_bound(i)) {
        snprintf(out, size, "%s takes effect when the game next starts (the next map)", name);
    }
}

void bf_settings_console_install(void)
{
    int i, n = 0;
    for (i = 0; i < bf_n_options; i++) {
        if (bf_options[i].flags & BF_OPT_SERVER) {
            continue;
        }
        if (!bf_console_add(bf_options[i].console, bf_options[i].type == BF_OPT_INT ? BF_CONSOLE_INT : BF_CONSOLE_STRING,
                            option_cmd)) {
            bf_log("settings: cannot add the console command %s", bf_options[i].console);
            return;
        }
        n++;
    }
    /* the profiles' GeneralOptions.con of earlier builds run bf1942patch.hudScale (menu.c) */
    bf_console_add("bf1942patch.hudScale", BF_CONSOLE_INT, option_cmd);
    bf_log("settings: %d options are console commands (game.maxFps [value], ...)", n);
}
