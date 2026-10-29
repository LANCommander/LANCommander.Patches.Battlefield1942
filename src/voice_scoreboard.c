/* Muting players from the scoreboard (bf1942patch.voiceMuteButton 1): a MUTE VOICE button with the game's own player
 * buttons. Pick a player in the scoreboard's list, as for VOTE KICK, and click it to stop hearing them; it reads
 * UNMUTE VOICE for a player who is muted. Mutes are kept by name with the profile, as its buddies are (voice.c).
 * To make room for it ADD BUDDY and DROP BUDDY become one button, ADD BUDDY or DROP BUDDY for the player picked.
 *
 * The scoreboard is part of menu/InGame: its buttons are a chain of TransformNodes in the frame under the lists
 * (ADD BUDDY at x 20, DROP BUDDY 140, VOTE KICK 270, VOTE KICK TEAM 390, MAP VOTE 510, all at y 8; the admins' KICK
 * at y 36), the last one MAP VOTE, whose NULL next (0x59b6) the new buttons are hung on: the buddy button at ADD
 * BUDDY's place, the mute button at DROP BUDDY's. The game's two buddy buttons stay in the file but are moved out of
 * sight (their TransformNodes' boxes, 0x5af6 and 0x5d5e). The new ones are copies of VOTE KICK (SplitNode 0x5fd8: a
 * greyed button shown while Scoreboard/EnableVoteKick is false, the live one while it is true) with
 *   - Scoreboard/EnableBuddy, Scoreboard/EnableVoiceMute for EnableVoteKick;
 *   - the WstringData Scoreboard/BuddyText (ADD BUDDY / DROP BUDDY), Scoreboard/VoiceMuteText (MUTE VOICE /
 *     UNMUTE VOICE) for its BfLocaleStringData labels;
 *   - the Functions Scoreboard/Buddy, Scoreboard/VoiceMute for Scoreboard/VoteKick.
 * The scoreboard (vtable 0x924bc8) binds its variables in registerVariables (0x6df500, vtable slot 0x924bd0, its
 * meme manager at +4), which is hooked to bind these after it. Its selected player is the game's player id at +0x4c
 * (-1 for none), set when a row of either team's list is clicked (0x6dfdb0 / 0x6dfe80). While it is shown its enables
 * are worked out every frame by 0x6e0380 (called at 0x45de20, redirected to work out these after it): EnableAddBuddy
 * (+0x43) and EnableRemBuddy (+0x44) are both true while a player is picked. Its AddBuddy (0x6dff50) and DropBuddy
 * (0x6dff80) add the picked player to, or drop them from, the buddy list ([[0x973ab8]+0x6e4], player ids in the
 * std::list at +0x138: head at +0x13c, the id at +8 of a node). The buddy button is live while a player is picked,
 * the mute button while that is another human player and the voice client runs. */
#include "voice.h"

#define SB_VARS_SLOT    0x924bd0u    /* Scoreboard::registerVariables -> 0x6df500 */
#define SB_VARS         0x6df500u
#define SB_UPDATE_CALL  0x45de20u    /* thiscall(scoreboard, int), ret 4: its enables, every frame it is shown */
#define SB_UPDATE       0x6e0380u
#define SB_ADD_BUDDY    0x6dff50u    /* thiscall(scoreboard) -> char: Scoreboard/AddBuddy */
#define SB_DROP_BUDDY   0x6dff80u    /* Scoreboard/DropBuddy */
#define SB_SELECTED     0x4c         /* int: the selected player's id, -1 for none */
#define SB_CAN_BUDDY    0x43         /* bool: Scoreboard/EnableAddBuddy */
#define GAME            0x973ab8u
#define GAME_BUDDIES    0x6e4
#define BUDDY_IDS       0x13c        /* std::list<int> head */

