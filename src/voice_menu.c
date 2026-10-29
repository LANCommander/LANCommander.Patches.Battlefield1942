/* Voice chat in the game's menus: the push-to-talk keys in Options -> Controls -> Common, the voice volume and the
 * microphone sensitivity in Options -> Sound (page 2).
 *
 * The keys. The Controls pages bind 83 actions, each row a label and two buttons (primary / alternate key) showing
 * the key's name, picked out of a list of 83 texts by the row's index (IndexDataData). A click sets WaitForKey,
 * Alternate and ActiveKey = the row's index; the page then reads the next key or button (0x6c67d0, the mouse's left
 * button through Options/Controls/PrimaryMouseButton) and hands it to 0x6c6040, thiscall(page, std::wstring shown,
 * std::string key, bool keep_waiting) by value, ret 0x3c, which stores it for action ActiveKey and clears
 * WaitForKey. The 83 are fixed arrays in the page object (0x6c5980, registerVariables), so voice gets rows of its
 * own: menu/CommonControlsPage3 (3 rows, room below) gets two copies of its VOTE NO row (0x2219, content 0x2239)
 * at y 85 / 110 with
 *   - no alternate button (its TransformNode 0x2249 becomes NULL);
 *   - the primary button's key list (IndexDataData 0x2aa3) replaced by Options/Controls/Voice/ProximityKey (or
 *     RadioKey), a WstringData bound here (0x69b8f0) and set with the key's name as the menu shows it (0x6c2940,
 *     thiscall(any, std::wstring *out, std::string key, int), ret 0x24; mouse buttons by IDButton_<n>);
 *   - its click setting Options/Controls/Voice/Capture = 1 (or 2) instead of ActiveKey (0x2a52 / 0x2a5e), and its
 *     hover the conflict index -1 (0x29b5);
 *   - a literal label (in place of the BfLocaleStringData 0x2f91).
 * Under them, two copies of menu/SoundOptionsPage1's MUSIC row (a check box, opened as the donor) at y 135 / 160:
 * VOICE ACTIVATION: NEARBY / RADIO on Options/Controls/Voice/NearbyActivation / RadioActivation, bound here.
 * 0x6c6040 is detoured: while Capture is set, the key goes to voice (kept in Settings.ini) and the game's own
 * bindings are not touched. Escape (an empty key) leaves the action without a key, as for the game's. Joystick
 * buttons and axes are refused (the page keeps waiting).
 *
 * The sliders. menu/SoundOptionsPage2 (hardware, speakers, localized voice; room from y 210) gets two copies of
 * page 1's MASTER VOLUME row (content 0x23ad: a number box, "%", a slider), opened through the game's opener as the
 * donor: VOICE CHAT VOLUME on Options/Sound/VoiceVolume and MIC SENSITIVITY on Options/Sound/MicSensitivity, 0..200
 * (the box's range at 0x24c8, the slider's at 0x2608), bound by SoundOptions::registerVariables (0x6b2ce0, vtable
 * slot 0x91e5c0). voice.c applies them as they move. */
#include <string.h>
#include "voice.h"

#define CONTROLS_VARS_SLOT 0x922b50u /* ControlsOptions::registerVariables -> 0x6c5980 (page vtable 0x922b48) */
#define CONTROLS_VARS   0x6c5980u
#define KEY_ASSIGN      0x6c6040u
#define KEY_NAME        0x6c2940u
#define PAGE_WAITING    0x18         /* bool Options/Controls/WaitForKey */
#define SOUND_VARS_SLOT 0x91e5c0u    /* SoundOptions::registerVariables -> 0x6b2ce0 (page vtable 0x91e5b8) */
#define SOUND_VARS      0x6b2ce0u

/* ---------------------------------------------------------------- menu/CommonControlsPage3: the keys */

