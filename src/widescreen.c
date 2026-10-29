/* Widescreen: Hor+ 3D, a HUD that keeps its shape and sits at the screen edges, and a HUD scale (re/hud_notes.md).
 *
 * 3D (bf1942patch.horPlus). The projection (0x5b7fb0) takes a vertical FOV (RenderView+0x24) and the aspect
 * height/width (+0x30), so it is Hor+ as long as the aspect is the real one:
 *   - setAspect (0x5b7d10) of the retail exe replaces any aspect but 0.75 with 0.75 (the stretched 4:3 picture);
 *     the jump at 0x5b7d22 becomes unconditional (some community builds already have it);
 *   - views that are never given an aspect start with 0.75 (0x5b8165 mov [esi+0x30],0.75): that becomes the
 *     backbuffer's H/W;
 *   - the camera frustum (0x619e90, from 0x5b7e85) takes (fov/2)/aspect as the half-angle of its side planes, which
 *     goes past 90 degrees beyond about 3:1: it is given the aspect for which that is exactly atan(tan(fov/2)/aspect);
 *   - two grid gathers (0x682ed0, 0x6557a0; terrain / vegetation patches) take aspect*fov as the horizontal FOV,
 *     narrower than the vertical one, so wide screens lose patches at their edges (pop-in): they get the real
 *     horizontal FOV.
 *
 * 2D (bf1942patch.widescreenHud). Menus and the HUD are dice::meme trees (menu.rfa) laid out in an 800x600 space centred
 * on 0,0, drawn through an orthographic projection of exactly 800x600 set at the start of every 2D pass (0x6021f0):
 * on a wide screen everything is stretched sideways. That function is replaced: the projection covers W' x H'
 * virtual units, H' = 600 / scale and W' = H' * width / height (or the other way round on screens narrower than
 * 4:3), so everything keeps its shape, centred. The mouse pointer and hit tests are in virtual units already.
 * Pictures of the menus and the loading screen that reach the edge of the 800x600 frame get black bars out to the
 * screen edge in the quad batcher (0x665320), with their alpha, so the level does not show beside the loading
 * picture and the bars fade out with it. The main menu's 16:9 background movie is scaled to fit the screen
 * (letterboxed or pillarboxed, like CSS background-size: contain) with black bars around it. In game the same goes for the full-screen overlays (scopes, binoculars,
 * periscope, sub cockpit, artillery view, camera fade).
 *
 * Anchoring (bf1942patch.anchorHud). The layout has no anchors: every box is an offset from its parent's top-left
 * corner. While playing (BfMenu+0xfc == 1, no mouse pointer: BfMenu+0x148 == NULL) TransformNode / BfTransformNode
 * draw and event passes (0x7e92b0 / 0x7dab40, 0x7e93d0 / 0x7dac60) are detoured: the first box below the full
 * screen containers is classified by where it sits (across the middle: centre unless much nearer one edge; else
 * starts in the left third and nearer the left edge: left; ends in the right third and nearer the right edge:
 * right; else centre; the same for top / bottom; a few boxes the rule gets wrong are listed), moved by the extra
 * width / height to that edge, and its whole subtree is drawn with it. Menus, the spawn screen and anything with
 * a mouse pointer stay centred (and at scale 1), so clicks on the map keep working. bf1942patch.logHud 1 logs each
 * box and where it went.
 *
 * HUD scale: menu.c (Options -> General, bf1942patch.hudScale). */
#include <math.h>
#include <string.h>
#include "bf1942patch.h"
#include "d3d.h"

static int g_horplus, g_fixhud, g_anchor, g_debug;
static float g_aspect = 0.75f;        /* backbuffer height / width */
static UINT g_width = 800, g_height = 600;

/* this frame's 2D space */
static float g_vw = 800, g_vh = 600;  /* W', H' */
static float g_dx, g_dy;              /* anchor shifts */
static int g_active;                  /* anchoring on for this frame */
static int g_locked;                  /* inside an anchored subtree */

/* ---------------------------------------------------------------- 3D */

#define SET_ASPECT_JUMP 0x5b7d22u
#define VIEW_ASPECT_IMM 0x5b8168u     /* in c7 46 30 00 00 40 3f at 0x5b8165 */
#define FRUSTUM_CALL    0x5b7e85u
#define FRUSTUM_FN      0x619e90u
#define GATHER_1        0x682eecu     /* ff 52 3c d8 4c 24 14: call [edx+0x3c] (getAspect); fmul [esp+0x14] (fov) */
#define GATHER_2        0x655818u     /* ff 50 3c d8 4c 24 10 */

