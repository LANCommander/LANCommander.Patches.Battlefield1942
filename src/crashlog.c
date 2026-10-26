/* [Debug] CrashLog=1: a crash (access violation and the like) is written to bf1942patch.log with the registers and
 * a call chain: the stack is scanned for addresses that follow a call instruction in a loaded module. The
 * game's own handler (errorlog.txt) still runs afterwards. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bf1942patch.h"

static const char *module_of(DWORD addr, DWORD *base)
{
    static char name[MAX_PATH];
    HMODULE m;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m) && GetModuleFileNameA(m, name, sizeof name)) {
        char *s = strrchr(name, '\\');
        *base = (DWORD)(uintptr_t)m;
        return s ? s + 1 : name;
    }
    return NULL;
}

/* addr follows a call: E8 rel32, FF /2 with any operand (2..7 bytes before) */
static int after_call(DWORD addr)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    int k;
    if (IsBadReadPtr(p - 7, 7)) {
        return 0;
    }
    if (p[-5] == 0xe8) {
        return 1;
    }
    for (k = 2; k <= 7; k++) {
        if (p[-k] == 0xff && ((p[-k + 1] >> 3) & 7) == 2) {
            return 1;
        }
    }
    return 0;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    static int logged;
    DWORD code = ep->ExceptionRecord->ExceptionCode, base = 0, i, n = 0;
    CONTEXT *c = ep->ContextRecord;
    const char *mod;
    DWORD *sp;
    if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO
         && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION) || logged >= 3) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    logged++;
    mod = module_of(c->Eip, &base);
    bf_log("crash: %08lx at %08lx (%s+%lx), thread %lu, access %08lx", code, c->Eip, mod ? mod : "?", c->Eip - base,
            GetCurrentThreadId(), ep->ExceptionRecord->NumberParameters > 1 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0);
    bf_log("crash: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", c->Eax, c->Ebx,
            c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    sp = (DWORD *)(uintptr_t)c->Esp;
    for (i = 0; i < 24; i += 8) {
        DWORD *q = sp - 16 + i;
        if (!IsBadReadPtr(q, 32)) {
            bf_log("crash: esp%+04ld: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", (long)(i - 16) * 4, q[0], q[1],
                    q[2], q[3], q[4], q[5], q[6], q[7]);
        }
    }
    for (i = 0; i < 4096 && n < 24 && !IsBadReadPtr(sp + i, 4); i++) {
        DWORD v = sp[i];
        if (v > 0x10000 && (mod = module_of(v, &base)) != NULL && after_call(v)) {
            bf_log("crash:   [esp+%04lx] %08lx %s+%lx", i * 4, v, mod, v - base);
            n++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* the game quitting on its own (it often does so silently): where from */
static void (__cdecl *real_exit)(int);
static VOID (WINAPI *real_ExitProcess)(UINT);

static void __cdecl hk_exit(int code)
{
    bf_log("crash: exit(%d) called from %p", code, __builtin_return_address(0));
    real_exit(code);
}

static VOID WINAPI hk_ExitProcess(UINT code)
{
    bf_log("crash: ExitProcess(%u) called from %p", code, __builtin_return_address(0));
    real_ExitProcess(code);
}

/* the game's message boxes (always on): "Bf1942 Error" is how its critical errors (0x445120) end, and on a
 * borderless window covering the screen the box can end up out of sight, so the game just seems to quit. The text
 * goes to the log, and the box comes up on top. */
static int (WINAPI *real_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);

static int WINAPI hk_MessageBoxA(HWND wnd, LPCSTR text, LPCSTR caption, UINT type)
{
    int r;
    bf_log("message box \"%s\" (from %p): %s", caption ? caption : "", __builtin_return_address(0), text ? text : "");
    r = real_MessageBoxA(wnd, text, caption, type | MB_TOPMOST | MB_SETFOREGROUND);
    bf_log("message box answered %d", r);
    return r;
}

void bf_crashlog_install(void)
{
    real_MessageBoxA = bf_hook_import("USER32.dll", "MessageBoxA", hk_MessageBoxA);
    if (!real_MessageBoxA) {
        real_MessageBoxA = MessageBoxA;
    }
    if (bf_ini_int("Debug", "CrashLog", 0)) {
        AddVectoredExceptionHandler(1, on_exception);
        real_exit = bf_hook_import("MSVCR70.dll", "exit", hk_exit);
        real_ExitProcess = bf_hook_import("KERNEL32.dll", "ExitProcess", hk_ExitProcess);
        if (!real_exit) real_exit = exit;
        if (!real_ExitProcess) real_ExitProcess = ExitProcess;
        bf_log("crash: crash log on");
    }
}
