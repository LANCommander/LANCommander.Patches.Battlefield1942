/* The HUD scale in the game's own Options -> General page, as a console command, and in the profile.
 *
 * (re/menu_notes.md)
 * - Console command game.hudScale [percent]: a console method
 *   object of our own, laid out like the game's (vtable copied from game.setMinimapTransparency at 0x91fca0, invoke
 *   at +0x40 and isActive at +0x44 replaced), handed to ConsoleObjects::registerList (0x5ac970, registry 0x9ab610)
 *   right after the menu module registers its own commands (its vtable slot 0x8d2148 -> 0x6b4d80, before any .con
 *   file runs). Every option is added the same way (bf_console_add, settings_game.c): a property like hudScale, and
 *   the other modules add methods taking a std::string like game.addPlayerToBuddyListByName (0x418480).
 * - The profile: SaveGeneralOptions (0x6d5a20) rewrites Settings/Profiles/<profile>/GeneralOptions.con from the
 *   page's fields; its last line is written by the call at 0x6d61d4 (FileHandle::write 0x5839f0). That call is
 *   redirected to write "game.hudScale <n>" after it, so the value is saved with the profile and runs with
 *   it (at start-up and whenever the page loads).
 * - The slider: the page is data (MemeFile 2.0 "menu/GeneralOptionsMenu" in menu.rfa). When the meme stream opener
 *   (0x601e80) hands out that file, the minimap transparency row is cloned in memory into a "HUD SCALE" row
 *   (50..200) below the crosshair colour block, bound to the named variable Options/General/HudScale. Every byte the
 *   splice relies on is checked first; any other version of the file is left alone. The variable is bound to the
 *   patch's own int by GeneralOptions::registerVariables (0x6d4d10, vtable slot 0x923968), which runs before the
 *   page loads, through bindInt (0x69eb50). The slider writes the int as it moves, so the HUD follows live.
 * The starting value is game.hudScale from bf1942patch.con (default 100); a profile's saved value wins.
 *
 * The other modules add rows to the options pages the same way: bf_menu_add_row (one copied row, framerate.c), and
 * bf_menu_graft (voice_menu.c) for rows copied from another page (opened through the original opener as the donor
 * and closed again: the stream's deleting destructor, vtable slot 0), with whole nodes replaced by new ones (a NULL,
 * an IntData, a WstringData bound to a variable, a literal StringData). Every node of the copied content is listed
 * with its class, so the copy's string indices (class names and variables) are moved to the page's table, which
 * gets the names it lacks at its end, and the nodes around a replaced one get its new size. Text variables are
 * bound by bindWstring (0x69b8f0, like bindInt) and changed through their data node (vtable +0x50, set(manager,
 * text)). A page can take several grafts (voice_hud.c and voice_scoreboard.c both graft menu/InGame): they are
 * worked out on the file as shipped and spliced in together, each chain in place of its own NULL next. A graft can
 * also change bytes of the page itself in place (voice_scoreboard.c moves two of the game's buttons out of sight). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bf1942patch.h"

#define HUD_MIN 50
#define HUD_MAX 200

static int g_hud_scale = 100;
static int g_on;

int bf_hud_scale(void)
{
    return g_hud_scale < HUD_MIN ? HUD_MIN : g_hud_scale > HUD_MAX ? HUD_MAX : g_hud_scale;
}

static void set_scale(int pct)
{
    pct = pct < HUD_MIN ? HUD_MIN : pct > HUD_MAX ? HUD_MAX : pct;
    if (pct != g_hud_scale) {
        bf_log("menu: HUD scale %d%%", pct);
    }
    g_hud_scale = pct;
}

/* game.hudScale <percent>, in the console or the profile's GeneralOptions.con */
static void hud_scale_changed(void)
{
    set_scale(g_hud_scale);
}

/* ---------------------------------------------------------------- the console command */

#define REGISTRY        0x9ab610u
#define REGISTER_LIST   0x5ac970u
#define METHOD_VTBL     0x91fca0u    /* game.setMinimapTransparency's vtable: 25 entries */
#define MENU_REGISTER_SLOT 0x8d2148u /* menu module "register console objects" -> 0x6b4d80 */
#define MENU_REGISTER   0x6b4d80u

#define MAX_COMMANDS    96
#define MAX_ARGS        8
#define STD_STRING      28           /* sizeof std::string: argv is an array of them */

static struct command {
    BYTE obj[0xb0];                  /* the method object (first: the console hands it back as self) */
    bf_console_fn fn;
    const char *name;                /* object.method */
    char object[32];                 /* the object's name (the method's is in name) */
} g_cmds[MAX_COMMANDS];
static int g_ncmds;
static void *g_cmd_vtbl[25];
static struct node { struct node *next, *prev; void *value; } g_sentinel, g_elems[MAX_COMMANDS];
static struct { DWORD alloc; struct node *head; DWORD size; } g_list;

/* invoke(std::string *ret, const std::string *argv, int argc): ret is what the console prints */
static void *__attribute__((thiscall)) cmd_invoke(void *self, void *ret, const BYTE *argv, int argc)
{
    const char *args[MAX_ARGS];
    char text[320];
    int i;
    text[0] = '\0';
    argc = argc < 0 ? 0 : argc > MAX_ARGS ? MAX_ARGS : argc;
    for (i = 0; i < argc; i++) {
        args[i] = bf_string_chars(argv + i * STD_STRING);
    }
    for (i = 0; i < g_ncmds; i++) {
        if (self == g_cmds[i].obj) {
            g_cmds[i].fn(g_cmds[i].name, argc, args, text, sizeof text);
        }
    }
    bf_string_init(ret, text);
    return ret;
}

static char __attribute__((thiscall)) cmd_active(void *self)
{
    (void)self;
    return 1;
}

static char __attribute__((thiscall)) cmd_none(void *self)
{
    (void)self;
    return 0;
}