/* the aspect a for which the frustum's angle-linear (fov/2)/a is the exact side half-angle */
float __cdecl bf_frustum_aspect(float fov, float aspect)
{
    float h = fov * 0.5f, side;
    if (aspect <= 0 || h <= 0) {
        return aspect;
    }
    side = atanf(tanf(h) / aspect);
    return side > 0 ? h / side : aspect;
}

uint32_t bf_frustum_fn = FRUSTUM_FN;  /* where the stub goes on */
extern void bf_frustum_stub(void);
__asm__(".text\n"
        ".globl _bf_frustum_stub\n"
        "_bf_frustum_stub:\n"            /* [esp+4] fov, [esp+8] aspect, [esp+0xc] near, [esp+0x10] far; ecx frustum */
        "    pushl %ecx\n"
        "    pushl 12(%esp)\n"
        "    pushl 12(%esp)\n"
        "    call _bf_frustum_aspect\n"
        "    addl $8, %esp\n"
        "    fstps 12(%esp)\n"
        "    popl %ecx\n"
        "    jmp *_bf_frustum_fn\n");

/* thiscall on the RenderView: the full horizontal FOV, which the gathers then halve and take the tangent of */
static float __attribute__((thiscall)) gather_hfov(void *view)
{
    void **vt = *(void ***)view;
    float fov = ((float (__attribute__((thiscall)) *)(void *))vt[0x18 / 4])(view);
    float aspect = ((float (__attribute__((thiscall)) *)(void *))vt[0x3c / 4])(view);
    if (aspect <= 0) {
        return fov;
    }
    return 2.0f * atanf(tanf(fov * 0.5f) / aspect);
}

static void patch_gather(uint32_t site, const BYTE *expect)
{
    if (!bf_replace_call(site, expect, 7, (void *)gather_hfov)) {
        bf_log("widescreen: the patch gather at %08x differs, left alone", site);
    }
}

static void install_horplus(void)
{
    static const BYTE jnp = 0x7b, jmp = 0xeb, view_init[7] = {0xc7, 0x46, 0x30, 0x00, 0x00, 0x40, 0x3f};
    static const BYTE g1[7] = {0xff, 0x52, 0x3c, 0xd8, 0x4c, 0x24, 0x14}, g2[7] = {0xff, 0x50, 0x3c, 0xd8, 0x4c, 0x24, 0x10};
    BYTE b = *(BYTE *)BF_ADDR(SET_ASPECT_JUMP);
    if (b == jnp) {
        bf_write(SET_ASPECT_JUMP, &jmp, 1);
        bf_log("widescreen: the 3-D view uses the screen's own shape (was forced to 4:3)");
    } else if (b != jmp) {
        bf_log("widescreen: setAspect differs (%02x at %08x), left alone", b, SET_ASPECT_JUMP);
    }
    if (!bf_expect(VIEW_ASPECT_IMM - 3, view_init, sizeof view_init)) {
        bf_log("widescreen: the view constructor differs, new views keep their 4:3 start");
    }
    bf_redirect_call(FRUSTUM_CALL, FRUSTUM_FN, bf_frustum_stub);
    patch_gather(GATHER_1, g1);
    patch_gather(GATHER_2, g2);
}

/* ---------------------------------------------------------------- the backbuffer */

static void on_device(void *device, const bf_present_params *pp, int reset)
{
    (void)device;
    (void)reset;
    if (!pp->BackBufferWidth || !pp->BackBufferHeight) {
        return;
    }
    g_width = pp->BackBufferWidth;
    g_height = pp->BackBufferHeight;
    g_aspect = (float)g_height / (float)g_width;
    if (g_horplus) {
        static const BYTE view_init[3] = {0xc7, 0x46, 0x30};
        if (bf_expect(VIEW_ASPECT_IMM - 3, view_init, 3)) {
            bf_write(VIEW_ASPECT_IMM, &g_aspect, 4);
        }
    }
    bf_log("widescreen: %ux%u, 2-D space %.0fx%.0f at HUD scale 100%%", g_width, g_height,
           g_width * 3 >= g_height * 4 ? 600.0f * g_width / g_height : 800.0f,
           g_width * 3 >= g_height * 4 ? 600.0f : 800.0f * g_height / g_width);
}

