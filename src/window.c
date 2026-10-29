/* bf1942patch.borderless 1: the game in a borderless window that covers its monitor instead of exclusive full screen.
 * The desktop resolution never changes, alt-tab is instant, and the restarts the game does for every map (restart.c)
 * no longer switch the display mode back and forth.
 *
 * The window: 0x632480 registers the class (WndProc 0x632240) and creates it at 0x6325d7 as WS_OVERLAPPEDWINDOW
 * at (50, 50); the renderer then sizes it with SetWindowPos. Both are hooked for that window: popup style, the
 * monitor's rectangle. Direct3D 8: CreateDevice / Reset get Windowed=TRUE with the desktop's format, so a picture
 * of any resolution is scaled to the window by Present (pick the monitor's resolution in the video options for a
 * sharp picture). ChangeDisplaySettingsA (the desktop mode the game restores at 0x454f8d / 0x45625c, and its own
 * mode changes) does nothing. */
#include <string.h>
#include "bf1942patch.h"
#include "d3d.h"

#define CREATE_SITE_RET 0x6325ddu   /* return address of the game window's CreateWindowExA call */

static HWND (WINAPI *real_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static BOOL (WINAPI *real_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static LONG (WINAPI *real_ChangeDisplaySettingsA)(DEVMODEA *, DWORD);
static HWND g_window;
static int g_borderless;
static int g_vsync;

HWND bf_game_window(void)
{
    return g_window;
}

/* the monitor of the game window (the primary one before it exists, or bf1942patch.monitor <n>, from 1) */
void bf_game_monitor(RECT *r)
{
    MONITORINFO mi;
    HMONITOR mon = NULL;
    mi.cbSize = sizeof mi;
    if (g_window) {
        mon = MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY);
    } else {
        int want = bf_setting_int("monitor");
        DISPLAY_DEVICEA dd;
        DWORD i;
        int n = 0;
        for (i = 0; want > 0 && (dd.cb = sizeof dd, EnumDisplayDevicesA(NULL, i, &dd, 0)); i++) {
            DEVMODEA dm;
            if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) || ++n != want) {
                continue;
            }
            memset(&dm, 0, sizeof dm);
            dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                POINT p;
                p.x = dm.dmPosition.x + 1;
                p.y = dm.dmPosition.y + 1;
                mon = MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
            }
            break;
        }
        if (!mon) {
            POINT p = {0, 0};
            mon = MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
        }
    }
    if (GetMonitorInfoA(mon, &mi)) {
        *r = mi.rcMonitor;
    } else {
        SetRect(r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }
}

static HWND WINAPI hk_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                      HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND wnd;
    int game = (uintptr_t)__builtin_return_address(0) == CREATE_SITE_RET;
    if (game && g_borderless) {
        RECT r;
        bf_game_monitor(&r);
        ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_TOPMOST);
        style = WS_POPUP | (style & WS_VISIBLE);
        x = r.left;
        y = r.top;
        w = r.right - r.left;
        h = r.bottom - r.top;
    }
    wnd = real_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (game) {
        g_window = wnd;
        bf_log("window: game window %p (%s) at %d,%d %dx%d", (void *)wnd, g_borderless ? "borderless" : "as the game made it", x, y, w, h);
    }
    return wnd;
}

static BOOL WINAPI hk_SetWindowPos(HWND wnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    if (g_borderless && wnd && wnd == g_window) {
        RECT r;
        bf_game_monitor(&r);
        x = r.left;
        y = r.top;
        w = r.right - r.left;
        h = r.bottom - r.top;
        flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        if (after == HWND_TOPMOST) {
            after = HWND_TOP;    /* the full screen code makes the window topmost: other windows stay usable */
        }
        if (GetWindowLongA(wnd, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME)) {
            SetWindowLongA(wnd, GWL_STYLE, WS_POPUP | (GetWindowLongA(wnd, GWL_STYLE) & WS_VISIBLE));
            flags |= SWP_FRAMECHANGED;
        }
    }
    return real_SetWindowPos(wnd, after, x, y, w, h, flags);
}

static LONG WINAPI hk_ChangeDisplaySettingsA(DEVMODEA *mode, DWORD flags)
{
    (void)mode;
    (void)flags;
    return DISP_CHANGE_SUCCESSFUL;
}

