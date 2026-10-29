/* Voice chat on the HUD ([Voice] VoiceHud=1): under the minimap, one line per player talking (the local player
 * first, while their voice is sent), in the game's own HUD text and in the colour the game gives that player's team
 * in the chat, with an icon in front: a radio set for the team radio, a soldier with a megaphone for nearby.
 *
 * The lines are part of the HUD itself (menu/InGame), so they are drawn, scaled and moved to the screen edge with
 * the rest of it (widescreen.c): 8 rows of three nodes each are grafted onto the end of the HUD's chain of elements
 * (the NULL next of the disconnect message, 0x2c40):
 *   - the text: a copy of the kill message (CullNode 0x9c68: BfTextNode in the outlined HUD font, coloured by a
 *     VariableColorEffect) on Voice/Name<n>, Voice/Red|Green|Blue<n> and shown while Voice/Show<n>, with no timeout
 *     (its TimeoutActionNode becomes NULL) and left aligned (BfLeftOutlineStyle for BfOutlineStyle, which centres);
 *   - the icons: two copies of the reload icon (CullNode 0x13953: a PictureNode) on Voice/voice_radio.tga and
 *     Voice/voice_nearby.tga, shown while Voice/Radio<n> / Voice/Nearby<n>.
 * The HUD's variables live in BfMenu's meme manager (BfMenu+0x144), registered by 0x6e10b0 during BfMenu's set-up
 * (called at 0x45eee4, before the menus load): that call is redirected to bind the rows' variables after it.
 *
 * The icons (voice_icons.c) are not in the game's archives: the patch writes bf1942patch\Archives\menu_1942.rfa (two
 * uncompressed 32-bit DDS files in menu/Texture/Voice/, LZO segments of literals like the game's own archives)
 * to the save folder, and the FileManager ([0x9a2380], vtable +0x5c mount(dir, name))
 * mounts it next to menu.rfa at the same moment.
 *
 * The colours are the chat's: app [0x971eac] +0x128 holds game.axisChatColor (+0x34), alliedChatColor (+0x40) and
 * normalChatColor (+0x4c), RGB floats; a player's team is IPlayer +0xac (1 Axis, 2 Allies). */
#include <stdio.h>
#include <string.h>
#include "voice.h"

#define ROWS            8
#define ROW_TOP         229.0f       /* under the minimap (30..212) and the control point bar (207..223) */
#define ROW_PITCH       22.0f
#define TEXT_DROP       5.0f         /* the text (about 10 high) centred on the icon */
#define ROW_LEFT        613.0f       /* the minimap's left edge */
#define ICON_SIZE       20.0f        /* the icon's 24 pixels; its texture is 32 */

#define BFMENU          0x973ab8u
#define BFMENU_MGR      0x144        /* the HUD's meme manager */
#define BFMENU_MODE     0xfc
#define MODE_INGAME     1
#define HUD_VARS_CALL   0x45eee4u    /* BfMenu set-up: call 0x6e10b0, the HUD's variables */
#define HUD_VARS        0x6e10b0u
#define FILE_MANAGER    0x9a2380u
#define FM_EXISTS       0x10         /* thiscall(fm, const std::string *path) -> bool */
#define FM_MOUNT        0x5c         /* thiscall(fm, const std::string *dir, const std::string *archive) */
#define APP_PTR         0x971eacu
#define APP_CHAT        0x128

extern const unsigned char bf_voice_icon_radio[32 * 32 * 4], bf_voice_icon_nearby[32 * 32 * 4];

/* ---------------------------------------------------------------- the rows in menu/InGame */