/* ---------------------------------------------------------------- 2D projection */

#define ORTHO_FN    0x6021f0u         /* thiscall(Ref2System): the 2-D pass's projection */
#define SET_TRANSFORM_PROJ 0x45fdc0u  /* fastcall(ecx = matrix) */
#define SYS_VIEW    0x4a0             /* Ref2System: its RenderView */
#define RV_SET_NEAR       0x28        /* RenderView vtable slots */
#define RV_SET_FAR        0x30
#define RV_SET_MODE       0x40        /* 1 = orthographic */
#define RV_SET_ORTHO_W    0x48
#define RV_SET_ORTHO_H    0x50
#define RV_GET_PROJECTION 0x64

#define BFMENU      0x973ab8u         /* the BfMenu */
#define BFMENU_MODE    0xfc           /* 0 main menu, 1 in game, 3 loading */
#define BFMENU_POINTER 0x148          /* the mouse pointer, NULL while hidden */
#define MODE_INGAME 1

typedef void (__attribute__((thiscall)) *set_int_fn)(void *, int);
typedef void (__attribute__((thiscall)) *set_float_fn)(void *, float);
typedef void *(__attribute__((thiscall)) *get_ptr_fn)(void *);

static BYTE *bfmenu(void)
{
    return *(BYTE **)BF_ADDR(BFMENU);
}

static int ingame(const BYTE *menu)
{
    return *(const int *)(menu + BFMENU_MODE) == MODE_INGAME;
}

/* playing, with no mouse pointer on the screen */
static int playing(void)
{
    BYTE *menu = bfmenu();
    return menu && ingame(menu) && !*(void **)(menu + BFMENU_POINTER);
}

static void __attribute__((thiscall)) hk_ortho(BYTE *sys)
{
    void *rv = *(void **)(sys + SYS_VIEW);
    void **vt = *(void ***)rv;
    float s = 1.0f;
    int play = playing();
    g_active = g_anchor && play;
    if (play) {
        s = bf_hud_scale() / 100.0f;
    }
    if (g_width * 3 >= g_height * 4) {
        g_vh = 600.0f;
        g_vw = 600.0f * g_width / g_height;
    } else {
        g_vw = 800.0f;
        g_vh = 800.0f * g_height / g_width;
    }
    g_vw /= s;
    g_vh /= s;
    g_dx = (g_vw - 800.0f) / 2;
    g_dy = (g_vh - 600.0f) / 2;
    ((set_int_fn)vt[RV_SET_MODE / 4])(rv, 1);
    vt = *(void ***)rv;
    ((set_float_fn)vt[RV_SET_NEAR / 4])(rv, 0.01f);
    ((set_float_fn)vt[RV_SET_FAR / 4])(rv, 1.0f);
    ((set_float_fn)vt[RV_SET_ORTHO_W / 4])(rv, g_vw);
    ((set_float_fn)vt[RV_SET_ORTHO_H / 4])(rv, g_vh);
    ((void (__attribute__((fastcall)) *)(void *))BF_ADDR(SET_TRANSFORM_PROJ))(((get_ptr_fn)vt[RV_GET_PROJECTION / 4])(rv));
}

/* ---------------------------------------------------------------- anchoring */

#define TNODE_DRAW   0x7e92b0u        /* TransformNode: box +0x0c..+0x18, child +0x08 */
#define TNODE_EVENT  0x7e93d0u
#define TNODE_BOX    0x0c
#define BFTNODE_DRAW 0x7dab40u        /* BfTransformNode: box +0x28..+0x34, child +0x24 */
#define BFTNODE_EVENT 0x7dac60u
#define BFTNODE_BOX  0x28
#define NODE_NEXT    0x04             /* the next sibling */
#define NODE_DRAW_SLOT  0x54          /* node vtable: draw, event */
#define NODE_EVENT_SLOT 0x58

/* the edge a span lo..hi of 0..size belongs to: -1 the low one, 1 the high one, 0 the middle. A span across the
 * middle stays there unless it is much nearer one edge (chat, 30..640); else it goes with the nearer edge if it
 * starts (ends) in the outer third */
static int edge(float lo, float hi, float size)
{
    float a = lo, c = size - hi;
    if (lo < size / 2 && hi > size / 2) {
        return a < c * 2 / 3 ? -1 : c < a * 2 / 3 ? 1 : 0;
    }
    return (lo < size / 3 && a <= c) ? -1 : (hi > size * 2 / 3 && c < a) ? 1 : 0;
}

