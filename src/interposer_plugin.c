/* LANCommander Interposer plugin build (LANCommander.Interposer.Plugin.Battlefield1942.dll).
 *
 * The Interposer loads every DLL in .interposer\Plugins\ from its own DLL_PROCESS_ATTACH, after its
 * hooks are installed, and calls InterposerPluginInit with its module handle. With the Interposer
 * deployed as a proxy DLL (dinput.dll, version.dll) or through the injector's --launch mode this is
 * before BF1942.exe runs any of its own code. The same patches as the dinput8.dll proxy are applied;
 * settings come from Plugins.Battlefield1942 in .interposer\Config.yml (defaults are added on first run)
 * and log lines go to the Interposer's session log as [BF1942].
 *
 * Interposer exports are __cdecl (declared without a convention in its config.h); only
 * InterposerPluginInit itself is WINAPI.
 *
 * InterposerGetConfigString is only used when the Interposer also exports InterposerSetConfigString
 * (added in eb42c5a, 2026-10-18). In older builds its path walk assigned each child over its parent,
 * so the first lookup clobbered the in-memory config root: later lookups (ours and the Interposer's
 * own) fail. With those builds the plugin reads its section of Config.yml itself instead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "bf1942patch.h"

#define SECTION "Battlefield1942"

typedef void (*InterposerLog_fn)(const wchar_t *verb, const wchar_t *message);
typedef BOOL (*InterposerGetConfigString_fn)(const wchar_t *dot_path, wchar_t *buffer, DWORD size);
typedef BOOL (*InterposerRegisterPluginConfig_fn)(const wchar_t *plugin_name, const wchar_t *yaml_defaults);
typedef BOOL (*InterposerSetConfigString_fn)(const wchar_t *dot_path, const wchar_t *value);

static InterposerLog_fn g_log;
static InterposerGetConfigString_fn g_get_config;   /* NULL with Interposer builds that predate the fix */

/* Written to Config.yml only when there is no Plugins.Battlefield1942 section yet; a missing key falls back
   to the built-in default in the code, which is the same value. Key names are unique across the sections of
   bf1942patch.ini, which they mirror. */
static const wchar_t DEFAULTS[] =
    L"Enabled: true\n"
    L"Redirect: true\n"
    L"Folder: \"\"\n"
    L"LogFiles: 0\n"
    L"UseFile: true\n"
    L"LogAccess: false\n"
    L"Key: \"\"\n"
    L"XPack1: \"\"\n"
    L"XPack2: \"\"\n"
    L"Language: \"\"\n"
    L"OpenSpy: true\n"
    L"MasterServer: \"\"\n"
    L"Borderless: true\n"
    L"VSync: false\n"
    L"Monitor: 0\n"
    L"DpiAware: true\n"
    L"ModeFallback: true\n"
    L"NativeMenus: true\n"
    L"Seamless: true\n"
    L"HorPlus: true\n"
    L"FixHud: true\n"
    L"AnchorHud: true\n"
    L"HudScale: 100\n"
    L"HudScaleOption: true\n"
    L"UseSDL: true\n"
    L"Deadzone: 15\n"
    L"Sensitivity: 100\n"
    L"Bindings: true\n"
    L"HideDuplicates: true\n"
    L"StartKey: 0\n"
    L"BackKey: 0\n"
    L"VoiceChat: true\n"
    L"VoicePort: 27800\n"
    L"VoicePassword: \"\"\n"
    L"VoiceServer: \"\"\n"
    L"VoiceQueryPorts: \"22000,23000\"\n"
    L"VoiceActivation: false\n"
    L"RadioActivation: false\n"
    L"ProximityKey: IDKey_Capital\n"
    L"RadioKey: Mouse_IDButton_3\n"
    L"VoiceVolume: 100\n"
    L"MicSensitivity: 100\n"
    L"VoiceHud: true\n"
    L"VoiceOptions: true\n"
    L"CrashLog: false\n";
/* Plugins.Battlefield1942 from Config.yml, for Interposer builds whose InterposerGetConfigString is unusable */
#define MAX_KEYS 64
static struct { char key[48]; char value[MAX_PATH]; } g_keys[MAX_KEYS];
static int g_nkeys;

