/* Lip sync: the soldiers of the players heard in voice chat move their mouths with their voice.
 *
 * The game has lip sync of its own, for the radio messages: BFSoldier::speak (0x4fec90) queues the message's speak
 * symbols in the soldier (std::list<int> at +0x5a4, its head node at +0x5a8, size +0x5ac), and the soldier's update
 * (0x4fc49e) plays one every 0.1 s: the face AnimationStateMachine (0x612460, [0x9c9664]) maps the symbol to a face
 * state (+0x24, int[256], set by AnimationStateMachine.addSpeakConnectionSymbol in animations/AnimationStatesFace.con:
 * '_' 'i' 'e' 'o' 'm' the normal mouth, upper case the angry one), which BFSoldier (0x4f7a00, thiscall(state)) sets on
 * its face. The countdown at +0x5a0 (float, seconds) goes back to FaceLook, the idle face, once it runs out with
 * nothing queued.
 *
 * Here the same face states follow each talker's speech level instead of a queue: open (i, e, o, wider the louder)
 * and closed (m) in turn at about a syllable's pace, with the countdown kept short so the idle face comes back as the
 * voice stops. A soldier saying a radio message keeps its own lip sync.
 *
 * The soldier of a player is found the way the speak console command does (0x4133c3): IPlayer vtable +0x38 the
 * object controlled (+0xa9 alive); when that is not a soldier (a vehicle), the soldier in its tree (0x413430, fastcall(object,
 * class, stop class)). An object's class: +0x4c its template, whose vtable +0xc is the class id; BFSoldier's is
 * [0x8dcd10]. */
#include <stdlib.h>
#include "voice.h"

#define PLAYER_MANAGER  0x97d76cu
#define PM_BY_ID        0x18
#define PLAYER_OBJECT   0x38
#define PLAYER_ALIVE    0xa9         /* bool */
#define OBJECT_TEMPLATE 0x4c
#define TEMPLATE_CLASS  0xc
#define SOLDIER_CLASS   0x8dcd10u    /* int: BFSoldier's class id */
#define FIND_CLASS      0x413430u    /* fastcall(object, class, stop class): the object of class in object's tree */
#define OBJECT_ROOT     0x78         /* vtable: the tree an object is part of (the vehicle) */
#define SET_FACE        0x4f7a00u    /* thiscall(soldier, face state) */
#define FACE_MACHINE    0x9c9664u    /* the face AnimationStateMachine */
#define FACE_SYMBOLS    0x24         /* int[256]: the face state of each speak symbol */
#define SOLDIER_FACE_TIME 0x5a0      /* float: seconds until the face goes back to FaceLook */
#define SOLDIER_SPEAKING  0x5ac      /* the speak symbols queued (a radio message) */
#define SOLDIER_UPDATE  0x4fc49e     /* where the update reads the two above */

#define QUIET_DB        -50.0f       /* the mouth stays closed below this level */
#define LOUD_DB         -20.0f       /* and opens widest above this */
#define HOLD_TIME       0.25f        /* the face goes back to idle this long after the last change */

static int g_ok;
static ULONGLONG g_next[256];        /* by the game's player id: when the mouth changes next */
static unsigned char g_open[256];

static int readable(const void *p, size_t n)
{
    return p && !IsBadReadPtr(p, n);
}

/* an engine object: readable, with a vtable in the exe */
static int object(const void *p)
{
    uintptr_t vt;
    if (!readable(p, 8)) {
        return 0;
    }
    vt = *(const uintptr_t *)p;
    return vt >= 0x8c0000 && vt < 0xa00000 && readable((const void *)vt, 0x80);
}

static void *vcall(void *self, int offset)
{
    return ((void *(__attribute__((thiscall)) *)(void *))(*(void ***)self)[offset / 4])(self);
}

static int class_of(void *o)
{
    void *t = readable((BYTE *)o + OBJECT_TEMPLATE, 4) ? *(void **)((BYTE *)o + OBJECT_TEMPLATE) : NULL;
    return object(t) ? (int)(intptr_t)vcall(t, TEMPLATE_CLASS) : 0;
}