/* menu/InGame boxes (0..800 / 0..600) that the rule above gets wrong */
static const struct { float l, t, w, h; int ax, ay; } g_fixed[] = {
    {210, 525, 512, 64, 0, 1},       /* the weapon bar: a 512-wide box, but its six slots fill 210..594 */
    {620, 207, 256, 16, 1, -1},      /* the control point bar: right under the minimap (30..205), goes with it */
};

/* the shift for a box (virtual units, centred space); 0 = full width or height container: decide further down */
static int classify(float x, float y, float w, float h, float *sx, float *sy)
{
    float l = x + 400, t = y + 300, r = l + w, b = t + h;
    int ax, ay;
    size_t i;
    if (w >= 790 || h >= 590) {
        return 0;
    }
    ax = edge(l, r, 800);
    ay = edge(t, b, 600);
    for (i = 0; i < sizeof g_fixed / sizeof g_fixed[0]; i++) {
        if (fabsf(l - g_fixed[i].l) < 0.5f && fabsf(t - g_fixed[i].t) < 0.5f && fabsf(w - g_fixed[i].w) < 0.5f
            && fabsf(h - g_fixed[i].h) < 0.5f) {
            ax = g_fixed[i].ax;
            ay = g_fixed[i].ay;
        }
    }
    if (g_debug) {
        static float seen[512][4];
        static int n;
        float key[4] = {l, t, w, h};
        int j;
        for (j = 0; j < n && memcmp(seen[j], key, sizeof key); j++) {
        }
        if (j == n && n < 512) {
            memcpy(seen[n++], key, sizeof key);
            bf_log("hud: box %.1f,%.1f %.1fx%.1f -> %c%c", l, t, w, h, "LCR"[ax + 1], "TMB"[ay + 1]);
        }
    }
    *sx = ax * g_dx;
    *sy = ay * g_dy;
    return 1;
}

typedef void (__attribute__((thiscall)) *draw_fn)(BYTE *node, void *sys, void *ctx, float *r, float *clip, void *eff);
typedef int (__attribute__((thiscall)) *event_fn)(BYTE *node, void *sys, void *ctx, float *r, void *ev);

/* the original functions (bf_detour trampolines) */
static draw_fn g_tnode_draw, g_bftnode_draw;
static event_fn g_tnode_event, g_bftnode_event;

static void anchored_draw(draw_fn orig, int box, BYTE *n, void *sys, void *ctx, float *r, float *clip, void *eff)
{
    float *b = (float *)(n + box), sx, sy, r2[5], c2[5];
    BYTE *next;
    if (!g_active || g_locked || !r || !classify(r[0] + b[0], r[1] + b[1], b[2], b[3], &sx, &sy)) {
        orig(n, sys, ctx, r, clip, eff);
        return;
    }
    memcpy(r2, r, sizeof r2);
    r2[0] += sx;
    r2[1] += sy;
    if (clip) {
        memcpy(c2, clip, sizeof c2);
        c2[0] += sx;
        c2[1] += sy;
    }
    /* the original also draws the rest of the sibling chain with r: detach it, it is drawn (and anchored) on its own */
    next = *(BYTE **)(n + NODE_NEXT);
    *(BYTE **)(n + NODE_NEXT) = NULL;
    g_locked++;
    orig(n, sys, ctx, r2, clip ? c2 : NULL, eff);
    g_locked--;
    *(BYTE **)(n + NODE_NEXT) = next;
    if (next) {
        ((draw_fn)(*(void ***)next)[NODE_DRAW_SLOT / 4])(next, sys, ctx, r, clip, eff);
    }
}

static int anchored_event(event_fn orig, int box, BYTE *n, void *sys, void *ctx, float *r, void *ev)
{
    float *b = (float *)(n + box), sx, sy, r2[5];
    BYTE *next;
    int ret;
    if (!g_active || g_locked || !r || !classify(r[0] + b[0], r[1] + b[1], b[2], b[3], &sx, &sy)) {
        return orig(n, sys, ctx, r, ev);
    }
    memcpy(r2, r, sizeof r2);
    r2[0] += sx;
    r2[1] += sy;
    next = *(BYTE **)(n + NODE_NEXT);
    *(BYTE **)(n + NODE_NEXT) = NULL;
    g_locked++;
    ret = orig(n, sys, ctx, r2, ev);
    g_locked--;
    *(BYTE **)(n + NODE_NEXT) = next;
    if (next) {
        ret = ((event_fn)(*(void ***)next)[NODE_EVENT_SLOT / 4])(next, sys, ctx, r, ev);
    }
    return ret;
}

