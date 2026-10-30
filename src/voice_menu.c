/* Voice chat in the game's menus: the push-to-talk keys in Options -> Controls -> Common, the microphone in
 * Options -> Sound (page 1), the voice volume, the voice input mode and the microphone sensitivity in Options -> Sound
 * (page 2).
 *
 * The keys. The Controls pages bind 83 actions, each row a label and two buttons (primary / alternate key) showing
 * the key's name, picked out of a list of 83 texts by the row's index (IndexDataData). A click sets WaitForKey,
 * Alternate and ActiveKey = the row's index; the page then reads the next key or button (0x6c67d0, the mouse's left
 * button through Options/Controls/PrimaryMouseButton) and hands it to 0x6c6040, thiscall(page, std::wstring shown,
 * std::string key, bool keep_waiting) by value, ret 0x3c, which stores it for action ActiveKey and clears
 * WaitForKey. The 83 are fixed arrays in the page object (0x6c5980, registerVariables), so voice gets rows of its
 * own: menu/CommonControlsPage3 (3 rows, room below) gets two copies of its VOTE NO row (0x2219, content 0x2239)
 * at y 85 / 110 with
 *   - each button's key list (IndexDataData 0x2aa3, the alternate's 0x2420) replaced by
 *     Options/Controls/Voice/ProximityKey and ProximityKeyAlternate (or RadioKey, RadioKeyAlternate), WstringData
 *     bound here (0x69b8f0) and set with the key's name as the menu shows it (0x6c2940, thiscall(any, std::wstring
 *     *out, std::string key, int), ret 0x24; mouse buttons by IDButton_<n>);
 *   - each button's click setting Options/Controls/Voice/Capture = 1 (or 2; 3 or 4 for the alternate) instead of
 *     ActiveKey (0x2a52 / 0x2a5e, the alternate's 0x23cf / 0x23db), and its hover the conflict index -1 (0x29b5,
 *     0x2332);
 *   - a literal label (in place of the BfLocaleStringData 0x2f91).
 * 0x6c6040 is detoured: while Capture is set, the key goes to voice (kept in the player's bf1942patch.con) and the game's own
 * bindings are not touched. Escape (an empty key) leaves the action without a key, as for the game's. Joystick
 * buttons and axes are refused (the page keeps waiting).
 *
 * The selects. menu/SoundOptionsPage1 gets MICROPHONE in place of DISABLE ALL SOUND, and menu/SoundOptionsPage2
 * VOICE INPUT MODE: each one more copy of the VOTE NO key row, without its alternate button, its key button moved
 * and widened to the page's value column to show a text of ours, and its label moved in line with the page's. A
 * click sets Options/Sound/Voice/NextMicrophone (the next voice tick steps to the next device) or NextInputMode (push
 * to talk <-> voice activation).
 *
 * The sliders. menu/SoundOptionsPage2 (hardware, speakers, localized voice; room from y 210) gets two copies of
 * page 1's MASTER VOLUME row (content 0x23ad: a number box, "%", a slider), opened through the game's opener as the
 * donor: VOICE CHAT VOLUME on Options/Sound/VoiceVolume above VOICE INPUT MODE, and MIC SENSITIVITY on
 * Options/Sound/MicSensitivity under it, 0..200 (the box's range at 0x24c8, the slider's at 0x2608), bound by
 * SoundOptions::registerVariables (0x6b2ce0, vtable slot 0x91e5c0). voice.c applies them as they move. */
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
/* the VOTE NO row but the inside of its alternate button (0x2249, replaced by a NULL in the selects) */
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
/* the whole VOTE NO row, its alternate button too (the keys) */
static const bf_meme_node KEYS_ALT_NODES[] = {
    {0x2239, "TransformNode"}, {0x2241, "TransformNode"}, {0x2249, "TransformNode"}, {0x2269, "TransformNode"},
    {0x2271, "TransformNode"}, {0x2279, "TransformNode"}, {0x2281, "CullEventActionNode"},
    {0x2289, "CullEventActionNode"}, {0x2291, "CullEventActionNode"}, {0x22a1, "ActionListAction"},
    {0x22a9, "SetVariableAction"}, {0x22b1, "IntData"}, {0x22bd, "IntData"}, {0x22d1, "TypeEvent"},
    {0x22de, "ActionListAction"}, {0x22e6, "SetVariableAction"}, {0x22ee, "IntData"}, {0x22fa, "PointerXData"},
    {0x2302, "SetVariableAction"}, {0x230a, "IntData"}, {0x2316, "PointerYData"}, {0x231e, "SetVariableAction"},
    {0x2326, "IntData"}, {0x2332, "IntData"}, {0x2346, "TypeEvent"}, {0x2353, "ActionListAction"},
    {0x235b, "SetVariableAction"}, {0x2363, "BoolData"}, {0x236c, "BoolData"}, {0x2375, "SetVariableAction"},
    {0x237d, "BoolData"}, {0x2386, "BoolData"}, {0x238f, "SetVariableAction"}, {0x2397, "IntData"},
    {0x23a3, "PointerXData"}, {0x23ab, "SetVariableAction"}, {0x23b3, "IntData"}, {0x23bf, "PointerYData"},
    {0x23c7, "SetVariableAction"}, {0x23cf, "IntData"}, {0x23db, "IntData"}, {0x23ef, "ButtonEvent"},
    {0x2410, "TextNode"}, {0x2420, "IndexDataData"}, {0x2844, "CenterAlignedStyle"}, {0x286a, "EffectNode"},
    {0x2872, "PictureNode"}, {0x2883, "ColorEffect"}, {0x28ab, "EffectNode"}, {0x28b3, "PictureNode"},
    {0x28c4, "ColorEffect"}, {0x28ec, "TransformNode"}, {0x28f4, "TransformNode"}, {0x28fc, "TransformNode"},
    {0x2904, "CullEventActionNode"}, {0x290c, "CullEventActionNode"}, {0x2914, "CullEventActionNode"},
    {0x2924, "ActionListAction"}, {0x292c, "SetVariableAction"}, {0x2934, "IntData"}, {0x2940, "IntData"},
    {0x2954, "TypeEvent"}, {0x2961, "ActionListAction"}, {0x2969, "SetVariableAction"}, {0x2971, "IntData"},
    {0x297d, "PointerXData"}, {0x2985, "SetVariableAction"}, {0x298d, "IntData"}, {0x2999, "PointerYData"},
    {0x29a1, "SetVariableAction"}, {0x29a9, "IntData"}, {0x29b5, "IntData"}, {0x29c9, "TypeEvent"},
    {0x29d6, "ActionListAction"}, {0x29de, "SetVariableAction"}, {0x29e6, "BoolData"}, {0x29ef, "BoolData"},
    {0x29f8, "SetVariableAction"}, {0x2a00, "BoolData"}, {0x2a09, "BoolData"}, {0x2a12, "SetVariableAction"},
    {0x2a1a, "IntData"}, {0x2a26, "PointerXData"}, {0x2a2e, "SetVariableAction"}, {0x2a36, "IntData"},
    {0x2a42, "PointerYData"}, {0x2a4a, "SetVariableAction"}, {0x2a52, "IntData"}, {0x2a5e, "IntData"},
    {0x2a72, "ButtonEvent"}, {0x2a93, "TextNode"}, {0x2aa3, "IndexDataData"}, {0x2ed9, "CenterAlignedStyle"},
    {0x2eff, "EffectNode"}, {0x2f07, "PictureNode"}, {0x2f18, "ColorEffect"}, {0x2f40, "EffectNode"},
    {0x2f48, "PictureNode"}, {0x2f59, "ColorEffect"}, {0x2f81, "TextNode"}, {0x2f91, "BfLocaleStringData"},
    {0x2fd5, "Style"},
};
#define VOTE_NO    "Options/Controls/Common/Index/VoteNo"
#define ACTIVE_KEY "Options/Controls/Key/ActiveKey"
#define CAPTURE    "Options/Controls/Voice/Capture"
#define KEY_EDITS(n, var, alt_var, label) {                                                         \
    {0x2332, VOTE_NO, BF_MEME_INT, NULL, -1, NULL, 0},                    /* alternate: no conflict */ \
    {0x23cf, ACTIVE_KEY, BF_MEME_INT, CAPTURE, 0, NULL, 0},                                            \
    {0x23db, VOTE_NO, BF_MEME_INT, NULL, (n) + 2, NULL, 0},               /* click: Capture = n + 2 */ \
    {0x2420, NULL, BF_MEME_WSTRING, (alt_var), 0, NULL, 0},               /* the key's name */         \
    {0x29b5, VOTE_NO, BF_MEME_INT, NULL, -1, NULL, 0},                    /* primary: no conflict */   \
    {0x2a52, ACTIVE_KEY, BF_MEME_INT, CAPTURE, 0, NULL, 0},                                            \
    {0x2a5e, VOTE_NO, BF_MEME_INT, NULL, (n), NULL, 0},                   /* click: Capture = n */     \
    {0x2aa3, NULL, BF_MEME_WSTRING, (var), 0, NULL, 0},                                                \
    {0x2f91, NULL, BF_MEME_STRING, NULL, 0, (label), 0},                                               \
}
static const bf_meme_edit KEYS_NEARBY[] = KEY_EDITS(1, "Options/Controls/Voice/ProximityKey",
                                                    "Options/Controls/Voice/ProximityKeyAlternate",
                                                    "PUSH TO TALK: NEARBY");
