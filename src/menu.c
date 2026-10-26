/* The HUD scale in the game's own Options -> General page, as a console command, and in the profile.
 *
 * (re/menu_notes.md)
 * - Console command bf1942patch.hudScale [percent] (also bf1942patch.setHudScale / getHudScale): a console method
 *   object of our own, laid out like the game's (vtable copied from game.setMinimapTransparency at 0x91fca0, invoke
 *   at +0x40 and isActive at +0x44 replaced), handed to ConsoleObjects::registerList (0x5ac970, registry 0x9ab610)
 *   right after the menu module registers its own commands (its vtable slot 0x8d2148 -> 0x6b4d80, before any .con
 *   file runs).
 * - The profile: SaveGeneralOptions (0x6d5a20) rewrites Settings/Profiles/<profile>/GeneralOptions.con from the
 *   page's fields; its last line is written by the call at 0x6d61d4 (FileHandle::write 0x5839f0). That call is
 *   redirected to write "bf1942patch.hudScale <n>" after it, so the value is saved with the profile and runs with
 *   it (at start-up and whenever the page loads).
 * - The slider: the page is data (MemeFile 2.0 "menu/GeneralOptionsMenu" in menu.rfa). When the meme stream opener
 *   (0x601e80) hands out that file, the minimap transparency row is cloned in memory into a "HUD SCALE" row
 *   (50..200) below the crosshair colour block, bound to the named variable Options/General/HudScale. Every byte the
 *   splice relies on is checked first; any other version of the file is left alone. The variable is bound to the
 *   patch's own int by GeneralOptions::registerVariables (0x6d4d10, vtable slot 0x923968), which runs before the
 *   page loads, through bindInt (0x69eb50). The slider writes the int as it moves, so the HUD follows live.
 * The starting value is [Widescreen] HudScale (default 100); a profile's saved value wins. */
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

/* ---------------------------------------------------------------- the console command */

#define REGISTRY        0x9ab610u
#define REGISTER_LIST   0x5ac970u
#define METHOD_VTBL     0x91fca0u    /* game.setMinimapTransparency's vtable: 25 entries */
#define MENU_REGISTER_SLOT 0x8d2148u /* menu module "register console objects" -> 0x6b4d80 */
#define MENU_REGISTER   0x6b4d80u

static BYTE g_cmd[0xb0];             /* the method object */
static void *g_cmd_vtbl[25];
static struct node { struct node *next, *prev; void *value; } g_sentinel, g_elem;
static struct { DWORD alloc; struct node *head; DWORD size; } g_list;

