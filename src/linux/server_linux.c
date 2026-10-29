/* libbf1942patch.so: voice for the Battlefield 1942 Linux dedicated server (bf1942_lnxded 1.61, static or dynamic),
 * preloaded into it (start-with-bf1942patch.sh). Like the Windows server build (voice_server_win.c) it runs
 * voice_server.c: the voice server of the LANCommander Interposer's voice API (libLANCommander.Interposer.so, x86,
 * next to this one or already preloaded), the game's players, teams and positions mirrored into it, and the voice
 * port in the game's query replies. There is no Linux game client.
 *
 * (integrations/bf1942 of LANCommander.Comms, docs/integrations/bf1942.md) Both binaries are i386 ELF built with GCC
 * 3.x, loaded through ld.so ("static" only links libstdc++ in), and not stripped:
 * - The frame: Setup::start runs `while (setup->mainLoop())`, a rel32 call (E8 74 09 00 00 in both) that is pointed
 *   at main_loop: the voice server runs after each frame, and stops when mainLoop says the game quits.
 * - dice::ref2::world::playerManager (IPlayerManager *), vtable +0x2c: std::list<IPlayer *> *, whose first word is
 *   the head node (MSVC: +4). The Itanium ABI gives the interfaces two destructor slots, so IPlayer and IObject
 *   methods sit 4 bytes later than on Windows; `this` is the first stack argument; a std::string is one pointer to
 *   the characters. IPlayer: +0x1c name, +0x3c the soldier or vehicle, +0x48 id, +0x5c is AI; BFPlayer +0x79 alive,
 *   +0x7c team. IObject +0x38: getAbsolutePosition.
 * - The query replies go out through glibc's sendto, which this library interposes (it is the only symbol it
 *   exports).
 * - Engine state can go away under a read (between rounds, a player leaving): reads run under a SIGSEGV / SIGBUS
 *   handler installed for just that long, which jumps back out (SEH's counterpart).
 * The game's 2004 GCC does not keep the 16-byte stack alignment today's GCC assumes, so what the game calls into
 * (main_loop, sendto) realigns the stack.
 *
 * Options: <console name> <value> lines (voiceChat.port 27800) in bf1942patch.con next to this library (or the file
 * BF1942PATCH_CONFIG names), each overridden by an environment variable BF1942PATCH_<OPTION>, by the name settings.c
 * gives it (BF1942PATCH_VOICEPORT=27800). The log is bf1942patch.log next to the server; game.logToStderr 1 copies it
 * to the console. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../voice_server.h"

#ifndef BF_VERSION
#define BF_VERSION "dev"             /* build-linux.sh passes the release tag */
#endif
#define GAME_ENTRY __attribute__((force_align_arg_pointer))
#define EXPORT __attribute__((visibility("default")))

typedef struct {
    const char *name;
    uintptr_t frame_call;            /* the call of Setup::mainLoop */
    uintptr_t player_manager;        /* the IPlayerManager * global */
    uintptr_t player_manager_ref;    /* an instruction that reads it */
    unsigned char ref[5];
} game_version;

static const unsigned char MAIN_LOOP_CALL[19] = {0x83, 0xec, 0x0c, 0x8b, 0x45, 0x08, 0x50, 0xe8, 0x74, 0x09, 0x00, 0x00,
                                                 0x83, 0xc4, 0x10, 0x84, 0xc0, 0x75, 0xed};
static const game_version VERSIONS[] = {
    {"bf1942_lnxded.static 1.61", 0x080bb717, 0x0871dc2c, 0x08132b7d, {0xa1, 0x2c, 0xdc, 0x71, 0x08}},
    {"bf1942_lnxded.dynamic 1.61", 0x080c2787, 0x086cb72c, 0x08139bed, {0xa1, 0x2c, 0xb7, 0x6c, 0x08}},
};
#define CALL_IN_SIGNATURE 7          /* the call's offset in MAIN_LOOP_CALL */

#define PM_PLAYERS      0x2c
#define PLAYER_NAME     0x1c
#define PLAYER_VEHICLE  0x3c
#define PLAYER_ID       0x48
#define PLAYER_IS_AI    0x5c
#define PLAYER_ALIVE    0x79
#define PLAYER_TEAM     0x7c
#define OBJECT_POSITION 0x38
#define MAX_WALK        512