/* Direct3D: a windowed device in whatever resolution the game picked */
static void present_params(bf_present_params *pp, int reset)
{
    bf_display_mode desk;
    (void)reset;
    if (!pp->Windowed) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        pp->FullScreen_PresentationInterval = 0;
        if (bf_d3d() && bf_d3d_adapter_mode(bf_d3d(), 0, &desk) >= 0) {
            pp->BackBufferFormat = desk.Format;
        }
        if (pp->SwapEffect == BF_D3DSWAPEFFECT_FLIP) {
            pp->SwapEffect = BF_D3DSWAPEFFECT_DISCARD;
        }
    }
    if (g_vsync && pp->MultiSampleType == 0) {
        pp->SwapEffect = BF_D3DSWAPEFFECT_COPY_VSYNC;
        pp->BackBufferCount = 1;
    }
}

/* Display modes. The renderer keeps the modes it accepts at renderer+0xa8 (count +0xac; entries {width, height,
 * format, refresh, windowed byte}, built at 0x640110), and only accepts a Direct3D mode the desktop also lists with
 * the same refresh rate (0x63fc70, from 0x64019a; the desktop list keeps 60 Hz modes only). The profile's mode
 * (game.setGameDisplayMode W H bpp Hz in Video.con) must then be in that list exactly (0x63dc40, from 0x63f00e and
 * 0x63fa59), else the game stops with "Invalid videomode specified". That breaks as soon as the monitor changes:
 * a 144 Hz screen, another monitor, a remote desktop at 66 Hz. So:
 *   - a mode that is not there is looked up again at any refresh rate, then at the desktop's size, then 800x600;
 *   - with the borderless window, the refresh rate means nothing (the desktop's is used): every mode the driver
 *     reports is accepted. */
#define MODE_FIND      0x63dc40u      /* thiscall(renderer, const int mode[5]), returns the index or -1 */
#define MODE_FIND_AT_1 0x63f00eu      /* its two callers (the profile's mode) */
#define MODE_FIND_AT_2 0x63fa59u
#define MODE_ACCEPT    0x64019au      /* call 0x63fc70 (thiscall(renderer, const int mode[5]), returns bool) */
#define MODE_ACCEPT_FN 0x63fc70u
#define FULLSCREEN_FORCE 0x442686u    /* in the renderer.setFullScreen setter 0x442670: jz +8; mov [esp+4], 0 */
#define RENDERER_MODES 0xa8           /* renderer: the accepted modes, and their count */
#define RENDERER_NMODES 0xac

typedef int (__attribute__((thiscall)) *mode_fn)(void *renderer, int *mode);

static int g_native_menus;

static int __attribute__((thiscall)) hk_mode_find(BYTE *renderer, int *want)
{
    static const char *const why[] = {"at another refresh rate", "at the desktop's size", "at 800x600", "the first one"};
    int tmp[5], i, r;
    RECT mon;
    /* the menus (and start-up) always ask for 800x600: in the borderless window they get the monitor's resolution,
     * drawn at their own shape by widescreen.c, instead of a blurry 800x600 picture stretched to the monitor */
    if (g_native_menus && want && want[0] == 800 && want[1] == 600) {
        bf_game_monitor(&mon);
        memcpy(tmp, want, sizeof tmp);
        tmp[0] = mon.right - mon.left;
        tmp[1] = mon.bottom - mon.top;
        tmp[3] = 0;
        if ((r = ((mode_fn)BF_ADDR(MODE_FIND))(renderer, tmp)) >= 0) {
            return r;
        }
    }
    r = ((mode_fn)BF_ADDR(MODE_FIND))(renderer, want);
    if (r >= 0 || !want) {
        return r;
    }
    bf_game_monitor(&mon);
    for (i = 0; i < 4 && r < 0; i++) {
        memcpy(tmp, want, sizeof tmp);
        tmp[3] = 0;                   /* any refresh rate */
        if (i == 1) {
            tmp[0] = mon.right - mon.left;
            tmp[1] = mon.bottom - mon.top;
        } else if (i == 2) {
            tmp[0] = 800;
            tmp[1] = 600;
        } else if (i == 3) {
            r = *(int *)(renderer + RENDERER_NMODES) > 0 ? 0 : -1;
            break;
        }
        if ((r = ((mode_fn)BF_ADDR(MODE_FIND))(renderer, tmp)) >= 0) {
            break;
        }
    }
    if (r >= 0) {
        int *e = *(int **)(renderer + RENDERER_MODES) + r * 5;
        *(BYTE *)(e + 4) = (BYTE)want[4];
        bf_log("window: the video mode %dx%d @ %d Hz is not available, using %dx%d @ %d Hz (%s)", want[0], want[1],
               want[3], e[0], e[1], e[3], why[i]);
    }
    return r;
}