/* every command added, in one list */
static void register_commands(void)
{
    static int done;
    void **tmpl = (void **)BF_ADDR(METHOD_VTBL);
    struct node *prev = &g_sentinel;
    int i;
    if (done || !g_ncmds) {
        return;
    }
    done = 1;
    for (i = 0; i < 25; i++) {
        g_cmd_vtbl[i] = tmpl[i];
    }
    g_cmd_vtbl[0x40 / 4] = (void *)cmd_invoke;
    g_cmd_vtbl[0x44 / 4] = (void *)cmd_active;
    g_cmd_vtbl[0x48 / 4] = (void *)cmd_none;   /* execute / getResult / parseArg: the invoke above does it all */
    g_cmd_vtbl[0x4c / 4] = (void *)cmd_none;
    g_cmd_vtbl[0x50 / 4] = (void *)cmd_none;
    for (i = 0; i < g_ncmds; i++) {
        BYTE *c = g_cmds[i].obj;
        *(void ***)(c + 0x00) = g_cmd_vtbl;
        g_elems[i].value = c;
        g_elems[i].prev = prev;
        prev->next = &g_elems[i];
        prev = &g_elems[i];
    }
    prev->next = &g_sentinel;
    g_sentinel.prev = prev;
    g_list.head = &g_sentinel;
    g_list.size = (DWORD)g_ncmds;
    ((char (__attribute__((thiscall)) *)(void *, void *))BF_ADDR(REGISTER_LIST))(BF_ADDR(REGISTRY), &g_list);
    for (i = 0; i < g_ncmds; i++) {
        bf_log("menu: console command %s registered", g_cmds[i].name);
    }
}

static char __attribute__((thiscall)) hk_menu_register(void *module)
{
    char r = ((char (__attribute__((thiscall)) *)(void *))BF_ADDR(MENU_REGISTER))(module);
    register_commands();
    return r;
}

int bf_console_add(const char *name, int kind, bf_console_fn fn)
{
    static int hooked;
    int property = kind != BF_CONSOLE_METHOD;
    const char *dot = strchr(name, '.');
    struct command *cmd = &g_cmds[g_ncmds];
    BYTE *c;
    if (g_ncmds == MAX_COMMANDS || !dot || dot - name >= (int)sizeof cmd->object
        || (!hooked && !(hooked = bf_hook_slot(MENU_REGISTER_SLOT, MENU_REGISTER, hk_menu_register)))) {
        return 0;
    }
    g_ncmds++;
    c = cmd->obj;
    cmd->fn = fn;
    cmd->name = name;
    lstrcpynA(cmd->object, name, (int)(dot - name) + 1);
    *(DWORD *)(c + 0x04) = property ? 1 : 0;            /* a property: no argument reads it */
    *(const char **)(c + 0x08) = cmd->object;           /* the console's key is object.method: an object of the game's
                                                         * (game, renderer) takes the new method beside its own */
    *(const char **)(c + 0x0c) = dot + 1;
    *(DWORD *)(c + 0x10) = 3;                           /* typed in the console and run from .con files */
    *(DWORD *)(c + 0x14) = property ? 0 : 1;            /* a method's argument */
    *(DWORD *)(c + 0x18) = 1;
    *(const char **)(c + 0x4c) = "std::string";
    *(const char **)(c + 0x50) = kind == BF_CONSOLE_INT ? "int" : "std::string";
    *(DWORD *)(c + 0x78) = 0;
    c[0xa1] = 1;
    return 1;
}

/* ---------------------------------------------------------------- saving with the profile */

#define WRITE_LINE      0x5839f0u    /* FileHandle::write(const std::string *) */
#define LAST_LINE_CALL  0x6d61d4u    /* chat.setTimeUntilMessageRemoved, the last line of GeneralOptions.con */

static int __attribute__((thiscall)) hk_write_last(void *fh, const void *line)
{
    typedef int (__attribute__((thiscall)) *write_fn)(void *, const void *);
    BYTE s[BF_STRING_SIZE];
    char text[48];
    int r = ((write_fn)BF_ADDR(WRITE_LINE))(fh, line);
    snprintf(text, sizeof text, "game.hudScale %d\n", bf_hud_scale());
    if (bf_string_init(s, text)) {
        ((write_fn)BF_ADDR(WRITE_LINE))(fh, s);
        bf_string_free(s);
    }
    return r;
}

/* ---------------------------------------------------------------- the variable */

#define REGISTER_VARS_SLOT 0x923968u /* GeneralOptions::registerVariables -> 0x6d4d10 */
#define REGISTER_VARS   0x6d4d10u
#define BIND_INT        0x69eb50u    /* __fastcall(meme manager, int *, const std::string *name, const std::string *path) */

int bf_menu_bind_int(void *page, const char *path, const char *name, int *var)
{
    typedef char (__attribute__((fastcall)) *bind_int_fn)(void *mgr, int *var, const void *name, const void *path);
    BYTE n[BF_STRING_SIZE], p[BF_STRING_SIZE];
    char ok = 0;
    if (!bf_string_init(n, name)) {
        return 0;
    }
    if (bf_string_init(p, path)) {
        ok = ((bind_int_fn)BF_ADDR(BIND_INT))(*(void **)((BYTE *)page + 4), var, n, p);
        bf_string_free(p);
    }
    bf_string_free(n);
    bf_log("menu: %s/%s %s", path, name, ok ? "bound" : "not bound");
    return ok;
}

#define BIND_WSTRING    0x69b8f0u    /* __fastcall(meme manager, std::wstring *, const std::string *name, const std::string *path) */
#define WSTRING_SET     0x50         /* the data node's vtable: set(manager, const wchar_t *) */

/* the node copies the text (only its c_str is read), so a temporary std::wstring does */
void *bf_meme_bind_wstring(void *mgr, const char *path, const char *name, const wchar_t *text)
{
    typedef void *(__attribute__((fastcall)) *bind_wstring_fn)(void *mgr, void *var, const void *name, const void *path);
    BYTE n[BF_STRING_SIZE], p[BF_STRING_SIZE], w[BF_STRING_SIZE];
    void *node = NULL;
    if (!bf_wstring_init(w, text)) {
        return NULL;
    }
    if (bf_string_init(n, name)) {
        if (bf_string_init(p, path)) {
            node = ((bind_wstring_fn)BF_ADDR(BIND_WSTRING))(mgr, w, n, p);
            bf_string_free(p);
        }
        bf_string_free(n);
    }
    bf_wstring_free(w);
    return node;
}