static const game_version *g_version;
static int g_running;
static int g_replies;                /* the query replies advertise a FastDL server (without voice too) */

/* ---------------------------------------------------------------- the log and the settings */

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log;
static int g_log_stderr;


void bf_log(const char *fmt, ...)
{
    char line[1024];
    struct timespec now;
    struct tm t;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &t);
    pthread_mutex_lock(&g_log_lock);
    if (g_log) {
        fprintf(g_log, "%02d:%02d:%02d.%03ld [%d] %s\n", t.tm_hour, t.tm_min, t.tm_sec, now.tv_nsec / 1000000,
                (int)getpid(), line);
        fflush(g_log);
    }
    if (g_log_stderr) {
        fprintf(stderr, "[bf1942patch] %s\n", line);
    }
    pthread_mutex_unlock(&g_log_lock);
}

/* the options: bf1942patch.con (or, before the options became console variables, a bf1942patch.ini: read when there
 * is no .con, or when BF1942PATCH_CONFIG names one), then an environment variable BF1942PATCH_<OPTION> for each */
static void load_settings(const char *path)
{
    char old[4096], env[64], *dot;
    const char *value;
    size_t len = strlen(path);
    int n, i, k;
    if (len > 4 && !strcasecmp(path + len - 4, ".ini")) {
        n = bf_settings_load_ini(path, 0);
    } else if ((n = bf_settings_load_con(path, 0)) < 0) {
        snprintf(old, sizeof old, "%s", path);
        if ((dot = strrchr(old, '.')) != NULL && !strchr(dot, '/')) {
            strcpy(dot, ".ini");
            if ((n = bf_settings_load_ini(old, 0)) >= 0) {
                path = old;
            }
        }
    }
    if (n >= 0) {
        bf_log("settings: %d options from %s", n, path);
    } else {
        bf_log("settings: no %s, the defaults", path);
    }
    for (i = 0; i < bf_n_options; i++) {
        snprintf(env, sizeof env, "BF1942PATCH_%s", bf_options[i].name);
        for (k = 12; env[k]; k++) {
            env[k] = (char)toupper((unsigned char)env[k]);
        }
        if ((value = getenv(env)) == NULL) {
            continue;
        }
        if (bf_option_set(i, value) < 0) {
            bf_log("settings: %s=%s is not a number, skipped", env, value);
        } else {
            bf_log("settings: %s from %s", bf_options[i].console, env);
        }
    }
}

uint64_t bf_server_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------- the voice library */

static void *g_lib;

static const char *self_dir(char *out, size_t size)
{
    Dl_info info;
    const char *slash;
    out[0] = '\0';
    if (dladdr((void *)self_dir, &info) && info.dli_fname && (slash = strrchr(info.dli_fname, '/')) != NULL) {
        snprintf(out, size, "%.*s", (int)(slash - info.dli_fname + 1), info.dli_fname);
    }
    return out;
}

/* a preloaded Interposer, else voiceChat.library, else the one next to this library */
static void *library(void)
{
    static int tried;
    char path[4096], dir[4096];
    if (tried) {
        return g_lib;
    }
    tried = 1;
    if ((g_lib = dlopen("libLANCommander.Interposer.so", RTLD_NOW | RTLD_NOLOAD)) != NULL) {
        bf_log("voice server: the preloaded Interposer");
        return g_lib;
    }
    snprintf(path, sizeof path, "%s", bf_setting_str("voiceLibrary"));
    if (!path[0]) {
        snprintf(path, sizeof path, "%slibLANCommander.Interposer.so", self_dir(dir, sizeof dir));
    }
    if (!(g_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL))) {
        bf_log("voice server: cannot load %s (%s), no voice", path, dlerror());
        return NULL;
    }
    bf_log("voice server: %s", path);
    return g_lib;
}

void *bf_server_symbol(const char *name)
{
    void *lib = library();
    return lib ? dlsym(lib, name) : NULL;
}

/* ---------------------------------------------------------------- reading the game, guarded */

static __thread sigjmp_buf *t_guard;
static struct sigaction g_old_segv, g_old_bus;

static void forward(int sig, siginfo_t *info, void *context)
{
    const struct sigaction *old = sig == SIGSEGV ? &g_old_segv : &g_old_bus;
    if (old->sa_flags & SA_SIGINFO) {
        old->sa_sigaction(sig, info, context);
    } else if (old->sa_handler != SIG_IGN && old->sa_handler != SIG_DFL) {
        old->sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);        /* the fault runs again and ends the process as it would have */
    }
}

