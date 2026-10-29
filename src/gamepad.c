/* Gamepads through SDL2.
 *
 * BF1942's input goes through DirectInput 8 (DINPUT8.dll!DirectInput8Create, IAT 0x8c3314). The IDirectInput8A
 * the game creates is wrapped (by the dinput8.dll proxy, or through the import with the ASI / plugin builds):
 * EnumDevices also offers one joystick per SDL gamepad slot (and hides the DirectInput twins of those gamepads), and
 * CreateDevice of such a joystick returns a fake IDirectInputDevice8A filled from SDL_GameController.
 *
 * What the game does with a joystick (re/input_notes.md): every joystick object (0x66efa0, at most two:
 * IDFGameController_0 / _1) creates its own IDirectInput8A, enumerates DI8DEVCLASS_GAMECTRL and takes the N-th
 * device (so the gamepads are offered first, in slot order), sets c_dfDIJoystick2, DIPROP_RANGE -1000..1000 per
 * axis by ID (a failure drops that axis and all after it), EXCLUSIVE | FOREGROUND, Acquire (S_FALSE counts as
 * failed), then per tick Poll, GetDeviceState and GetDeviceData (button presses). IDAxis_n is the n-th axis that
 * exists in the order X Y Z Rx Ry Rz; the value is raw * 0.001 with no dead zone (so rest must be exactly 0); a
 * POV becomes four extra buttons that ignore diagonals; the Controls menu binds axes 0..3 and buttons 0..31 of
 * IDFGameController_0. Hence this layout:
 *   IDAxis_0 X   left stick X                 IDAxis_3 Rx  right stick Y
 *   IDAxis_1 Y   left stick Y (down +)        IDAxis_4 Ry  LT (0..1, rest 0)
 *   IDAxis_2 Z   right stick X                IDAxis_5 Rz  RT (0..1, rest 0)
 *   IDButton_  0 A  1 B  2 X  3 Y  4 LB  5 RB  6 Back  7 Start  8 LS  9 RS  10 LT  11 RT
 *              12 D-pad up  13 down  14 left  15 right                            (no POV)
 * The sticks get a radial dead zone (exactly 0 inside) and a response curve. Default bindings for every vehicle
 * type: Settings/Gamepad.con, run right after the profile's controls (see run_gamepad_con). Other DirectInput
 * devices (keyboard, mouse, wheels, sticks) are untouched. SDL runs on the game's thread: initialised by the
 * first EnumDevices, pumped from the fake devices' reads. */
#include "pad.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bf1942patch.h"

/* ---------------------------------------------------------------- GUIDs (no dxguid) */

static const GUID IID_Unknown_ = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_DI8A = { 0xbf798030, 0x483a, 0x4da2, { 0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00 } };
static const GUID IID_DIDev8A = { 0x54d41080, 0xdc15, 0x4833, { 0xa4, 0x1b, 0x74, 0x8f, 0x73, 0xa3, 0x81, 0x79 } };
#define OBJ_GUID(d1) { (d1), 0xc9f3, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } }
static const GUID G_XAxis = OBJ_GUID(0xa36d02e0), G_YAxis = OBJ_GUID(0xa36d02e1), G_ZAxis = OBJ_GUID(0xa36d02e2);
static const GUID G_RxAxis = OBJ_GUID(0xa36d02f4), G_RyAxis = OBJ_GUID(0xa36d02f5), G_RzAxis = OBJ_GUID(0xa36d02e3);
static const GUID G_Button = OBJ_GUID(0xa36d02f0);
/* the fake joysticks: {42463432-5041-4400-8000-424634325000 + k} ("BF42PAD", k = slot). Fixed, so the game's
 * restarts (a new process for every map) find the same device again */
static const GUID PAD_INSTANCE = { 0x42463432, 0x5041, 0x4400, { 0x80, 0x00, 0x42, 0x46, 0x34, 0x32, 0x50, 0x00 } };
static const GUID PAD_PRODUCT = { 0x42463432, 0x5041, 0x4401, { 0x80, 0x00, 0x42, 0x46, 0x34, 0x32, 0x50, 0x00 } };

#define DI8DEVTYPE_GAMEPAD_ 0x15
#define DI8DEVTYPEGAMEPAD_STANDARD_ 2
#define DI8DEVCLASS_GAMECTRL_ 4
#define PAD_DEVTYPE (DI8DEVTYPE_GAMEPAD_ | DI8DEVTYPEGAMEPAD_STANDARD_ << 8 | DIDEVTYPE_HID)

/* ---------------------------------------------------------------- settings and SDL slots */

static int g_on;
static int g_deadzone = 15;
static float g_curve = 1.0f;          /* stick response exponent, from bf1942patch.gamepadSensitivity */
static int g_hide_twins = 1;
static int g_nslots;                  /* slots offered: the gamepads at start-up */
static int g_start_key = 0x01;        /* DIK_ESCAPE: the in-game menu */
static int g_back_key = 0x0f;         /* DIK_TAB: the score board */
static int g_sdl;                     /* 1 = up, -1 = failed */
static DWORD g_thread;

typedef struct {
    SDL_GameController *gc;
    SDL_JoystickID id;
    SDL_JoystickGUID last;
    int had;
    int keys;                         /* bit 0 Start held, bit 1 Back held (keyboard keys down) */
} slot_t;
static slot_t g_pads[PAD_MAX];

SDL_GameController *pad_controller(int slot)
{
    return slot >= 0 && slot < PAD_MAX ? g_pads[slot].gc : NULL;
}

static int find_id(SDL_JoystickID id)
{
    int i;
    for (i = 0; i < PAD_MAX; i++) {
        if (g_pads[i].gc && g_pads[i].id == id) {
            return i;
        }
    }
    return -1;
}

static void open_pad(int index)
{
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
    int i, slot = -1;
    if (!SDL_IsGameController(index) || id < 0 || find_id(id) >= 0) {
        return;
    }
    for (i = 0; i < g_nslots && slot < 0; i++) {   /* the slot this pad had before */
        if (!g_pads[i].gc && g_pads[i].had && !memcmp(&g_pads[i].last, &guid, sizeof guid)) {
            slot = i;
        }
    }
    for (i = 0; i < g_nslots && slot < 0; i++) {
        if (!g_pads[i].gc) {
            slot = i;
        }
    }
    if (slot < 0) {
        bf_log("gamepad: %s connected, but all %d gamepad slots are taken", SDL_GameControllerNameForIndex(index), g_nslots);
        return;
    }
    g_pads[slot].gc = SDL_GameControllerOpen(index);
    if (!g_pads[slot].gc) {
        bf_log("gamepad: cannot open %s: %s", SDL_GameControllerNameForIndex(index), SDL_GetError());
        return;
    }
    g_pads[slot].id = id;
    g_pads[slot].last = guid;
    g_pads[slot].had = 1;
    bf_log("gamepad: %s connected as gamepad %d (VID %04x PID %04x)", SDL_GameControllerName(g_pads[slot].gc), slot + 1,
           SDL_GameControllerGetVendor(g_pads[slot].gc), SDL_GameControllerGetProduct(g_pads[slot].gc));
}

static void close_pad(int slot)
{
    bf_log("gamepad: %s disconnected (gamepad %d)", SDL_GameControllerName(g_pads[slot].gc), slot + 1);
    SDL_GameControllerClose(g_pads[slot].gc);
    g_pads[slot].gc = NULL;
}

