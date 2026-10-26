#ifndef BF_D3D_H
#define BF_D3D_H
#include <windows.h>

/* D3DPRESENT_PARAMETERS of Direct3D 8 (no d3d8.h needed) */
typedef struct {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    DWORD BackBufferFormat;
    UINT BackBufferCount;
    DWORD MultiSampleType;
    DWORD SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
} bf_present_params;

#define BF_D3DSWAPEFFECT_DISCARD 1
#define BF_D3DSWAPEFFECT_FLIP    2
#define BF_D3DSWAPEFFECT_COPY    3
#define BF_D3DSWAPEFFECT_COPY_VSYNC 4

typedef void (*bf_params_hook)(bf_present_params *pp, int reset);           /* before CreateDevice / Reset */
typedef void (*bf_device_hook)(void *device, const bf_present_params *pp, int reset);   /* after they succeed */
typedef void (*bf_present_hook)(void *device, int after);                   /* around every Present */

void bf_d3d_install(void);           /* hooks Direct3DCreate8 (once; every module that needs the hooks calls it) */
void bf_d3d_on_params(bf_params_hook fn);
void bf_d3d_on_device(bf_device_hook fn);
void bf_d3d_on_present(bf_present_hook fn);
void *bf_d3d(void);                  /* the game's IDirect3D8 */
void *bf_d3d_device(void);           /* the game's IDirect3DDevice8 */

/* IDirect3D8::GetAdapterDisplayMode (vtable slot 8): D3DDISPLAYMODE {Width, Height, RefreshRate, Format} */
typedef struct { UINT Width, Height, RefreshRate; DWORD Format; } bf_display_mode;
static inline HRESULT bf_d3d_adapter_mode(void *d3d, UINT adapter, bf_display_mode *mode)
{
    return ((HRESULT (WINAPI *)(void *, UINT, bf_display_mode *))(*(void ***)d3d)[8])(d3d, adapter, mode);
}
#endif