/* invoke(std::string *ret, const std::string *argv, int argc): ret is what the console prints */
static void *__attribute__((thiscall)) cmd_invoke(void *self, void *ret, const BYTE *argv, int argc)
{
    char text[16];
    (void)self;
    if (argc >= 1) {
        set_scale(atoi(bf_string_chars(argv)));
        text[0] = '\0';
    } else {
        snprintf(text, sizeof text, "%d", bf_hud_scale());
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

static void register_command(void)
{
    static int done;
    void **tmpl = (void **)BF_ADDR(METHOD_VTBL);
    int i;
    if (done) {
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
    *(void ***)(g_cmd + 0x00) = g_cmd_vtbl;
    *(DWORD *)(g_cmd + 0x04) = 1;                       /* a property: no argument reads it */
    *(const char **)(g_cmd + 0x08) = "bf1942patch";
    *(const char **)(g_cmd + 0x0c) = "hudScale";
    *(DWORD *)(g_cmd + 0x10) = 3;                       /* typed in the console and run from .con files */
    *(DWORD *)(g_cmd + 0x14) = 0;
    *(DWORD *)(g_cmd + 0x18) = 1;
    *(const char **)(g_cmd + 0x4c) = "std::string";
    *(const char **)(g_cmd + 0x50) = "int";
    *(DWORD *)(g_cmd + 0x78) = 0;
    g_cmd[0xa1] = 1;
    g_elem.value = g_cmd;
    g_sentinel.next = g_sentinel.prev = &g_elem;
    g_elem.next = g_elem.prev = &g_sentinel;
    g_list.head = &g_sentinel;
    g_list.size = 1;
    ((char (__attribute__((thiscall)) *)(void *, void *))BF_ADDR(REGISTER_LIST))(BF_ADDR(REGISTRY), &g_list);
    bf_log("menu: console command bf1942patch.hudScale registered");
}

static char __attribute__((thiscall)) hk_menu_register(void *module)
{
    char r = ((char (__attribute__((thiscall)) *)(void *))BF_ADDR(MENU_REGISTER))(module);
    register_command();
    return r;
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
    snprintf(text, sizeof text, "bf1942patch.hudScale %d\n", bf_hud_scale());
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

typedef void *(__attribute__((thiscall)) *open_fn)(void *mgr, const char *name);
typedef void *(__attribute__((thiscall)) *buf_data_fn)(void *buf);
typedef void (__attribute__((thiscall)) *buf_resize_fn)(void *buf, DWORD size);

static open_fn g_menu_open;

#define MAX_SPLICES 4
static struct { const char *page; const bf_meme_row *row; const char *what; } g_splices[MAX_SPLICES];
static int g_nsplices;

static void *__attribute__((thiscall)) hk_menu_open(void *mgr, const char *name)
{
    void *is = g_menu_open(mgr, name);
    void *buf;
    BYTE *data, *patched;
    DWORD size;
    int i;
    if (!is || !name) {
        return is;
    }
    for (i = 0; i < g_nsplices && _stricmp(name, g_splices[i].page); i++) {
    }
    if (i == g_nsplices) {
        return is;
    }
    buf = *(void **)((BYTE *)is + 8);
    data = buf ? ((buf_data_fn)(*(void ***)buf)[0x34 / 4])(buf) : NULL;
    if (!data || IsBadReadPtr(data, g_splices[i].row->size)) {
        return is;
    }
    patched = bf_meme_add_row(g_splices[i].row, data, g_splices[i].row->size, &size);
    if (!patched) {
        bf_log("menu: menu/%s is not the one the patch knows, no %s", name, g_splices[i].what);
        return is;
    }
    ((buf_resize_fn)(*(void ***)buf)[0x38 / 4])(buf, size);
    data = ((buf_data_fn)(*(void ***)buf)[0x34 / 4])(buf);
    if (data) {
        memcpy(data, patched, size);
        bf_log("menu: %s added to menu/%s", g_splices[i].what, name);
    }
    free(patched);
    return is;
}

/* adds row to menu/<page> whenever the game opens it. The opener 0x601e80 is detoured the first time. */
int bf_menu_add_row(const char *page, const bf_meme_row *row, const char *what)
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
    g_splices[g_nsplices].what = what;
    g_nsplices++;
    return 1;
}

/* ---------------------------------------------------------------- install */

void bf_menu_install(void)
{
    g_hud_scale = bf_ini_int("Widescreen", "HudScale", 100);
    set_scale(g_hud_scale);
    if (!bf_ini_int("Widescreen", "HudScaleOption", 1)) {
        bf_log("menu: HudScaleOption=0, the HUD scale is only [Widescreen] HudScale");
        return;
    }
    g_on = 1;
    if (!bf_hook_slot(MENU_REGISTER_SLOT, MENU_REGISTER, hk_menu_register)) {
        bf_log("menu: the menu module's command registration differs, no bf1942patch.hudScale");
    }
    if (!bf_redirect_call(LAST_LINE_CALL, WRITE_LINE, hk_write_last)) {
        bf_log("menu: GeneralOptions.con is written differently, the HUD scale is not saved with the profile");
    }
    if (*(DWORD *)BF_ADDR(REGISTER_VARS_SLOT) == REGISTER_VARS
        && bf_menu_add_row("GeneralOptionsMenu", &GEN_ROW, "the HUD scale slider")) {
        bf_hook_slot(REGISTER_VARS_SLOT, REGISTER_VARS, hk_register_vars);
    } else {
        bf_log("menu: the options page code differs, no HUD scale slider");
    }
    bf_log("menu: HUD scale %d%% (Options -> General, bf1942patch.hudScale)", g_hud_scale);
}