static const DWORD HUD_ANCESTORS[] = {
    0x2aa0, 0x2aa8, 0x2ab0, 0x2ab8, 0x2ac0, 0x2ac8, 0x2ad0, 0x2ad8, 0x2ae0, 0x2ae8, 0x2af0, 0x2af8, 0x2b00, 0x2b08,
    0x2b10, 0x2b18, 0x2b20, 0x2b28, 0x2b30, 0x2b38, 0x2b40, 0x2b48, 0x2b50, 0x2b58, 0x2b60, 0x2b68, 0x2b70, 0x2b78,
    0x2b80, 0x2b88, 0x2b90, 0x2b98, 0x2ba0, 0x2ba8, 0x2bb0, 0x2bb8, 0x2bc0, 0x2bc8, 0x2bd0, 0x2bd8, 0x2be0, 0x2be8,
    0x2bf0, 0x2bf8, 0x2c00, 0x2c08, 0x2c10, 0x2c18, 0x2c20, 0x2c28, 0x2c30, 0x2c38,
};
static const bf_meme_node TEXT_NODES[] = {                  /* the kill message */
    {0x9c68, "CullNode"}, {0x9c70, "EffectNode"}, {0x9c78, "BfTextNode"}, {0x9c80, "TimeoutActionNode"},
    {0x9cde, "WstringData"}, {0x9cea, "BfOutlineStyle"}, {0x9d00, "IntData"}, {0x9d0c, "VariableColorEffect"},
    {0x9d14, "FloatData"}, {0x9d20, "FloatData"}, {0x9d2c, "FloatData"}, {0x9d38, "FloatData"}, {0x9d44, "BoolData"},
};
static const bf_meme_node ICON_NODES[] = {                  /* the reload icon */
    {0x13953, "CullNode"}, {0x1395b, "PictureNode"}, {0x1397b, "BoolData"},
};
static const bf_meme_piece TEXT_PIECE = {0x9c68, 237, TEXT_NODES, sizeof TEXT_NODES / sizeof TEXT_NODES[0], NULL, 0, 0};
static const bf_meme_piece ICON_PIECE = {0x13953, 57, ICON_NODES, sizeof ICON_NODES / sizeof ICON_NODES[0], NULL, 0, 0};

static char g_names[ROWS][7][24];                           /* Voice/Name<n>, Red, Green, Blue, Show, Radio, Nearby */
static bf_meme_edit g_text_edits[ROWS][7], g_icon_edits[ROWS][2][2];
static bf_meme_graft_row g_rows[ROWS * 3];
static bf_meme_graft g_graft = {
    .size = 86295, .strings_end = 0x2a9d,
    .hang_at = 0x2c40,                                      /* after the disconnect message, the chain's last */
    .ancestors = HUD_ANCESTORS, .n_ancestors = sizeof HUD_ANCESTORS / sizeof HUD_ANCESTORS[0],
    .rows = g_rows, .n_rows = ROWS * 3,
};

static void edit(bf_meme_edit *e, DWORD at, const char *was, int kind, const char *var, const char *text)
{
    e->at = at;
    e->was_var = was;
    e->kind = kind;
    e->var = var;
    e->text = text;
    e->real = 1.0f;
}

static void make_rows(void)
{
    static const char *const fields[7] = {"Name", "Red", "Green", "Blue", "Show", "Radio", "Nearby"};
    int i, k;
    for (i = 0; i < ROWS; i++) {
        float y = ROW_TOP + i * ROW_PITCH, pad = ICON_SIZE * 4 / 24, box = ICON_SIZE * 32 / 24;
        bf_meme_graft_row *r = &g_rows[i * 3];
        bf_meme_edit *t = g_text_edits[i];
        for (k = 0; k < 7; k++) {
            snprintf(g_names[i][k], sizeof g_names[i][k], "Voice/%s%d", fields[k], i + 1);
        }
        edit(&t[0], 0x9c80, NULL, BF_MEME_NULL, NULL, NULL);                    /* no timeout */
        edit(&t[1], 0x9cde, "KillMessage/KillMessage", BF_MEME_WSTRING, g_names[i][0], NULL);
        edit(&t[2], 0x9cea, NULL, BF_MEME_CLASS, NULL, "BfLeftOutlineStyle");
        edit(&t[3], 0x9d14, "Color/Red", BF_MEME_FLOAT, g_names[i][1], NULL);
        edit(&t[4], 0x9d20, "Color/Green", BF_MEME_FLOAT, g_names[i][2], NULL);
        edit(&t[5], 0x9d2c, "Color/Blue", BF_MEME_FLOAT, g_names[i][3], NULL);
        edit(&t[6], 0x9d44, "KillMessage/ShowKillMessage", BF_MEME_BOOL, g_names[i][4], NULL);
        r[0].rect[0] = ROW_LEFT + ICON_SIZE + 4;
        r[0].rect[1] = y + TEXT_DROP;
        r[0].rect[2] = 180;
        r[0].rect[3] = ROW_PITCH - TEXT_DROP;
        r[0].edits = t;
        r[0].n_edits = 7;
        r[0].piece = &TEXT_PIECE;
        for (k = 0; k < 2; k++) {
            bf_meme_edit *e = g_icon_edits[i][k];
            edit(&e[0], 0x1395b, NULL, BF_MEME_PICTURE, NULL, k ? "Voice/voice_nearby.tga" : "Voice/voice_radio.tga");
            edit(&e[1], 0x1397b, "ShowReloadIcon", BF_MEME_BOOL, g_names[i][5 + k], NULL);
            r[1 + k].rect[0] = ROW_LEFT - pad;               /* the texture's empty border outside the icon's box */
            r[1 + k].rect[1] = y - pad;
            r[1 + k].rect[2] = box;
            r[1 + k].rect[3] = box;
            r[1 + k].edits = e;
            r[1 + k].n_edits = 2;
            r[1 + k].piece = &ICON_PIECE;
        }
    }
}