static void on_fault(int sig, siginfo_t *info, void *context)
{
    if (t_guard) {
        siglongjmp(*t_guard, 1);
    }
    forward(sig, info, context);
}

typedef void (*guarded_fn)(void *arg);

/* fn(arg) with a fault turned into 0 (only plain data in fn: siglongjmp skips the rest of it) */
static int guarded(guarded_fn fn, void *arg)
{
    struct sigaction a;
    sigjmp_buf env;
    volatile int ok = 0;
    memset(&a, 0, sizeof a);
    a.sa_sigaction = on_fault;
    a.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&a.sa_mask);
    sigaction(SIGSEGV, &a, &g_old_segv);
    sigaction(SIGBUS, &a, &g_old_bus);
    if (sigsetjmp(env, 1) == 0) {
        t_guard = &env;
        fn(arg);
        ok = 1;
    }
    t_guard = NULL;
    sigaction(SIGSEGV, &g_old_segv, NULL);
    sigaction(SIGBUS, &g_old_bus, NULL);
    return ok;
}

static void *vcall(void *self, int offset)
{
    return ((void *(*)(void *))(*(void ***)self)[offset / 4])(self);
}

static unsigned char vcall_bool(void *self, int offset)
{
    return ((unsigned char (*)(void *))(*(void ***)self)[offset / 4])(self);
}

static void read_player(void *player, bf_server_player *out)
{
    const unsigned char *b = player;
    const char *const *name;
    void *vehicle;
    const float *at;
    memset(out, 0, sizeof *out);
    out->id = (int)(intptr_t)vcall(player, PLAYER_ID);
    name = vcall(player, PLAYER_NAME);
    if (name && *name) {
        snprintf(out->name, sizeof out->name, "%s", *name);
    }
    memcpy(&out->team, b + PLAYER_TEAM, sizeof out->team);
    out->alive = b[PLAYER_ALIVE] != 0;
    out->ai = vcall_bool(player, PLAYER_IS_AI) != 0;
    if ((vehicle = vcall(player, PLAYER_VEHICLE)) != NULL && (at = vcall(vehicle, OBJECT_POSITION)) != NULL) {
        memcpy(out->position, at, sizeof out->position);
        out->has_position = 1;
    }
}

struct read_job {
    bf_server_player *out;
    int capacity, count;
};

static void read_players(void *arg)
{
    struct read_job *job = arg;
    struct node { struct node *next, *prev; void *value; } *head, *n;
    void *pm = *(void **)g_version->player_manager;
    struct node *const *list;
    int walked = 0;
    job->count = -1;
    if (!pm || !(list = vcall(pm, PM_PLAYERS))) {
        return;
    }
    job->count = 0;
    if (!(head = *list)) {
        return;
    }
    for (n = head->next; n && n != head && walked < MAX_WALK; n = n->next, walked++) {
        if (n->value && job->count < job->capacity) {
            read_player(n->value, &job->out[job->count++]);
        }
    }
}

int bf_server_read_players(bf_server_player *out, int capacity)
{
    struct read_job job = {out, capacity, -1};
    return guarded(read_players, &job) ? job.count : -1;
}

struct match_job {
    const game_version *v;
    int ok;
};

static void match(void *arg)
{
    struct match_job *job = arg;
    job->ok = !memcmp((const void *)(job->v->frame_call - CALL_IN_SIGNATURE), MAIN_LOOP_CALL, sizeof MAIN_LOOP_CALL)
           && !memcmp((const void *)job->v->player_manager_ref, job->v->ref, sizeof job->v->ref);
}

