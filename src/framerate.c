/* bf1942patch.unlockFps: no 60 / 100 fps cap, and a HUD that keeps time at any frame rate (re/fps_notes.md).
 *
 * The cap is game.lockFps (app+0x17c, app = [0x971eac]): the main loop step 0x44abc0 busy-waits until 1 / lockFps
 * has passed since the last frame, and the render function 0x466d80 sets lockFps to 60.0 (vsync on) or 100.0 (off)
 * whenever renderer getVSyncEnabled changes, so a console "game.lockFps" does not stay; the menus run at the 100.0
 * the app constructor sets (0x4569cb). unlockFps 1 puts maxFps into all three immediates (not into the dedicated
 * server's 2 x tick rate at 0x455e19), and the wait sleeps instead of spinning (the call to the timer 0x581960 inside the
 * loop goes through frame_wait). lockFps 0 means no limit; in the original it is 1/0 and the loop never ends.
 *
 * The HUD: BFMenu::paint 0x45d7c0 runs once per rendered frame and updates the meme System (every timeout, fade
 * and animation of the HUD and the menus) by app+0x184, which is the 30 Hz tick (1/30 s), not the frame time. So
 * the HUD runs twice as fast as real time at 60 fps, which is what its timeouts were tuned for, and 2.4 times as
 * fast as that at 144 fps (the weapon bar is gone after 0.6 s instead of 1.5 s). realTimeHud 1 gives it the time
 * that really passed, scaled to look as it does at 60 fps.
 *
 * The Video options page gets a MAX FPS row (maxFpsOption 1): menu/VideoMenu is spliced as it is opened
 * (bf_menu_add_row), the row's int is bound by VideoOptions::registerVariables (0x6b02f0), and SaveVideoOptions
 * (0x6b11a0, on Apply / Back) applies it and keeps it in the player's bf1942patch.con. The
 * console's bf1942patch.maxFps applies at once too. */
#include <string.h>
#include "bf1942patch.h"
#include "d3d.h"
#include <mmsystem.h>

#define APP_PTR       0x971eacu
#define APP_LOCKFPS   0x17c
#define APP_TICK      0x184      /* float, 1 / 30 */
#define GAME_NOW      0x581960u  /* double now(void), seconds (QPC) */
#define WAIT_CALL     0x44ac9cu  /* call GAME_NOW inside the busy-wait; the deadline is the loop's [esp+8] */
#define CAP_VSYNC     0x466e37u  /* imm32 60.0 of mov [eax+0x17c], 0x42700000 */
#define CAP_NOVSYNC   0x466e49u  /* imm32 100.0 of mov [ecx+0x17c], 0x42c80000 */
#define CAP_INIT      0x4569d1u  /* imm32 100.0 of mov [esi+0x17c], 0x42c80000: app init, what the menus run at */
#define HUD_DT_SITE   0x45ddd1u  /* mov edx, [edx+0x184]: the dt BFMenu::paint gives System::update */
#define HUD_REF_FPS   60.0       /* the frame rate the HUD timeouts were made for */

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

typedef double (*now_fn)(void);
static HANDLE g_timer;           /* high-resolution waitable timer (Windows 10 1803+), else Sleep */

static BYTE *app(void)
{
    return *(BYTE **)BF_ADDR(APP_PTR);
}

/* ---------------------------------------------------------------- the cap */