void bf_meme_set_wstring(void *mgr, void *node, const wchar_t *text)
{
    if (node) {
        ((void (__attribute__((thiscall)) *)(void *, void *, const wchar_t *))(*(void ***)node)[WSTRING_SET / 4])(
            node, mgr, text);
    }
}

void *bf_menu_bind_wstring(void *page, const char *path, const char *name, const wchar_t *text)
{
    void *node = bf_meme_bind_wstring(*(void **)((BYTE *)page + 4), path, name, text);
    bf_log("menu: %s/%s %s", path, name, node ? "bound" : "not bound");
    return node;
}

void bf_menu_set_wstring(void *page, void *node, const wchar_t *text)
{
    bf_meme_set_wstring(*(void **)((BYTE *)page + 4), node, text);
}

/* a variable bound to *var, as bindInt does it: the data object (operator new 0x7ecb40; IntData 0x7e9be0, BoolData
 * 0x7e3500, FloatData 0x7e4730, thiscall(object, var, name)) registered under path (0x7ebb70, thiscall(manager,
 * object, path)) */
#define MEME_NEW      0x7ecb40u
#define MEME_REGISTER 0x7ebb70u

int bf_meme_bind(void *mgr, const char *path, const char *name, int type, void *var)
{
    static const struct { uint32_t ctor; size_t size; } TYPES[] = {{0x7e9be0, 0x14}, {0x7e3500, 0x10}, {0x7e4730, 0x14}};
    void *obj = ((void *(__cdecl *)(size_t))BF_ADDR(MEME_NEW))(TYPES[type].size);
    if (!obj) {
        return 0;
    }
    obj = ((void *(__attribute__((thiscall)) *)(void *, void *, const char *))BF_ADDR(TYPES[type].ctor))(obj, var, name);
    if (!obj) {
        return 0;
    }
    ((void (__attribute__((thiscall)) *)(void *, void *, const char *))BF_ADDR(MEME_REGISTER))(mgr, obj, path);
    return 1;
}

/* a function the menus call by name (a Function node, e.g. a button's CallFunctionAction), as the scoreboard registers
 * its own (0x6e0070): a dice::meme::Function (0x7f2f50, thiscall(object, name), which copies the name) with the
 * vtable of the scoreboard's member functions (0x924de0: +0x40 calls +0xc with ecx = +8), registered under path */
#define FUNCTION_CTOR 0x7f2f50u
#define FUNCTION_VTBL 0x924de0u

int bf_meme_bind_function(void *mgr, const char *path, const char *name, void *owner, bf_meme_fn fn)
{
    BYTE *obj = ((void *(__cdecl *)(size_t))BF_ADDR(MEME_NEW))(0x10);
    if (!obj) {
        return 0;
    }
    ((void *(__attribute__((thiscall)) *)(void *, const char *))BF_ADDR(FUNCTION_CTOR))(obj, name);
    *(DWORD *)obj = FUNCTION_VTBL;
    *(void **)(obj + 8) = owner;
    *(bf_meme_fn *)(obj + 0xc) = fn;
    ((void (__attribute__((thiscall)) *)(void *, void *, const char *))BF_ADDR(MEME_REGISTER))(mgr, obj, path);
    return 1;
}

static int __attribute__((thiscall)) hk_register_vars(void *page)
{
    int r = ((int (__attribute__((thiscall)) *)(void *))BF_ADDR(REGISTER_VARS))(page);
    bf_menu_bind_int(page, "Options/General", "HudScale", &g_hud_scale);
    return r;
}

/* ---------------------------------------------------------------- new rows on the options pages */

#define MENU_OPEN 0x601e80u          /* meme platform: open "menu/<name>", thiscall(mgr, const char *name), ret 4 */

static DWORD rd32(const BYTE *p) { DWORD v; memcpy(&v, p, 4); return v; }
static void wr32(BYTE *p, DWORD v) { memcpy(p, &v, 4); }
static void wr16(BYTE *p, WORD v) { memcpy(p, &v, 2); }

/* menu/GeneralOptionsMenu of 1.61 with a HUD scale row (port of re/menu/splice_poc.py): the minimap transparency
 * row's content, 50..200, under the crosshair colour's BLUE slider (about y 232) at its rows' spacing, inside the
 * panel (it ends at about 265) */
static const float GEN_ROW_RECT[4] = {5, 170, 360, 60}, GEN_SLIDER[3] = {0, 100, 2}, GEN_NEW_SLIDER[3] = {HUD_MIN, HUD_MAX, 3};
static const DWORD GEN_ANCESTORS[] = {0x6f6, 0x6fe, 0x706, 0x70e, 0x793, 0x79b, 0x7bb, 0x7c3, 0x7cb, 0x7d3, 0x7e3,
                                      0x7eb, 0x7f3, 0x7fb, 0x803, 0x80b};
static const bf_meme_bytes GEN_BYTES[] = {
    {0xefb, 16, GEN_ROW_RECT, NULL},                   /* the transparency row's box */
    {0xfce, 8, "\x0c\0\0\0\x21\0\x08\0", NULL},        /* IntData (class 8) of string 33: number box, slider */
    {0x1049, 8, "\x0c\0\0\0\x21\0\x08\0", NULL},
    {0x1055, 12, GEN_SLIDER, GEN_NEW_SLIDER},          /* the slider's min, max, step */
};
static const bf_meme_row GEN_ROW = {
    .size = 8527, .strings_end = 0x6f3,
    .var = "Options/General/HudScale", .var_index = 72,
    .content = 0xf0b, .content_len = 0x260,
    .ints = {0xfce, 0x1049}, .int_default = 100,
    .label = 0x1125, .label_len = 0x30, .string_class = 29, .label_text = "HUD SCALE:",
    .label_parents = {0x1103, 0x10f3, 0xf0b},
    .rect = {5, 248, 360, 20}, .hang_at = 0x813,       /* after the crosshair row */
    .ancestors = GEN_ANCESTORS, .n_ancestors = sizeof GEN_ANCESTORS / sizeof GEN_ANCESTORS[0],
    .bytes = GEN_BYTES, .n_bytes = sizeof GEN_BYTES / sizeof GEN_BYTES[0],
};

