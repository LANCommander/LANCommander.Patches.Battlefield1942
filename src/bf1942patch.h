#ifndef BF1942PATCH_H
#define BF1942PATCH_H
#include <windows.h>
#include <stdint.h>

#ifndef BF_VERSION
#define BF_VERSION "dev"             /* build.sh passes the release tag */
#endif

int bf_patch_start(void);            /* exe check + Enabled + not loaded twice; 1 = go ahead and install the modules */
void bf_install(void);               /* installs the patch modules */
void bf_log(const char *fmt, ...);
void bf_standalone_init(HINSTANCE dll);   /* proxy / ASI DllMain: settings and log next to the DLL, then the patches */

/* where settings and log lines go: bf1942patch.ini / bf1942patch.log (proxy, ASI), or callbacks (Interposer plugin) */
void bf_set_files(const char *ini, const char *log);
const char *bf_log_file(void);       /* "" with the Interposer backend */
void bf_set_backend(int (*config_int)(const char *section, const char *key, int def), void (*log_line)(const char *line));
void bf_set_string_backend(int (*config_str)(const char *section, const char *key, char *out, int size));
HMODULE bf_module(void);             /* this DLL */
void bf_set_module(HMODULE dll);

/* memory helpers (all addresses are BF1942.exe VAs, image base 0x400000, no relocations) */
int bf_write(uint32_t addr, const void *bytes, size_t len);
int bf_expect(uint32_t addr, const void *bytes, size_t len);    /* 1 if memory matches */
int bf_patch(uint32_t addr, const void *expect, const void *bytes, size_t len);
int bf_jmp(uint32_t from, const void *to);    /* E9 rel32 at from */
int bf_call(uint32_t from, const void *to);   /* E8 rel32 at from */
int bf_redirect_call(uint32_t site, uint32_t old_target, const void *to);   /* E8 rel32 at site, if it calls old_target */
/* the len bytes at site (if they are expect) become a call to `to`, padded with NOPs */
int bf_replace_call(uint32_t site, const void *expect, size_t len, const void *to);
/* a detour: the len bytes at fn (whole instructions, no relative jumps or calls) are checked against prologue, copied
 * into a trampoline that goes on at fn + len, and replaced by a jump to hook. Returns the trampoline (the original
 * function, called like it), NULL if the bytes differ. */
void *bf_detour(uint32_t fn, const void *prologue, size_t len, const void *hook);
int bf_hook_slot(uint32_t slot, uint32_t expect, const void *hook);   /* a vtable entry, if it holds expect */

/* settings: [section] key in Settings.ini in the save folder, else in bf1942patch.ini (or
 * Plugins.Battlefield1942.<key> in the Interposer's Config.yml) */
int bf_ini_int(const char *section, const char *key, int def);
int bf_ini_str(const char *section, const char *key, char *out, int size);   /* "" if missing */
const char *bf_ini_file(void);       /* bf1942patch.ini (next to BF1942.exe with the Interposer plugin) */
const char *bf_user_ini_file(void);  /* Settings.ini; NULL while the save folder is not known yet */
void bf_ini_set_int(const char *section, const char *key, int value);   /* into Settings.ini (or bf1942patch.ini) */
void bf_ini_set_str(const char *section, const char *key, const char *value);

void *bf_hook_import(const char *dll, const char *name, void *replacement);  /* returns the old target */
void *bf_hook_import_in(HMODULE module, const char *dll, const char *name, void *replacement);

/* paths.c: the install folder and the user's save folder (both with a trailing backslash) */
const char *bf_game_dir(void);
void bf_set_game_dir(const char *dir);   /* the restart helper (rundll32) works for a game it is not part of */
const char *bf_save_dir(void);       /* "" when saves are not redirected */
const char *bf_save_dir_ready(void); /* bf_save_dir(), or NULL from within its own setting lookups */
void bf_make_dirs(const char *path); /* creates every missing parent folder of path (and path itself if it ends in \) */

/* registry.c: the CD key the game is given (the main game's, or an expansion's: "", "XPack1", "XPack2") */
int bf_cdkey(const char *pack, char *out, int size);