static void __attribute__((thiscall)) hk_tnode_draw(BYTE *n, void *sys, void *ctx, float *r, float *clip, void *eff)
{
    anchored_draw(g_tnode_draw, TNODE_BOX, n, sys, ctx, r, clip, eff);
}

static void __attribute__((thiscall)) hk_bftnode_draw(BYTE *n, void *sys, void *ctx, float *r, float *clip, void *eff)
{
    anchored_draw(g_bftnode_draw, BFTNODE_BOX, n, sys, ctx, r, clip, eff);
}

static int __attribute__((thiscall)) hk_tnode_event(BYTE *n, void *sys, void *ctx, float *r, void *ev)
{
    return anchored_event(g_tnode_event, TNODE_BOX, n, sys, ctx, r, ev);
}

static int __attribute__((thiscall)) hk_bftnode_event(BYTE *n, void *sys, void *ctx, float *r, void *ev)
{
    return anchored_event(g_bftnode_event, BFTNODE_BOX, n, sys, ctx, r, ev);
}

/* BfTextNode's paint (0x7d4fc0, vtable slot 0x68) lays its glyph quads out once into a cache at +0x18 and then only
 * redraws that cache until the text changes (+0x70 = rebuild): a text first drawn somewhere else (centred, before
 * the anchoring took it, or the radio labels laid out while hidden) stays there. It is rebuilt when its place moves,
 * the way the paint itself does for a new text: the cache's buffers go back to its pool first (0x666680), or every
 * rebuild takes another one until the pool is empty and endCapture (0x6660c0) crashes on a NULL buffer (seen when
 * the minimap zooms in at spawn and its texts move every frame) */
#define BFTEXT_PAINT_SLOT 0x93d000u
#define BFTEXT_PAINT      0x7d4fc0u
#define TEXT_CACHE_RELEASE 0x666680u  /* thiscall(cache = node + 0x18) */
#define TEXT_CACHE   0x18             /* BfTextNode: the glyph quad cache */
#define TEXT_REBUILD 0x70             /* BfTextNode: set = lay the cache out again */

static draw_fn g_bftext_paint;

static void __attribute__((thiscall)) hk_bftext_paint(BYTE *n, void *sys, void *ctx, float *r, float *clip, void *eff)
{
    static struct { BYTE *node; float x, y; } seen[1024];
    if (r) {
        unsigned h = ((uintptr_t)n >> 3) % 1024, i = h, k;
        for (k = 0; k < 8 && seen[i].node && seen[i].node != n; k++) {
            i = (i + 1) % 1024;
        }
        if (k == 8) {
            i = h;                   /* full: take the first slot; the evicted node just rebuilds once */
        }
        if (seen[i].node != n || seen[i].x != r[0] || seen[i].y != r[1]) {
            seen[i].node = n;
            seen[i].x = r[0];
            seen[i].y = r[1];
            if (!n[TEXT_REBUILD]) {  /* already set: a rebuild is pending and holds no buffers yet */
                ((void (__attribute__((thiscall)) *)(BYTE *))BF_ADDR(TEXT_CACHE_RELEASE))(n + TEXT_CACHE);
                n[TEXT_REBUILD] = 1;
            }
        }
    }
    g_bftext_paint(n, sys, ctx, r, clip, eff);
}

