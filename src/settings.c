/* The table of the patch's options, their values, and the bf1942patch.con parser (settings.h). Plain C: the game,
 * the Windows dedicated server and the Linux dedicated server all use it.
 * The console names sit with the game's own: game.* (like game.setMinimapTransparency and game.lockFps), renderer.*
 * (like renderer.fieldOfView), and voiceChat.* for the voice chat. None of them is a name the game has. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

#define I(name, console, flags, def, section, key) {name, console, BF_OPT_INT, flags, def, NULL, section, key}
#define S(name, console, flags, def, section, key) {name, console, BF_OPT_STR, flags, 0, def, section, key}

const bf_option bf_options[] = {
    I("enabled", "game.patchEnabled", BF_OPT_READONLY, 1, "General", "Enabled"),
    /* saves */
    I("redirectSaves", "game.redirectSaves", BF_OPT_INSTALL | BF_OPT_READONLY, 1, "Saves", "Redirect"),
    S("saveFolder", "game.saveFolder", BF_OPT_INSTALL | BF_OPT_READONLY, "", "Saves", "Folder"),
    I("logFiles", "game.logFiles", 0, 0, "Saves", "LogFiles"),
    /* registry, CD keys */
    I("noRegistry", "game.noRegistry", 0, 1, "Registry", "UseFile"),
    I("logRegistry", "game.logRegistry", 0, 0, "Registry", "LogAccess"),
    S("cdKey", "game.cdKey", 0, "", "CDKey", "Key"),
    S("cdKeyXPack1", "game.cdKeyXPack1", 0, "", "CDKey", "XPack1"),
    S("cdKeyXPack2", "game.cdKeyXPack2", 0, "", "CDKey", "XPack2"),
    S("language", "game.installLanguage", 0, "", "Game", "Language"),
    /* network */
    I("openSpy", "game.openSpy", 0, 1, "Network", "OpenSpy"),
    S("masterServer", "game.masterServer", 0, "", "Network", "MasterServer"),
    /* display */
    I("borderless", "renderer.borderless", 0, 1, "Display", "Borderless"),
    I("monitor", "renderer.monitor", 0, 0, "Display", "Monitor"),
    I("vsync", "renderer.vSync", 0, 0, "Display", "VSync"),
    I("dpiAware", "renderer.dpiAware", 0, 1, "Display", "DpiAware"),
    I("modeFallback", "renderer.modeFallback", 0, 1, "Display", "ModeFallback"),
    I("nativeMenus", "renderer.nativeMenus", 0, 1, "Display", "NativeMenus"),
    /* frame rate */
    I("unlockFps", "game.unlockFps", 0, 1, "FrameRate", "Unlock"),
    I("maxFps", "game.maxFps", 0, 0, "FrameRate", "MaxFps"),
    I("maxFpsOption", "game.maxFpsOption", 0, 1, "FrameRate", "MaxFpsOption"),
    I("realTimeHud", "game.realTimeHud", 0, 1, "FrameRate", "FixHud"),
    /* restarts */
    I("seamlessRestart", "game.seamlessRestart", 0, 1, "Restart", "Seamless"),
    /* widescreen, HUD */
    I("horPlus", "renderer.horPlus", 0, 1, "Widescreen", "HorPlus"),
    I("widescreenHud", "game.widescreenHud", 0, 1, "Widescreen", "FixHud"),
    I("anchorHud", "game.anchorHud", 0, 1, "Widescreen", "AnchorHud"),
    I("hudScale", "game.hudScale", BF_OPT_PROFILE, 100, "Widescreen", "HudScale"),
    I("hudScaleOption", "game.hudScaleOption", 0, 1, "Widescreen", "HudScaleOption"),
    /* detail */
    I("meshLod", "renderer.meshLod", 0, 300, "Detail", "MeshLod"),
    I("treeLod", "renderer.treeLod", 0, 300, "Detail", "TreeLod"),
    I("terrainLod", "renderer.terrainLod", 0, 300, "Detail", "TerrainLod"),
    I("terrainDetail", "renderer.terrainDetail", 0, 400, "Detail", "TerrainDetail"),
    /* gamepads */
    I("gamepad", "game.gamepad", 0, 1, "Gamepad", "UseSDL"),
    I("gamepadDeadzone", "game.gamepadDeadzone", 0, 15, "Gamepad", "Deadzone"),
    I("gamepadSensitivity", "game.gamepadSensitivity", 0, 100, "Gamepad", "Sensitivity"),
    I("gamepadBindings", "game.gamepadBindings", 0, 1, "Gamepad", "Bindings"),
    I("gamepadHideDuplicates", "game.gamepadHideDuplicates", 0, 1, "Gamepad", "HideDuplicates"),
    I("gamepadStartKey", "game.gamepadStartKey", 0, 0, "Gamepad", "StartKey"),
    I("gamepadBackKey", "game.gamepadBackKey", 0, 0, "Gamepad", "BackKey"),
    /* voice chat */
    I("voiceChat", "voiceChat.enabled", 0, 1, "Voice", "VoiceChat"),
    I("voicePort", "voiceChat.port", 0, 27800, "Voice", "VoicePort"),
    S("voicePassword", "voiceChat.password", 0, "", "Voice", "VoicePassword"),
    S("voiceServer", "voiceChat.server", 0, "", "Voice", "VoiceServer"),
    S("voiceQueryPorts", "game.queryPorts", 0, "22000,23000", "Voice", "VoiceQueryPorts"),   /* FastDL asks too */
    I("voiceActivation", "voiceChat.voiceActivation", 0, 0, "Voice", "VoiceActivation"),
    I("radioActivation", "voiceChat.radioActivation", 0, 0, "Voice", "RadioActivation"),
    S("proximityKey", "voiceChat.proximityKey", 0, "IDKey_Capital", "Voice", "ProximityKey"),
    S("radioKey", "voiceChat.radioKey", 0, "Mouse_IDButton_3", "Voice", "RadioKey"),
    S("proximityKeyAlternate", "voiceChat.proximityKeyAlternate", 0, "none", "Voice", "ProximityKeyAlternate"),
    S("radioKeyAlternate", "voiceChat.radioKeyAlternate", 0, "none", "Voice", "RadioKeyAlternate"),
    I("voiceVolume", "voiceChat.volume", 0, 100, "Voice", "VoiceVolume"),
    I("micSensitivity", "voiceChat.micSensitivity", 0, 100, "Voice", "MicSensitivity"),
    S("inputDevice", "voiceChat.inputDevice", 0, "", "Voice", "InputDevice"),
    I("voiceHud", "voiceChat.hud", 0, 1, "Voice", "VoiceHud"),
    I("voiceOptions", "voiceChat.options", 0, 1, "Voice", "VoiceOptions"),
    I("voiceMuteButton", "voiceChat.muteButton", 0, 1, "Voice", "VoiceMuteButton"),
    S("voiceLibrary", "voiceChat.library", 0, "", "Voice", "VoiceLibrary"),
    /* FastDL: maps and mods the game lacks, downloaded through the Interposer */
    I("fastDl", "game.fastDl", 0, 1, "FastDL", "FastDL"),
    S("fastDlUrl", "game.fastDlUrl", 0, "", "FastDL", "FastDLUrl"),
    /* the dedicated server's FastDL server, advertised in its query replies */
    S("fastDlHost", "game.fastDlHost", BF_OPT_SERVER, "", "FastDL", "FastDLHost"),
    I("fastDlPort", "game.fastDlPort", BF_OPT_SERVER, 0, "FastDL", "FastDLPort"),
    S("fastDlPath", "game.fastDlPath", BF_OPT_SERVER, "", "FastDL", "FastDLPath"),
    /* the dedicated server's voice server */
    S("voiceBindAddress", "voiceChat.bindAddress", BF_OPT_SERVER, "", "Voice", "VoiceBindAddress"),
    S("voiceAdminKey", "voiceChat.adminKey", BF_OPT_SERVER, "", "Voice", "VoiceAdminKey"),
    I("voiceMaxPlayers", "voiceChat.maxPlayers", BF_OPT_SERVER, 256, "Voice", "VoiceMaxPlayers"),
    I("voiceMaxBitrate", "voiceChat.maxBitrate", BF_OPT_SERVER, 32000, "Voice", "VoiceMaxBitrate"),
    /* metres, may have decimals */
    S("proximityRange", "voiceChat.proximityRange", BF_OPT_SERVER, "50", "Voice", "ProximityRange"),
    S("proximityFullVolumeRange", "voiceChat.proximityFullVolumeRange", BF_OPT_SERVER, "4", "Voice",
      "ProximityFullVolumeRange"),
    I("voiceIncludeBots", "voiceChat.includeBots", BF_OPT_SERVER, 0, "Voice", "VoiceIncludeBots"),
    I("suppressDialogs", "game.suppressDialogs", BF_OPT_SERVER, 1, "Server", "SuppressDialogs"),
    I("logToStderr", "game.logToStderr", BF_OPT_SERVER, 0, "Server", "LogToStderr"),
    /* troubleshooting */
    I("crashLog", "game.crashLog", 0, 0, "Debug", "CrashLog"),
    I("logHud", "game.logHud", 0, 0, "Debug", "LogHud"),
    I("logFps", "game.logFps", 0, 0, "Debug", "LogFps"),
};
const int bf_n_options = sizeof bf_options / sizeof bf_options[0];