/* patch modules */
void bf_crashlog_install(void);      /* crashlog.c: [Debug] CrashLog=1 logs crashes with a call chain */
void bf_saves_install(void);         /* saves.c: profiles, campaign saves and settings in Saved Games */
void bf_registry_install(void);      /* registry.c: no registry; the CD keys from a file or the settings */
void bf_master_install(void);        /* master.c: the server list and server reporting through OpenSpy */
void bf_window_install(void);        /* window.c: borderless window, no display mode changes */
void bf_restart_install(void);       /* restart.c: seamless restarts (the game relaunches itself for each map) */
void bf_widescreen_install(void);    /* widescreen.c: Hor+ view and the HUD at its own shape */
void bf_gamepad_install(void);       /* gamepad.c: SDL gamepads */
void bf_framerate_install(void);     /* framerate.c: no 60 / 100 fps cap, HUD timing in real time */
void bf_detail_install(void);        /* detail.c: mesh, tree and terrain LOD farther out (not the view distance) */
void bf_voice_install(void);         /* voice.c: voice chat (LANCommander Comms), its HUD, keys and sound options */

/* restart.c: exported for rundll32 (the helper that bridges a restart) */
void CALLBACK bf_restart_helper(HWND hwnd, HINSTANCE inst, LPSTR cmdline, int show);

/* gamepad.c: the DirectInput 8 object the game created (an IDirectInput8A is wrapped to add the SDL gamepads) */
void *bf_gamepad_wrap_dinput(const GUID *iid, void *direct_input);

/* the game's std::string (MSVC 7 layout, 28 bytes; constructed by bfcprt.dll) */
#define BF_STRING_SIZE 32
int bf_string_init(void *mem, const char *s);   /* 0 if bfcprt.dll lacks the functions */
void bf_string_free(void *mem);
const char *bf_string_chars(const void *s);
/* the game's std::wstring (basic_string<unsigned short>: the same 28 bytes, 8 characters inline) */
int bf_wstring_init(void *mem, const wchar_t *s);
void bf_wstring_free(void *mem);
const wchar_t *bf_wstring_chars(const void *s);
typedef struct { BYTE b[28]; } bf_msvc_string;   /* either, passed by value to the game's functions (callee destroys) */

/* menu.c: the HUD scale in percent (the General options slider, bf1942patch.hudScale, [Widescreen] HudScale) */
int bf_hud_scale(void);
void bf_menu_install(void);

/* menu.c: a new row on an options page (MemeFile 2.0 menu/<page> in menu.rfa), added as the game opens the page: a
 * copy of an existing row's content, bound to a new variable, with a literal label, hung on the NULL next of the
 * page's last row. Offsets are into the file as shipped. Every byte the splice relies on is checked first; any other
 * version of the file is left alone. */
typedef struct {
    DWORD at, len;
    const void *was;                 /* the bytes there */
    const void *now;                 /* what they become in the copied content, or NULL (only checked) */
} bf_meme_bytes;

typedef struct {
    DWORD size;                      /* the file as shipped */
    DWORD strings_end;               /* the 0 that ends the string table (right after "IsMainMenu") */
    const char *var;                 /* the new variable, e.g. "Options/General/HudScale" */
    WORD var_index;                  /* its string index: the table's entry count + 1 */
    DWORD content, content_len;      /* the row content that is copied */
    DWORD ints[2];                   /* its IntData nodes, re-pointed at var with int_default */
    DWORD int_default;
    DWORD label, label_len;          /* its label's data node, replaced by a StringData of label_text */
    WORD string_class;               /* StringData's class index in this file */
    const char *label_text;
    DWORD label_parents[3];          /* the nodes in content around the label, outermost (content) last */
    float rect[4];                   /* the new row's box */
    DWORD hang_at;                   /* the NULL next it is hung on */
    const DWORD *ancestors;          /* every node around hang_at */
    DWORD n_ancestors;
    const bf_meme_bytes *bytes;      /* more bytes to check, or to change in the copy */
    DWORD n_bytes;
} bf_meme_row;