static const DWORD KEYS_ANCESTORS[] = {0x1f97, 0x2209, 0x2211, 0x2219};
static const bf_meme_node KEYS_NODES[] = {
    {0x2239, "TransformNode"}, {0x2241, "TransformNode"}, {0x2249, "TransformNode"}, {0x28ec, "TransformNode"},
    {0x28f4, "TransformNode"}, {0x28fc, "TransformNode"}, {0x2904, "CullEventActionNode"},
    {0x290c, "CullEventActionNode"}, {0x2914, "CullEventActionNode"}, {0x2924, "ActionListAction"},
    {0x292c, "SetVariableAction"}, {0x2934, "IntData"}, {0x2940, "IntData"}, {0x2954, "TypeEvent"},
    {0x2961, "ActionListAction"}, {0x2969, "SetVariableAction"}, {0x2971, "IntData"}, {0x297d, "PointerXData"},
    {0x2985, "SetVariableAction"}, {0x298d, "IntData"}, {0x2999, "PointerYData"}, {0x29a1, "SetVariableAction"},
    {0x29a9, "IntData"}, {0x29b5, "IntData"}, {0x29c9, "TypeEvent"}, {0x29d6, "ActionListAction"},
    {0x29de, "SetVariableAction"}, {0x29e6, "BoolData"}, {0x29ef, "BoolData"}, {0x29f8, "SetVariableAction"},
    {0x2a00, "BoolData"}, {0x2a09, "BoolData"}, {0x2a12, "SetVariableAction"}, {0x2a1a, "IntData"},
    {0x2a26, "PointerXData"}, {0x2a2e, "SetVariableAction"}, {0x2a36, "IntData"}, {0x2a42, "PointerYData"},
    {0x2a4a, "SetVariableAction"}, {0x2a52, "IntData"}, {0x2a5e, "IntData"}, {0x2a72, "ButtonEvent"},
    {0x2a93, "TextNode"}, {0x2aa3, "IndexDataData"}, {0x2ed9, "CenterAlignedStyle"}, {0x2eff, "EffectNode"},
    {0x2f07, "PictureNode"}, {0x2f18, "ColorEffect"}, {0x2f40, "EffectNode"}, {0x2f48, "PictureNode"},
    {0x2f59, "ColorEffect"}, {0x2f81, "TextNode"}, {0x2f91, "BfLocaleStringData"}, {0x2fd5, "Style"},
};
#define VOTE_NO    "Options/Controls/Common/Index/VoteNo"
#define KEY_EDITS(n, var, label) {                                                                  \
    {0x2249, NULL, BF_MEME_NULL, NULL, 0, NULL, 0},                       /* the alternate button */ \
    {0x29b5, VOTE_NO, BF_MEME_INT, NULL, -1, NULL, 0},                    /* hover: no conflict text */ \
    {0x2a52, "Options/Controls/Key/ActiveKey", BF_MEME_INT, "Options/Controls/Voice/Capture", 0, NULL, 0}, \
    {0x2a5e, VOTE_NO, BF_MEME_INT, NULL, (n), NULL, 0},                   /* click: Capture = n */     \
    {0x2aa3, NULL, BF_MEME_WSTRING, (var), 0, NULL, 0},                   /* the key's name */         \
    {0x2f91, NULL, BF_MEME_STRING, NULL, 0, (label), 0},                                               \
}
static const bf_meme_edit KEYS_NEARBY[] = KEY_EDITS(1, "Options/Controls/Voice/ProximityKey", "PUSH TO TALK: NEARBY");
static const bf_meme_edit KEYS_RADIO[] = KEY_EDITS(2, "Options/Controls/Voice/RadioKey", "PUSH TO TALK: RADIO");
/* the voice activation check boxes: menu/SoundOptionsPage1's MUSIC row (content 0x20b5: a label and a check box on
 * Options/Sound/MusicOnOff, which its picture, its two sounds and its toggle all name) */
static const bf_meme_node CHECK_NODES[] = {
    {0x20b5, "TransformNode"}, {0x20bd, "TransformNode"}, {0x20dd, "TransformNode"}, {0x20fd, "DisableNode"},
    {0x2105, "DisableNode"}, {0x210d, "TransformNode"}, {0x2115, "TransformNode"}, {0x211d, "TransformNode"},
    {0x2125, "TransformNode"}, {0x2145, "CullNode"}, {0x214d, "EffectNode"}, {0x2155, "PictureNode"},
    {0x2166, "ColorEffect"}, {0x217e, "BoolData"}, {0x219f, "CullEventActionNode"}, {0x21a7, "SplitNode"},
    {0x21b7, "CullVariableAndEventActionNode"}, {0x21bf, "CullVariableAndEventActionNode"},
    {0x21cf, "CallFunctionAction"}, {0x21d7, "Function"}, {0x21df, "NotData"}, {0x21e7, "BoolData"},
    {0x21f0, "ButtonEvent"}, {0x2201, "CallFunctionAction"}, {0x2209, "Function"}, {0x2211, "BoolData"},
    {0x221a, "ButtonEvent"}, {0x222b, "ActionListAction"}, {0x2233, "ToggleVariableAction"}, {0x223b, "BoolData"},
    {0x224c, "ButtonEvent"}, {0x226d, "EffectNode"}, {0x2275, "PictureNode"}, {0x2286, "ColorEffect"},
    {0x22ae, "EffectNode"}, {0x22b6, "PictureNode"}, {0x22c7, "ColorEffect"}, {0x22df, "NotEqualData"},
    {0x22e7, "IntData"}, {0x22f3, "IntData"}, {0x22ff, "NotEqualData"}, {0x2307, "IntData"}, {0x2313, "IntData"},
    {0x232f, "TextNode"}, {0x233f, "BfLocaleStringData"}, {0x2387, "Style"},
};
static const bf_meme_piece CHECK_PIECE = {0x20b5, 744, CHECK_NODES, sizeof CHECK_NODES / sizeof CHECK_NODES[0],
                                          "SoundOptionsPage1", 10684, 0x502};