static void sleep_for(double seconds)
{
    if (g_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(seconds * 1e7);   /* relative, 100 ns units */
        if (SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(g_timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD)(seconds * 1000.0));
}

/* in place of the timer call inside the wait: sleeps through most of what is left of it, so a capped game does not
 * keep a core busy, then gives the loop the time again. The last moment is still spun by the loop. */
double bf_frame_wait(double *deadline)
{
    now_fn now = (now_fn)BF_ADDR(GAME_NOW);
    double t = now(), left = *deadline - t;
    float cap = *(float *)(app() + APP_LOCKFPS);
    if (!(cap > 0.0f) || left > 0.5) {   /* lockFps 0 (or below 2): no limit, instead of a wait that never ends */
        *deadline = t;
        return t;
    }
    if (left > (g_timer ? 0.001 : 0.002)) {
        sleep_for(left - (g_timer ? 0.0005 : 0.0015));
        t = now();
    }
    return t;
}

__asm__(".text\n"
        ".globl _bf_frame_wait_stub\n"
        "_bf_frame_wait_stub:\n"
        "    leal 12(%esp), %eax\n"   /* the loop's [esp+8] (deadline), past our return address */
        "    pushl %eax\n"
        "    call _bf_frame_wait\n"   /* the time in st(0), as from 0x581960 */
        "    addl $4, %esp\n"
        "    ret\n");
void bf_frame_wait_stub(void);

/* ---------------------------------------------------------------- the HUD's time */

/* the dt for this frame's System::update, as the bits of a float (it goes into edx) */
DWORD bf_hud_dt(BYTE *a)
{
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER t;
    DWORD bits;
    float tick = *(float *)(a + APP_TICK), dt = tick;
    QueryPerformanceCounter(&t);
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
    } else {
        double real = (double)(t.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        if (real >= 0.0 && real <= 0.25) {   /* longer: a load or a stall, which gets one frame's worth as before */
            dt = (float)(real * HUD_REF_FPS * tick);
        }
    }
    last = t;
    memcpy(&bits, &dt, 4);
    return bits;
}

__asm__(".text\n"
        ".globl _bf_hud_dt_stub\n"
        "_bf_hud_dt_stub:\n"          /* edx = app; ecx (the System) must survive */
        "    pushl %ecx\n"
        "    pushl %edx\n"
        "    call _bf_hud_dt\n"
        "    addl $4, %esp\n"
        "    movl %eax, %edx\n"
        "    popl %ecx\n"
        "    ret\n");
void bf_hud_dt_stub(void);

/* ---------------------------------------------------------------- bf1942patch.logFps */

static void count_frame(void *device, int after)
{
    static LARGE_INTEGER freq, start;
    static int frames;
    LARGE_INTEGER t;
    double s;
    (void)device;
    if (!after) {
        return;
    }
    QueryPerformanceCounter(&t);
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        start = t;
    }
    frames++;
    s = (double)(t.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    if (s >= 5.0) {
        bf_log("framerate: %.1f fps", frames / s);
        frames = 0;
        start = t;
    }
}

/* ---------------------------------------------------------------- the limit, and the Video page row */

static int g_max_fps;            /* bf1942patch.maxFps; also the menu variable Options/Video/MaxFps */
static int g_saved_fps;          /* what the player's bf1942patch.con holds */

static int clamp_fps(int fps)
{
    return fps <= 0 ? 0 : fps < 30 ? 30 : fps > 1000 ? 1000 : fps;
}

/* the limit into the three places that set lockFps, and into lockFps itself (0 = no limit) */
static void apply_max_fps(void)
{
    float cap = (float)g_max_fps;
    bf_write(CAP_VSYNC, &cap, 4);
    bf_write(CAP_NOVSYNC, &cap, 4);
    bf_write(CAP_INIT, &cap, 4);
    if (app()) {
        *(float *)(app() + APP_LOCKFPS) = cap;
    }
}

#define VIDEO_VARS_SLOT 0x91dfb8u /* VideoOptions::registerVariables -> 0x6b02f0 (page vtable 0x91dfb0) */
#define VIDEO_VARS      0x6b02f0u
#define VIDEO_SAVE      0x6b11a0u /* SaveVideoOptions, thiscall(page): writes Video.con (Apply, Back) */

static int __attribute__((thiscall)) hk_video_vars(void *page)
{
    int r = ((int (__attribute__((thiscall)) *)(void *))BF_ADDR(VIDEO_VARS))(page);
    bf_menu_bind_int(page, "Options/Video", "MaxFps", &g_max_fps);
    return r;
}

static void (__attribute__((thiscall)) *g_video_save)(void *page);

/* the Video page is saved: the slider's value becomes the limit, kept in the player's bf1942patch.con */
static void __attribute__((thiscall)) hk_video_save(void *page)
{
    g_video_save(page);
    g_max_fps = clamp_fps(g_max_fps);
    apply_max_fps();
    if (g_max_fps != g_saved_fps) {
        g_saved_fps = g_max_fps;
        bf_setting_keep_int("maxFps", g_max_fps);
        bf_log("framerate: MaxFps %d from the Video options", g_max_fps);
    }
}

/* menu/VideoMenu of 1.61 with a MAX FPS row (port of re/menu/splice_video_poc.py): the view distance row's content
 * (number box, suffix, slider, label; 0xb0f..0xe77) on Options/Video/MaxFps, 0..300 and a blank suffix, hung at
 * y=275 on the NULL next of the last row (render with spawn menu) */
static const BYTE VIDEO_INT_HEAD[12] = {0x0c, 0, 0, 0, 0x22, 0, 0x08, 0, 100, 0, 0, 0};   /* IntData of string 34 */
static const BYTE VIDEO_PERCENT[13] = {0x0d, 0, 0, 0, 0, 0, 0x1f, 0, 1, 0, 0, 0, '%'}, VIDEO_BLANK[13] = {0x0d, 0, 0, 0, 0, 0, 0x1f, 0, 1, 0, 0, 0, ' '};
static const int VIDEO_BOX[2] = {50, 100}, VIDEO_NEW_BOX[2] = {0, 300};
static const float VIDEO_SLIDER[3] = {50, 100, 10}, VIDEO_NEW_SLIDER[3] = {0, 300, 10};
static const DWORD VIDEO_ANCESTORS[] = {0x71d, 0x725, 0x72d, 0x735, 0x7ba, 0x7c2, 0x7ca, 0x7ea, 0x7f2, 0x7fa, 0x802,
                                        0x80a, 0x812, 0x81a, 0x822, 0x82a, 0x832, 0x83a};
static const bf_meme_bytes VIDEO_BYTES[] = {
    {0xb77, 13, VIDEO_PERCENT, VIDEO_BLANK},           /* the suffix */
    {0xc10, 12, VIDEO_INT_HEAD, NULL},                 /* IntData Options/Video/Viewdistance: number box, slider */
    {0xd5f, 12, VIDEO_INT_HEAD, NULL},
    {0xc2b, 8, VIDEO_BOX, VIDEO_NEW_BOX},              /* the number box's range */
    {0xd6b, 12, VIDEO_SLIDER, VIDEO_NEW_SLIDER},       /* the slider's min, max, step */
    {0xe3b + 12, 26, "VIDEO_OPTIONS_VIEWDISTANCE", NULL},
};
static const bf_meme_row VIDEO_ROW = {
    .size = 14359, .strings_end = 0x71a,
    .var = "Options/Video/MaxFps", .var_index = 71,
    .content = 0xb0f, .content_len = 0x368,
    .ints = {0xc10, 0xd5f}, .int_default = 0,
    .label = 0xe3b, .label_len = 0x26, .string_class = 0x1f, .label_text = "MAX FPS (0 = NO LIMIT)",
    .label_parents = {0xe19, 0xe09, 0xb0f},            /* BfLocaleStringData, TextNode, the content */
    .rect = {5, 275, 295, 20}, .hang_at = 0x842,
    .ancestors = VIDEO_ANCESTORS, .n_ancestors = sizeof VIDEO_ANCESTORS / sizeof VIDEO_ANCESTORS[0],
    .bytes = VIDEO_BYTES, .n_bytes = sizeof VIDEO_BYTES / sizeof VIDEO_BYTES[0],
};

static void video_option_install(void)
{
    static const BYTE save_head[6] = {0x81, 0xec, 0x2c, 0x02, 0x00, 0x00};   /* sub esp, 0x22c */
    if (*(DWORD *)BF_ADDR(VIDEO_VARS_SLOT) != VIDEO_VARS || !bf_expect(VIDEO_SAVE, save_head, sizeof save_head)
        || !bf_menu_add_row("VideoMenu", &VIDEO_ROW, "the MAX FPS row")) {
        bf_log("framerate: the Video options code differs, no MAX FPS row");
        return;
    }
    bf_hook_slot(VIDEO_VARS_SLOT, VIDEO_VARS, hk_video_vars);
    g_video_save = (void (__attribute__((thiscall)) *)(void *))bf_detour(VIDEO_SAVE, save_head, sizeof save_head, hk_video_save);
}

/* bf1942patch.maxFps <fps> in the console */
static void max_fps_changed(void)
{
    g_max_fps = g_saved_fps = clamp_fps(g_max_fps);
    apply_max_fps();
    bf_log("framerate: at most %d fps (0 = no limit)", g_max_fps);
}

/* ---------------------------------------------------------------- install */

void bf_framerate_install(void)
{
    if (bf_setting_int("unlockFps")) {
        static const BYTE fps60[4] = {0x00, 0x00, 0x70, 0x42}, fps100[4] = {0x00, 0x00, 0xc8, 0x42};
        bf_setting_bind("maxFps", &g_max_fps);
        g_max_fps = g_saved_fps = clamp_fps(g_max_fps);
        if (!bf_expect(CAP_VSYNC, fps60, 4) || !bf_expect(CAP_NOVSYNC, fps100, 4) || !bf_expect(CAP_INIT, fps100, 4)) {
            bf_log("framerate: the 60 / 100 fps caps at %08x / %08x / %08x differ, left alone", CAP_VSYNC, CAP_NOVSYNC,
                   CAP_INIT);
        } else if (bf_redirect_call(WAIT_CALL, GAME_NOW, bf_frame_wait_stub)) {
            apply_max_fps();
            bf_setting_watch("maxFps", max_fps_changed);
            g_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!g_timer) {
                timeBeginPeriod(1);
            }
            if (g_max_fps > 0) {
                bf_log("framerate: at most %d fps (%s)", g_max_fps, g_timer ? "high-resolution timer" : "Sleep");
            } else {
                bf_log("framerate: no limit");
            }
            if (bf_setting_int("maxFpsOption")) {
                video_option_install();
            }
        }
    }
    if (bf_setting_int("realTimeHud")) {
        static const BYTE orig[6] = {0x8b, 0x92, 0x84, 0x01, 0x00, 0x00};
        if (bf_replace_call(HUD_DT_SITE, orig, sizeof orig, bf_hud_dt_stub)) {
            bf_log("framerate: HUD timing follows real time");
        } else {
            bf_log("framerate: HUD timing site %08x differs, left alone", HUD_DT_SITE);
        }
    }
    if (bf_setting_int("logFps")) {
        bf_d3d_install();
        bf_d3d_on_present(count_frame);
    }
}
