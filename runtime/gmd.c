// Shared progress, first step: where the game keeps its GameData flags while it runs. The save (progress.sav) holds
// (hash, value) pairs per type (achievements.c ach_save_flags); the game's flag manager keeps a table of the same
// hashes in its heap. TOTK_GMD_PROBE=1 (a research switch, off by default) finds that table once the world runs: it
// scans the heap for runs of the save's bool hashes laid out at a fixed stride, ascending, logs each run with the
// stride and how often the word after the hash matches the save's value, then watches the longest run and logs every
// flag whose word changes while the game is played (opening a chest, finding a Korok), which shows the table is the
// live one. Read only: nothing is written.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "achievements.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

enum { MAX_FLAGS = 1 << 18, MIN_RUN = 32, CHUNK = 1 << 20 };
static const uint32_t kStrides[] = {8, 12, 16, 24, 32};

static uint32_t *g_hash, *g_val;   // the save's bools, sorted by hash, and their values
static int g_n;
static uint8_t *g_bits;            // 1 << 24 bits: the low 24 bits of every hash, a quick filter

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int find(uint32_t h) {   // index in g_hash, -1 when not a save bool
    if (!(g_bits[(h & 0xffffff) >> 3] >> (h & 7) & 1)) return -1;
    int lo = 0, hi = g_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_hash[mid] == h) return mid;
        if (g_hash[mid] < h) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static uint32_t rd(uint64_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }

typedef struct { uint64_t at; uint32_t stride, len, match; } Run;

static void scan_and_watch(void *arg) {
    (void)arg;
    while (frame_step_world_us() < 20000000) plat_sleep_ms(1000);   // the world has run for 20 s: a save is loaded
    static uint32_t h[MAX_FLAGS], v[MAX_FLAGS];
    int n = ach_save_flags(0, h, v, MAX_FLAGS);   // type 0: bool
    if (n <= 0) {
        rt_log("gmd probe: no save flags to look for");
        return;
    }
    // (hash, value) sorted by hash, for the lookups, and the hashes' filter bits
    uint64_t *pairs = malloc(sizeof *pairs * n);
    g_hash = malloc(sizeof *g_hash * n), g_val = malloc(sizeof *g_val * n), g_bits = calloc(1, 1 << 21);
    if (!pairs || !g_hash || !g_val || !g_bits) return;
    for (int i = 0; i < n; i++) pairs[i] = (uint64_t)h[i] << 32 | v[i];
    qsort(pairs, n, sizeof *pairs, cmp_u64);
    for (int i = 0; i < n; i++) {
        g_hash[i] = (uint32_t)(pairs[i] >> 32), g_val[i] = (uint32_t)pairs[i];
        g_bits[(g_hash[i] & 0xffffff) >> 3] |= (uint8_t)(1 << (g_hash[i] & 7));
    }
    free(pairs);
    g_n = n;
    uint64_t base, size;
    mem_heap_range(&base, &size);
    rt_log("gmd probe: %d bool flags in the save; scanning the heap %#llx + %llu MiB", n, (unsigned long long)base,
           (unsigned long long)(size >> 20));
    Run runs[64];
    int nruns = 0;
    uint64_t t0 = plat_now_ms();
    for (uint64_t c = base; c < base + size; c += CHUNK) {
        uint64_t end = c + CHUNK < base + size ? c + CHUNK : base + size;
        if (!plat_mem_readable(c, end - c)) continue;
        for (uint64_t a = c; a + 4 <= end; a += 4) {
            uint32_t w = rd(a);
            if (find(w) < 0) continue;
            for (size_t s = 0; s < sizeof kStrides / sizeof *kStrides; s++) {
                uint32_t st = kStrides[s], len = 1, prev = w, match = 0;
                while (a + (uint64_t)(len + 1) * st <= base + size && plat_mem_readable(a + (uint64_t)len * st, 4)) {
                    uint32_t nx = rd(a + (uint64_t)len * st);
                    if (nx <= prev || find(nx) < 0) break;
                    prev = nx, len++;
                }
                if (len < MIN_RUN) continue;
                for (uint32_t k = 0; k < len && k < 256; k++) {   // the word after the hash against the save's value
                    int i = find(rd(a + (uint64_t)k * st));
                    match += i >= 0 && rd(a + (uint64_t)k * st + 4) == g_val[i];
                }
                if (nruns < 64) runs[nruns++] = (Run){a, st, len, match};
                rt_log("gmd probe: run at %#llx, stride %u, %u flags ascending, value word matches the save in %u of %u",
                       (unsigned long long)a, st, len, match, len < 256 ? len : 256);
                a += (uint64_t)len * st - 4;
                break;
            }
        }
    }
    rt_log("gmd probe: scan done in %llu ms, %d runs", (unsigned long long)(plat_now_ms() - t0), nruns);
    if (!nruns) return;
    int best = 0;
    for (int i = 1; i < nruns; i++)
        if (runs[i].len > runs[best].len) best = i;
    Run r = runs[best];
    uint32_t *was = malloc(sizeof *was * r.len);
    if (!was) return;
    for (uint32_t k = 0; k < r.len; k++) was[k] = rd(r.at + (uint64_t)k * r.stride + 4);
    rt_log("gmd probe: watching the run at %#llx (%u flags): changes follow", (unsigned long long)r.at, r.len);
    for (;;) {
        plat_sleep_ms(1000);
        for (uint32_t k = 0; k < r.len; k++) {
            uint64_t at = r.at + (uint64_t)k * r.stride;
            uint32_t now = rd(at + 4);
            if (now != was[k]) {
                rt_log("gmd probe: flag %08x at %#llx: %u -> %u", rd(at), (unsigned long long)at, was[k], now);
                was[k] = now;
            }
        }
    }
}

void gmd_start(void) {
    static volatile int32_t started;
    if (plat_cas32(&started, 1, 0) != 0) return;
    const char *e = cfg_get("TOTK_GMD_PROBE");
    if (!e || strcmp(e, "1")) return;
    if (!plat_thread_spawn(scan_and_watch, NULL, "gmd probe", 0)) rt_log("gmd probe: no thread");
}