BYTE *bf_meme_add_row(const bf_meme_row *row, const BYTE *src, DWORD size, DWORD *out_size);   /* malloc'd, or NULL */
int bf_menu_add_row(const char *page, const bf_meme_row *row, const char *what);   /* 0 if the opener is not hooked */
/* binds the menu variable <path>/<name> to *var (a page's registerVariables, page+4 is the meme manager) */
int bf_menu_bind_int(void *page, const char *path, const char *name, int *var);
/* a text variable <path>/<name> (WstringData) starting as text; returns its data node, NULL if it failed */
void *bf_menu_bind_wstring(void *page, const char *path, const char *name, const wchar_t *text);
void bf_menu_set_wstring(void *page, void *node, const wchar_t *text);   /* new text for a bound one */
/* the same with the meme manager itself (the in-game HUD's variables are in BfMenu's, not a page's) */
enum { BF_MENU_INT, BF_MENU_BOOL, BF_MENU_FLOAT };
int bf_meme_bind(void *mgr, const char *path, const char *name, int type, void *var);
void *bf_meme_bind_wstring(void *mgr, const char *path, const char *name, const wchar_t *text);
void bf_meme_set_wstring(void *mgr, void *node, const wchar_t *text);

/* menu.c: rows grafted onto a page as the game opens it: copies of a row's content from the page itself or from
 * another page (the donor), with whole nodes replaced and some bytes changed, hung one after the other on a NULL
 * next. Every node of the content is listed (but NULLs and the inside of replaced nodes) with its class, so the
 * copy's string indices are moved to the page's string table (names it lacks are added) and the nodes around a
 * replaced one get its new size. Offsets are into the files as shipped; any other version is left alone. */
typedef struct { DWORD at; const char *cls; } bf_meme_node;   /* cls without "dice::meme::" */

enum { BF_MEME_NULL, BF_MEME_INT, BF_MEME_WSTRING, BF_MEME_STRING, BF_MEME_BOOL, BF_MEME_FLOAT, BF_MEME_CLASS,
       BF_MEME_PICTURE };
typedef struct {
    DWORD at;                        /* the node replaced (one of the listed nodes) */
    const char *was_var;             /* its variable as shipped, or NULL (not checked) */
    int kind;                        /* what it becomes: */
    const char *var;                 /* INT, BOOL, FLOAT, WSTRING: bound to var (INT, BOOL, FLOAT: NULL = a constant) */
    int value;                       /* INT, BOOL */
    const char *text;                /* STRING: a literal StringData; CLASS: the same node as another class
                                      * (dice::meme::<text>, of the same layout); PICTURE: a PictureNode of texture text */
    float real;                      /* FLOAT */
} bf_meme_edit;

/* a row content to copy, and every node of it (but NULLs and the inside of replaced ones), in file order */
typedef struct {
    DWORD content, content_len;
    const bf_meme_node *nodes;
    DWORD n_nodes;
    const char *donor;               /* the page it is in; NULL = the graft's donor */
    DWORD donor_size, donor_strings_end;
} bf_meme_piece;

typedef struct {
    float rect[4];                   /* the row's box */
    const bf_meme_edit *edits;       /* in file order */
    DWORD n_edits;
    const bf_meme_bytes *bytes;      /* bytes changed in the copy (outside the replaced nodes) */
    DWORD n_bytes;
    const bf_meme_piece *piece;      /* the content copied; NULL = the graft's */
} bf_meme_graft_row;

typedef struct {
    DWORD size, strings_end;         /* the page as shipped, the 0 that ends its string table */
    DWORD hang_at;                   /* the NULL next the rows are hung on */
    const DWORD *ancestors;          /* every node around hang_at */
    DWORD n_ancestors;
    const char *donor;               /* the page the content is copied from; NULL = this one */
    DWORD donor_size, donor_strings_end;
    DWORD content, content_len;      /* the row content copied, in the donor */
    const bf_meme_node *nodes;       /* in file order, the first one is the content */
    DWORD n_nodes;
    const bf_meme_graft_row *rows;
    DWORD n_rows;
} bf_meme_graft;

/* an opened donor page */
typedef struct {
    const char *name;
    const BYTE *data;
    DWORD size;
} bf_meme_donor;

/* the page with the rows; malloc'd, or NULL if a file is not the one the graft was worked out on */
BYTE *bf_meme_graft_rows(const bf_meme_graft *g, const BYTE *page, DWORD size, const bf_meme_donor *donors,
                         int n_donors, DWORD *out_size);
int bf_menu_graft(const char *page, const bf_meme_graft *g, const char *what);   /* 0 if the opener is not hooked */

/* window.c: the game's main window (NULL until it exists) and the rectangle of the monitor it is on */
HWND bf_game_window(void);
void bf_game_monitor(RECT *r);
void bf_dpi_aware(void);             /* real pixels for this process (the game, the restart helper) */

#define BF_ADDR(a) ((void *)(uintptr_t)(a))
#endif
