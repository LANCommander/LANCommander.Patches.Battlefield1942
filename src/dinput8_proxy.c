/* dinput8.dll proxy: BF1942.exe imports DINPUT8.dll!DirectInput8Create, so Windows loads this file from the game
 * folder first. The real DLL is loaded from the system directory and every call forwarded; DllMain applies the
 * patches before BF1942.exe's entry point runs. */
#include <windows.h>
#include "bf1942patch.h"

static FARPROC real_proc(const char *name)
{
    static HMODULE real;
    if (!real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);   /* SysWOW64 for a 32-bit process */
        lstrcpynA(path + n, "\\dinput8.dll", MAX_PATH - n);
        real = LoadLibraryA(path);
        if (!real) {
            bf_log("cannot load %s (%lu)", path, GetLastError());
            return NULL;
        }
    }
    return GetProcAddress(real, name);
}

#define REAL(name, type) static type real; if (!real) { real = (type)(void (*)(void))real_proc(#name); if (!real) return E_FAIL; }

typedef HRESULT (WINAPI *DirectInput8Create_fn)(HINSTANCE, DWORD, REFIID, void **, void *);
typedef HRESULT (WINAPI *DllGetClassObject_fn)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *Void_fn)(void);

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, void **out, void *outer)
{
    HRESULT hr;
    REAL(DirectInput8Create, DirectInput8Create_fn);
    hr = real(inst, ver, iid, out, outer);
    if (hr == 0 && out && *out) {
        *out = bf_gamepad_wrap_dinput(iid, *out);
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    REAL(DllGetClassObject, DllGetClassObject_fn);
    return real(clsid, iid, out);
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;   /* the patch stays loaded for the life of the process */
}

__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void)
{
    REAL(DllRegisterServer, Void_fn);
    return real();
}

__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void)
{
    REAL(DllUnregisterServer, Void_fn);
    return real();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        bf_standalone_init(inst);
    }
    return TRUE;
}