static const bf_meme_edit KEYS_RADIO[] = KEY_EDITS(2, "Options/Controls/Voice/RadioKey",
                                                   "Options/Controls/Voice/RadioKeyAlternate", "PUSH TO TALK: RADIO");
static const bf_meme_graft_row KEYS_ROWS[] = {
    {{0, 85, 380, 20}, KEYS_NEARBY, sizeof KEYS_NEARBY / sizeof KEYS_NEARBY[0], NULL, 0, NULL},
    {{0, 110, 380, 20}, KEYS_RADIO, sizeof KEYS_RADIO / sizeof KEYS_RADIO[0], NULL, 0, NULL},
};
const bf_meme_graft bf_voice_keys_graft = {
    .size = 19317, .strings_end = 0x1f94,
    .hang_at = 0x2221,                                 /* after VOTE NO */
    .ancestors = KEYS_ANCESTORS, .n_ancestors = sizeof KEYS_ANCESTORS / sizeof KEYS_ANCESTORS[0],
    .content = 0x2239, .content_len = 0xdb2,
    .nodes = KEYS_ALT_NODES, .n_nodes = sizeof KEYS_ALT_NODES / sizeof KEYS_ALT_NODES[0],
    .rows = KEYS_ROWS, .n_rows = sizeof KEYS_ROWS / sizeof KEYS_ROWS[0],
};

