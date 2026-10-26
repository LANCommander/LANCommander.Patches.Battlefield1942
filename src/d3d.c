/* Direct3D 8 hooks for the other modules: the exe imports d3d8.dll!Direct3DCreate8 (IAT 0x8c3720, thunk at
 * 0x81f9b8); the IDirect3D8 it gets has CreateDevice (vtable slot 15) patched, and the device's Reset (14) and
 * Present (15) are patched in turn. Vtable slots are shared by every object of the class, so this also covers a
 * d3d8.dll wrapper such as dgVoodoo 2. */
#include <string.h>
#include "bf1942patch.h"
#include "d3d.h"

typedef void *(WINAPI *Direct3DCreate8_fn)(UINT);
typedef HRESULT (WINAPI *CreateDevice_fn)(void *self, UINT adapter, DWORD type, HWND focus, DWORD flags,
                                          bf_present_params *pp, void **device);
typedef HRESULT (WINAPI *Reset_fn)(void *self, bf_present_params *pp);
typedef HRESULT (WINAPI *Present_fn)(void *self, const RECT *src, const RECT *dst, HWND wnd, const void *dirty);

static Direct3DCreate8_fn real_Direct3DCreate8;
static CreateDevice_fn real_CreateDevice;
static Reset_fn real_Reset;
static Present_fn real_Present;
static void *g_d3d;
static void *g_device;

#define BF_D3D_MAX_HOOKS 8
static bf_params_hook g_params_hooks[BF_D3D_MAX_HOOKS];
static bf_device_hook g_device_hooks[BF_D3D_MAX_HOOKS];
static bf_present_hook g_present_hooks[BF_D3D_MAX_HOOKS];
static int g_nparams, g_ndevice, g_npresent;

void bf_d3d_on_params(bf_params_hook fn) { if (g_nparams < BF_D3D_MAX_HOOKS) g_params_hooks[g_nparams++] = fn; }
void bf_d3d_on_device(bf_device_hook fn) { if (g_ndevice < BF_D3D_MAX_HOOKS) g_device_hooks[g_ndevice++] = fn; }
void bf_d3d_on_present(bf_present_hook fn) { if (g_npresent < BF_D3D_MAX_HOOKS) g_present_hooks[g_npresent++] = fn; }

void *bf_d3d(void) { return g_d3d; }
void *bf_d3d_device(void) { return g_device; }

static void *patch_slot(void *object, int slot, void *hook)
{
    void **vtbl = *(void ***)object, *old = vtbl[slot];
    if (old != hook) {
        bf_write((uint32_t)(uintptr_t)&vtbl[slot], &hook, sizeof hook);
    }
    return old;
}

static void adjust(bf_present_params *pp, int reset)
{
    int i;
    for (i = 0; i < g_nparams; i++) {
        g_params_hooks[i](pp, reset);
    }
}

static HRESULT WINAPI hk_Present(void *self, const RECT *src, const RECT *dst, HWND wnd, const void *dirty)
{
    HRESULT hr;
    int i;
    for (i = 0; i < g_npresent; i++) {
        g_present_hooks[i](self, 0);
    }
    hr = real_Present(self, src, dst, wnd, dirty);
    for (i = 0; i < g_npresent; i++) {
        g_present_hooks[i](self, 1);
    }
    return hr;
}

static HRESULT WINAPI hk_Reset(void *self, bf_present_params *pp)
{
    HRESULT hr;
    int i;
    adjust(pp, 1);
    hr = real_Reset(self, pp);
    bf_log("d3d: Reset %ux%u %s -> %08lx", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed ? "windowed" : "full screen", hr);
    if (hr >= 0) {
        for (i = 0; i < g_ndevice; i++) {
            g_device_hooks[i](self, pp, 1);
        }
    }
    return hr;
}

static HRESULT WINAPI hk_CreateDevice(void *self, UINT adapter, DWORD type, HWND focus, DWORD flags, bf_present_params *pp,
                                      void **device)
{
    HRESULT hr;
    int i;
    adjust(pp, 0);
    hr = real_CreateDevice(self, adapter, type, focus, flags, pp, device);
    bf_log("d3d: CreateDevice adapter %u, %ux%u format %lu, %s, refresh %u, interval %08x -> %08lx", adapter,
           pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->Windowed ? "windowed" : "full screen",
           pp->FullScreen_RefreshRateInHz, pp->FullScreen_PresentationInterval, hr);
    if (hr >= 0 && device && *device) {
        g_device = *device;
        real_Reset = patch_slot(*device, 14, hk_Reset);
        real_Present = patch_slot(*device, 15, hk_Present);
        for (i = 0; i < g_ndevice; i++) {
            g_device_hooks[i](*device, pp, 0);
        }
    }
    return hr;
}

static void *WINAPI hk_Direct3DCreate8(UINT sdk)
{
    void *d3d = real_Direct3DCreate8(sdk);
    if (d3d) {
        g_d3d = d3d;
        real_CreateDevice = patch_slot(d3d, 15, hk_CreateDevice);
    }
    return d3d;
}

void bf_d3d_install(void)
{
    static int done;
    if (done) {
        return;
    }
    done = 1;
    real_Direct3DCreate8 = bf_hook_import("d3d8.dll", "Direct3DCreate8", hk_Direct3DCreate8);
    if (!real_Direct3DCreate8) {
        bf_log("d3d: Direct3DCreate8 is not imported, the video hooks are off");
    }
}