static int sdl_init(void)
{
    int i, n;
    if (g_sdl) {
        return g_sdl == 1;
    }
    g_thread = GetCurrentThreadId();
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Xbox pads through XInput, not SDL's RawInput driver (which needs input before it matches XInput twins) */
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        bf_log("gamepad: SDL_Init failed: %s", SDL_GetError());
        g_sdl = -1;
        return 0;
    }
    {
        /* optional extra mappings (https://github.com/mdqinc/SDL_GameControllerDB) next to the exe */
        char path[MAX_PATH + 32];
        snprintf(path, sizeof path, "%sgamecontrollerdb.txt", bf_game_dir());
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            bf_log("gamepad: %d mappings from %s", SDL_GameControllerAddMappingsFromFile(path), path);
        }
    }
    {
        SDL_version v;
        SDL_GetVersion(&v);
        bf_log("gamepad: SDL %d.%d.%d", v.major, v.minor, v.patch);
    }
    g_sdl = 1;
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
            int connected = 0, k;
            for (k = 0; k < PAD_MAX; k++) {
                connected += g_pads[k].gc != NULL;
            }
            if (connected >= g_nslots && g_nslots < PAD_MAX) {
                g_nslots++;           /* every gamepad there at start-up gets a slot */
            }
            open_pad(i);
        }
    }
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    return 1;
}

/* the keyboard key a pad button stands for, pressed / released through the system (DirectInput sees it) */
static void key(int dik, int down)
{
    INPUT in;
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    GetWindowThreadProcessId(fg, &pid);
    if (!dik || pid != GetCurrentProcessId()) {
        return;
    }
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)dik;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof in);
}

static void pump(void)
{
    static DWORD last;
    SDL_Event ev[16];
    DWORD now;
    int n, i, slot;
    if (g_sdl != 1 || GetCurrentThreadId() != g_thread) {
        return;
    }
    now = GetTickCount();
    if (now - last < 2) {
        return;
    }
    last = now;
    SDL_PumpEvents();
    while ((n = SDL_PeepEvents(ev, 16, SDL_GETEVENT, SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMOVED)) > 0) {
        for (i = 0; i < n; i++) {
            if (ev[i].type == SDL_CONTROLLERDEVICEADDED) {
                open_pad(ev[i].cdevice.which);
            } else if (ev[i].type == SDL_CONTROLLERDEVICEREMOVED && (slot = find_id(ev[i].cdevice.which)) >= 0) {
                close_pad(slot);
            }
        }
    }
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    for (i = 0; i < PAD_MAX; i++) {
        int start = g_pads[i].gc && SDL_GameControllerGetButton(g_pads[i].gc, SDL_CONTROLLER_BUTTON_START);
        int back = g_pads[i].gc && SDL_GameControllerGetButton(g_pads[i].gc, SDL_CONTROLLER_BUTTON_BACK);
        if (start != (g_pads[i].keys & 1)) {
            key(g_start_key, start);
        }
        if (back != ((g_pads[i].keys >> 1) & 1)) {
            key(g_back_key, back);
        }
        g_pads[i].keys = start | back << 1;
    }
}

/* ---------------------------------------------------------------- the pad as a joystick */

enum { O_AXIS, O_BUTTON, O_POV };
#define NAXES    6
#define NBUTTONS 16
#define NOBJS    (NAXES + NBUTTONS)

typedef struct { int kind, inst; const GUID *guid; const char *name; } object_t;
static const object_t OBJS[NOBJS] = {
    { O_AXIS, 0, &G_XAxis, "Left Stick X" }, { O_AXIS, 1, &G_YAxis, "Left Stick Y" },
    { O_AXIS, 2, &G_ZAxis, "Right Stick X" }, { O_AXIS, 3, &G_RxAxis, "Right Stick Y" },
    { O_AXIS, 4, &G_RyAxis, "Left Trigger" }, { O_AXIS, 5, &G_RzAxis, "Right Trigger" },
    { O_BUTTON, 0, &G_Button, "A" }, { O_BUTTON, 1, &G_Button, "B" }, { O_BUTTON, 2, &G_Button, "X" },
    { O_BUTTON, 3, &G_Button, "Y" }, { O_BUTTON, 4, &G_Button, "LB" }, { O_BUTTON, 5, &G_Button, "RB" },
    { O_BUTTON, 6, &G_Button, "Back" }, { O_BUTTON, 7, &G_Button, "Start" }, { O_BUTTON, 8, &G_Button, "Left Stick" },
    { O_BUTTON, 9, &G_Button, "Right Stick" }, { O_BUTTON, 10, &G_Button, "LT" }, { O_BUTTON, 11, &G_Button, "RT" },
    { O_BUTTON, 12, &G_Button, "D-pad Up" }, { O_BUTTON, 13, &G_Button, "D-pad Down" },
    { O_BUTTON, 14, &G_Button, "D-pad Left" }, { O_BUTTON, 15, &G_Button, "D-pad Right" },
};

/* the pad's controls right now: axes -1..1 (triggers 0..1), buttons */
typedef struct { float axis[NAXES]; BYTE button[NBUTTONS]; DWORD pov; } padstate_t;