/* the page with the new row; NULL if the file is not the one row was worked out on */
BYTE *bf_meme_add_row(const bf_meme_row *row, const BYTE *src, DWORD size, DWORD *out_size)
{
    static const BYTE null_next[8] = {8, 0, 0, 0, 0, 0, 0, 0};
    const DWORD content = row->content, content_end = row->content + row->content_len;
    DWORD nlen = (DWORD)strlen(row->var), llen = (DWORD)strlen(row->label_text), sdlen = 12 + llen;
    DWORD clen, rlen, i, at, lo = row->label - content;
    int d3 = (int)sdlen - (int)row->label_len;
    BYTE *child, *out, *p;

    if (size != row->size || size < 13 || memcmp(src, "\x0cMemeFile 2.0", 13) || row->strings_end < 11
        || row->strings_end >= size || src[row->strings_end] != 0
        || memcmp(src + row->strings_end - 11, "\x0aIsMainMenu", 11) || content_end > size
        || rd32(src + content) != row->content_len || row->label < content || row->label + row->label_len > content_end
        || rd32(src + row->label) != row->label_len || row->hang_at + 8 > size
        || memcmp(src + row->hang_at, null_next, 8) || nlen > 255) {
        return NULL;
    }
    for (i = 0; i < 2; i++) {
        if (row->ints[i] < content || row->ints[i] + 12 > content_end) {
            return NULL;
        }
    }
    for (i = 0; i < 3; i++) {
        if (row->label_parents[i] < content || row->label_parents[i] > row->label) {
            return NULL;
        }
    }
    for (i = 0; i < row->n_bytes; i++) {
        const bf_meme_bytes *b = &row->bytes[i];
        if (b->at + b->len > size || memcmp(src + b->at, b->was, b->len)
            || (b->now && (b->at < content || b->at + b->len > content_end))) {
            return NULL;
        }
    }
    for (i = 0; i < row->n_ancestors; i++) {
        DWORD a = row->ancestors[i];
        if (!(a > row->strings_end && a < row->hang_at && row->hang_at < a + rd32(src + a))) {
            return NULL;
        }
    }

    /* 1. the content, re-pointed at the new variable, with its new bytes and a literal label (StringData: no
     * lexicon has the key) */
    clen = (DWORD)((int)row->content_len + d3);
    if (!(child = malloc(row->content_len + sdlen))) {
        return NULL;
    }
    memcpy(child, src + content, row->content_len);
    for (i = 0; i < 2; i++) {
        p = child + row->ints[i] - content;
        wr16(p + 4, row->var_index);
        wr32(p + 8, row->int_default);
    }
    for (i = 0; i < row->n_bytes; i++) {
        if (row->bytes[i].now) {
            memcpy(child + row->bytes[i].at - content, row->bytes[i].now, row->bytes[i].len);
        }
    }
    memmove(child + lo + sdlen, child + lo + row->label_len, row->content_len - lo - row->label_len);
    p = child + lo;
    wr32(p, sdlen);
    wr16(p + 4, 0);
    wr16(p + 6, row->string_class);
    wr32(p + 8, llen);
    memcpy(p + 12, row->label_text, llen);
    for (i = 0; i < 3; i++) {
        p = child + row->label_parents[i] - content;
        wr32(p, (DWORD)((int)rd32(p) + d3));
    }

    /* 2. the variable's name at the end of the string table, the nodes around hang_at grown by the new row */
    rlen = 32 + clen;
    *out_size = size + 1 + nlen + rlen - 8;
    if (!(out = malloc(*out_size))) {
        free(child);
        return NULL;
    }
    memcpy(out, src, row->strings_end);
    at = row->strings_end;
    out[at++] = (BYTE)nlen;
    memcpy(out + at, row->var, nlen);
    at += nlen;
    memcpy(out + at, src + row->strings_end, row->hang_at - row->strings_end);
    for (i = 0; i < row->n_ancestors; i++) {
        p = out + row->ancestors[i] + 1 + nlen;
        wr32(p, rd32(p) + rlen - 8);
    }
    at += row->hang_at - row->strings_end;

    /* 3. the row in place of the NULL next: TransformNode {next = NULL, rect, content} */
    p = out + at;
    wr32(p, rlen);
    wr16(p + 4, 0);
    wr16(p + 6, 4);                  /* TransformNode */
    memcpy(p + 8, null_next, 8);
    memcpy(p + 16, row->rect, 16);
    memcpy(p + 32, child, clen);
    at += rlen;
    memcpy(out + at, src + row->hang_at + 8, size - row->hang_at - 8);
    free(child);
    return out;
}

/* ---------------------------------------------------------------- grafted rows */

#define MAX_STRINGS 1024
#define MAX_ADDED   96

/* a MemeFile's string table: the offset of each entry (index 1..n), its length byte first */
typedef struct {
    const BYTE *f;
    DWORD at[MAX_STRINGS];
    int n;
    char added[MAX_ADDED][64];       /* names appended to the table (only for the page) */
    int n_added;
} meme_strings;

static int read_strings(meme_strings *t, const BYTE *f, DWORD size, DWORD strings_end)
{
    DWORD at = 13;
    t->f = f;
    t->n = 0;
    t->n_added = 0;
    if (size < 16 || memcmp(f, "\x0cMemeFile 2.0", 13) || strings_end >= size) {
        return 0;
    }
    while (at < strings_end && f[at]) {
        if (t->n == MAX_STRINGS - 1) {
            return 0;
        }
        t->at[++t->n] = at;
        at += 1 + f[at];
    }
    return at == strings_end && f[at] == 0;
}

static int string_is(const meme_strings *t, int i, const char *prefix, const char *s)
{
    size_t lp = prefix ? strlen(prefix) : 0, ls = strlen(s);
    const BYTE *e;
    if (i < 1 || i > t->n) {
        return 0;
    }
    e = t->f + t->at[i];
    return e[0] == lp + ls && (!lp || !memcmp(e + 1, prefix, lp)) && !memcmp(e + 1 + lp, s, ls);
}

/* the index of name in the page's table, added at its end if it is not there; 0 if the table is full */
static int string_index(meme_strings *t, const char *name)
{
    int i;
    for (i = 1; i <= t->n; i++) {
        if (string_is(t, i, NULL, name)) {
            return i;
        }
    }
    for (i = 0; i < t->n_added; i++) {
        if (!strcmp(t->added[i], name)) {
            return t->n + 1 + i;
        }
    }
    if (t->n_added == MAX_ADDED || strlen(name) >= sizeof t->added[0]) {
        return 0;
    }
    strcpy(t->added[t->n_added++], name);
    return t->n + t->n_added;
}