static int indent_of(const char *line)
{
    int n = 0;
    while (line[n] == ' ') {
        n++;
    }
    return n;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

/* block-style "Plugins:" / "  Battlefield1942:" / "    Key: value" (what InterposerRegisterPluginConfig writes) */
static void read_section(const wchar_t *config_path)
{
    char line[256];
    int in_plugins = 0, section_indent = -1;
    FILE *f = _wfopen(config_path, L"r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof line, f) && g_nkeys < MAX_KEYS) {
        char *text = line, *colon, *value, *hash;
        int indent;
        if (!strncmp(text, "\xef\xbb\xbf", 3)) {
            text += 3;
        }
        trim(text);
        indent = indent_of(text);
        text += indent;
        if (!*text || *text == '#') {
            continue;
        }
        if (indent == 0) {
            in_plugins = !strcmp(text, "Plugins:");
            section_indent = -1;
            continue;
        }
        if (!in_plugins) {
            continue;
        }
        if (section_indent >= 0 && indent <= section_indent) {
            section_indent = -1;               /* left Plugins.Battlefield1942 */
        }
        if (section_indent < 0) {
            if (!strcmp(text, SECTION ":")) {
                section_indent = indent;
            }
            continue;
        }
        colon = strchr(text, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        value = colon + 1;
        while (*value == ' ') {
            value++;
        }
        if (*value == '"' || *value == '\'') {
            char *end = strchr(value + 1, *value);
            if (end) {
                *end = '\0';
            }
            value++;
        } else if ((hash = strstr(value, " #")) != NULL) {
            *hash = '\0';
        }
        trim(text);
        trim(value);
        lstrcpynA(g_keys[g_nkeys].key, text, sizeof g_keys[0].key);
        lstrcpynA(g_keys[g_nkeys].value, value, sizeof g_keys[0].value);
        g_nkeys++;
    }
    fclose(f);
}

static int get_value(const char *key, wchar_t *value, int size)
{
    int i;
    if (g_get_config) {
        wchar_t path[96];
        swprintf(path, sizeof path / sizeof path[0], L"Plugins." SECTION ".%hs", key);
        return g_get_config(path, value, (DWORD)size);
    }
    for (i = 0; i < g_nkeys; i++) {
        if (!_stricmp(g_keys[i].key, key)) {
            return MultiByteToWideChar(CP_UTF8, 0, g_keys[i].value, -1, value, size) > 0;
        }
    }
    return 0;
}

/* bf1942patch.ini's [Section] Key -> Plugins.Battlefield1942.Key (key names are unique across sections) */
static int config_int(const char *section, const char *key, int def)
{
    wchar_t value[64];
    wchar_t *end;
    long n;
    (void)section;
    if (!get_value(key, value, sizeof value / sizeof value[0])) {
        return def;
    }
    if (!_wcsicmp(value, L"true") || !_wcsicmp(value, L"yes") || !_wcsicmp(value, L"on")) {
        return 1;
    }
    if (!_wcsicmp(value, L"false") || !_wcsicmp(value, L"no") || !_wcsicmp(value, L"off")) {
        return 0;
    }
    n = wcstol(value, &end, 0);
    if (end == value) {
        bf_log("config: Plugins." SECTION ".%s = \"%ls\" is not a number or true/false, using %d", key, value, def);
        return def;
    }
    return (int)n;
}

static int config_str(const char *section, const char *key, char *out, int size)
{
    wchar_t value[MAX_PATH];
    (void)section;
    if (!get_value(key, value, MAX_PATH)) {
        return 0;
    }
    return WideCharToMultiByte(CP_ACP, 0, value, -1, out, size, NULL, NULL) > 1;
}

static void log_line(const char *line)
{
    wchar_t wide[1024];
    if (g_log && MultiByteToWideChar(CP_ACP, 0, line, -1, wide, sizeof wide / sizeof wide[0]) > 0) {
        g_log(L"BF1942", wide);
    }
}

__declspec(dllexport) void WINAPI InterposerPluginInit(HMODULE interposer)
{
    InterposerRegisterPluginConfig_fn register_config;
    g_log = (InterposerLog_fn)(void (*)(void))GetProcAddress(interposer, "InterposerLog");
    register_config = (InterposerRegisterPluginConfig_fn)(void (*)(void))GetProcAddress(interposer, "InterposerRegisterPluginConfig");
    if (register_config) {
        register_config(L"" SECTION, DEFAULTS);
    }
    bf_set_module(GetModuleHandleA("LANCommander.Interposer.Plugin.Battlefield1942.dll"));
    bf_set_backend(config_int, log_line);
    bf_set_string_backend(config_str);

    if (GetProcAddress(interposer, "InterposerSetConfigString")) {
        g_get_config = (InterposerGetConfigString_fn)(void (*)(void))GetProcAddress(interposer, "InterposerGetConfigString");
    }
    if (!g_get_config) {
        wchar_t path[MAX_PATH + 32], *slash;
        GetModuleFileNameW(interposer, path, MAX_PATH);
        slash = wcsrchr(path, L'\\');
        if (slash) {
            slash[1] = L'\0';
        }
        wcscat(path, L".interposer\\Config.yml");
        read_section(path);
        bf_log("config: older Interposer, read %d Plugins." SECTION " settings from %ls", g_nkeys, path);
    }

    if (bf_patch_start()) {
        bf_install();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