static int __attribute__((thiscall)) hk_mode_accept(void *renderer, int *mode)
{
    (void)renderer;
    (void)mode;
    return 1;
}

/* the game sees real pixels: without this, Windows scales a DPI-unaware window on a display set to more than 100%
 * (a blurry picture, a window that does not match the monitor) */
void bf_dpi_aware(void)
{
    HMODULE user = GetModuleHandleA("user32.dll");
    BOOL (WINAPI *ctx)(HANDLE) = (BOOL (WINAPI *)(HANDLE))(void (*)(void))GetProcAddress(user, "SetProcessDpiAwarenessContext");
    if (!ctx || !ctx((HANDLE)-4)) {   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, else the system-wide kind */
        SetProcessDPIAware();
    }
}

/* once the device exists: what the window really is (troubleshooting the picture) */
static void log_window(void *device, const bf_present_params *pp, int reset)
{
    RECT rc;
    (void)device;
    if (g_window && GetClientRect(g_window, &rc)) {
        bf_log("window: client area %ldx%ld, backbuffer %ux%u%s", rc.right, rc.bottom, pp->BackBufferWidth, pp->BackBufferHeight,
               reset ? " (reset)" : "");
    }
}

void bf_window_install(void)
{
    if (bf_setting_int("dpiAware")) {
        bf_dpi_aware();
    }
    g_borderless = bf_setting_int("borderless");
    real_CreateWindowExA = bf_hook_import("USER32.dll", "CreateWindowExA", hk_CreateWindowExA);
    if (bf_setting_int("modeFallback")) {
        bf_redirect_call(MODE_FIND_AT_1, MODE_FIND, (void *)hk_mode_find);
        bf_redirect_call(MODE_FIND_AT_2, MODE_FIND, (void *)hk_mode_find);
    }
    if (g_borderless) {
        /* the game's own full screen path: in its windowed path (renderer.setFullScreen 0, which many profiles
         * carry for the borderless tools) it ignores game.setGameDisplayMode and runs at 800x600. The setter
         * (0x442670, thiscall(app, bool)) already forces the value at 0x442688 in one case: that is made
         * unconditional, with 1 (Direct3D stays windowed through present_params) */
        static const BYTE orig[10] = {0x74, 0x08, 0xc7, 0x44, 0x24, 0x04, 0x00, 0x00, 0x00, 0x00};
        static const BYTE full[10] = {0x90, 0x90, 0xc7, 0x44, 0x24, 0x04, 0x01, 0x00, 0x00, 0x00};
        bf_redirect_call(MODE_ACCEPT, MODE_ACCEPT_FN, (void *)hk_mode_accept);
        bf_patch(FULLSCREEN_FORCE, orig, full, sizeof orig);
        g_native_menus = bf_setting_int("nativeMenus");
    }
    if (!g_borderless) {
        bf_log("window: borderless 0, the game picks full screen or window itself");
        return;
    }
    g_vsync = bf_setting_int("vsync");
    real_SetWindowPos = bf_hook_import("USER32.dll", "SetWindowPos", hk_SetWindowPos);
    real_ChangeDisplaySettingsA = bf_hook_import("USER32.dll", "ChangeDisplaySettingsA", hk_ChangeDisplaySettingsA);
    bf_d3d_install();
    bf_d3d_on_params(present_params);
    bf_d3d_on_device(log_window);
    bf_log("window: borderless window on the monitor, the display mode is never changed%s", g_vsync ? ", vsync on" : "");
}