/* ---------------------------------------------------------------- the selects: a key row showing a text of ours */

/* The copy of menu/CommonControlsPage3's VOTE NO row has no alternate button; its key button (the boxes of 0x2241,
 * 0x28ec, 0x28f4, 0x28fc) takes the Sound pages' value column, and its label (0x2239's box) moves 7 in, as theirs.
 * Its click sets click_var = 1; the Controls page's variables it also sets (the conflict text's place, WaitForKey,
 * ...) become Options/Sound/Voice/Unused (an int) and UnusedFlag. */
#define UNUSED     "Options/Sound/Voice/Unused"
#define UNUSED_FLAG "Options/Sound/Voice/UnusedFlag"
#define SELECT_EDITS(click_var, text_var, label) {                                                  \
    {0x2249, NULL, BF_MEME_NULL, NULL, 0, NULL, 0},                       /* the alternate button */   \
    {0x2934, "Options/Controls/Conflict/MouseOverPrimary", BF_MEME_INT, UNUSED, 0, NULL, 0},           \
    {0x2971, "Options/Controls/Conflict/ConflictX", BF_MEME_INT, UNUSED, 0, NULL, 0},                  \
    {0x298d, "Options/Controls/Conflict/ConflictY", BF_MEME_INT, UNUSED, 0, NULL, 0},                  \
    {0x29a9, "Options/Controls/Conflict/MouseOverPrimary", BF_MEME_INT, UNUSED, 0, NULL, 0},           \
    {0x29b5, VOTE_NO, BF_MEME_INT, NULL, -1, NULL, 0},                                                 \
    {0x29e6, "Options/Controls/WaitForKey", BF_MEME_BOOL, UNUSED_FLAG, 0, NULL, 0},                    \
    {0x2a00, "Options/Controls/Alternate", BF_MEME_BOOL, UNUSED_FLAG, 0, NULL, 0},                     \
    {0x2a1a, "Options/Controls/Key/DefineKeyX", BF_MEME_INT, UNUSED, 0, NULL, 0},                      \
    {0x2a36, "Options/Controls/Key/DefineKeyY", BF_MEME_INT, UNUSED, 0, NULL, 0},                      \
    {0x2a52, ACTIVE_KEY, BF_MEME_INT, (click_var), 0, NULL, 0},                                        \
    {0x2a5e, VOTE_NO, BF_MEME_INT, NULL, 1, NULL, 0},                     /* click: click_var = 1 */   \
    {0x2aa3, NULL, BF_MEME_WSTRING, (text_var), 0, NULL, 0},                                           \
    {0x2f91, NULL, BF_MEME_STRING, NULL, 0, (label), 0},                                               \
}
static const bf_meme_edit SELECT_MIC[] = SELECT_EDITS("Options/Sound/Voice/NextMicrophone",
                                                      "Options/Sound/Voice/Microphone", "MICROPHONE");