#define MUSIC "Options/Sound/MusicOnOff"
#define CHECK_EDITS(var, label) {                                                                   \
    {0x217e, MUSIC, BF_MEME_BOOL, (var), 0, NULL, 0},                     /* the tick */               \
    {0x21e7, MUSIC, BF_MEME_BOOL, (var), 0, NULL, 0},                     /* the sounds */             \
    {0x2211, MUSIC, BF_MEME_BOOL, (var), 0, NULL, 0},                                                  \
    {0x223b, MUSIC, BF_MEME_BOOL, (var), 0, NULL, 0},                     /* the toggle */             \
    {0x233f, NULL, BF_MEME_STRING, NULL, 0, (label), 0},                                               \
}
static const bf_meme_edit CHECK_NEARBY[] = CHECK_EDITS("Options/Controls/Voice/NearbyActivation",
                                                       "VOICE ACTIVATION: NEARBY");
static const bf_meme_edit CHECK_RADIO[] = CHECK_EDITS("Options/Controls/Voice/RadioActivation",
                                                      "VOICE ACTIVATION: RADIO");
static const bf_meme_graft_row KEYS_ROWS[] = {
    {{0, 85, 380, 20}, KEYS_NEARBY, sizeof KEYS_NEARBY / sizeof KEYS_NEARBY[0], NULL, 0, NULL},
    {{0, 110, 380, 20}, KEYS_RADIO, sizeof KEYS_RADIO / sizeof KEYS_RADIO[0], NULL, 0, NULL},
    {{0, 135, 380, 20}, CHECK_NEARBY, sizeof CHECK_NEARBY / sizeof CHECK_NEARBY[0], NULL, 0, &CHECK_PIECE},
    {{0, 160, 380, 20}, CHECK_RADIO, sizeof CHECK_RADIO / sizeof CHECK_RADIO[0], NULL, 0, &CHECK_PIECE},
};
const bf_meme_graft bf_voice_keys_graft = {
    .size = 19317, .strings_end = 0x1f94,
    .hang_at = 0x2221,                                 /* after VOTE NO */
    .ancestors = KEYS_ANCESTORS, .n_ancestors = sizeof KEYS_ANCESTORS / sizeof KEYS_ANCESTORS[0],
    .content = 0x2239, .content_len = 0xdb2,
    .nodes = KEYS_NODES, .n_nodes = sizeof KEYS_NODES / sizeof KEYS_NODES[0],
    .rows = KEYS_ROWS, .n_rows = sizeof KEYS_ROWS / sizeof KEYS_ROWS[0],
};

/* ---------------------------------------------------------------- menu/SoundOptionsPage2: the sliders */