/* ---------------------------------------------------------------- the variables */

static void *g_mgr;                  /* BfMenu's meme manager */
static struct {
    BYTE show, radio, nearby;
    float rgb[3];
    void *name_node;
    wchar_t name[64];
} g_row[ROWS];

/* ---------------------------------------------------------------- the icons' archive */

/* n bytes as one LZO1X stream of literals (n >= 19), and its end */
static DWORD lzo_literals(BYTE *out, const BYTE *in, DWORD n)
{
    /* a literal run of n = 3 + 15 + 255 * zeros + last: 0, the zeros, last (1..255) */
    DWORD r = n - 18, zeros = (r - 1) / 255, last = r - zeros * 255, at = 0;
    out[at++] = 0;
    while (zeros--) {
        out[at++] = 0;
    }
    out[at++] = (BYTE)last;
    memcpy(out + at, in, n);
    at += n;
    memcpy(out + at, "\x11\0\0", 3);
    return at + 3;
}

/* a 32 x 32 A8R8G8B8 DDS, as menu/Texture/Menu/serverinfo/menu_icon_punkb_16x16.dds */
static void dds(BYTE *out, const unsigned char *bgra)
{
    static const DWORD head[32] = {0x20534444, 124, 0x81007, 32, 32, 4096, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   32, 0x41, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000, 0x1000, 0, 0, 0, 0};
    memcpy(out, head, 128);
    memcpy(out + 128, bgra, 4096);
}

#define DDS_SIZE (128 + 4096)

/* the archive: a header of 156 bytes {directory, 1 = compressed, 144 bytes, check}, each file {1 segment, its sizes,
 * the stream}, the directory {count, then name, compressed size, size, offset, 0, 0, 0}. The archive reader
 * (0x63af70) takes an archive whose check (+0x98) minus the sum of the bytes 8..0x96 is 0x81671213 (DICE's own) or
 * one of the keys the game lists (app +0x8ec); the 144 bytes are 0 here, so the check is that number. */
#define ARCHIVE_KEY 0x81671213u
static DWORD build_archive(BYTE *out)
{
    static const char *const names[2] = {"menu/Texture/Voice/voice_radio.dds", "menu/Texture/Voice/voice_nearby.dds"};
    const unsigned char *const icons[2] = {bf_voice_icon_radio, bf_voice_icon_nearby};
    BYTE file[DDS_SIZE];
    DWORD at = 156, offs[2], sizes[2], i, n;
    memset(out, 0, 156);
    for (i = 0; i < 2; i++) {
        dds(file, icons[i]);
        offs[i] = at;
        n = lzo_literals(out + at + 16, file, DDS_SIZE);
        memcpy(out + at, &(DWORD){1}, 4);
        memcpy(out + at + 4, &n, 4);
        memcpy(out + at + 8, &(DWORD){DDS_SIZE}, 4);
        memset(out + at + 12, 0, 4);
        sizes[i] = 16 + n;
        at += sizes[i];
    }
    memcpy(out, &at, 4);
    memcpy(out + 4, &(DWORD){1}, 4);
    memcpy(out + 0x98, &(DWORD){ARCHIVE_KEY}, 4);
    memcpy(out + at, &(DWORD){2}, 4);
    at += 4;
    for (i = 0; i < 2; i++) {
        DWORD len = (DWORD)strlen(names[i]), tail[6] = {sizes[i], DDS_SIZE, offs[i], 0, 0, 0};
        memcpy(out + at, &len, 4);
        memcpy(out + at + 4, names[i], len);
        memcpy(out + at + 4 + len, tail, sizeof tail);
        at += 4 + len + sizeof tail;
    }
    return at;
}