static const bf_meme_edit SELECT_MODE[] = SELECT_EDITS("Options/Sound/Voice/NextInputMode",
                                                       "Options/Sound/Voice/InputMode", "VOICE INPUT MODE");
#define SELECT_X 120                 /* the button, 120..360: the Sound pages' values end at 360 */
#define SELECT_W 240
static const float SEL_LABEL[4] = {0, 5, 180, 20}, SEL_NEW_LABEL[4] = {7, 5, SELECT_X - 10, 20};
static const float SEL_AT[4] = {180, 0, 90, 19}, SEL_NEW_AT[4] = {SELECT_X, 0, SELECT_W, 19};
static const float SEL_BOX[4] = {0, 0, 90, 19}, SEL_NEW_BOX[4] = {0, 0, SELECT_W, 19};
static const float SEL_FILL[4] = {1, 1, 88, 17}, SEL_NEW_FILL[4] = {1, 1, SELECT_W - 2, 17};
static const float SEL_TEXT[4] = {0, 4, 90, 20}, SEL_NEW_TEXT[4] = {0, 4, SELECT_W, 20};
static const bf_meme_bytes SELECT_BYTES[] = {
    {0x28dc, 16, SEL_AT, SEL_NEW_AT},
    {0x2a83, 16, SEL_TEXT, SEL_NEW_TEXT},
    {0x2eef, 16, SEL_FILL, SEL_NEW_FILL},
    {0x2f30, 16, SEL_BOX, SEL_NEW_BOX},
    {0x2f71, 16, SEL_LABEL, SEL_NEW_LABEL},
};
static const bf_meme_piece SELECT_PIECE = {0x2239, 0xdb2, KEYS_NODES, sizeof KEYS_NODES / sizeof KEYS_NODES[0],
                                           "CommonControlsPage3", 19317, 0x1f94};