static const DWORD SB_ANCESTORS[] = {
    0x2aa0, 0x2aa8, 0x2ab0, 0x2ab8, 0x2ac0, 0x2ac8, 0x2ad0, 0x2ad8, 0x2ae0, 0x2ae8, 0x2af0, 0x2af8, 0x2b00, 0x2b08,
    0x2b10, 0x2b18, 0x2b20, 0x2b28, 0x2b30, 0x2b38, 0x2b40, 0x2b48, 0x2b50, 0x2b58, 0x2b60, 0x2b68, 0x2b70, 0x2b78,
    0x2b80, 0x2b88, 0x2b90, 0x2b98, 0x2ba0, 0x2ba8, 0x2bb0, 0x2bb8, 0x2bc0, 0x2bc8, 0x2bd0, 0x2bd8, 0x2be0, 0x2be8,
    0x2bf0, 0x2bf8, 0x2c00, 0x2c08, 0x2c10, 0x3d47, 0x3d4f, 0x595e, 0x5966, 0x596e, 0x5976, 0x597e, 0x5986, 0x598e,
    0x5996, 0x599e, 0x59a6, 0x59ae,
};
static const bf_meme_node SB_NODES[] = {                    /* VOTE KICK */
    {0x5fd8, "SplitNode"}, {0x5fe0, "SplitNode"}, {0x5ff0, "CullNode"}, {0x5ff8, "EffectNode"},
    {0x6000, "BfButtonNode"}, {0x6008, "TransformNode"}, {0x6028, "TextNode"}, {0x6038, "BfLocaleStringData"},
    {0x607a, "CenterAlignedStyle"}, {0x60c4, "ColorEffect"}, {0x60dc, "NotData"}, {0x60e4, "BoolData"},
    {0x60f5, "CullNode"}, {0x60fd, "BfButtonNode"}, {0x6105, "TransformNode"}, {0x6125, "TextNode"},
    {0x612d, "TransformNode"}, {0x614d, "CullEventActionNode"}, {0x615d, "CallFunctionAction"}, {0x6165, "Function"},
    {0x616d, "TypeEvent"}, {0x617a, "BfLocaleStringData"}, {0x61bc, "CenterAlignedStyle"},
    {0x61f7, "ActionListAction"}, {0x61ff, "CallFunctionAction"}, {0x6207, "Function"}, {0x621f, "BoolData"},
};
#define ENABLE  "Scoreboard/EnableVoteKick"
#define BUTTON_EDITS(enable, text, fn) {                                                                       \
    {0x6038, NULL, BF_MEME_WSTRING, (text), 0, NULL, 0},                  /* the greyed button's label */        \
    {0x60e4, ENABLE, BF_MEME_BOOL, (enable), 0, NULL, 0},                                                      \
    {0x617a, NULL, BF_MEME_WSTRING, (text), 0, NULL, 0},                  /* the live button's */                \
    {0x6207, "Scoreboard/VoteKick", BF_MEME_FUNCTION, (fn), 0, NULL, 0},                                       \
    {0x621f, ENABLE, BF_MEME_BOOL, (enable), 0, NULL, 0},                                                      \
}
static const bf_meme_edit SB_BUDDY[] = BUTTON_EDITS("Scoreboard/EnableBuddy", "Scoreboard/BuddyText", "Scoreboard/Buddy");
static const bf_meme_edit SB_MUTE[] = BUTTON_EDITS("Scoreboard/EnableVoiceMute", "Scoreboard/VoiceMuteText",
                                                   "Scoreboard/VoiceMute");
static const bf_meme_graft_row SB_ROWS[] = {
    {{20, 8, 128, 128}, SB_BUDDY, sizeof SB_BUDDY / sizeof SB_BUDDY[0], NULL, 0, NULL},     /* ADD BUDDY's place */
    {{140, 8, 128, 128}, SB_MUTE, sizeof SB_MUTE / sizeof SB_MUTE[0], NULL, 0, NULL},       /* DROP BUDDY's */
};
static const float ADD_BOX[4] = {20, 8, 128, 128}, DROP_BOX[4] = {140, 8, 128, 128}, HIDDEN[4] = {-4096, -4096, 128, 128};
static const bf_meme_bytes SB_BYTES[] = {
    {0x5af6, 16, ADD_BOX, HIDDEN},                          /* ADD BUDDY */
    {0x5d5e, 16, DROP_BOX, HIDDEN},                         /* DROP BUDDY */
};
const bf_meme_graft bf_voice_scoreboard_graft = {
    .size = 86295, .strings_end = 0x2a9d,
    .hang_at = 0x59b6,                                      /* after MAP VOTE, the chain's last */
    .ancestors = SB_ANCESTORS, .n_ancestors = sizeof SB_ANCESTORS / sizeof SB_ANCESTORS[0],
    .content = 0x5fd8, .content_len = 600,
    .nodes = SB_NODES, .n_nodes = sizeof SB_NODES / sizeof SB_NODES[0],
    .rows = SB_ROWS, .n_rows = sizeof SB_ROWS / sizeof SB_ROWS[0],
    .bytes = SB_BYTES, .n_bytes = sizeof SB_BYTES / sizeof SB_BYTES[0],
};

/* ---------------------------------------------------------------- the variables */

typedef char (__attribute__((thiscall)) *sb_fn)(void *sb);

static BYTE *g_sb;                   /* the scoreboard */
static void *g_mgr;                  /* its meme manager */
static unsigned char g_enable;       /* Scoreboard/EnableVoiceMute */
static unsigned char g_buddy_enable; /* Scoreboard/EnableBuddy */
static void *g_text;                 /* Scoreboard/VoiceMuteText's data node */
static void *g_buddy_text;           /* Scoreboard/BuddyText's */
static int g_shown = -1;             /* what the labels say: 1 UNMUTE VOICE, 0 MUTE VOICE */
static int g_buddy_shown = -1;       /* 1 DROP BUDDY, 0 ADD BUDDY */

/* the selected player, if one that can be muted */
static int selected(char *name, int size)
{
    return g_sb && !IsBadReadPtr(g_sb + SB_SELECTED, 4) && bf_voice_running()
        && bf_voice_player(*(int *)(g_sb + SB_SELECTED), name, size);
}