/* the donor's string i in the page's table (0 stays 0); -1 if there is no room */
static int move_index(meme_strings *page, const meme_strings *donor, int i)
{
    char name[256];
    const BYTE *e;
    if (!i) {
        return 0;
    }
    if (i > donor->n) {
        return -1;
    }
    e = donor->f + donor->at[i];
    memcpy(name, e + 1, e[0]);
    name[e[0]] = '\0';
    return string_index(page, name) ? string_index(page, name) : -1;
}

/* the size of a replacement node for one of was bytes (at most that plus 300) */
static DWORD edit_len(const bf_meme_edit *e, DWORD was)
{
    switch (e->kind) {
    case BF_MEME_NULL:
        return 8;
    case BF_MEME_STRING:
        return 12 + (DWORD)strlen(e->text);
    case BF_MEME_BOOL:
        return 9;
    case BF_MEME_CLASS:
        return was;
    case BF_MEME_PICTURE:
        return 17 + (DWORD)strlen(e->text);   /* {next = NULL, the texture: u8 length, characters} */
    case BF_MEME_FUNCTION:
        return 8;
    default:
        return 12;
    }
}

/* the content a row copies */
static const bf_meme_piece *piece_of(const bf_meme_graft *g, const bf_meme_graft_row *r, bf_meme_piece *own)
{
    own->content = g->content;
    own->content_len = g->content_len;
    own->nodes = g->nodes;
    own->n_nodes = g->n_nodes;
    own->donor = g->donor;
    own->donor_size = g->donor_size;
    own->donor_strings_end = g->donor_strings_end;
    return r->piece ? r->piece : own;
}

/* the piece is in the donor, its first node is the content, and node i's class is dice::meme::<cls> */
static int piece_ok(const bf_meme_piece *pc, const meme_strings *d, DWORD size)
{
    DWORD end = pc->content + pc->content_len, i, len;
    if (!pc->n_nodes || pc->n_nodes > 256 || pc->content < 13 || end > size || pc->nodes[0].at != pc->content
        || rd32(d->f + pc->content) != pc->content_len) {
        return 0;
    }
    for (i = 0; i < pc->n_nodes; i++) {
        const bf_meme_node *n = &pc->nodes[i];
        if (n->at < pc->content || n->at + 8 > end || (i && n->at <= pc->nodes[i - 1].at)) {
            return 0;
        }
        len = rd32(d->f + n->at);
        if (len < 8 || n->at + len > end || !string_is(d, *(const WORD *)(d->f + n->at + 6), "dice::meme::", n->cls)) {
            return 0;
        }
    }
    return 1;
}

/* a data node {size, variable, class, value} of the page's dice::meme::<cls>; 0 if the table is full */
static int data_node(BYTE *p, meme_strings *page, const char *var, const char *cls)
{
    char name[64];
    int a = var ? string_index(page, var) : 0, c;
    snprintf(name, sizeof name, "dice::meme::%s", cls);
    c = string_index(page, name);
    wr16(p + 4, (WORD)a);
    wr16(p + 6, (WORD)c);
    return (!var || a) && c;
}

/* row r's copy of its content into out (room for content_len plus the replacements); returns its length, 0 if a
 * table does not fit the file */
static DWORD graft_content(const bf_meme_piece *pc, const bf_meme_graft_row *r, meme_strings *page, meme_strings *donor,
                           BYTE *out, DWORD *out_at)
{
    const BYTE *src = donor->f;
    DWORD end = pc->content + pc->content_len, pos = pc->content, q = 0, i, k, e = 0;
    int delta[256];
    for (i = 0; i < pc->n_nodes; i++) {
        const bf_meme_node *n = &pc->nodes[i];
        DWORD len = rd32(src + n->at);
        int a, c;
        if (n->at < pos) {
            return 0;                /* inside a replaced node */
        }
        memcpy(out + q, src + pos, n->at - pos);
        q += n->at - pos;
        out_at[i] = q;
        delta[i] = 0;
        if (e < r->n_edits && r->edits[e].at == n->at) {
            const bf_meme_edit *x = &r->edits[e++];
            BYTE *p = out + q;
            DWORD nl = edit_len(x, len);
            if (x->was_var && !string_is(donor, *(const WORD *)(src + n->at + 4), NULL, x->was_var)) {
                return 0;
            }
            wr32(p, nl);
            switch (x->kind) {
            case BF_MEME_NULL:
                memset(p + 4, 0, 4);
                break;
            case BF_MEME_INT:
                wr32(p + 8, (DWORD)x->value);
                if (!data_node(p, page, x->var, "IntData")) {
                    return 0;
                }
                break;
            case BF_MEME_BOOL:
                p[8] = (BYTE)(x->value != 0);
                if (!data_node(p, page, x->var, "BoolData")) {
                    return 0;
                }
                break;
            case BF_MEME_FLOAT:
                memcpy(p + 8, &x->real, 4);
                if (!data_node(p, page, x->var, "FloatData")) {
                    return 0;
                }
                break;
            case BF_MEME_WSTRING:
                wr32(p + 8, 0);      /* no characters */
                if (!data_node(p, page, x->var, "WstringData")) {
                    return 0;
                }
                break;
            case BF_MEME_CLASS:      /* the node as it is, but its class (and so its variable moved too) */
                memcpy(p, src + n->at, len);
                a = move_index(page, donor, *(const WORD *)(src + n->at + 4));
                if (a < 0 || !data_node(p, page, NULL, x->text)) {
                    return 0;
                }
                wr16(p + 4, (WORD)a);
                break;
            case BF_MEME_PICTURE:
                memcpy(p + 8, "\x08\0\0\0\0\0\0\0", 8);
                p[16] = (BYTE)strlen(x->text);
                memcpy(p + 17, x->text, strlen(x->text));
                if (strlen(x->text) > 255 || !data_node(p, page, NULL, "PictureNode")) {
                    return 0;
                }
                break;
            case BF_MEME_FUNCTION:   /* {size, the function's name, class}: nothing else */
                if (!data_node(p, page, x->var, "Function")) {
                    return 0;
                }
                break;
            default:
                wr32(p + 8, (DWORD)strlen(x->text));
                memcpy(p + 12, x->text, strlen(x->text));
                if (!data_node(p, page, NULL, "StringData")) {
                    return 0;
                }
            }
            delta[i] = (int)nl - (int)len;
            q += nl;
            pos = n->at + len;
            continue;
        }
        a = move_index(page, donor, *(const WORD *)(src + n->at + 4));
        c = move_index(page, donor, *(const WORD *)(src + n->at + 6));
        if (a < 0 || c <= 0) {
            return 0;
        }
        memcpy(out + q, src + n->at, 8);
        wr16(out + q + 4, (WORD)a);
        wr16(out + q + 6, (WORD)c);
        q += 8;
        pos = n->at + 8;
    }
    if (e != r->n_edits) {
        return 0;                    /* an edit of a node that is not listed */
    }
    memcpy(out + q, src + pos, end - pos);
    q += end - pos;
    /* the changed bytes, at their place in the copy */
    for (k = 0; k < r->n_bytes; k++) {
        const bf_meme_bytes *b = &r->bytes[k];
        DWORD at = b->at - pc->content;
        if (b->at < pc->content || b->at + b->len > end || memcmp(src + b->at, b->was, b->len)) {
            return 0;
        }
        /* moved by the replaced nodes before it */
        for (i = 0; i < pc->n_nodes; i++) {
            DWORD len = rd32(src + pc->nodes[i].at);
            if (delta[i] && pc->nodes[i].at + len <= b->at) {
                at = (DWORD)((int)at + delta[i]);
            } else if (delta[i] && pc->nodes[i].at < b->at + b->len) {
                return 0;            /* inside a replaced node */
            }
        }
        if (b->now) {
            memcpy(out + at, b->now, b->len);
        }
    }
    /* every node around a replaced one grows (or shrinks) by its new size */
    for (i = 0; i < pc->n_nodes; i++) {
        DWORD len = rd32(src + pc->nodes[i].at);
        int grow = 0;
        for (k = i + 1; k < pc->n_nodes; k++) {
            if (delta[k] && pc->nodes[k].at < pc->nodes[i].at + len) {
                grow += delta[k];
            }
        }
        if (!delta[i]) {             /* (a replacement already has its size) */
            wr32(out + out_at[i], (DWORD)((int)len + grow));
        }
    }
    return q;
}