/* ---------------------------------------------------------------- menu/SoundOptionsPage2: the sliders, the mode */

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
/* 25 apart, clear of the page's footer (y 279) */
static const bf_meme_graft_row SOUND_ROWS[] = {
    {{5, 210, 360, 20}, SOUND_VOICE, sizeof SOUND_VOICE / sizeof SOUND_VOICE[0], SOUND_BYTES, 2, NULL},
    {{5, 235, 360, 20}, SELECT_MODE, sizeof SELECT_MODE / sizeof SELECT_MODE[0], SELECT_BYTES,
     sizeof SELECT_BYTES / sizeof SELECT_BYTES[0], &SELECT_PIECE},
    {{5, 260, 360, 20}, SOUND_MIC, sizeof SOUND_MIC / sizeof SOUND_MIC[0], SOUND_BYTES, 2, NULL},
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

/* ---------------------------------------------------------------- menu/SoundOptionsPage1: the microphone */

/* DISABLE ALL SOUND is the content of the page's outermost row (0x765, box 5 40 360 20); that row is moved out of
 * sight, and the MICROPHONE select takes its place, hung on the NULL next of the SOUND DETAIL row (0x78d), the
 * innermost of the chain. */
static const DWORD MIC_ANCESTORS[] = {0x505, 0x765, 0x76d, 0x775, 0x77d, 0x785, 0x78d};
static const bf_meme_graft_row MIC_ROWS[] = {
    {{5, 40, 360, 20}, SELECT_MIC, sizeof SELECT_MIC / sizeof SELECT_MIC[0], SELECT_BYTES,
     sizeof SELECT_BYTES / sizeof SELECT_BYTES[0], NULL},
};
static const float DISABLE_ALL[4] = {5, 40, 360, 20}, HIDDEN[4] = {-4096, -4096, 360, 20};
static const bf_meme_bytes MIC_PAGE_BYTES[] = {
    {0x2715, 16, DISABLE_ALL, HIDDEN},                     /* DISABLE ALL SOUND's row */
};
const bf_meme_graft bf_voice_mic_graft = {
    .size = 10684, .strings_end = 0x502,
    .hang_at = 0x795,                                  /* after SOUND DETAIL */
    .ancestors = MIC_ANCESTORS, .n_ancestors = sizeof MIC_ANCESTORS / sizeof MIC_ANCESTORS[0],
    .donor = "CommonControlsPage3", .donor_size = 19317, .donor_strings_end = 0x1f94,
    .content = 0x2239, .content_len = 0xdb2,           /* VOTE NO */
    .nodes = KEYS_NODES, .n_nodes = sizeof KEYS_NODES / sizeof KEYS_NODES[0],
    .rows = MIC_ROWS, .n_rows = sizeof MIC_ROWS / sizeof MIC_ROWS[0],
    .bytes = MIC_PAGE_BYTES, .n_bytes = sizeof MIC_PAGE_BYTES / sizeof MIC_PAGE_BYTES[0],
};

/* ---------------------------------------------------------------- the variables */

static BYTE *g_controls;             /* the Controls page */
static void *g_key_node[2][2];       /* its Options/Controls/Voice/ProximityKey, RadioKey; [1] their ...Alternate */
static int g_capture;                /* Options/Controls/Voice/Capture: 1 nearby, 2 radio, 3 / 4 their alternates (set
                                      * by the rows' buttons) */
static void *g_sound;                /* the Sound options */
static void *g_mic_node;             /* their Options/Sound/Voice/Microphone */
static int g_mic_shown;              /* its text is the device's (the device list needs the library) */
static int g_next_mic;               /* Options/Sound/Voice/NextMicrophone: the row was clicked */
static void *g_mode_node;            /* Options/Sound/Voice/InputMode */
static int g_mode_shown = -1;        /* the mode its text shows */
static int g_next_mode;              /* Options/Sound/Voice/NextInputMode: the row was clicked */
static int g_unused;                 /* Options/Sound/Voice/Unused, UnusedFlag: what the copied buttons set */
static unsigned char g_unused_flag;

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
    static const char *const vars[2][2] = {{"ProximityKey", "ProximityKeyAlternate"}, {"RadioKey", "RadioKeyAlternate"}};
    wchar_t text[64];
    int r = ((int (__attribute__((thiscall)) *)(void *))BF_ADDR(CONTROLS_VARS))(page), i, alt;
    g_controls = page;
    bf_menu_bind_int(page, "Options/Controls/Voice", "Capture", &g_capture);
    for (i = 0; i < 2; i++) {
        for (alt = 0; alt < 2; alt++) {
            key_name(page, bf_voice_key(i, alt), text, 64);
            g_key_node[i][alt] = bf_menu_bind_wstring(page, "Options/Controls/Voice", vars[i][alt], text);
        }
    }
    return r;
}

/* voice activation (nearby, or on the radio from voiceChat.radioActivation) or push to talk */
static int voice_activation(void)
{
    return bf_voice_va[0] || bf_voice_va[1];
}

static const wchar_t *mode_text(int va)
{
    return va ? L"VOICE ACTIVATION" : L"PUSH TO TALK";
}