static const DWORD SOUND_ANCESTORS[] = {0x45f, 0x6bf, 0x6c7, 0x6cf};
static const bf_meme_node SOUND_NODES[] = {
    {0x23ad, "TransformNode"}, {0x23b5, "TransformNode"}, {0x23d5, "TransformNode"}, {0x23dd, "TransformNode"},
    {0x23e5, "TransformNode"}, {0x2405, "TextNode"}, {0x2415, "StringData"}, {0x2422, "Style"},
    {0x2450, "TransformNode"}, {0x2458, "TransformNode"}, {0x2460, "TransformNode"}, {0x2480, "EffectNode"},
    {0x2488, "BfEditNodeInt"}, {0x24ae, "IntData"}, {0x24ba, "StringData"}, {0x24d8, "BfEditData"},
    {0x24e1, "ColorEffect"}, {0x2509, "EffectNode"}, {0x2511, "PictureNode"}, {0x2522, "ColorEffect"},
    {0x254a, "EffectNode"}, {0x2552, "PictureNode"}, {0x2563, "ColorEffect"}, {0x258b, "TransformNode"},
    {0x2593, "TransformNode"}, {0x259b, "TransformNode"}, {0x25bb, "BfSliderNode"}, {0x25cb, "TransformNode"},
    {0x25eb, "PictureNode"}, {0x25fc, "IntData"}, {0x2624, "EffectNode"}, {0x262c, "PictureNode"},
    {0x263d, "ColorEffect"}, {0x2665, "EffectNode"}, {0x266d, "PictureNode"}, {0x267e, "ColorEffect"},
    {0x26a6, "TextNode"}, {0x26b6, "BfLocaleStringData"}, {0x26ff, "Style"},
};
#define MASTER "Options/Sound/MasterVolume"
#define SLIDER_EDITS(var, label) {                                                                  \
    {0x24ae, MASTER, BF_MEME_INT, (var), 100, NULL, 0},                   /* the number box */         \
    {0x24ba, NULL, BF_MEME_STRING, NULL, 0, "100", 0},                    /* its text */               \
    {0x25fc, MASTER, BF_MEME_INT, (var), 100, NULL, 0},                   /* the slider */             \
    {0x26b6, NULL, BF_MEME_STRING, NULL, 0, (label), 0},                                               \
}
static const bf_meme_edit SOUND_VOICE[] = SLIDER_EDITS("Options/Sound/VoiceVolume", "VOICE CHAT VOLUME");
static const bf_meme_edit SOUND_MIC[] = SLIDER_EDITS("Options/Sound/MicSensitivity", "MIC SENSITIVITY");
static const int BOX_RANGE[2] = {0, 100}, BOX_NEW_RANGE[2] = {0, 200};
static const float SLIDER_RANGE[3] = {0, 100, 10}, SLIDER_NEW_RANGE[3] = {0, 200, 10};
static const bf_meme_bytes SOUND_BYTES[] = {
    {0x24c8, 8, BOX_RANGE, BOX_NEW_RANGE},
    {0x2608, 12, SLIDER_RANGE, SLIDER_NEW_RANGE},
};
static const bf_meme_graft_row SOUND_ROWS[] = {
    {{5, 210, 360, 20}, SOUND_VOICE, sizeof SOUND_VOICE / sizeof SOUND_VOICE[0], SOUND_BYTES, 2, NULL},
    {{5, 240, 360, 20}, SOUND_MIC, sizeof SOUND_MIC / sizeof SOUND_MIC[0], SOUND_BYTES, 2, NULL},
};
const bf_meme_graft bf_voice_sound_graft = {
    .size = 6788, .strings_end = 0x45c,
    .hang_at = 0x6d7,                                  /* after LOCALIZED VOICE */
    .ancestors = SOUND_ANCESTORS, .n_ancestors = sizeof SOUND_ANCESTORS / sizeof SOUND_ANCESTORS[0],
    .donor = "SoundOptionsPage1", .donor_size = 10684, .donor_strings_end = 0x502,
    .content = 0x23ad, .content_len = 0x368,           /* MASTER VOLUME */
    .nodes = SOUND_NODES, .n_nodes = sizeof SOUND_NODES / sizeof SOUND_NODES[0],
    .rows = SOUND_ROWS, .n_rows = sizeof SOUND_ROWS / sizeof SOUND_ROWS[0],
};

/* ---------------------------------------------------------------- the variables */

static BYTE *g_controls;             /* the Controls page */
static void *g_key_node[2];          /* its Options/Controls/Voice/ProximityKey, RadioKey */
static int g_capture;                /* Options/Controls/Voice/Capture: 1 nearby, 2 radio (set by the rows' buttons) */

/* the key's name as the Controls menu shows it */
static void key_name(void *page, const char *key, wchar_t *out, int size)
{
    typedef void *(__attribute__((thiscall)) *key_name_fn)(void *self, void *out, bf_msvc_string key, int flag);
    BYTE w[BF_STRING_SIZE];
    bf_msvc_string k;
    out[0] = L'\0';
    if (!key[0] || !bf_string_init(&k, !strncmp(key, "Mouse_", 6) ? key + 6 : key)) {
        return;
    }
    ((key_name_fn)BF_ADDR(KEY_NAME))(page, w, k, 0);   /* destroys k */
    lstrcpynW(out, bf_wstring_chars(w), size);
    bf_wstring_free(w);
}