#define MAX_DONORS 4

/* the string table of the piece's donor (the page itself without one), read the first time; NULL if it is not there */
static meme_strings *donor_of(const bf_meme_piece *pc, meme_strings *page, DWORD size, const bf_meme_donor *donors,
                              int n_donors, meme_strings **tables, DWORD *donor_size)
{
    int i;
    if (!pc->donor) {
        *donor_size = size;
        return page;
    }
    for (i = 0; i < n_donors && i < MAX_DONORS; i++) {
        if (_stricmp(donors[i].name, pc->donor)) {
            continue;
        }
        if (donors[i].size != pc->donor_size) {
            return NULL;
        }
        if (!tables[i] && (!(tables[i] = malloc(sizeof *tables[i]))
                           || !read_strings(tables[i], donors[i].data, donors[i].size, pc->donor_strings_end))) {
            return NULL;
        }
        *donor_size = donors[i].size;
        return tables[i];
    }
    return NULL;
}

static const BYTE null_next[8] = {8, 0, 0, 0, 0, 0, 0, 0};

/* g's rows, chained: row k = TransformNode {next = row k+1 (or NULL), rect, content}; malloc'd, NULL if a file is
 * not the one the graft was worked out on. Names the copies need are added to the page's table. */
static BYTE *graft_chain(const bf_meme_graft *g, meme_strings *page, const BYTE *src, DWORD size,
                         const bf_meme_donor *donors, int n_donors, meme_strings **tables, DWORD *chain_len)
{
    meme_strings *donor;
    BYTE *rows = NULL, *p;
    DWORD *out_at = NULL, room = 0, rows_len = 0, i, at, donor_size;
    DWORD row_at[32], row_len[32];
    bf_meme_piece own;
    int tn;
    if (g->n_rows > 32 || size != g->size || g->hang_at + 8 > size || g->hang_at <= g->strings_end
        || memcmp(src + g->hang_at, null_next, 8)) {
        goto done;
    }
    for (i = 0; i < g->n_ancestors; i++) {
        DWORD a = g->ancestors[i];
        if (!(a > g->strings_end && a < g->hang_at && g->hang_at < a + rd32(src + a))) {
            goto done;
        }
    }
    for (i = 0; i < g->n_bytes; i++) {
        const bf_meme_bytes *b = &g->bytes[i];
        if (b->at <= g->strings_end || b->at + b->len > size || !b->now || memcmp(src + b->at, b->was, b->len)) {
            goto done;
        }
    }
    /* the rows' contents */
    for (i = 0; i < g->n_rows; i++) {
        const bf_meme_piece *pc = piece_of(g, &g->rows[i], &own);
        DWORD k;
        if (!(donor = donor_of(pc, page, size, donors, n_donors, tables, &donor_size))
            || !piece_ok(pc, donor, donor_size)) {
            goto done;
        }
        room += 32 + pc->content_len;
        for (k = 0; k < g->rows[i].n_edits; k++) {
            room += edit_len(&g->rows[i].edits[k], pc->content_len);
        }
    }
    room += 8;
    if (!(rows = malloc(room)) || !(out_at = malloc(256 * sizeof *out_at))) {
        goto done;
    }
    at = 0;
    for (i = 0; i < g->n_rows; i++) {
        const bf_meme_piece *pc = piece_of(g, &g->rows[i], &own);
        row_at[i] = at;
        donor = donor_of(pc, page, size, donors, n_donors, tables, &donor_size);
        if (!(row_len[i] = graft_content(pc, &g->rows[i], page, donor, rows + at, out_at))) {
            goto done;
        }
        at += row_len[i];
    }
    tn = string_index(page, "dice::meme::TransformNode");
    if (!tn) {
        goto done;
    }
    /* nest them: built back to front in a second buffer */
    {
        BYTE *chain = malloc(room);
        DWORD len = 8;
        if (!chain) {
            goto done;
        }
        memcpy(chain, null_next, 8);
        for (i = g->n_rows; i-- > 0;) {
            DWORD total = 8 + len + 16 + row_len[i];
            memmove(chain + 8, chain, len);
            p = chain;
            wr32(p, total);
            wr16(p + 4, 0);
            wr16(p + 6, (WORD)tn);
            memcpy(p + 8 + len, g->rows[i].rect, 16);
            memcpy(p + 8 + len + 16, rows + row_at[i], row_len[i]);
            len = total;
        }
        free(rows);
        rows = chain;
        rows_len = len;
    }
    *chain_len = rows_len;
    free(out_at);
    return rows;
done:
    free(rows);
    free(out_at);
    return NULL;
}