/* the archive in <save folder>\bf1942patch\Archives\ (the install folder without one), written when it differs. The
 * reader mounts an archive at its path after "Archives/", without the extension and a "_<digits>" suffix, and quits
 * the game if an entry is not below that: so menu_1942.rfa, mounted at menu/ next to menu.rfa */
#define ARCHIVE "menu_1942.rfa"

static int write_archive(char *dir, int size)
{
    static BYTE data[2 * (DDS_SIZE + 64) + 512], old[sizeof data];
    char path[MAX_PATH + 32];
    const char *base = bf_save_dir()[0] ? bf_save_dir() : bf_game_dir();
    DWORD n = build_archive(data), got = 0;
    HANDLE f;
    snprintf(dir, size, "%sbf1942patch\\Archives\\", base);
    snprintf(path, sizeof path, "%s" ARCHIVE, dir);
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        ReadFile(f, old, sizeof old, &got, NULL);
        CloseHandle(f);
        if (got == n && !memcmp(old, data, n)) {
            return 1;
        }
    }
    bf_make_dirs(dir);
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, data, n, &got, NULL) || got != n) {
        bf_log("voice: cannot write %s (%lu)", path, GetLastError());
        if (f != INVALID_HANDLE_VALUE) {
            CloseHandle(f);
        }
        return 0;
    }
    CloseHandle(f);
    return 1;
}

static void mount_icons(void)
{
    typedef char (__attribute__((thiscall)) *mount_fn)(void *fm, const void *dir, const void *name);
    typedef char (__attribute__((thiscall)) *exists_fn)(void *fm, const void *path);
    void *fm = *(void **)BF_ADDR(FILE_MANAGER);
    char dir[MAX_PATH], *c;
    BYTE d[BF_STRING_SIZE], n[BF_STRING_SIZE], p[BF_STRING_SIZE];
    int ok = 0;
    if (!fm || !write_archive(dir, sizeof dir)) {
        return;
    }
    for (c = dir; *c; c++) {
        if (*c == '\\') {
            *c = '/';                /* as the game names its archive folders */
        }
    }
    if (bf_string_init(d, dir)) {
        if (bf_string_init(n, ARCHIVE)) {
            ((mount_fn)(*(void ***)fm)[FM_MOUNT / 4])(fm, d, n);
            if (bf_string_init(p, "menu/Texture/Voice/voice_radio.dds")) {
                ok = ((exists_fn)(*(void ***)fm)[FM_EXISTS / 4])(fm, p) != 0;
                bf_string_free(p);
            }
            bf_string_free(n);
        }
        bf_string_free(d);
    }
    bf_log("voice: the HUD icons %s (%s" ARCHIVE ")", ok ? "are mounted" : "could not be mounted", dir);
}

static char __attribute__((thiscall)) hk_hud_vars(BYTE *menu)
{
    char r = ((char (__attribute__((thiscall)) *)(void *))BF_ADDR(HUD_VARS))(menu);
    int i, k, ok = 1;
    g_mgr = *(void **)(menu + BFMENU_MGR);
    for (i = 0; i < ROWS && g_mgr; i++) {
        /* names after "Voice/" */
        ok &= bf_meme_bind(g_mgr, "Voice", g_names[i][4] + 6, BF_MENU_BOOL, &g_row[i].show);
        ok &= bf_meme_bind(g_mgr, "Voice", g_names[i][5] + 6, BF_MENU_BOOL, &g_row[i].radio);
        ok &= bf_meme_bind(g_mgr, "Voice", g_names[i][6] + 6, BF_MENU_BOOL, &g_row[i].nearby);
        for (k = 0; k < 3; k++) {
            g_row[i].rgb[k] = 1.0f;
            ok &= bf_meme_bind(g_mgr, "Voice", g_names[i][1 + k] + 6, BF_MENU_FLOAT, &g_row[i].rgb[k]);
        }
        g_row[i].name_node = bf_meme_bind_wstring(g_mgr, "Voice", g_names[i][0] + 6, L"");
        ok &= g_row[i].name_node != NULL;
    }
    bf_log("voice: the HUD's variables %s", ok ? "are bound" : "could not all be bound");
    mount_icons();
    return r;
}

