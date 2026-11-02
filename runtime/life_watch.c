// TOTK_ANIM_PROBE=lifewatch (research, Windows; read only but for the debug registers): which of the game's code writes
// Link's life (frame_rate.c game_player_life_addr), to find the function that hurts him, so a friend's duel strike can
// go through the game's own reaction (the stagger, the knock-back, the sound) instead of a bare write of the number.
// A hardware data breakpoint (DR0 on the life word, DR7 write of 4 bytes) is set on every thread of the process; a
// vectored handler takes the single-step exception each write raises and logs, for the first writes from each place,
// the lifted function the host code belongs to (main.c rt_lifted_function), the guest's return address and its
// frame-pointer chain (the callers up to the attack's start), and every write that changes the life (a hit, a heal)
// the same way, as the game also writes the same life every frame. Then the execution goes on.
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

static uint64_t g_addr;
static volatile LONG g_hits, g_changes;
static int32_t g_last = -1;   // the life as last seen: a write that changes it is the one worth a chain
enum { AROUND = 0x600 };       // the life's object as last written, AROUND bytes each side: what else a hit changes
static uint8_t g_prev[2 * AROUND], g_now[2 * AROUND];
static int g_have_prev;

static void log_diff(void) {   // the 4-byte words around the life that changed since the frame before
    int n = 0;
    for (int o = 0; o < 2 * AROUND && n < 40; o += 4) {
        uint32_t a, b;
        memcpy(&a, g_prev + o, 4), memcpy(&b, g_now + o, 4);
        if (a == b || o == AROUND) continue;
        float fa, fb;
        memcpy(&fa, &a, 4), memcpy(&fb, &b, 4);
        rt_log("life watch:   life%c%#x: %08x -> %08x (%d -> %d, %g -> %g)", o < AROUND ? '-' : '+', o < AROUND ? AROUND - o : o - AROUND, a, b, (int32_t)a, (int32_t)b, fa, fb);
        n++;
    }
}
static struct { uintptr_t rip; LONG n; } g_seen[16];
// TOTK_LIFEWATCH_READ=<off>[,<off>...] (up to 4, signed offsets from the life): every access, reads included, to those
// words; each place that touches them (host code address) is logged once with its lifted function and guest chain: who
// consumes a hit's record.
static void log_chain(Cpu *c);
static uint64_t g_rd[4];
static int g_nrd;
static struct { uintptr_t rip; int dr; } g_rd_seen[64];
static volatile LONG g_rd_n;