/* the soldier of the game's player id, NULL while it has none (dead: talking on the radio, or between rounds) */
static BYTE *soldier_of(int id)
{
    void *pm = *(void **)BF_ADDR(PLAYER_MANAGER), *player, *o;
    int soldier = *(int *)BF_ADDR(SOLDIER_CLASS);
    if (id < 0 || id > 255 || !object(pm)) {
        return NULL;
    }
    player = ((void *(__attribute__((thiscall)) *)(void *, int))(*(void ***)pm)[PM_BY_ID / 4])(pm, id);
    if (!object(player) || !readable((BYTE *)player + PLAYER_ALIVE, 1) || !((BYTE *)player)[PLAYER_ALIVE]
        || !object(o = vcall(player, PLAYER_OBJECT))) {
        return NULL;
    }
    if (class_of(o) != soldier && object(o = vcall(o, OBJECT_ROOT))) {
        o = ((void *(__attribute__((fastcall)) *)(void *, int, int))BF_ADDR(FIND_CLASS))(o, soldier, -1);
    }
    return object(o) && class_of(o) == soldier && readable((BYTE *)o + SOLDIER_FACE_TIME, 0x10) ? o : NULL;
}

/* the next mouth for a level: shut after an open one or when quiet, else open as wide as the voice is loud */
static int next_symbol(int id, float level_db, ULONGLONG now)
{
    float loud = (level_db - QUIET_DB) / (LOUD_DB - QUIET_DB);
    int r = rand();
    if (g_open[id] || loud <= 0) {
        g_open[id] = 0;
        g_next[id] = now + 60 + r % 60;
        return 'm';
    }
    g_open[id] = 1;
    g_next[id] = now + 100 + r % 120;
    if (loud > 0.66f) {
        return r & 0x100 ? 'o' : 'e';
    }
    return loud > 0.33f && (r & 0x100) ? 'e' : 'i';
}

void bf_voice_lips_update(const bf_voice_mouth *talkers, int n)
{
    BYTE *machine, *soldier;
    ULONGLONG now;
    int i, state;
    if (!g_ok || !n || !readable(machine = *(BYTE **)BF_ADDR(FACE_MACHINE), FACE_SYMBOLS + 256 * 4)) {
        return;
    }
    now = GetTickCount64();
    for (i = 0; i < n; i++) {
        int id = talkers[i].id;
        if (id < 0 || id > 255 || now < g_next[id] || !(soldier = soldier_of(id))
            || *(int *)(soldier + SOLDIER_SPEAKING)) {
            continue;
        }
        state = *(int *)(machine + FACE_SYMBOLS + next_symbol(id, talkers[i].level_db, now) * 4);
        if (state > 0) {
            ((void (__attribute__((thiscall)) *)(void *, int))BF_ADDR(SET_FACE))(soldier, state);
            *(float *)(soldier + SOLDIER_FACE_TIME) = HOLD_TIME;
        }
    }
}

void bf_voice_lips_install(void)
{
    static const BYTE set_face[10] = {0x56, 0x57, 0x8b, 0xf9, 0x8b, 0xb7, 0xa4, 0x02, 0x00, 0x00};
    static const BYTE find_class[10] = {0x56, 0x8b, 0xf1, 0x85, 0xf6, 0x57, 0x8b, 0xfa, 0x75, 0x07};
    static const BYTE machine[6] = {0xa1, 0x64, 0x96, 0x9c, 0x00, 0xc3};   /* mov eax,[009c9664]; ret */
    /* mov eax,[esi+5ac]; fld dword ptr [esi+5a0] */
    static const BYTE update[12] = {0x8b, 0x86, 0xac, 0x05, 0x00, 0x00, 0xd9, 0x86, 0xa0, 0x05, 0x00, 0x00};
    /* the speak command: mov ecx,[esi+4c]; mov edx,[ecx]; call [edx+0c]; cmp eax,[008dcd10] */
    static const BYTE speak[14] = {0x8b, 0x4e, 0x4c, 0x8b, 0x11, 0xff, 0x52, 0x0c, 0x3b, 0x05, 0x10, 0xcd, 0x8d, 0x00};
    g_ok = bf_expect(SET_FACE, set_face, sizeof set_face) && bf_expect(FIND_CLASS, find_class, sizeof find_class)
           && bf_expect(0x612460, machine, sizeof machine) && bf_expect(SOLDIER_UPDATE, update, sizeof update)
           && bf_expect(0x4133d3, speak, sizeof speak);
    if (!g_ok) {
        bf_log("voice: the soldier's face code differs, no lip sync");
    }
}