/* ---------------------------------------------------------------- every frame */

static int readable(const void *p, size_t n)
{
    return p && !IsBadReadPtr(p, n);
}

/* the team's colour: the chat's (Bf1942/Game/Init/Menu.con: axis 1/0.7/0.7, allied 0.7/0.9/1, normal 0.7/0.7/0.7),
 * at full saturation, so Axis reads red and Allies blue on the HUD (a grey stays grey) */
static void team_color(int team, float rgb[3])
{
    BYTE *app = *(BYTE **)BF_ADDR(APP_PTR), *chat;
    const float *c;
    float lo, hi;
    int k;
    rgb[0] = rgb[1] = rgb[2] = 1.0f;
    if (!readable(app + APP_CHAT, 4) || !readable(chat = *(BYTE **)(app + APP_CHAT), 0x58)) {
        return;
    }
    c = (const float *)(chat + (team == 1 ? 0x34 : team == 2 ? 0x40 : 0x4c));
    if (!(c[0] >= 0 && c[0] <= 1 && c[1] >= 0 && c[1] <= 1 && c[2] >= 0 && c[2] <= 1)) {
        return;
    }
    lo = hi = c[0];
    for (k = 1; k < 3; k++) {
        lo = c[k] < lo ? c[k] : lo;
        hi = c[k] > hi ? c[k] : hi;
    }
    for (k = 0; k < 3; k++) {
        rgb[k] = hi - lo > 0.05f ? (c[k] - lo) / (hi - lo) * hi : c[k];
    }
}

static void set_row(int i, const char *name, int team, int radio)
{
    wchar_t w[64];
    MultiByteToWideChar(1252, 0, name, -1, w, 64);
    w[63] = L'\0';
    g_row[i].show = 1;
    g_row[i].radio = (BYTE)(radio != 0);
    g_row[i].nearby = (BYTE)(radio == 0);
    team_color(team, g_row[i].rgb);
    if (wcscmp(w, g_row[i].name)) {
        wcscpy(g_row[i].name, w);
        bf_meme_set_wstring(g_mgr, g_row[i].name_node, w);
    }
}

void bf_voice_hud_update(const bf_voice_state *s)
{
    BYTE *menu = *(BYTE **)BF_ADDR(BFMENU);
    int n = 0, i;
    if (!g_mgr) {
        return;
    }
    if (s->connected && menu && *(int *)(menu + BFMENU_MODE) == MODE_INGAME) {
        if (s->talking && s->me[0]) {
            set_row(n++, s->me, s->my_team, s->radio);
        }
        for (i = 0; i < s->n && n < ROWS; i++) {
            set_row(n++, s->talkers[i].name, s->talkers[i].team, s->talkers[i].radio);
        }
    }
    for (; n < ROWS; n++) {
        g_row[n].show = g_row[n].radio = g_row[n].nearby = 0;
    }
}

void bf_voice_hud_install(void)
{
    if (!bf_ini_int("Voice", "VoiceHud", 1)) {
        bf_log("voice: VoiceHud=0, no voice chat on the HUD");
        return;
    }
    make_rows();
    if (!bf_menu_graft("InGame", &g_graft, "who is talking (voice chat)")) {
        bf_log("voice: the menu opener differs, no voice chat on the HUD");
        return;
    }
    if (!bf_redirect_call(HUD_VARS_CALL, HUD_VARS, hk_hud_vars)) {
        bf_log("voice: BfMenu's set-up differs, no voice chat on the HUD");
    }
}