static const game_version *detect(void)
{
    size_t i;
    for (i = 0; i < sizeof VERSIONS / sizeof VERSIONS[0]; i++) {
        struct match_job job = {&VERSIONS[i], 0};
        if (guarded(match, &job) && job.ok) {
            return &VERSIONS[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- hooks */

typedef unsigned char (*main_loop_fn)(void *setup);
static main_loop_fn g_main_loop;

/* Setup::mainLoop runs one frame and returns "keep running" */
static GAME_ENTRY unsigned char main_loop(void *setup)
{
    unsigned char keep = g_main_loop(setup);
    if (g_running) {
        if (keep) {
            bf_server_tick();
        } else {
            g_running = 0;
            bf_server_stop();        /* the game quits: voice stops before exit runs under its threads */
        }
    }
    return keep;
}

/* the rel32 call at site (checked by the version's signature) goes to hook; *original gets its old target */
static int retarget_call(uintptr_t site, void *hook, void **original)
{
    int32_t rel;
    uintptr_t next = site + 5, page = (uintptr_t)sysconf(_SC_PAGESIZE), first = site & ~(page - 1);
    memcpy(&rel, (const void *)(site + 1), 4);
    *original = (void *)(next + rel);
    if (mprotect((void *)first, next - first, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        return 0;
    }
    rel = (int32_t)((uintptr_t)hook - next);
    memcpy((void *)(site + 1), &rel, 4);
    mprotect((void *)first, next - first, PROT_READ | PROT_EXEC);
    return 1;
}

/* the game's own UDP sends: its query replies get \commsport\ and the FastDL server (the voice library sends with
 * sendmsg) */
EXPORT GAME_ENTRY ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to,
                                 socklen_t tolen)
{
    typedef ssize_t (*sendto_fn)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
    static sendto_fn real;
    char reply[2048];
    int n;
    ssize_t sent;
    if (!real) {
        real = (sendto_fn)dlsym(RTLD_NEXT, "sendto");
    }
    if ((g_running || g_replies) && len < sizeof reply && (n = bf_server_query_reply(buf, (int)len, reply, sizeof reply)) > 0) {
        sent = real(fd, reply, (size_t)n, flags, to, tolen);
        return sent < 0 ? sent : (ssize_t)len;
    }
    return real(fd, buf, len, flags, to, tolen);
}

/* ---------------------------------------------------------------- start */

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* this library out of LD_PRELOAD, so what the server starts (64-bit programs especially) does not inherit it */
static void leave_preload(void)
{
    const char *preload = getenv("LD_PRELOAD");
    char kept[4096], entry[1024];
    Dl_info info;
    const char *p, *self;
    size_t n;
    if (!preload || !dladdr((void *)leave_preload, &info) || !info.dli_fname) {
        return;
    }
    self = base_name(info.dli_fname);
    kept[0] = '\0';
    for (p = preload; *p;) {
        n = strcspn(p, ": ");
        snprintf(entry, sizeof entry, "%.*s", (int)n, p);
        if (entry[0] && strcmp(base_name(entry), self) && strlen(kept) + n + 2 < sizeof kept) {
            if (kept[0]) {
                strcat(kept, ":");
            }
            strcat(kept, entry);
        }
        p += n;
        p += *p ? 1 : 0;
    }
    if (kept[0]) {
        setenv("LD_PRELOAD", kept, 1);
    } else {
        unsetenv("LD_PRELOAD");
    }
}

__attribute__((constructor)) static void start(void)
{
    char exe[4096], path[4096], dir[4096];
    const char *config;
    ssize_t n;
    leave_preload();
    /* preloaded into anything started with it: only the server */
    if ((n = readlink("/proc/self/exe", exe, sizeof exe - 1)) <= 0) {
        return;
    }
    exe[n] = '\0';
    if (strncmp(base_name(exe), "bf1942_lnxded", 13)) {
        return;
    }
    snprintf(path, sizeof path, "%.*sbf1942patch.log", (int)(base_name(exe) - exe), exe);
    g_log = fopen(path, "w");
    config = getenv("BF1942PATCH_CONFIG");
    snprintf(path, sizeof path, "%sbf1942patch.con", self_dir(dir, sizeof dir));
    load_settings(config && *config ? config : path);
    g_log_stderr = bf_setting_int("logToStderr");
    g_replies = bf_server_fastdl_init();
    if (!bf_setting_int("voiceChat")) {
        bf_log("voice server: voiceChat 0, no voice");
        return;
    }
    if (!(g_version = detect())) {
        bf_log("voice server: %s is not bf1942_lnxded 1.61 (static or dynamic); no voice", exe);
        return;
    }
    if (!retarget_call(g_version->frame_call, (void *)main_loop, (void **)&g_main_loop)) {
        bf_log("voice server: cannot hook the frame at %08lx, no voice", (unsigned long)g_version->frame_call);
        return;
    }
    g_running = 1;
    bf_log("bf1942patch " BF_VERSION ": %s recognised, voice server installed", g_version->name);
}