static void install_anchor(void)
{
    static const BYTE draw[8] = {0x83, 0xec, 0x30, 0x53, 0x8b, 0x5c, 0x24, 0x38};    /* sub esp,0x30; push ebx; mov ebx,[esp+0x38] */
    static const BYTE event[8] = {0x83, 0xec, 0x18, 0x53, 0x8b, 0x5c, 0x24, 0x28};   /* sub esp,0x18; push ebx; mov ebx,[esp+0x28] */
    static const BYTE release[8] = {0x51, 0x53, 0x55, 0x8b, 0xe9, 0x8b, 0x45, 0x04};
    /* all four or none: the draw and the event (mouse) passes have to agree */
    if (!bf_expect(TNODE_DRAW, draw, 8) || !bf_expect(BFTNODE_DRAW, draw, 8) || !bf_expect(TNODE_EVENT, event, 8)
        || !bf_expect(BFTNODE_EVENT, event, 8)) {
        bf_log("widescreen: the meme node code differs, no HUD anchoring");
        g_anchor = 0;
        return;
    }
    g_tnode_draw = (draw_fn)bf_detour(TNODE_DRAW, draw, 8, hk_tnode_draw);
    g_bftnode_draw = (draw_fn)bf_detour(BFTNODE_DRAW, draw, 8, hk_bftnode_draw);
    g_tnode_event = (event_fn)bf_detour(TNODE_EVENT, event, 8, hk_tnode_event);
    g_bftnode_event = (event_fn)bf_detour(BFTNODE_EVENT, event, 8, hk_bftnode_event);
    g_bftext_paint = (draw_fn)BF_ADDR(BFTEXT_PAINT);
    if (!bf_expect(TEXT_CACHE_RELEASE, release, sizeof release)
        || !bf_hook_slot(BFTEXT_PAINT_SLOT, BFTEXT_PAINT, hk_bftext_paint)) {
        bf_log("widescreen: BfTextNode's vtable differs, texts may stay where they were first drawn");
    }
}

/* the menus never clear the picture (their background covers the 800x600 middle only): the bars at the sides would
 * show whatever the buffer held. So each new backbuffer starts black (IDirect3DDevice8::Clear, slot 36; the 3-D
 * scene clears itself anyway). Clear only covers the viewport, so the whole backbuffer is made the viewport for it
 * (SetViewport / GetViewport, slots 40 / 41) */
static void clear_after_present(void *device, int after)
{
    typedef HRESULT (WINAPI *clear_fn)(void *, DWORD, const void *, DWORD, DWORD, float, DWORD);
    typedef HRESULT (WINAPI *viewport_fn)(void *, DWORD *);
    void **vt = *(void ***)device;
    DWORD old[6], full[6] = {0, 0, g_width, g_height, 0, 0x3f800000 /* MinZ 0, MaxZ 1 */};
    int saved;
    if (!after) {
        return;
    }
    saved = ((viewport_fn)vt[41])(device, old) >= 0;
    ((viewport_fn)vt[40])(device, full);
    ((clear_fn)vt[36])(device, 0, NULL, 1 /* D3DCLEAR_TARGET */, 0xff000000, 1.0f, 0);
    if (saved) {
        ((viewport_fn)vt[40])(device, old);
    }
}

/* ---------------------------------------------------------------- the 2-D quad batcher */

#define BATCH_QUAD 0x665320u          /* thiscall(batcher 0x9ca188; tex, quad, u0, v0, u1, v1), ret 0x18 */

/* the quad as 0x468260 builds it */
typedef struct {
    float *clip;                     /* x, y, w, h or NULL */
    BYTE blend, pad[3];              /* 1 */
    DWORD src, dst;                  /* 7, 8: D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA */
    DWORD state[2];
    BYTE flag, pad2[3];
    struct { float x, y, z, r, g, b, a; } v[4];
} quad;

typedef void (__attribute__((thiscall)) *batch_fn)(void *b, void *tex, quad *q, float u0, float v0, float u1, float v1);
static batch_fn g_batch;              /* the original (bf_detour trampoline) */

/* a quad x0..x1, y0..y1 (top-left, top-right, bottom-right, bottom-left, as 0x468260 builds them), its corners'
 * alpha from the picture's, drawn untextured right after it: black beside a picture, the quad's own colour beside an
 * untextured fill */
static void black_bar(void *b, const quad *q, int fill, float x0, float y0, float x1, float y1, const int corner[4])
{
    quad bar = *q;
    int i;
    bar.clip = NULL;
    bar.blend = 1;
    bar.src = 7;
    bar.dst = 8;
    for (i = 0; i < 4; i++) {
        bar.v[i].x = i == 0 || i == 3 ? x0 : x1;
        bar.v[i].y = i < 2 ? y0 : y1;
        bar.v[i].r = fill ? q->v[corner[i]].r : 0;
        bar.v[i].g = fill ? q->v[corner[i]].g : 0;
        bar.v[i].b = fill ? q->v[corner[i]].b : 0;
        bar.v[i].a = q->blend ? q->v[corner[i]].a : 1.0f;
    }
    g_batch(b, NULL, &bar, 0, 0, 0, 0);
}