/* whether the game's player id is on the buddy list */
static int is_buddy(int id)
{
    BYTE *game = *(BYTE **)BF_ADDR(GAME), *buddies, *head, *node;
    int n;
    if (!game || IsBadReadPtr(game + GAME_BUDDIES, 4) || !(buddies = *(BYTE **)(game + GAME_BUDDIES))
        || IsBadReadPtr(buddies + BUDDY_IDS, 4) || !(head = *(BYTE **)(buddies + BUDDY_IDS)) || IsBadReadPtr(head, 4)) {
        return 0;
    }
    for (node = *(BYTE **)head, n = 0; node && node != head && n < 256; node = *(BYTE **)node, n++) {
        if (IsBadReadPtr(node, 12)) {
            return 0;
        }
        if (*(int *)(node + 8) == id) {
            return 1;
        }
    }
    return 0;
}

static void show(int muted)
{
    if (muted != g_shown && g_text) {
        g_shown = muted;
        bf_meme_set_wstring(g_mgr, g_text, muted ? L"UNMUTE VOICE" : L"MUTE VOICE");
    }
}

static void show_buddy(int buddy)
{
    if (buddy != g_buddy_shown && g_buddy_text) {
        g_buddy_shown = buddy;
        bf_meme_set_wstring(g_mgr, g_buddy_text, buddy ? L"DROP BUDDY" : L"ADD BUDDY");
    }
}

/* the buttons as the selected player stands */
static void update(void)
{
    char name[64];
    int buddy = g_sb && g_sb[SB_CAN_BUDDY] && is_buddy(*(int *)(g_sb + SB_SELECTED));
    g_buddy_enable = g_sb && g_sb[SB_CAN_BUDDY];
    show_buddy(buddy);
    g_enable = (unsigned char)selected(name, sizeof name);
    show(g_enable && bf_voice_muted(name));
}

/* Scoreboard/VoiceMute: the button's click */
static char __attribute__((thiscall)) on_mute(void *sb)
{
    char name[64];
    (void)sb;
    if (!selected(name, sizeof name)) {
        return 0;
    }
    bf_voice_set_muted(name, !bf_voice_muted(name));
    show(bf_voice_muted(name));
    return 1;
}

/* Scoreboard/Buddy: the game's DropBuddy for a buddy, its AddBuddy for anyone else */
static char __attribute__((thiscall)) on_buddy(void *sb)
{
    char r;
    if (!g_sb || !g_sb[SB_CAN_BUDDY]) {
        return 0;
    }
    r = ((sb_fn)BF_ADDR(is_buddy(*(int *)(g_sb + SB_SELECTED)) ? SB_DROP_BUDDY : SB_ADD_BUDDY))(sb);
    update();
    return r;
}

static int __attribute__((thiscall)) hk_sb_vars(BYTE *sb)
{
    int r = ((int (__attribute__((thiscall)) *)(void *))BF_ADDR(SB_VARS))(sb), ok;
    g_sb = sb;
    g_mgr = *(void **)(sb + 4);
    g_shown = 0;
    g_buddy_shown = 0;
    ok = bf_meme_bind(g_mgr, "Scoreboard", "EnableVoiceMute", BF_MENU_BOOL, &g_enable);
    ok &= (g_text = bf_meme_bind_wstring(g_mgr, "Scoreboard", "VoiceMuteText", L"MUTE VOICE")) != NULL;
    ok &= bf_meme_bind_function(g_mgr, "Scoreboard", "VoiceMute", sb, on_mute);
    ok &= bf_meme_bind(g_mgr, "Scoreboard", "EnableBuddy", BF_MENU_BOOL, &g_buddy_enable);
    ok &= (g_buddy_text = bf_meme_bind_wstring(g_mgr, "Scoreboard", "BuddyText", L"ADD BUDDY")) != NULL;
    ok &= bf_meme_bind_function(g_mgr, "Scoreboard", "Buddy", sb, on_buddy);
    bf_log("voice: the scoreboard's buddy and mute buttons %s", ok ? "are bound" : "could not be bound");
    return r;
}

/* the scoreboard's own enables, then these: every frame it is shown, on the game thread */
static void __attribute__((thiscall)) hk_sb_update(BYTE *sb, int arg)
{
    ((void (__attribute__((thiscall)) *)(void *, int))BF_ADDR(SB_UPDATE))(sb, arg);
    if (sb == g_sb) {
        update();
    }
}

void bf_voice_scoreboard_install(void)
{
    if (!bf_setting_int("voiceMuteButton")) {
        bf_log("voice: voiceMuteButton 0, no mute button on the scoreboard");
        return;
    }
    if (*(DWORD *)BF_ADDR(SB_VARS_SLOT) != SB_VARS || !bf_redirect_call(SB_UPDATE_CALL, SB_UPDATE, hk_sb_update)
        || !bf_menu_graft("InGame", &bf_voice_scoreboard_graft, "the scoreboard's buddy and mute buttons (voice chat)")) {
        bf_log("voice: the scoreboard code differs, no mute button on the scoreboard");
        return;
    }
    bf_hook_slot(SB_VARS_SLOT, SB_VARS, hk_sb_vars);
}