static LONG on_read(EXCEPTION_POINTERS *e, int dr) {
    uintptr_t rip = (uintptr_t)e->ContextRecord->Rip;
    LONG n = g_rd_n;
    for (LONG i = 0; i < n && i < 64; i++)
        if (g_rd_seen[i].rip == rip && g_rd_seen[i].dr == dr) return EXCEPTION_CONTINUE_EXECUTION;
    if (n >= 64) return EXCEPTION_CONTINUE_EXECUTION;
    LONG k = InterlockedIncrement(&g_rd_n) - 1;
    if (k >= 64) return EXCEPTION_CONTINUE_EXECUTION;
    g_rd_seen[k].rip = rip, g_rd_seen[k].dr = dr;
    char d[160];
    uint64_t f = rt_lifted_function(rip);
    int32_t v = 0;
    plat_mem_read(g_rd[dr], &v, 4);
    rt_log("life watch: access %ld to life%+lld (now %#x): host totk+%#llx in %s", k, (long long)(g_rd[dr] - g_addr), v,
           (unsigned long long)(rip - (uintptr_t)GetModuleHandleW(NULL)), f ? ldr_describe(f, d, sizeof d) : "host code");
    log_chain(kern_cpu());
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void log_chain(Cpu *c) {
    char d[160];
    if (!c) {
        rt_log("life watch:   (no guest thread)");
        return;
    }
    rt_log("life watch:   lr %s", ldr_describe(c->x[30], d, sizeof d));
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp && (fp & 7) == 0 && plat_mem_readable(fp, 16); i++) {
        uint64_t next = ((const uint64_t *)(uintptr_t)fp)[0], ret = ((const uint64_t *)(uintptr_t)fp)[1];
        rt_log("life watch:   frame %2d %s", i, ldr_describe(ret, d, sizeof d));
        if (next <= fp) break;
        fp = next;
    }
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (g_nrd) {
        DWORD64 d6 = e->ContextRecord->Dr6;
        e->ContextRecord->Dr6 = 0;
        for (int i = 0; i < g_nrd; i++)
            if (d6 & (1ull << i)) return on_read(e, i);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!(e->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Dr6 = 0;
    LONG hit = InterlockedIncrement(&g_hits);
    uintptr_t rip = (uintptr_t)e->ContextRecord->Rip;
    int k = 0;
    while (k < 16 && g_seen[k].rip && g_seen[k].rip != rip) k++;
    if (k < 16 && !g_seen[k].rip) g_seen[k].rip = rip;
    LONG n = k < 16 ? InterlockedIncrement(&g_seen[k].n) : 0;
    int32_t v = 0;
    plat_mem_read(g_addr, &v, 4);
    int changed = v != g_last;
    g_last = v;
    int snap = g_have_prev >= 0 && plat_mem_read(g_addr - AROUND, g_now, sizeof g_now);
    if ((n <= 2 && hit <= 20) || (changed && InterlockedIncrement(&g_changes) <= 12)) {
        char d[160];
        uint64_t f = rt_lifted_function(rip);
        rt_log("life watch: write %ld (place %d, its %ld): life now %d, host totk+%#llx in %s", hit, k, n, v,
               (unsigned long long)(rip - (uintptr_t)GetModuleHandleW(NULL)), f ? ldr_describe(f, d, sizeof d) : "host code");
        log_chain(kern_cpu());
        if (changed && snap && g_have_prev) log_diff();
    }
    if (snap) memcpy(g_prev, g_now, sizeof g_prev), g_have_prev = 1;
    if (g_have_prev < 0 && changed && InterlockedIncrement(&g_changes) <= 4)   // another word: its chain, then its args
        rt_log("life watch:   x0 %#llx x1 %#llx x2 %#llx x3 %#llx", kern_cpu() ? (unsigned long long)kern_cpu()->x[0] : 0ull,
               kern_cpu() ? (unsigned long long)kern_cpu()->x[1] : 0ull, kern_cpu() ? (unsigned long long)kern_cpu()->x[2] : 0ull,
               kern_cpu() ? (unsigned long long)kern_cpu()->x[3] : 0ull);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void arm_all(uint64_t addr) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {.dwSize = sizeof te};
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int armed = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT ctx = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
            if (GetThreadContext(t, &ctx)) {
                if (g_nrd) {
                    uint64_t dr7 = ctx.Dr7 & ~0xffff00ffull;
                    for (int i = 0; i < g_nrd; i++) dr7 |= (1ull << (2 * i)) | (3ull << (16 + 4 * i)) | (3ull << (18 + 4 * i));
                    ctx.Dr0 = g_rd[0], ctx.Dr1 = g_nrd > 1 ? g_rd[1] : 0, ctx.Dr2 = g_nrd > 2 ? g_rd[2] : 0;
                    ctx.Dr3 = g_nrd > 3 ? g_rd[3] : 0, ctx.Dr7 = dr7;   // Ln, read or write, 4 bytes each
                } else {
                    ctx.Dr0 = addr;
                    ctx.Dr7 = (ctx.Dr7 & ~0xf0003ull) | 1 | (1ull << 16) | (3ull << 18);   // L0, write, 4 bytes
                }
                armed += SetThreadContext(t, &ctx) != 0;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    rt_log("life watch: the life word %#llx watched on %d threads", (unsigned long long)addr, armed);
}

void life_watch_start(void) {
    static volatile int32_t started;
    if (plat_cas32(&started, 1, 0) != 0) return;
    while (!(g_addr = game_player_life_addr())) plat_sleep_ms(500);
    const char *at = getenv("TOTK_LIFEWATCH_AT");   // another word near the life to watch instead, as a signed offset
    if (at && *at) g_addr += (uint64_t)strtoll(at, NULL, 0), g_have_prev = -1;
    const char *rd = getenv("TOTK_LIFEWATCH_READ");
    for (const char *p = rd; p && *p && g_nrd < 4;) {
        char *end;
        long long off = strtoll(p, &end, 0);
        if (end == p) break;
        g_rd[g_nrd++] = (g_addr + (uint64_t)off) & ~3ull;
        p = *end == ',' ? end + 1 : end;
    }
    AddVectoredExceptionHandler(1, on_exception);
    for (int i = 0; i < 6; i++) arm_all(g_addr), plat_sleep_ms(5000);   // threads made since are armed too
    rt_log("life watch: %ld writes so far, %ld reading places", g_hits, g_rd_n);
}
#else
void life_watch_start(void) { rt_log("life watch: Windows only"); }
#endif