#define N_OPTIONS (sizeof bf_options / sizeof bf_options[0])

static int g_int[N_OPTIONS];
static int *g_ptr[N_OPTIONS];        /* g_int[i], or the module's variable */
static char g_str[N_OPTIONS][BF_OPT_STR_SIZE];
static unsigned char g_user[N_OPTIONS];
static int g_ready;

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int same(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

static void init(void)
{
    size_t i;
    if (g_ready) {
        return;
    }
    g_ready = 1;
    for (i = 0; i < N_OPTIONS; i++) {
        g_int[i] = bf_options[i].def;
        g_ptr[i] = &g_int[i];
        snprintf(g_str[i], sizeof g_str[i], "%s", bf_options[i].def_str ? bf_options[i].def_str : "");
    }
}

int bf_option_find(const char *name)
{
    size_t i;
    for (i = 0; i < N_OPTIONS; i++) {
        if (same(bf_options[i].name, name)) {
            return (int)i;
        }
    }
    return bf_option_find_console(name);
}

int bf_option_find_console(const char *name)
{
    static const char OLD[] = "bf1942patch.";   /* what earlier builds named every option: bf1942patch.<name> */
    size_t i, k, n = sizeof OLD - 1;
    for (k = 0; k < n && lower((unsigned char)name[k]) == OLD[k]; k++) {
    }
    for (i = 0; i < N_OPTIONS; i++) {
        if (same(bf_options[i].console, name) || (k == n && same(bf_options[i].name, name + n))) {
            return (int)i;
        }
    }
    return -1;
}

static int find(const char *name)
{
    int i = bf_option_find(name);
    if (i < 0) {
        bf_log("settings: no option %s (a bug)", name);
    }
    return i;
}

int bf_option_int(int i)
{
    init();
    return *g_ptr[i];
}

const char *bf_option_str(int i)
{
    init();
    return g_str[i];
}

int bf_setting_int(const char *name)
{
    int i = find(name);
    return i < 0 ? 0 : bf_option_int(i);
}

const char *bf_setting_str(const char *name)
{
    int i = find(name);
    return i < 0 ? "" : bf_option_str(i);
}

void bf_setting_bind(const char *name, int *var)
{
    int i = find(name);
    if (i >= 0 && bf_options[i].type == BF_OPT_INT) {
        init();
        *var = *g_ptr[i];
        g_ptr[i] = var;
    }
}

int bf_option_bound(int i)
{
    init();
    return g_ptr[i] != &g_int[i];
}

static int parse_int(const char *text, int *out)
{
    static const char *const yes[] = {"true", "on", "yes"}, *const no[] = {"false", "off", "no"};
    char *end;
    long n;
    size_t k;
    for (k = 0; k < 3; k++) {
        if (same(text, yes[k]) || same(text, no[k])) {
            *out = same(text, yes[k]);
            return 1;
        }
    }
    n = strtol(text, &end, 10);
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (end == text || *end) {
        return 0;
    }
    *out = (int)n;
    return 1;
}

int bf_option_set(int i, const char *value)
{
    init();
    if (bf_options[i].type == BF_OPT_INT) {
        int n;
        if (!parse_int(value, &n)) {
            return -1;
        }
        if (n == *g_ptr[i]) {
            return 0;
        }
        *g_ptr[i] = n;
        return 1;
    }
    if (!strcmp(g_str[i], value)) {
        return 0;
    }
    snprintf(g_str[i], sizeof g_str[i], "%s", value);
    return 1;
}

int bf_setting_set_int(const char *name, int value)
{
    char text[16];
    int i = find(name);
    snprintf(text, sizeof text, "%d", value);
    return i < 0 ? -1 : bf_option_set(i, text);
}

int bf_setting_set_str(const char *name, const char *value)
{
    int i = find(name);
    return i < 0 ? -1 : bf_option_set(i, value);
}

void bf_option_text(int i, char *out, int size, int quoted)
{
    if (bf_options[i].type == BF_OPT_INT) {
        snprintf(out, (size_t)size, "%d", bf_option_int(i));
    } else {
        snprintf(out, (size_t)size, quoted ? "\"%s\"" : "%s", bf_option_str(i));
    }
}

int bf_option_from_user(int i)
{
    return g_user[i];
}

/* ---------------------------------------------------------------- files */

static char *skip_space(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static void trim_end(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

int bf_settings_parse_line(char *line, char **value)
{
    char *s = skip_space(line), *name = s, *v;
    int k, dot = 0;
    trim_end(s);
    if (*s == ';' || *s == '#' || *s == '/') {
        return -1;                   /* a comment (; # //) */
    }
    for (v = name; *v && *v != ' ' && *v != '\t'; v++) {
        dot |= *v == '.';
    }
    if (!dot) {
        return -1;                   /* rem, run, ...: not an object.name */
    }
    if (!*v) {
        return -1;                   /* no value: a read */
    }
    *v++ = '\0';
    v = skip_space(v);
    if (*v == '"') {                 /* a quoted value is one argument, as in the console */
        char *end = strchr(++v, '"');
        if (end) {
            *end = '\0';
        }
    } else {
        char *end = v;
        while (*end && *end != ' ' && *end != '\t') {
            end++;
        }
        *end = '\0';
    }
    *value = v;
    if ((k = bf_option_find_console(name)) < 0) {
        bf_log("settings: %s is not an option, skipped", name);
    }
    return k;
}

static int take(int i, const char *value, int user, const char *path)
{
    if (user && (bf_options[i].flags & BF_OPT_INSTALL)) {
        bf_log("settings: %s is only read from the install's bf1942patch.con, not %s", bf_options[i].console, path);
        return 0;
    }
    if (bf_option_set(i, value) < 0) {
        bf_log("settings: %s %s in %s is not a number, skipped", bf_options[i].console, value, path);
        return 0;
    }
    if (user) {
        g_user[i] = 1;
    }
    return 1;
}

int bf_settings_load_con(const char *path, int user)
{
    char line[512], *value;
    int n = 0, i;
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        if (!strncmp(s, "\xef\xbb\xbf", 3)) {
            s += 3;
        }
        if ((i = bf_settings_parse_line(s, &value)) >= 0) {
            n += take(i, value, user, path);
        }
    }
    fclose(f);
    return n;
}

int bf_settings_load_ini(const char *path, int user)
{
    char line[512], section[64] = "";
    int n = 0;
    size_t i;
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        char *s = skip_space(line), *eq, *value, *close;
        trim_end(s);
        if (*s == '[' && (close = strchr(s, ']')) != NULL) {
            *close = '\0';
            snprintf(section, sizeof section, "%s", s + 1);
            continue;
        }
        if (!*s || *s == ';' || *s == '#' || !(eq = strchr(s, '='))) {
            continue;
        }
        *eq = '\0';
        trim_end(s);
        value = skip_space(eq + 1);
        if (*value == '"' && strlen(value) >= 2 && value[strlen(value) - 1] == '"') {
            value[strlen(value) - 1] = '\0';
            value++;
        }
        for (i = 0; i < N_OPTIONS; i++) {
            if (same(bf_options[i].section, section) && same(bf_options[i].key, s)) {
                n += take((int)i, value, user, path);
                break;
            }
        }
    }
    fclose(f);
    return n;
}
