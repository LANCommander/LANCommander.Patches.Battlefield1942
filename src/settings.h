/* The patch's options. Every option is a console variable bf1942patch.<name>: a line "bf1942patch.<name> <value>" in a
 * bf1942patch.con file (read by the patch itself as it loads, before the game runs any code), and a console command of
 * the same name in the game (settings_game.c), so the console and the game's .con files read and set it too.
 * Plain C with no Windows headers: the Linux server build (linux/server_linux.c) compiles it too. */
#ifndef BF_SETTINGS_H
#define BF_SETTINGS_H

enum { BF_OPT_INT, BF_OPT_STR };

enum {
    BF_OPT_INSTALL = 1,              /* only from the install's bf1942patch.con (or Config.yml): it decides where the
                                      * player's file is */
    BF_OPT_READONLY = 2,             /* the console reads it, but cannot set it */
    BF_OPT_SERVER = 4,               /* the dedicated server's: no console command in the game */
    BF_OPT_PROFILE = 8,              /* the game saves it with the profile: not kept in the player's file */
};

typedef struct {
    const char *name;                /* bf1942patch.<name> */
    int type, flags;
    int def;                         /* BF_OPT_INT */
    const char *def_str;             /* BF_OPT_STR */
    const char *section, *key;       /* where the old bf1942patch.ini / Settings.ini had it */
} bf_option;

#define BF_OPT_STR_SIZE 264          /* a path, with room to spare */

extern const bf_option bf_options[];
extern const int bf_n_options;

int bf_option_find(const char *name);                /* its index, -1 if there is no such option */
int bf_setting_int(const char *name);
const char *bf_setting_str(const char *name);        /* "" for none; valid until the option is set again */
int bf_option_int(int i);
const char *bf_option_str(int i);
/* binds an int option to the module's own variable (it gets the current value): reads and sets go through it, so a
 * menu that writes the variable and the console see the same value */
void bf_setting_bind(const char *name, int *var);
int bf_option_bound(int i);          /* bound: the module reads it as it goes, so a new value applies at once */
/* a new value from text (a number, true / false / on / off / yes / no, or a string): 1 if it changed, 0 if it is the
 * same, -1 if it is not a number */
int bf_option_set(int i, const char *value);
int bf_setting_set_int(const char *name, int value);
int bf_setting_set_str(const char *name, const char *value);
/* the value as a .con file writes it: 1, "text" (quoted = 1), or as the console prints it (quoted = 0) */
void bf_option_text(int i, char *out, int size, int quoted);

/* "bf1942patch.<name> <value>" lines; rem / ; / # / // lines and other commands are skipped. Options flagged
 * BF_OPT_INSTALL are skipped in a player's file (user = 1). The options set are marked as the player's (user = 1).
 * The count set, -1 if the file cannot be read. */
int bf_settings_load_con(const char *path, int user);
/* the same from the old bf1942patch.ini / Settings.ini ([Section] Key=value) */
int bf_settings_load_ini(const char *path, int user);
int bf_option_from_user(int i);      /* set by the player's file */
/* one line of a .con file: the option it sets (-1 if none) and the value's text (in place) */
int bf_settings_parse_line(char *line, char **value);

void bf_log(const char *fmt, ...);
#endif