/* where a node of the page as shipped is in the grafted one: after the new names, and moved by every chain hung before
 * it (each in place of an 8-byte NULL) */
static DWORD grafted_at(DWORD at, DWORD added, const bf_meme_graft *const *gs, const DWORD *lens, int n)
{
    DWORD to = at + added;
    int i;
    for (i = 0; i < n; i++) {
        if (gs[i]->hang_at < at) {
            to += lens[i] - 8;
        }
    }
    return to;
}

BYTE *bf_meme_graft_page(const bf_meme_graft *const *gs, int n, const BYTE *src, DWORD size,
                         const bf_meme_donor *donors, int n_donors, DWORD *out_size)
{
    meme_strings *page = malloc(sizeof *page), *tables[MAX_DONORS] = {NULL, NULL, NULL, NULL};
    BYTE *chains[BF_MEME_MAX_GRAFTS] = {NULL}, *out = NULL, *p;
    DWORD lens[BF_MEME_MAX_GRAFTS], added = 0, grown = 0, strings_end, at, pos, i;
    int k, j, order[BF_MEME_MAX_GRAFTS];
    if (!page || n < 1 || n > BF_MEME_MAX_GRAFTS) {
        goto done;
    }
    strings_end = gs[0]->strings_end;
    if (!read_strings(page, src, size, strings_end)) {
        goto done;
    }
    for (k = 0; k < n; k++) {
        for (j = 0; j < k; j++) {
            if (gs[j]->hang_at == gs[k]->hang_at) {
                goto done;
            }
        }
        /* the bytes changed in place stay clear of every chain's NULL */
        for (i = 0; i < gs[k]->n_bytes; i++) {
            const bf_meme_bytes *b = &gs[k]->bytes[i];
            for (j = 0; j < n; j++) {
                if (b->at < gs[j]->hang_at + 8 && gs[j]->hang_at < b->at + b->len) {
                    goto done;
                }
            }
        }
        if (gs[k]->strings_end != strings_end
            || !(chains[k] = graft_chain(gs[k], page, src, size, donors, n_donors, tables, &lens[k]))) {
            goto done;
        }
        grown += lens[k] - 8;
    }
    /* the chains in the order of their places in the page */
    for (k = 0; k < n; k++) {
        for (j = k; j > 0 && gs[order[j - 1]]->hang_at > gs[k]->hang_at; j--) {
            order[j] = order[j - 1];
        }
        order[j] = k;
    }
    for (i = 0; i < (DWORD)page->n_added; i++) {
        added += 1 + (DWORD)strlen(page->added[i]);
    }
    *out_size = size + added + grown;
    if (!(out = malloc(*out_size))) {
        goto done;
    }
    /* the table with its new names, then the page with each chain in place of its NULL next, then the rest */
    memcpy(out, src, strings_end);
    at = strings_end;
    for (i = 0; i < (DWORD)page->n_added; i++) {
        DWORD l = (DWORD)strlen(page->added[i]);
        out[at++] = (BYTE)l;
        memcpy(out + at, page->added[i], l);
        at += l;
    }
    pos = strings_end;
    for (k = 0; k < n; k++) {
        const bf_meme_graft *g = gs[order[k]];
        memcpy(out + at, src + pos, g->hang_at - pos);
        at += g->hang_at - pos;
        memcpy(out + at, chains[order[k]], lens[order[k]]);
        at += lens[order[k]];
        pos = g->hang_at + 8;
    }
    memcpy(out + at, src + pos, size - pos);
    /* every node around a hang point grows by its chain */
    for (k = 0; k < n; k++) {
        for (i = 0; i < gs[k]->n_ancestors; i++) {
            p = out + grafted_at(gs[k]->ancestors[i], added, gs, lens, n);
            wr32(p, rd32(p) + lens[k] - 8);
        }
        for (i = 0; i < gs[k]->n_bytes; i++) {
            const bf_meme_bytes *b = &gs[k]->bytes[i];
            memcpy(out + grafted_at(b->at, added, gs, lens, n), b->now, b->len);
        }
    }
done:
    free(page);
    for (i = 0; i < MAX_DONORS; i++) {
        free(tables[i]);
    }
    for (k = 0; k < BF_MEME_MAX_GRAFTS; k++) {
        free(chains[k]);
    }
    return out;
}

BYTE *bf_meme_graft_rows(const bf_meme_graft *g, const BYTE *src, DWORD size, const bf_meme_donor *donors,
                         int n_donors, DWORD *out_size)
{
    return bf_meme_graft_page(&g, 1, src, size, donors, n_donors, out_size);
}

/* ---------------------------------------------------------------- the opener */

typedef void *(__attribute__((thiscall)) *open_fn)(void *mgr, const char *name);
typedef void *(__attribute__((thiscall)) *buf_data_fn)(void *buf);
typedef void (__attribute__((thiscall)) *buf_resize_fn)(void *buf, DWORD size);

static open_fn g_menu_open;

#define MAX_SPLICES 8
static struct { const char *page; const bf_meme_row *row; const bf_meme_graft *graft; const char *what; } g_splices[MAX_SPLICES];
static int g_nsplices;

/* the bytes of an opened menu stream, if size of them can be read */
static BYTE *stream_data(void *is, DWORD size)
{
    void *buf = *(void **)((BYTE *)is + 8);
    BYTE *data = buf ? ((buf_data_fn)(*(void ***)buf)[0x34 / 4])(buf) : NULL;
    return data && !IsBadReadPtr(data, size) ? data : NULL;
}