static int __attribute__((thiscall)) hk_controls_vars(BYTE *page)
{
    static const char *const vars[2] = {"ProximityKey", "RadioKey"};
    wchar_t text[64];
    int r = ((int (__attribute__((thiscall)) *)(void *))BF_ADDR(CONTROLS_VARS))(page), i;
    g_controls = page;
    bf_menu_bind_int(page, "Options/Controls/Voice", "Capture", &g_capture);
    bf_meme_bind(*(void **)(page + 4), "Options/Controls/Voice", "NearbyActivation", BF_MENU_BOOL, &bf_voice_va[0]);
    bf_meme_bind(*(void **)(page + 4), "Options/Controls/Voice", "RadioActivation", BF_MENU_BOOL, &bf_voice_va[1]);
    for (i = 0; i < 2; i++) {
        key_name(page, bf_voice_key(i), text, 64);
        g_key_node[i] = bf_menu_bind_wstring(page, "Options/Controls/Voice", vars[i], text);
    }
    return r;
}

static int __attribute__((thiscall)) hk_sound_vars(void *page)
{
    int r = ((int (__attribute__((thiscall)) *)(void *))BF_ADDR(SOUND_VARS))(page);
    bf_menu_bind_int(page, "Options/Sound", "VoiceVolume", &bf_voice_volume);
    bf_menu_bind_int(page, "Options/Sound", "MicSensitivity", &bf_voice_mic);
    return r;
}

/* ---------------------------------------------------------------- the key capture */

typedef void (__attribute__((thiscall)) *assign_fn)(BYTE *page, bf_msvc_string shown, bf_msvc_string key, int keep);
static assign_fn g_assign;

static void __attribute__((thiscall)) hk_assign(BYTE *page, bf_msvc_string shown, bf_msvc_string key, int keep)
{
    int which = g_capture - 1;
    const char *k;
    if (page != g_controls || (which != BF_VOICE_NEARBY && which != BF_VOICE_RADIO)) {
        g_assign(page, shown, key, keep);
        return;
    }
    k = bf_string_chars(&key);
    if (!k[0] || bf_voice_vk(k)) {
        bf_voice_set_key(which, k);
        bf_menu_set_wstring(page, g_key_node[which], k[0] ? bf_wstring_chars(&shown) : L"");
        g_capture = 0;
        if (!(keep & 0xff)) {
            page[PAGE_WAITING] = 0;
        }
    } else {
        static char refused[48];     /* once per key: the page hands it over again while it is held */
        if (strcmp(refused, k)) {
            lstrcpynA(refused, k, sizeof refused);
            bf_log("voice: %s cannot be held to talk (keyboard keys and mouse buttons can)", k);
        }
    }
    /* by value: this end destroys them */
    bf_string_free(&key);
    bf_wstring_free(&shown);
}

/* a capture the page gave up without a key (the menu was left) */
void bf_voice_menu_tick(void)
{
    if (g_capture && g_controls && !IsBadReadPtr(g_controls + PAGE_WAITING, 1) && !g_controls[PAGE_WAITING]) {
        g_capture = 0;
    }
}

void bf_voice_menu_install(void)
{
    static const BYTE assign_head[7] = {0x8a, 0x44, 0x24, 0x3c, 0x83, 0xec, 0x38};   /* mov al,[esp+0x3c]; sub esp,0x38 */
    static const BYTE name_head[6] = {0x81, 0xec, 0x90, 0x00, 0x00, 0x00};           /* sub esp, 0x90 */
    if (!bf_ini_int("Voice", "VoiceOptions", 1)) {
        bf_log("voice: VoiceOptions=0, the keys and volumes are only in the settings");
        return;
    }
    if (*(DWORD *)BF_ADDR(CONTROLS_VARS_SLOT) != CONTROLS_VARS || !bf_expect(KEY_NAME, name_head, sizeof name_head)
        || !(g_assign = (assign_fn)bf_detour(KEY_ASSIGN, assign_head, sizeof assign_head, hk_assign))
        || !bf_menu_graft("CommonControlsPage3", &bf_voice_keys_graft, "the push-to-talk keys")) {
        bf_log("voice: the Controls options code differs, the push-to-talk keys are only in the settings");
    } else {
        bf_hook_slot(CONTROLS_VARS_SLOT, CONTROLS_VARS, hk_controls_vars);
    }
    if (*(DWORD *)BF_ADDR(SOUND_VARS_SLOT) != SOUND_VARS
        || !bf_menu_graft("SoundOptionsPage2", &bf_voice_sound_graft, "the voice volume and microphone rows")) {
        bf_log("voice: the Sound options code differs, the voice volumes are only in the settings");
    } else {
        bf_hook_slot(SOUND_VARS_SLOT, SOUND_VARS, hk_sound_vars);
    }
}