static int __attribute__((thiscall)) hk_sound_vars(void *page)
{
    wchar_t text[64];
    int r = ((int (__attribute__((thiscall)) *)(void *))BF_ADDR(SOUND_VARS))(page);
    g_sound = page;
    bf_menu_bind_int(page, "Options/Sound", "VoiceVolume", &bf_voice_volume);
    bf_menu_bind_int(page, "Options/Sound", "MicSensitivity", &bf_voice_mic);
    bf_menu_bind_int(page, "Options/Sound/Voice", "NextMicrophone", &g_next_mic);
    bf_menu_bind_int(page, "Options/Sound/Voice", "NextInputMode", &g_next_mode);
    bf_menu_bind_int(page, "Options/Sound/Voice", "Unused", &g_unused);
    bf_meme_bind(*(void **)((BYTE *)page + 4), "Options/Sound/Voice", "UnusedFlag", BF_MENU_BOOL, &g_unused_flag);
    bf_voice_mic_text(text, 64);
    g_mic_node = bf_menu_bind_wstring(page, "Options/Sound/Voice", "Microphone", text);
    g_mic_shown = 0;
    g_mode_shown = voice_activation();
    g_mode_node = bf_menu_bind_wstring(page, "Options/Sound/Voice", "InputMode", mode_text(g_mode_shown));
    return r;
}

/* ---------------------------------------------------------------- the key capture */

typedef void (__attribute__((thiscall)) *assign_fn)(BYTE *page, bf_msvc_string shown, bf_msvc_string key, int keep);
static assign_fn g_assign;

static void __attribute__((thiscall)) hk_assign(BYTE *page, bf_msvc_string shown, bf_msvc_string key, int keep)
{
    int which = (g_capture - 1) & 1, alt = (g_capture - 1) >> 1;
    const char *k;
    if (page != g_controls || g_capture < 1 || g_capture > 4) {
        g_assign(page, shown, key, keep);
        return;
    }
    k = bf_string_chars(&key);
    if (!k[0] || bf_voice_vk(k)) {
        bf_voice_set_key(which, alt, k);
        bf_menu_set_wstring(page, g_key_node[which][alt], k[0] ? bf_wstring_chars(&shown) : L"");
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

/* the MICROPHONE row's click, and its text once the library is up (the Sound options are bound before it loads); the
 * VOICE INPUT MODE row's click (voice activation becomes push to talk, and push to talk nearby voice activation), and
 * its text when the mode changes (also in the console); a capture the Controls page gave up without a key (the menu
 * was left) */
void bf_voice_menu_tick(void)
{
    wchar_t text[64];
    if (g_next_mic) {
        g_next_mic = 0;
        bf_voice_next_mic();
        g_mic_shown = 0;
    }
    if (g_mic_node && !g_mic_shown) {
        g_mic_shown = 1;
        bf_voice_mic_text(text, 64);
        bf_menu_set_wstring(g_sound, g_mic_node, text);
    }
    if (g_next_mode) {
        int va = !voice_activation();
        g_next_mode = 0;
        bf_voice_va[0] = (unsigned char)va;
        bf_voice_va[1] = 0;
    }
    if (g_mode_node && g_mode_shown != voice_activation()) {
        g_mode_shown = voice_activation();
        bf_menu_set_wstring(g_sound, g_mode_node, mode_text(g_mode_shown));
    }
    if (g_capture && g_controls && !IsBadReadPtr(g_controls + PAGE_WAITING, 1) && !g_controls[PAGE_WAITING]) {
        g_capture = 0;
    }
}

void bf_voice_menu_install(void)
{
    static const BYTE assign_head[7] = {0x8a, 0x44, 0x24, 0x3c, 0x83, 0xec, 0x38};   /* mov al,[esp+0x3c]; sub esp,0x38 */
    static const BYTE name_head[6] = {0x81, 0xec, 0x90, 0x00, 0x00, 0x00};           /* sub esp, 0x90 */
    if (!bf_setting_int("voiceOptions")) {
        bf_log("voice: voiceOptions 0, the keys and volumes are only in the settings");
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
        || !bf_menu_graft("SoundOptionsPage2", &bf_voice_sound_graft, "the voice volume, input mode and sensitivity rows")
        || !bf_menu_graft("SoundOptionsPage1", &bf_voice_mic_graft, "the microphone row")) {
        bf_log("voice: the Sound options code differs, the voice volumes, the input mode and the microphone are only in "
               "the settings");
    } else {
        bf_hook_slot(SOUND_VARS_SLOT, SOUND_VARS, hk_sound_vars);
    }
}