/* the stream's bytes become patched */
static int stream_replace(void *is, const BYTE *patched, DWORD size)
{
    void *buf = *(void **)((BYTE *)is + 8);
    BYTE *data;
    ((buf_resize_fn)(*(void ***)buf)[0x38 / 4])(buf, size);
    data = ((buf_data_fn)(*(void ***)buf)[0x34 / 4])(buf);
    if (data) {
        memcpy(data, patched, size);
    }
    return data != NULL;
}

/* the grafts for an opened page: their donor pages are opened (and closed again: the stream's deleting destructor,
 * vtable slot 0) through the original opener */
static BYTE *graft_page(void *mgr, const bf_meme_graft *const *gs, int n_gs, const BYTE *data, DWORD *size)
{
    bf_meme_donor donors[MAX_DONORS];
    void *streams[MAX_DONORS];
    BYTE *patched = NULL;
    int n = 0, i, k, x;
    DWORD r;
    for (x = 0; x < n_gs; x++) {
        const bf_meme_graft *g = gs[x];
        for (r = 0; r <= g->n_rows; r++) {
            /* the graft's donor, then each row's */
            const char *name = r == g->n_rows ? g->donor : g->rows[r].piece ? g->rows[r].piece->donor : NULL;
            DWORD dsize = r == g->n_rows ? g->donor_size : g->rows[r].piece ? g->rows[r].piece->donor_size : 0;
            for (k = 0; name && k < n && _stricmp(donors[k].name, name); k++) {
            }
            if (!name || k < n) {
                continue;
            }
            if (n == MAX_DONORS || !(streams[n] = g_menu_open(mgr, name))) {
                goto done;
            }
            donors[n].name = name;
            donors[n].size = dsize;
            donors[n].data = stream_data(streams[n], dsize);
            n++;
            if (!donors[n - 1].data) {
                goto done;
            }
        }
    }
    patched = bf_meme_graft_page(gs, n_gs, data, gs[0]->size, donors, n, size);
done:
    for (i = 0; i < n; i++) {
        ((void (__attribute__((thiscall)) *)(void *, int))(*(void ***)streams[i])[0])(streams[i], 1);
    }
    return patched;
}

/* a page gets one row (bf_menu_add_row), or every graft made for it at once (they are worked out on the file as
 * shipped) */
static void *__attribute__((thiscall)) hk_menu_open(void *mgr, const char *name)
{
    void *is = g_menu_open(mgr, name);
    const bf_meme_graft *grafts[BF_MEME_MAX_GRAFTS];
    const char *what[BF_MEME_MAX_GRAFTS];
    const bf_meme_row *row = NULL;
    BYTE *data, *patched = NULL;
    DWORD size;
    int i, n = 0, ok;
    if (!is || !name) {
        return is;
    }
    for (i = 0; i < g_nsplices && !row; i++) {
        if (_stricmp(name, g_splices[i].page)) {
            continue;
        }
        if (g_splices[i].row) {
            row = g_splices[i].row;
            what[0] = g_splices[i].what;
            n = 1;
        } else if (n < BF_MEME_MAX_GRAFTS) {
            grafts[n] = g_splices[i].graft;
            what[n++] = g_splices[i].what;
        }
    }
    if (!n) {
        return is;
    }
    if ((data = stream_data(is, row ? row->size : grafts[0]->size)) != NULL) {
        patched = row ? bf_meme_add_row(row, data, row->size, &size) : graft_page(mgr, grafts, n, data, &size);
    }
    ok = patched && stream_replace(is, patched, size);
    for (i = 0; i < n; i++) {
        if (ok) {
            bf_log("menu: %s added to menu/%s", what[i], name);
        } else {
            bf_log("menu: menu/%s is not the one the patch knows, no %s", name, what[i]);
        }
    }
    free(patched);
    return is;
}

static int add_splice(const char *page, const bf_meme_row *row, const bf_meme_graft *graft, const char *what)
{
    static const BYTE open_head[7] = {0x8b, 0x44, 0x24, 0x04, 0x83, 0xec, 0x38};   /* mov eax,[esp+4]; sub esp,0x38 */
    if (g_nsplices == MAX_SPLICES) {
        return 0;
    }
    if (!g_menu_open && !(g_menu_open = (open_fn)bf_detour(MENU_OPEN, open_head, sizeof open_head, hk_menu_open))) {
        return 0;
    }
    g_splices[g_nsplices].page = page;
    g_splices[g_nsplices].row = row;
    g_splices[g_nsplices].graft = graft;
    g_splices[g_nsplices].what = what;
    g_nsplices++;
    return 1;
}

/* adds row to menu/<page> whenever the game opens it. The opener 0x601e80 is detoured the first time. */
int bf_menu_add_row(const char *page, const bf_meme_row *row, const char *what)
{
    return add_splice(page, row, NULL, what);
}

/* grafts g's rows onto menu/<page> whenever the game opens it, together with the page's other grafts */
int bf_menu_graft(const char *page, const bf_meme_graft *g, const char *what)
{
    return add_splice(page, NULL, g, what);
}

/* ---------------------------------------------------------------- install */

void bf_menu_install(void)
{
    bf_setting_bind("hudScale", &g_hud_scale);   /* the slider, the profile and the console share it */
    set_scale(g_hud_scale);
    bf_setting_watch("hudScale", hud_scale_changed);
    if (!bf_setting_int("hudScaleOption")) {
        bf_log("menu: hudScaleOption 0, no HUD scale slider (the HUD scale is only game.hudScale)");
        return;
    }
    g_on = 1;
    if (!bf_redirect_call(LAST_LINE_CALL, WRITE_LINE, hk_write_last)) {
        bf_log("menu: GeneralOptions.con is written differently, the HUD scale is not saved with the profile");
    }
    if (*(DWORD *)BF_ADDR(REGISTER_VARS_SLOT) == REGISTER_VARS
        && bf_menu_add_row("GeneralOptionsMenu", &GEN_ROW, "the HUD scale slider")) {
        bf_hook_slot(REGISTER_VARS_SLOT, REGISTER_VARS, hk_register_vars);
    } else {
        bf_log("menu: the options page code differs, no HUD scale slider");
    }
    bf_log("menu: HUD scale %d%% (Options -> General, game.hudScale)", g_hud_scale);
}