/* the main menu's background (menu/Background: the Bink movie Movies/background, or Menu/Background.tga in its
 * place) is a 16:9 picture in the box 0,85 800x450. It is scaled to fit the screen the way CSS background-size:
 * contain does (centred, as near its own place as fits) and black bars cover the rest of the screen, with its alpha */
#define BG_L (-400.0f)
#define BG_T (-215.0f)
#define BG_W 800.0f
#define BG_H 450.0f

static int is_background(float l, float t, float r, float bt)
{
    return fabsf(l - BG_L) < 0.5f && fabsf(t - BG_T) < 0.5f && fabsf(r - (BG_L + BG_W)) < 0.5f
        && fabsf(bt - (BG_T + BG_H)) < 0.5f;
}

static void draw_background(void *b, BYTE *tex, const quad *q, float u0, float v0, float u1, float v1)
{
    static const int left[4] = {0, 0, 3, 3}, right[4] = {1, 1, 2, 2}, top[4] = {0, 1, 1, 0}, bottom[4] = {3, 2, 2, 3};
    float hw = g_vw / 2, hh = g_vh / 2, s = fminf(g_vw / BG_W, g_vh / BG_H);
    float ocx = BG_L + BG_W / 2, ocy = BG_T + BG_H / 2, cy, x0, x1, y0, y1, room;
    quad big = *q;
    int i;
    room = hh - BG_H * s / 2;
    cy = fmaxf(-room, fminf(room, ocy));
    for (i = 0; i < 4; i++) {
        big.v[i].x = (q->v[i].x - ocx) * s;
        big.v[i].y = cy + (q->v[i].y - ocy) * s;
    }
    big.clip = NULL;
    g_batch(b, tex, &big, u0, v0, u1, v1);
    x0 = -BG_W * s / 2, x1 = -x0, y0 = cy - BG_H * s / 2, y1 = cy + BG_H * s / 2;
    /* the corners' alpha: by screen side, whichever way round the picture is */
    if (x0 > -hw + 0.01f) {
        black_bar(b, q, 0, -hw, -hh, x0, hh, q->v[0].x <= q->v[1].x ? left : right);
    }
    if (x1 < hw - 0.01f) {
        black_bar(b, q, 0, x1, -hh, hw, hh, q->v[0].x <= q->v[1].x ? right : left);
    }
    if (y0 > -hh + 0.01f) {
        black_bar(b, q, 0, x0, -hh, x1, y0, q->v[0].y <= q->v[3].y ? top : bottom);
    }
    if (y1 < hh - 0.01f) {
        black_bar(b, q, 0, x0, y1, x1, hh, q->v[0].y <= q->v[3].y ? bottom : top);
    }
}

/* The menus and the loading screen (BfMenu mode != 1) are 800x600 pictures in the middle of the screen. Once a
 * level is loaded the 3-D view shows beside the loading picture: each picture that spans the 800x600 frame from
 * side to side (top to bottom) gets a black bar from there to the edge of the screen, with the picture's own alpha,
 * so the bars fade out with it. (The menus only ever show black there anyway.) The main menu's background movie is scaled to fit the
 * screen instead (draw_background), and a fill of the whole frame (its black backdrop) goes out to the screen edges.
 * In game the full-screen overlays of menu/InGame (sniper.tga, binocular.tga, Submarine/periscope.tga,
 * Submarine/cockpit_512x512.tga, the artillery view, the camera fade) are 825x625 boxes that keep their 4:3 shape:
 * they get the same bars, and untextured fills are carried out to the edge in their own colour. Boxes moved to an
 * edge by the anchoring (g_locked) are left alone: the minimap ends a few units short of the right edge. */