static float stick(Sint16 v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

/* radial dead zone, the rest rescaled to the full range, then the response curve */
static void shape(float *x, float *y)
{
    float mag = sqrtf(*x * *x + *y * *y), dz = g_deadzone / 100.0f, k;
    if (mag <= dz) {
        *x = *y = 0;
        return;
    }
    k = (mag > 1 ? 1 : mag);
    k = (k - dz) / (1 - dz);
    if (g_curve != 1.0f) {
        k = powf(k, g_curve);
    }
    *x *= k / mag;
    *y *= k / mag;
}

static void read_pad(int slot, padstate_t *s)
{
    static const SDL_GameControllerButton map[10] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    };
    static const SDL_GameControllerButton dpad[4] = {
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    };
    SDL_GameController *gc = pad_controller(slot);
    float lt, rt, dz = g_deadzone / 100.0f;
    int i;
    memset(s, 0, sizeof *s);
    s->pov = (DWORD)-1;
    if (!gc) {
        return;
    }
    s->axis[0] = stick(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
    s->axis[1] = stick(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
    s->axis[2] = stick(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
    s->axis[3] = stick(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));
    shape(&s->axis[0], &s->axis[1]);
    shape(&s->axis[2], &s->axis[3]);
    lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
    rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
    s->axis[4] = lt <= dz / 2 ? 0 : (lt - dz / 2) / (1 - dz / 2);   /* a resting trigger is exactly 0 */
    s->axis[5] = rt <= dz / 2 ? 0 : (rt - dz / 2) / (1 - dz / 2);
    for (i = 0; i < 10; i++) {
        s->button[i] = SDL_GameControllerGetButton(gc, map[i]) ? 0x80 : 0;
    }
    s->button[10] = lt > 0.3f ? 0x80 : 0;
    s->button[11] = rt > 0.3f ? 0x80 : 0;
    for (i = 0; i < 4; i++) {
        s->button[12 + i] = SDL_GameControllerGetButton(gc, dpad[i]) ? 0x80 : 0;
    }
}

/* ---------------------------------------------------------------- the fake IDirectInputDevice8A */

#define QUEUE_MAX 256

typedef struct paddev {
    void **vtbl;
    LONG ref;
    int slot;
    int acquired;
    HANDLE event;
    /* the data format the game set: per format object, the pad object (or -1) */
    int nfmt;
    DWORD datasize;
    DWORD fmt_ofs[256], fmt_type[256];
    int fmt_obj[256];
    int obj_fmt[NOBJS];               /* pad object -> format object index (or -1) */
    DWORD last[256];                  /* what the game was told (buffered reads) */
    int primed;
    /* properties */
    LONG rmin[NAXES], rmax[NAXES];
    DWORD dead[NAXES], sat[NAXES];
    DWORD bufsize;
    /* buffered data */
    DIDEVICEOBJECTDATA queue[QUEUE_MAX];
    int qhead, qcount, overflow;
    DWORD seq;
    padstate_t now;
} paddev;

static void *g_dev_vtbl[];

static int dev_slot_of(REFGUID guid)
{
    GUID g = PAD_INSTANCE;
    int s;
    for (s = 0; s < PAD_MAX; s++) {
        g.Data4[7] = (BYTE)s;
        if (IsEqualGUID(guid, &g)) {
            return s;
        }
    }
    return -1;
}

static DWORD obj_type(int o)
{
    switch (OBJS[o].kind) {
    case O_AXIS: return DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(OBJS[o].inst);
    case O_BUTTON: return DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(OBJS[o].inst);
    default: return DIDFT_POV | DIDFT_MAKEINSTANCE(OBJS[o].inst);
    }
}

/* the default offset of a pad object (DIJOYSTATE layout), used before the game sets a data format */
static DWORD default_ofs(int o)
{
    switch (OBJS[o].kind) {
    case O_AXIS: return (DWORD)OBJS[o].inst * 4;          /* lX lY lZ lRx lRy */
    case O_BUTTON: return 0x30 + (DWORD)OBJS[o].inst;
    default: return 0x20;
    }
}

static DWORD obj_ofs(paddev *d, int o)
{
    return d->nfmt ? (d->obj_fmt[o] >= 0 ? d->fmt_ofs[d->obj_fmt[o]] : (DWORD)-1) : default_ofs(o);
}

static int kind_of_type(DWORD type)
{
    if (type & DIDFT_AXIS) return O_AXIS;
    if (type & DIDFT_BUTTON) return O_BUTTON;
    if (type & DIDFT_POV) return O_POV;
    return -1;
}

/* a pad object from a DIPH_BYOFFSET / DIPH_BYID reference */
static int find_obj(paddev *d, DWORD obj, DWORD how)
{
    int o;
    for (o = 0; o < NOBJS; o++) {
        if ((how == DIPH_BYOFFSET && obj_ofs(d, o) == obj)
            || (how == DIPH_BYID && (obj_type(o) & 0x00ffffff) == (obj & 0x00ffffff))) {
            return o;
        }
    }
    return -1;
}

/* the raw value of a pad object as the game's data format wants it */
static DWORD raw_value(paddev *d, int o, const padstate_t *s)
{
    if (OBJS[o].kind == O_AXIS) {
        int a = OBJS[o].inst;
        float v = s->axis[a], dz = d->dead[a] / 10000.0f, sat = d->sat[a] / 10000.0f, m = fabsf(v);
        double x;
        if (m <= dz) {
            v = 0;
        } else if (sat > dz) {
            m = (m - dz) / (sat - dz);
            v = (v < 0 ? -1 : 1) * (m > 1 ? 1 : m);
        }
        /* the centre of the range exactly at rest (the game has no dead zone of its own) */
        x = d->rmin[a] + (double)(v + 1) / 2 * ((double)d->rmax[a] - d->rmin[a]);
        return (DWORD)(LONG)floor(x + 0.5);
    }
    if (OBJS[o].kind == O_BUTTON) {
        return s->button[OBJS[o].inst];
    }
    return s->pov;
}

/* the format object's value when no pad object feeds it */
static DWORD empty_value(paddev *d, int f)
{
    return kind_of_type(d->fmt_type[f]) == O_POV ? (DWORD)-1 : 0;
}

static void push_event(paddev *d, DWORD ofs, DWORD data)
{
    DIDEVICEOBJECTDATA *ev;
    DWORD cap = d->bufsize < QUEUE_MAX ? d->bufsize : QUEUE_MAX;
    if (!cap) {
        return;
    }
    if ((DWORD)d->qcount >= cap) {
        d->qhead = (d->qhead + 1) % QUEUE_MAX;   /* the oldest is lost, like DirectInput */
        d->qcount--;
        d->overflow = 1;
    }
    ev = &d->queue[(d->qhead + d->qcount) % QUEUE_MAX];
    memset(ev, 0, sizeof *ev);
    ev->dwOfs = ofs;
    ev->dwData = data;
    ev->dwTimeStamp = GetTickCount();
    ev->dwSequence = ++d->seq;
    d->qcount++;
}

/* reads the pad; queues the changes for GetDeviceData */
static void refresh(paddev *d)
{
    int f;
    pump();
    read_pad(d->slot, &d->now);
    if (!d->nfmt) {
        return;
    }
    for (f = 0; f < d->nfmt; f++) {
        DWORD v = d->fmt_obj[f] >= 0 ? raw_value(d, d->fmt_obj[f], &d->now) : empty_value(d, f);
        if (d->primed && v != d->last[f] && d->acquired) {
            push_event(d, d->fmt_ofs[f], v);
        }
        d->last[f] = v;
    }
    d->primed = 1;
}

#define DEV paddev *d = (paddev *)self

static HRESULT WINAPI dev_QueryInterface(void *self, REFIID iid, void **out)
{
    if (!out) {
        return E_POINTER;
    }
    if (IsEqualGUID(iid, &IID_DIDev8A) || IsEqualGUID(iid, &IID_Unknown_)) {
        *out = self;
        ((paddev *)self)->ref++;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dev_AddRef(void *self) { DEV; return (ULONG)++d->ref; }

static ULONG WINAPI dev_Release(void *self)
{
    DEV;
    LONG n = --d->ref;
    if (n > 0) {
        return (ULONG)n;
    }
    HeapFree(GetProcessHeap(), 0, d);
    return 0;
}

static HRESULT WINAPI dev_GetCapabilities(void *self, LPDIDEVCAPS caps)
{
    DWORD size;
    (void)self;
    if (!caps || (caps->dwSize != sizeof(DIDEVCAPS) && caps->dwSize != sizeof(DIDEVCAPS_DX3))) {
        return DIERR_INVALIDPARAM;
    }
    size = caps->dwSize;
    memset(caps, 0, size);
    caps->dwSize = size;
    caps->dwFlags = DIDC_ATTACHED | DIDC_EMULATED;
    caps->dwDevType = PAD_DEVTYPE;
    caps->dwAxes = NAXES;
    caps->dwButtons = NBUTTONS;
    caps->dwPOVs = 0;
    if (size == sizeof(DIDEVCAPS)) {
        caps->dwFirmwareRevision = 1;
        caps->dwHardwareRevision = 1;
    }
    return DI_OK;
}

static void fill_object(paddev *d, int o, LPDIDEVICEOBJECTINSTANCEA oi)
{
    DWORD size = oi->dwSize;
    memset(oi, 0, size);
    oi->dwSize = size;
    oi->guidType = *OBJS[o].guid;
    oi->dwOfs = obj_ofs(d, o);
    oi->dwType = obj_type(o);
    oi->dwFlags = OBJS[o].kind == O_AXIS ? DIDOI_ASPECTPOSITION : 0;
    lstrcpynA(oi->tszName, OBJS[o].name, MAX_PATH);
    if (size == sizeof(DIDEVICEOBJECTINSTANCEA)) {
        oi->wUsagePage = OBJS[o].kind == O_BUTTON ? 9 : 1;
        oi->wUsage = OBJS[o].kind == O_BUTTON ? (WORD)(OBJS[o].inst + 1)
                     : OBJS[o].kind == O_POV ? 0x39 : (WORD)(0x30 + OBJS[o].inst);
    }
}

static HRESULT WINAPI dev_EnumObjects(void *self, LPDIENUMDEVICEOBJECTSCALLBACKA cb, void *ref, DWORD flags)
{
    DEV;
    int o;
    if (!cb) {
        return DIERR_INVALIDPARAM;
    }
    for (o = 0; o < NOBJS; o++) {
        DIDEVICEOBJECTINSTANCEA oi;
        DWORD t = obj_type(o);
        if (flags != DIDFT_ALL && !(t & flags & 0xff)) {
            continue;
        }
        if (flags & (DIDFT_FFACTUATOR | DIDFT_FFEFFECTTRIGGER)) {
            continue;                 /* no force feedback */
        }
        oi.dwSize = sizeof oi;
        fill_object(d, o, &oi);
        if (cb(&oi, ref) == DIENUM_STOP) {
            break;
        }
    }
    return DI_OK;
}

/* which axes a property header names (DIPH_DEVICE = all) */
static int prop_axes(paddev *d, LPCDIPROPHEADER h, int *first, int *last)
{
    int o;
    if (h->dwHow == DIPH_DEVICE) {
        *first = 0;
        *last = NAXES - 1;
        return 1;
    }
    o = find_obj(d, h->dwObj, h->dwHow);
    if (o < 0 || OBJS[o].kind != O_AXIS) {
        return 0;
    }
    *first = *last = OBJS[o].inst;
    return 1;
}

static void fill_string(LPDIPROPHEADER h, const char *s)
{
    if (h->dwSize >= sizeof(DIPROPSTRING)) {
        MultiByteToWideChar(CP_ACP, 0, s, -1, ((LPDIPROPSTRING)h)->wsz, MAX_PATH);
    }
}

static HRESULT WINAPI dev_GetProperty(void *self, REFGUID prop, LPDIPROPHEADER h)
{
    DEV;
    uintptr_t id = (uintptr_t)prop;
    int a, b;
    if (!h) {
        return DIERR_INVALIDPARAM;
    }
    if (id == (uintptr_t)DIPROP_BUFFERSIZE) {
        ((LPDIPROPDWORD)h)->dwData = d->bufsize;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_AUTOCENTER) {
        ((LPDIPROPDWORD)h)->dwData = DIPROPAUTOCENTER_OFF;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_GRANULARITY) {
        ((LPDIPROPDWORD)h)->dwData = 1;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_AXISMODE) {
        ((LPDIPROPDWORD)h)->dwData = DIPROPAXISMODE_ABS;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_JOYSTICKID) {
        ((LPDIPROPDWORD)h)->dwData = (DWORD)d->slot;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_VIDPID) {
        SDL_GameController *gc = pad_controller(d->slot);
        ((LPDIPROPDWORD)h)->dwData = gc ? (DWORD)SDL_GameControllerGetProduct(gc) << 16 | SDL_GameControllerGetVendor(gc) : 0;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_PRODUCTNAME || id == (uintptr_t)DIPROP_INSTANCENAME) {
        char name[MAX_PATH];
        SDL_GameController *gc = pad_controller(d->slot);
        if (id == (uintptr_t)DIPROP_INSTANCENAME) {
            snprintf(name, sizeof name, "Gamepad %d", d->slot + 1);
        } else {
            snprintf(name, sizeof name, "%s", gc && SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "Gamepad");
        }
        fill_string(h, name);
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_RANGE || id == (uintptr_t)DIPROP_DEADZONE || id == (uintptr_t)DIPROP_SATURATION) {
        if (h->dwHow == DIPH_DEVICE || !prop_axes(d, h, &a, &b)) {
            return DIERR_OBJECTNOTFOUND;
        }
        if (id == (uintptr_t)DIPROP_RANGE) {
            ((LPDIPROPRANGE)h)->lMin = d->rmin[a];
            ((LPDIPROPRANGE)h)->lMax = d->rmax[a];
        } else {
            ((LPDIPROPDWORD)h)->dwData = id == (uintptr_t)DIPROP_DEADZONE ? d->dead[a] : d->sat[a];
        }
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_SetProperty(void *self, REFGUID prop, LPCDIPROPHEADER h)
{
    DEV;
    uintptr_t id = (uintptr_t)prop;
    int a, b, i;
    if (!h) {
        return DIERR_INVALIDPARAM;
    }
    if (id == (uintptr_t)DIPROP_BUFFERSIZE) {
        d->bufsize = ((LPCDIPROPDWORD)h)->dwData;
        d->qcount = 0;
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_AUTOCENTER || id == (uintptr_t)DIPROP_AXISMODE || id == (uintptr_t)DIPROP_CALIBRATIONMODE
        || id == (uintptr_t)DIPROP_FFGAIN) {
        return DI_OK;
    }
    if (id == (uintptr_t)DIPROP_RANGE || id == (uintptr_t)DIPROP_DEADZONE || id == (uintptr_t)DIPROP_SATURATION) {
        if (!prop_axes(d, h, &a, &b)) {
            return DIERR_OBJECTNOTFOUND;
        }
        for (i = a; i <= b; i++) {
            if (id == (uintptr_t)DIPROP_RANGE) {
                if (((LPCDIPROPRANGE)h)->lMin >= ((LPCDIPROPRANGE)h)->lMax) {
                    return DIERR_INVALIDPARAM;
                }
                d->rmin[i] = ((LPCDIPROPRANGE)h)->lMin;
                d->rmax[i] = ((LPCDIPROPRANGE)h)->lMax;
            } else {
                DWORD v = ((LPCDIPROPDWORD)h)->dwData;
                v = v > 10000 ? 10000 : v;
                if (id == (uintptr_t)DIPROP_DEADZONE) {
                    d->dead[i] = v;
                } else {
                    d->sat[i] = v;
                }
            }
        }
        d->primed = 0;
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_Acquire(void *self)
{
    DEV;
    if (!d->nfmt) {
        return DIERR_INVALIDPARAM;
    }
    if (d->acquired) {
        return DI_OK;                 /* not S_FALSE: the game takes anything else as a failure */
    }
    d->acquired = 1;
    d->primed = 0;
    d->qcount = 0;
    refresh(d);
    return DI_OK;
}

static HRESULT WINAPI dev_Unacquire(void *self)
{
    DEV;
    if (!d->acquired) {
        return DI_NOEFFECT;
    }
    d->acquired = 0;
    return DI_OK;
}

static HRESULT WINAPI dev_GetDeviceState(void *self, DWORD size, void *data)
{
    DEV;
    int f;
    if (!d->acquired) {
        return DIERR_NOTACQUIRED;
    }
    if (!data || size != d->datasize) {
        return DIERR_INVALIDPARAM;
    }
    refresh(d);
    memset(data, 0, size);
    for (f = 0; f < d->nfmt; f++) {
        DWORD v = d->last[f];
        BYTE *p = (BYTE *)data + d->fmt_ofs[f];
        if (kind_of_type(d->fmt_type[f]) == O_BUTTON) {
            if (d->fmt_ofs[f] < size) *p = (BYTE)v;
        } else if (d->fmt_ofs[f] + 4 <= size) {
            memcpy(p, &v, 4);
        }
    }
    return DI_OK;
}

static HRESULT WINAPI dev_GetDeviceData(void *self, DWORD cb, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inout, DWORD flags)
{
    DEV;
    DWORD want, n = 0;
    HRESULT hr;
    if (!d->acquired) {
        return DIERR_NOTACQUIRED;
    }
    if (!d->bufsize) {
        return DIERR_NOTBUFFERED;
    }
    if (!inout || (rgdod && cb < sizeof(DIDEVICEOBJECTDATA_DX3))) {
        return DIERR_INVALIDPARAM;
    }
    refresh(d);
    want = *inout;
    while (n < want && n < (DWORD)d->qcount) {
        if (rgdod) {
            BYTE *dst = (BYTE *)rgdod + n * cb;
            memset(dst, 0, cb);
            memcpy(dst, &d->queue[(d->qhead + n) % QUEUE_MAX], cb < sizeof(DIDEVICEOBJECTDATA) ? cb : sizeof(DIDEVICEOBJECTDATA));
        }
        n++;
    }
    if (!(flags & DIGDD_PEEK)) {
        d->qhead = (d->qhead + n) % QUEUE_MAX;
        d->qcount -= n;
    }
    *inout = n;
    hr = d->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    if (!(flags & DIGDD_PEEK)) {
        d->overflow = 0;
    }
    return hr;
}

static HRESULT WINAPI dev_SetDataFormat(void *self, LPCDIDATAFORMAT fmt)
{
    DEV;
    DWORD i;
    int used[NOBJS] = { 0 }, o;
    if (!fmt || fmt->dwSize != sizeof(DIDATAFORMAT) || fmt->dwObjSize != sizeof(DIOBJECTDATAFORMAT) || fmt->dwNumObjs > 256) {
        return DIERR_INVALIDPARAM;
    }
    if (d->acquired) {
        return DIERR_ACQUIRED;
    }
    d->nfmt = (int)fmt->dwNumObjs;
    d->datasize = fmt->dwDataSize;
    for (o = 0; o < NOBJS; o++) {
        d->obj_fmt[o] = -1;
    }
    for (i = 0; i < fmt->dwNumObjs; i++) {
        const DIOBJECTDATAFORMAT *f = &fmt->rgodf[i];
        int kind = kind_of_type(f->dwType);
        DWORD inst = DIDFT_GETINSTANCE(f->dwType);
        d->fmt_ofs[i] = f->dwOfs;
        d->fmt_type[i] = f->dwType;
        d->fmt_obj[i] = -1;
        for (o = 0; o < NOBJS; o++) {
            if (used[o] || OBJS[o].kind != kind) continue;
            if (f->pguid && !IsEqualGUID(f->pguid, OBJS[o].guid)) continue;
            if (inst != (DIDFT_ANYINSTANCE >> 8) && inst != (DWORD)OBJS[o].inst) continue;
            used[o] = 1;
            d->fmt_obj[i] = o;
            d->obj_fmt[o] = (int)i;
            break;
        }
    }
    d->primed = 0;
    d->qcount = 0;
    return DI_OK;
}

static HRESULT WINAPI dev_SetEventNotification(void *self, HANDLE ev) { DEV; d->event = ev; return DI_OK; }
static HRESULT WINAPI dev_SetCooperativeLevel(void *self, HWND w, DWORD flags) { (void)self; (void)w; (void)flags; return DI_OK; }

static HRESULT WINAPI dev_GetObjectInfo(void *self, LPDIDEVICEOBJECTINSTANCEA oi, DWORD obj, DWORD how)
{
    DEV;
    int o;
    if (!oi || (oi->dwSize != sizeof(DIDEVICEOBJECTINSTANCEA) && oi->dwSize != sizeof(DIDEVICEOBJECTINSTANCE_DX3A))) {
        return DIERR_INVALIDPARAM;
    }
    o = find_obj(d, obj, how);
    if (o < 0) {
        return DIERR_OBJECTNOTFOUND;
    }
    fill_object(d, o, oi);
    return DI_OK;
}

static void fill_instance(int slot, LPDIDEVICEINSTANCEA di)
{
    DWORD size = di->dwSize;
    SDL_GameController *gc = pad_controller(slot);
    memset(di, 0, size);
    di->dwSize = size;
    di->guidInstance = PAD_INSTANCE;
    di->guidInstance.Data4[7] = (BYTE)slot;
    di->guidProduct = PAD_PRODUCT;
    di->guidProduct.Data4[7] = (BYTE)slot;
    di->dwDevType = PAD_DEVTYPE;
    snprintf(di->tszInstanceName, MAX_PATH, "Gamepad %d", slot + 1);
    snprintf(di->tszProductName, MAX_PATH, "%s", gc && SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "Gamepad (SDL)");
    if (size == sizeof(DIDEVICEINSTANCEA)) {
        di->wUsagePage = 1;
        di->wUsage = 5;               /* game pad */
    }
}

static HRESULT WINAPI dev_GetDeviceInfo(void *self, LPDIDEVICEINSTANCEA di)
{
    DEV;
    if (!di || (di->dwSize != sizeof(DIDEVICEINSTANCEA) && di->dwSize != sizeof(DIDEVICEINSTANCE_DX3A))) {
        return DIERR_INVALIDPARAM;
    }
    fill_instance(d->slot, di);
    return DI_OK;
}

static HRESULT WINAPI dev_RunControlPanel(void *self, HWND w, DWORD flags) { (void)self; (void)w; (void)flags; return DI_OK; }
static HRESULT WINAPI dev_Initialize(void *self, HINSTANCE i, DWORD v, REFGUID g) { (void)self; (void)i; (void)v; (void)g; return DI_OK; }

static HRESULT WINAPI dev_CreateEffect(void *self, REFGUID guid, LPCDIEFFECT eff, LPDIRECTINPUTEFFECT *out, LPUNKNOWN outer)
{
    (void)self; (void)guid; (void)eff; (void)outer;
    if (out) {
        *out = NULL;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_EnumEffects(void *self, LPDIENUMEFFECTSCALLBACKA cb, void *ref, DWORD type)
{
    (void)self; (void)ref; (void)type;
    return cb ? DI_OK : DIERR_INVALIDPARAM;
}

static HRESULT WINAPI dev_GetEffectInfo(void *self, LPDIEFFECTINFOA info, REFGUID guid)
{
    (void)self; (void)info; (void)guid;
    return DIERR_DEVICENOTREG;
}

static HRESULT WINAPI dev_GetForceFeedbackState(void *self, LPDWORD out)
{
    (void)self;
    if (out) {
        *out = 0;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_SendForceFeedbackCommand(void *self, DWORD cmd) { (void)self; (void)cmd; return DIERR_UNSUPPORTED; }

static HRESULT WINAPI dev_EnumCreatedEffectObjects(void *self, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, void *ref, DWORD flags)
{
    (void)self; (void)ref; (void)flags;
    return cb ? DI_OK : DIERR_INVALIDPARAM;
}

static HRESULT WINAPI dev_Escape(void *self, LPDIEFFESCAPE esc) { (void)self; (void)esc; return DIERR_UNSUPPORTED; }

static HRESULT WINAPI dev_Poll(void *self)
{
    DEV;
    if (!d->acquired) {
        return DIERR_NOTACQUIRED;
    }
    refresh(d);
    return DI_NOEFFECT;
}

static HRESULT WINAPI dev_SendDeviceData(void *self, DWORD cb, LPCDIDEVICEOBJECTDATA data, LPDWORD inout, DWORD flags)
{
    (void)self; (void)cb; (void)data; (void)flags;
    if (inout) {
        *inout = 0;
    }
    return DI_OK;
}

static HRESULT WINAPI dev_EnumEffectsInFile(void *self, LPCSTR file, LPDIENUMEFFECTSINFILECALLBACK cb, void *ref, DWORD flags)
{
    (void)self; (void)file; (void)cb; (void)ref; (void)flags;
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_WriteEffectToFile(void *self, LPCSTR file, DWORD n, LPDIFILEEFFECT fx, DWORD flags)
{
    (void)self; (void)file; (void)n; (void)fx; (void)flags;
    return DIERR_UNSUPPORTED;
}

/* DirectInput 8 action mapping: not used by BF1942 */
static HRESULT WINAPI dev_BuildActionMap(void *self, void *af, LPCSTR user, DWORD flags) { (void)self; (void)af; (void)user; (void)flags; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI dev_SetActionMap(void *self, void *af, LPCSTR user, DWORD flags) { (void)self; (void)af; (void)user; (void)flags; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI dev_GetImageInfo(void *self, void *info) { (void)self; (void)info; return DIERR_UNSUPPORTED; }

static void *g_dev_vtbl[] = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCapabilities, dev_EnumObjects, dev_GetProperty,
    dev_SetProperty, dev_Acquire, dev_Unacquire, dev_GetDeviceState, dev_GetDeviceData, dev_SetDataFormat,
    dev_SetEventNotification, dev_SetCooperativeLevel, dev_GetObjectInfo, dev_GetDeviceInfo, dev_RunControlPanel,
    dev_Initialize, dev_CreateEffect, dev_EnumEffects, dev_GetEffectInfo, dev_GetForceFeedbackState,
    dev_SendForceFeedbackCommand, dev_EnumCreatedEffectObjects, dev_Escape, dev_Poll, dev_SendDeviceData,
    dev_EnumEffectsInFile, dev_WriteEffectToFile, dev_BuildActionMap, dev_SetActionMap, dev_GetImageInfo,
};

static HRESULT create_pad_device(int slot, void **out)
{
    paddev *d;
    int a;
    if (!out) {
        return E_POINTER;
    }
    d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    if (!d) {
        return DIERR_OUTOFMEMORY;
    }
    d->vtbl = g_dev_vtbl;
    d->ref = 1;
    d->slot = slot;
    for (a = 0; a < NAXES; a++) {
        d->rmin[a] = 0;
        d->rmax[a] = 65535;           /* DirectInput's default range */
        d->sat[a] = 10000;
    }
    for (a = 0; a < NOBJS; a++) {
        d->obj_fmt[a] = -1;
    }
    *out = d;
    bf_log("gamepad: gamepad %d opened by the game (%s)", slot + 1,
           pad_controller(slot) ? SDL_GameControllerName(pad_controller(slot)) : "not connected yet");
    return DI_OK;
}

/* ---------------------------------------------------------------- the IDirectInput8A wrapper */

typedef struct {
    void **vtbl;
    IDirectInput8A *real;
    LONG ref;
} diwrap;

static void *g_di_vtbl[];
#define DI diwrap *w = (diwrap *)self
#define REAL(m) w->real->lpVtbl->m

static HRESULT WINAPI di_QueryInterface(void *self, REFIID iid, void **out)
{
    DI;
    if (out && (IsEqualGUID(iid, &IID_DI8A) || IsEqualGUID(iid, &IID_Unknown_))) {
        *out = self;
        w->ref++;
        REAL(AddRef)(w->real);
        return S_OK;
    }
    return REAL(QueryInterface)(w->real, iid, out);
}

static ULONG WINAPI di_AddRef(void *self) { DI; w->ref++; return REAL(AddRef)(w->real); }

static ULONG WINAPI di_Release(void *self)
{
    DI;
    ULONG n = REAL(Release)(w->real);
    if (--w->ref == 0) {
        HeapFree(GetProcessHeap(), 0, w);
    }
    return n;
}

static HRESULT WINAPI di_CreateDevice(void *self, REFGUID guid, LPDIRECTINPUTDEVICE8A *out, LPUNKNOWN outer)
{
    DI;
    int slot = dev_slot_of(guid);
    if (slot >= 0) {
        return outer ? DIERR_NOAGGREGATION : create_pad_device(slot, (void **)out);
    }
    return REAL(CreateDevice)(w->real, guid, out, outer);
}

typedef struct { LPDIENUMDEVICESCALLBACKA cb; void *ref; int stopped, hidden; } enumctx;

/* a DirectInput device that is one of the SDL gamepads (its VID/PID from a HID product GUID, "PIDVID") */
static int is_twin(LPCDIDEVICEINSTANCEA di)
{
    static const BYTE pidvid[8] = { 0, 0, 'P', 'I', 'D', 'V', 'I', 'D' };
    int i, n;
    WORD vid, pid;
    if (!g_hide_twins || g_sdl != 1 || memcmp(di->guidProduct.Data4, pidvid, 8)) {
        return 0;
    }
    vid = LOWORD(di->guidProduct.Data1);
    pid = HIWORD(di->guidProduct.Data1);
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) {
        if (SDL_IsGameController(i) && SDL_JoystickGetDeviceVendor(i) == vid && SDL_JoystickGetDeviceProduct(i) == pid) {
            return 1;
        }
    }
    return 0;
}

static BOOL CALLBACK filter(LPCDIDEVICEINSTANCEA di, void *ref)
{
    enumctx *c = ref;
    if (is_twin(di)) {
        c->hidden++;
        bf_log("gamepad: DirectInput device \"%s\" is an SDL gamepad, hidden", di->tszProductName);
        return DIENUM_CONTINUE;
    }
    if (c->cb(di, c->ref) == DIENUM_STOP) {
        c->stopped = 1;
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

/* the device types an EnumDevices filter lets through that a gamepad is */
static int wants_pads(DWORD type)
{
    return type == DI8DEVCLASS_ALL || type == DI8DEVCLASS_GAMECTRL_ || type == DI8DEVTYPE_GAMEPAD_;
}

static HRESULT WINAPI di_EnumDevices(void *self, DWORD type, LPDIENUMDEVICESCALLBACKA cb, void *ref, DWORD flags)
{
    DI;
    enumctx c = { cb, ref, 0, 0 };
    HRESULT hr;
    int s;
    if (!cb) {
        return DIERR_INVALIDPARAM;
    }
    if (!wants_pads(type) && type != DI8DEVTYPE_JOYSTICK) {
        return REAL(EnumDevices)(w->real, type, cb, ref, flags);
    }
    sdl_init();
    /* the gamepads first, in slot order: the game takes the N-th device it is offered as IDFGameController_N */
    for (s = 0; g_sdl == 1 && !(flags & DIEDFL_FORCEFEEDBACK) && s < g_nslots; s++) {
        DIDEVICEINSTANCEA di;
        if (!pad_controller(s)) {
            continue;
        }
        di.dwSize = sizeof di;
        fill_instance(s, &di);
        bf_log("gamepad: offering gamepad %d (%s) to the game", s + 1, di.tszProductName);
        if (cb(&di, ref) == DIENUM_STOP) {
            return DI_OK;
        }
    }
    hr = REAL(EnumDevices)(w->real, type, filter, &c, flags);
    return hr;
}

static HRESULT WINAPI di_GetDeviceStatus(void *self, REFGUID guid)
{
    DI;
    if (dev_slot_of(guid) >= 0) {
        return pad_controller(dev_slot_of(guid)) ? DI_OK : DI_NOTATTACHED;
    }
    return REAL(GetDeviceStatus)(w->real, guid);
}

static HRESULT WINAPI di_RunControlPanel(void *self, HWND hwnd, DWORD flags) { DI; return REAL(RunControlPanel)(w->real, hwnd, flags); }
static HRESULT WINAPI di_Initialize(void *self, HINSTANCE inst, DWORD ver) { DI; return REAL(Initialize)(w->real, inst, ver); }
static HRESULT WINAPI di_FindDevice(void *self, REFGUID cls, LPCSTR name, LPGUID out) { DI; return REAL(FindDevice)(w->real, cls, name, out); }

static HRESULT WINAPI di_EnumDevicesBySemantics(void *self, LPCSTR user, LPDIACTIONFORMATA af, LPDIENUMDEVICESBYSEMANTICSCBA cb,
                                                void *ref, DWORD flags)
{
    DI;
    return REAL(EnumDevicesBySemantics)(w->real, user, af, cb, ref, flags);
}

static HRESULT WINAPI di_ConfigureDevices(void *self, LPDICONFIGUREDEVICESCALLBACK cb, LPDICONFIGUREDEVICESPARAMSA p, DWORD flags,
                                          void *ref)
{
    DI;
    return REAL(ConfigureDevices)(w->real, cb, p, flags, ref);
}

static void *g_di_vtbl[] = {
    di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus,
    di_RunControlPanel, di_Initialize, di_FindDevice, di_EnumDevicesBySemantics, di_ConfigureDevices,
};

void *bf_gamepad_wrap_dinput(const GUID *iid, void *direct_input)
{
    diwrap *w;
    if (!g_on || !direct_input || !IsEqualGUID(iid, &IID_DI8A)) {
        return direct_input;
    }
    w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w);
    if (!w) {
        return direct_input;
    }
    w->vtbl = g_di_vtbl;
    w->real = direct_input;
    w->ref = 1;
    bf_log("gamepad: the game's DirectInput 8 object is wrapped");
    return w;
}

/* ---------------------------------------------------------------- Settings/Gamepad.con */

/* The profile's controls are run from 0x44f350: console(*0x9a2380)->vfunc 0x1c("Settings/Controls.con") at
 * 0x44ffef, which runs the Common, Infantry, Land and Air .con files of Profiles/<name>/Controls. The Controls menu rewrites those files from its own table
 * and drops what it does not model (stick look, the second pad, axis scales), so the gamepad's bindings live in a
 * file of their own, run right after: Settings/Gamepad.con (found like Controls.con, i.e. in the save folder with
 * saves.c). ControlMap.create only selects a map that exists; every binding goes into the action's second slot
 * (the trailing 1), so keyboard and mouse keep working. The file is written with these defaults when there is none. */
#define CONTROLS_RUN_NEXT 0x44fff2u   /* lea ecx,[esp+0x50]; call [string dtor] after the Controls.con run */
#define CONSOLE_PTR       0x9a2380u   /* the console object */

static const char GAMEPAD_CON[] =
    "rem *** Gamepad bindings (bf1942patch) ***\r\n"
    "rem Run after the profile's controls, while a gamepad is connected. Edit freely; delete this file to get the\r\n"
    "rem defaults back. Axes: IDAxis_0/1 left stick X/Y (down +), IDAxis_2/3 right stick X/Y, IDAxis_4/5 LT/RT.\r\n"
    "rem Buttons: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back, 7 Start, 8 LS, 9 RS, 10 LT, 11 RT, 12-15 D-pad up/down/\r\n"
    "rem left/right. The last number of a line is the binding slot (1 = the second one, next to keyboard/mouse).\r\n"
    "rem Look speed: ControlMap.setAxisScale IDFGameController_0 IDAxis_2 <factor> (and IDAxis_3).\r\n"
    "\r\n"
    "rem *** Infantry ***\r\n"
    "ControlMap.create defaultPlayerInputControlMap\r\n"
    "ControlMap.addAxisToAxisMapping c_PIThrottle IDFGameController_0 IDAxis_1 1 1\r\n"
    "ControlMap.addAxisToAxisMapping c_PIYaw IDFGameController_0 IDAxis_0 0 1\r\n"
    "ControlMap.addAxisToAxisMapping c_PIMouseLookX IDFGameController_0 IDAxis_2 0 1\r\n"
    "ControlMap.addAxisToAxisMapping c_PIMouseLookY IDFGameController_0 IDAxis_3 0 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIFire IDFGameController_0 IDButton_11 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIAltFire IDFGameController_0 IDButton_10 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIAction IDFGameController_0 IDButton_0 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PICrouch IDFGameController_0 IDButton_1 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIReload IDFGameController_0 IDButton_2 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIUse IDFGameController_0 IDButton_3 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIPrevItem IDFGameController_0 IDButton_4 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PINextItem IDFGameController_0 IDButton_5 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIMap IDFGameController_0 IDButton_6 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIWalk IDFGameController_0 IDButton_8 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PILie IDFGameController_0 IDButton_9 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIShowScoreBoard IDFGameController_0 IDButton_12 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIDrop IDFGameController_0 IDButton_13 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIToggleCameraMode IDFGameController_0 IDButton_14 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIRadio1 IDFGameController_0 IDButton_15 c_CMNonRepetive 1\r\n"
    "\r\n"
    "rem *** Land and sea vehicles: left stick drives, right stick aims ***\r\n"
    "ControlMap.create LandSeaPlayerInputControlMap\r\n"
    "ControlMap.addAxisToAxisMapping c_PIThrottle IDFGameController_0 IDAxis_1 1 1\r\n"
    "ControlMap.addAxisToAxisMapping c_PIYaw IDFGameController_0 IDAxis_0 0 1\r\n"
    "ControlMap.addAxisToAxisMapping c_PIMouseLookX IDFGameController_0 IDAxis_2 0 1\r\n"
    "ControlMap.addAxisToAxisMapping c_PIMouseLookY IDFGameController_0 IDAxis_3 0 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIFire IDFGameController_0 IDButton_11 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIAltFire IDFGameController_0 IDButton_10 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIUse IDFGameController_0 IDButton_3 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIPrevItem IDFGameController_0 IDButton_4 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PINextItem IDFGameController_0 IDButton_5 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIMap IDFGameController_0 IDButton_6 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIShowScoreBoard IDFGameController_0 IDButton_12 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIToggleCameraMode IDFGameController_0 IDButton_14 c_CMNonRepetive 1\r\n"
    "\r\n"
    "rem *** Aircraft: left stick pitch and roll, right stick rudder and throttle ***\r\n"
    "ControlMap.create AirPlayerInputControlMap\r\n"
    "ControlMap.addAxisToAxisMapping c_PIRoll IDFGameController_0 IDAxis_0 0 1\r\n"
    "ControlMap.addAxisToAxisMapping c_PIPitch IDFGameController_0 IDAxis_1 1 1\r\n"
    "ControlMap.addAxisToAxisMapping c_PIYaw IDFGameController_0 IDAxis_2 0 1\r\n"
    "ControlMap.addAxisToAxisMapping c_PIThrottle IDFGameController_0 IDAxis_3 1 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIFire IDFGameController_0 IDButton_11 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIAltFire IDFGameController_0 IDButton_10 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIUse IDFGameController_0 IDButton_3 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIMouseLook IDFGameController_0 IDButton_9 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIMap IDFGameController_0 IDButton_6 c_CMNonRepetive 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIShowScoreBoard IDFGameController_0 IDButton_12 c_CMPushAndHold 1\r\n"
    "ControlMap.addButtonToTriggerMapping c_PIToggleCameraMode IDFGameController_0 IDButton_14 c_CMNonRepetive 1\r\n"
    "\r\n"
    "rem *** Menus: Start opens the in-game menu ***\r\n"
    "ControlMap.create defaultGameControlMap\r\n"
    "ControlMap.addButtonToTriggerMapping c_GIMenu IDFGameController_0 IDButton_7 c_CMNonRepetive 1\r\n";

static int g_bindings = 1;

/* the folder Settings/Gamepad.con is looked for in: Mods\<mod>\Settings of the save folder (or the install) */
static void gamepad_con_path(char *out, size_t size)
{
    char mod[64] = "bf1942";
    const char *p = strstr(GetCommandLineA(), "+game ");
    size_t n = 0;
    if (p) {
        for (p += 6; *p == ' '; p++) {
        }
        while (p[n] && p[n] != ' ' && p[n] != '"' && n + 1 < sizeof mod) {
            n++;
        }
        if (n) {
            memcpy(mod, p, n);
            mod[n] = '\0';
        }
    }
    snprintf(out, size, "%sMods\\%s\\Settings\\Gamepad.con", bf_save_dir()[0] ? bf_save_dir() : bf_game_dir(), mod);
}

typedef int (__attribute__((thiscall)) *run_file_fn)(void *console, const void *path);

void __cdecl bf_gamepad_after_controls(void)
{
    void *console = *(void **)BF_ADDR(CONSOLE_PTR);
    char path[MAX_PATH], install[MAX_PATH];
    BYTE str[BF_STRING_SIZE];
    int s, any = 0;
    if (!g_bindings || !console) {
        return;
    }
    sdl_init();
    for (s = 0; s < PAD_MAX; s++) {
        any |= pad_controller(s) != NULL;
    }
    if (!any) {
        return;                       /* the second binding slots stay as the profile has them */
    }
    gamepad_con_path(path, sizeof path);
    snprintf(install, sizeof install, "%s%s", bf_game_dir(), path + strlen(bf_save_dir()[0] ? bf_save_dir() : bf_game_dir()));
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(install) == INVALID_FILE_ATTRIBUTES) {
        FILE *f;
        bf_make_dirs(path);
        if ((f = fopen(path, "wb")) != NULL) {
            fwrite(GAMEPAD_CON, 1, sizeof GAMEPAD_CON - 1, f);
            fclose(f);
            bf_log("gamepad: wrote the default bindings to %s", path);
        }
    }
    if (!bf_string_init(str, "Settings/Gamepad.con")) {
        bf_log("gamepad: std::string of bfcprt.dll not found, Settings/Gamepad.con is not run");
        g_bindings = 0;
        return;
    }
    ((run_file_fn)(*(void ***)console)[0x1c / 4])(console, str);
    bf_string_free(str);
    bf_log("gamepad: ran Settings/Gamepad.con");
}

/* the hook at 0x44fff2 (lea ecx,[esp+0x50]; call [string dtor]): the gamepad file, then what was replaced */
uint32_t bf_gamepad_dtor_slot;       /* the import slot of the replaced call (0x8c311c) */
extern void bf_gamepad_controls_hook(void);
__asm__(".text\n"
        ".globl _bf_gamepad_controls_hook\n"
        "_bf_gamepad_controls_hook:\n"
        "    pushal\n"
        "    call _bf_gamepad_after_controls\n"
        "    popal\n"
        "    leal 0x54(%esp), %ecx\n"
        "    movl _bf_gamepad_dtor_slot, %eax\n"
        "    call *(%eax)\n"
        "    ret\n");

/* ---------------------------------------------------------------- install */

typedef HRESULT (WINAPI *DirectInput8Create_fn)(HINSTANCE, DWORD, REFIID, void **, void *);
static DirectInput8Create_fn real_DirectInput8Create;

/* ASI / plugin builds: the exe's import, wrapped here (the proxy wraps in its own export) */
static HRESULT WINAPI hk_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, void **out, void *outer)
{
    HRESULT hr = real_DirectInput8Create(inst, ver, iid, out, outer);
    if (hr == 0 && out && *out) {
        *out = bf_gamepad_wrap_dinput(iid, *out);
    }
    return hr;
}

/* the dead zone, the stick response and the keys Start / Back press (also when set in the console) */
static int read_sticks(void)
{
    int sens = bf_setting_int("gamepadSensitivity");
    g_deadzone = bf_setting_int("gamepadDeadzone");
    g_deadzone = g_deadzone < 0 ? 0 : g_deadzone > 90 ? 90 : g_deadzone;
    sens = sens < 0 ? 0 : sens > 100 ? 100 : sens;
    g_curve = 3.0f - 2.0f * sens / 100.0f;   /* 100 = linear, 50 = squared, 0 = cubed */
    g_start_key = bf_setting_int("gamepadStartKey");
    g_back_key = bf_setting_int("gamepadBackKey");
    return sens;
}

static void sticks_changed(void)
{
    read_sticks();
}

void bf_gamepad_install(void)
{
    int sens;
    char self[MAX_PATH], *base;
    if (!bf_setting_int("gamepad")) {
        bf_log("gamepad: gamepad 0, no SDL gamepads");
        return;
    }
    g_on = 1;
    sens = read_sticks();
    bf_setting_watch("gamepadDeadzone", sticks_changed);
    bf_setting_watch("gamepadSensitivity", sticks_changed);
    bf_setting_watch("gamepadStartKey", sticks_changed);
    bf_setting_watch("gamepadBackKey", sticks_changed);
    g_hide_twins = bf_setting_int("gamepadHideDuplicates");
    g_bindings = bf_setting_int("gamepadBindings");
    if (g_bindings) {
        static const BYTE orig[10] = {0x8d, 0x4c, 0x24, 0x50, 0xff, 0x15, 0x1c, 0x31, 0x8c, 0x00};
        memcpy(&bf_gamepad_dtor_slot, orig + 6, 4);
        if (!bf_replace_call(CONTROLS_RUN_NEXT, orig, sizeof orig, bf_gamepad_controls_hook)) {
            bf_log("gamepad: the controls loading code differs, Settings/Gamepad.con is not run");
        }
    }

    /* the dinput8.dll proxy wraps the object it creates; the other builds go through the exe's import */
    GetModuleFileNameA(bf_module(), self, MAX_PATH);
    base = strrchr(self, '\\');
    if (!bf_module() || _stricmp(base ? base + 1 : self, "dinput8.dll")) {
        real_DirectInput8Create = bf_hook_import("DINPUT8.dll", "DirectInput8Create", hk_DirectInput8Create);
    }
    bf_log("gamepad: SDL gamepads on (dead zone %d%%, sensitivity %d)", g_deadzone, sens);
}
