/* ASI plugin build (bf1942patch.asi), for an ASI loader such as Ultimate ASI Loader: the loader LoadLibrary()s
 * every .asi it finds (game folder, scripts\, plugins\) before BF1942.exe runs its own code, and DllMain applies
 * the same patches as the dinput8.dll proxy. The options come from bf1942patch.con next to the .asi, the log goes to
 * bf1942patch.log there.
 *
 * Without the proxy the game's DirectInput object is not wrapped at creation, so the gamepad module hooks
 * DINPUT8.dll!DirectInput8Create in the exe's import table instead (gamepad.c). */
#include <windows.h>
#include "bf1942patch.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        bf_standalone_init(inst);
    }
    return TRUE;
}