static void __attribute__((thiscall)) hk_batch(void *b, BYTE *tex, quad *q, float u0, float v0, float u1, float v1)
{
    static const int left[4] = {0, 0, 3, 3}, right[4] = {1, 1, 2, 2}, top[4] = {0, 1, 1, 0}, bottom[4] = {3, 2, 2, 3};
    BYTE *menu = bfmenu();
    const int *cl = left, *cr = right, *ct = top, *cb = bottom;
    float l, r, t, bt, hw = g_vw / 2, hh = g_vh / 2;
    int in_game, sides, ends;
    if (!q || !menu || (g_vw <= 800.5f && g_vh <= 600.5f)) {
        g_batch(b, tex, q, u0, v0, u1, v1);
        return;
    }
    in_game = ingame(menu);
    /* TL, TR, BR, BL; a mirrored picture has them the other way round */
    l = q->v[0].x, r = q->v[1].x, t = q->v[0].y, bt = q->v[3].y;
    if (q->v[3].x != l || q->v[2].x != r || q->v[1].y != t || q->v[2].y != bt) {
        g_batch(b, tex, q, u0, v0, u1, v1);
        return;
    }
    if (!in_game && tex && is_background(fminf(l, r), fminf(t, bt), fmaxf(l, r), fmaxf(t, bt))) {
        draw_background(b, tex, q, u0, v0, u1, v1);
        return;
    }
    g_batch(b, tex, q, u0, v0, u1, v1);
    if (!in_game && !tex && !q->clip && fminf(l, r) <= -399.5f && fmaxf(l, r) >= 399.5f && fminf(t, bt) <= -299.5f
        && fmaxf(t, bt) >= 299.5f) {
        /* a fill of the whole 800x600 frame (the main menu's black backdrop, fades): out to the screen edges in its
         * own colour, as in game */
    } else if (in_game ? g_locked != 0 : !tex) {
        return;
    }
    if (l > r) {
        float x = l;
        l = r, r = x, cl = right, cr = left;
    }
    if (t > bt) {
        float y = t;
        t = bt, bt = y, ct = bottom, cb = top;
    }
    if (q->clip) {
        l = fmaxf(l, q->clip[0]);
        r = fminf(r, q->clip[0] + q->clip[2]);
        t = fmaxf(t, q->clip[1]);
        bt = fminf(bt, q->clip[1] + q->clip[3]);
    }
    if (r - l < 100 || bt - t < 100) {
        return;
    }
    /* a menu picture only gets bars if it spans the whole frame that way: the option panels are 512x512 textures,
     * mostly transparent, that hang over the frame's right and bottom edges (menu_options_512x512.tga at 300,125)
     * and the bars cannot see the texture's alpha */
    sides = g_vw > 800.5f && (in_game || (l <= -399.5f && r >= 399.5f));
    ends = g_vh > 600.5f && (in_game || (t <= -299.5f && bt >= 299.5f));
    if (sides) {
        if (l <= -399.5f && l > -hw) {
            black_bar(b, q, !tex, -hw, t, l, bt, cl);
        }
        if (r >= 399.5f && r < hw) {
            black_bar(b, q, !tex, r, t, hw, bt, cr);
        }
    }
    if (ends) {
        /* across the side bars too, or their corners stay open */
        float bl = sides && l <= -399.5f ? -hw : l, br = sides && r >= 399.5f ? hw : r;
        if (t <= -299.5f && t > -hh) {
            black_bar(b, q, !tex, bl, -hh, br, t, ct);
        }
        if (bt >= 299.5f && bt < hh) {
            black_bar(b, q, !tex, bl, bt, br, hh, cb);
        }
    }
}

/* ---------------------------------------------------------------- install */

void bf_widescreen_install(void)
{
    static const BYTE ortho[9] = {0x56, 0x8b, 0xf1, 0x8b, 0x8e, 0xa0, 0x04, 0x00, 0x00};
    static const BYTE batch[6] = {0x81, 0xec, 0x28, 0x01, 0x00, 0x00};   /* sub esp, 0x128 */
    g_horplus = bf_setting_int("horPlus");
    g_fixhud = bf_setting_int("widescreenHud");
    g_anchor = bf_setting_int("anchorHud");
    bf_setting_bind("logHud", &g_debug);
    bf_d3d_install();
    bf_d3d_on_device(on_device);
    if (g_horplus) {
        install_horplus();
    }
    if (!g_fixhud) {
        bf_log("widescreen: widescreenHud 0, the HUD and menus are stretched to the screen as before");
        return;
    }
    if (!bf_expect(ORTHO_FN, ortho, sizeof ortho)) {
        bf_log("widescreen: the 2-D projection differs, the HUD stays as it is");
        return;
    }
    bf_jmp(ORTHO_FN, (void *)hk_ortho);
    bf_d3d_on_present(clear_after_present);
    if (!(g_batch = (batch_fn)bf_detour(BATCH_QUAD, batch, sizeof batch, hk_batch))) {
        bf_log("widescreen: the 2-D quad batcher differs, no black bars beside the loading screen");
    }
    if (g_anchor) {
        install_anchor();
    }
    bf_log("widescreen: Hor+ %s, HUD at its own shape%s", g_horplus ? "on" : "off", g_anchor ? ", anchored to the edges" : "");
}
