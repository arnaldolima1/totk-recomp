// Shared progress research: where the game keeps its GameData flags while it runs (TOTK_GMD_PROBE, off by default;
// read only). What it found on 1.4.3 is used by game_flags.c. The modes:
// 1         the save's bool hashes as ascending (hash, value) runs at a fixed stride in the heap (none: the first guess);
// clusters  where the save's bool hashes sit, whatever the layout: 64 KiB buckets of the heap and the alias region by
//           hits, the densest with their commonest distances (the game's list of the bool hashes, 4 bytes apart, and its
//           hash map of (hash, entry number) pairs), then <profile>/gmd_dump.bin: the save's bools and 10 MiB around
//           the list, for study offline;
// index     the values by the hash map's numbers worked out offline (<profile>/gmd_index.bin), as a bitset or bytes
//           anywhere (none: they are inside 24-byte entries);
// owner     the object holding the list and the pointers beside it, tried as the values (it showed the bool table:
//           the count, the 24-byte entries, the map);
// mgr       that table, what points at it (a global of the main module) and its entries' bytes 20..23 watched live
//           (byte 22 flips as the game sets a flag).
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

static uint64_t g_list_hint;   // cluster_scan: the first hit of the densest bucket of hashes 4 bytes apart

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

// The save's bools into g_hash / g_val (sorted by hash) and the filter bits; 0 when there are none.
static int load_flags(void) {
    static uint32_t h[MAX_FLAGS], v[MAX_FLAGS];
    int n = ach_save_flags(0, h, v, MAX_FLAGS);   // type 0: bool
    if (n <= 0) {
        rt_log("gmd probe: no save flags to look for");
        return 0;
    }
    // (hash, value) sorted by hash, for the lookups, and the hashes' filter bits
    uint64_t *pairs = malloc(sizeof *pairs * n);
    g_hash = malloc(sizeof *g_hash * n), g_val = malloc(sizeof *g_val * n), g_bits = calloc(1, 1 << 21);
    if (!pairs || !g_hash || !g_val || !g_bits) return 0;
    for (int i = 0; i < n; i++) pairs[i] = (uint64_t)h[i] << 32 | v[i];
    qsort(pairs, n, sizeof *pairs, cmp_u64);
    for (int i = 0; i < n; i++) {
        g_hash[i] = (uint32_t)(pairs[i] >> 32), g_val[i] = (uint32_t)pairs[i];
        g_bits[(g_hash[i] & 0xffffff) >> 3] |= (uint8_t)(1 << (g_hash[i] & 7));
    }
    free(pairs);
    g_n = n;
    return n;
}

// TOTK_GMD_PROBE=clusters: where the save's bool hashes sit in memory, whatever the layout. Every 4-byte word of the
// heap and the alias region that equals one of the save's bool hashes counts in its 64 KiB bucket; the densest buckets
// are logged, and for each the commonest distances between consecutive hits, how many of those steps go up, and the
// bytes around its first hit. A table of the flags (with their values beside them, or in an array of their own) shows
// as a bucket thick with hits at a steady distance; the game's flag list (a BYML) as one at varying distances.
enum { BUCKET_SHIFT = 16, TOP = 12 };

static void cluster_scan(void) {
    const RtGuestLayout *L = mem_layout();
    uint64_t hb, hs;
    mem_heap_range(&hb, &hs);
    uint64_t ranges[2][2] = {{hb, hs}, {L->alias.base, L->alias.size}};
    typedef struct { uint64_t at; uint32_t hits; } Bucket;
    static Bucket top[TOP];
    int ntop = 0;
    uint64_t t0 = plat_now_ms(), total = 0;
    for (int r = 0; r < 2; r++)
        for (uint64_t c = ranges[r][0]; c < ranges[r][0] + ranges[r][1]; c += 1ull << BUCKET_SHIFT) {
            uint64_t end = c + (1ull << BUCKET_SHIFT);
            if (end > ranges[r][0] + ranges[r][1] || !plat_mem_readable(c, end - c)) continue;
            uint32_t hits = 0;
            for (uint64_t a = c; a < end; a += 4)
                if (find(rd(a)) >= 0) hits++;
            total += hits;
            if (hits < 64) continue;
            int k = ntop < TOP ? ntop++ : TOP - 1;   // keep the TOP densest, sorted
            if (k == TOP - 1 && ntop == TOP && top[k].hits >= hits) continue;
            top[k] = (Bucket){c, hits};
            while (k > 0 && top[k - 1].hits < top[k].hits) {
                Bucket t = top[k - 1];
                top[k - 1] = top[k], top[k] = t, k--;
            }
        }
    rt_log("gmd probe: %llu hash hits in all, scanned in %llu ms", (unsigned long long)total,
           (unsigned long long)(plat_now_ms() - t0));
    for (int i = 0; i < ntop; i++) {
        uint64_t c = top[i].at, end = c + (1ull << BUCKET_SHIFT), last = 0;
        uint32_t dist[64] = {0}, nd = 0, up = 0, steps = 0, first_val = 0;
        uint64_t first = 0;
        uint32_t dcount[64] = {0};
        uint32_t prevh = 0;
        for (uint64_t a = c; a < end; a += 4) {
            uint32_t w = rd(a);
            int fi = find(w);
            if (fi < 0) continue;
            if (!first) first = a, first_val = g_val[fi];
            if (last) {
                uint32_t d = (uint32_t)(a - last);
                steps++, up += w > prevh;
                uint32_t j = 0;
                while (j < nd && dist[j] != d) j++;
                if (j == nd && nd < 64) dist[nd++] = d;
                if (j < 64) dcount[j]++;
            }
            last = a, prevh = w;
        }
        char ds[200];
        size_t m = 0;
        for (int pick = 0; pick < 4; pick++) {   // the four commonest distances
            uint32_t bj = 0;
            for (uint32_t j = 1; j < nd; j++)
                if (dcount[j] > dcount[bj]) bj = j;
            if (!nd || !dcount[bj]) break;
            m += (size_t)snprintf(ds + m, sizeof ds - m, " %u:%u", dist[bj], dcount[bj]);
            if (!pick && dist[bj] == 4 && !g_list_hint) g_list_hint = first;
            dcount[bj] = 0;
        }
        uint32_t hex[16];
        plat_mem_read(first, hex, sizeof hex);
        rt_log("gmd probe: bucket %#llx: %u hits, %u of %u steps up, distances (bytes:count)%s; first at %#llx (save value %u): "
               "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
               (unsigned long long)c, top[i].hits, up, steps, ds, (unsigned long long)first, first_val, hex[0], hex[1], hex[2],
               hex[3], hex[4], hex[5], hex[6], hex[7], hex[8], hex[9], hex[10], hex[11]);
    }
}

// The game's own list of the bool flags: the longest stretch of save hashes at 4 bytes from each other (found by
// cluster_scan, out of hash order, the game's own). Its start and length into *at, *len; 0 when there is none.
static int hash_array(uint64_t near, uint64_t *at, uint32_t *len) {
    uint64_t a = near;
    while (plat_mem_readable(a - 4, 4) && find(rd(a - 4)) >= 0) a -= 4;
    uint64_t e = near;
    while (plat_mem_readable(e, 4) && find(rd(e)) >= 0) e += 4;
    *at = a, *len = (uint32_t)((e - a) / 4);
    return *len > 1000;
}

// <profile>/gmd_dump.bin for study offline: u32 save bool count, then (hash, value) pairs in hash order; u64 the list's
// address, u32 its length; u64 a window's start, u32 its size and its bytes: from 2 MiB before the list to 8 MiB after
// (the game's hash maps of the flags lie there).
static void dump_window(uint64_t list, uint32_t len) {
    char path[1200];
    snprintf(path, sizeof path, "%s/gmd_dump.bin", rt_config()->profile);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t n = (uint32_t)g_n;
    fwrite(&n, 4, 1, f);
    for (int i = 0; i < g_n; i++) fwrite(&g_hash[i], 4, 1, f), fwrite(&g_val[i], 4, 1, f);
    fwrite(&list, 8, 1, f), fwrite(&len, 4, 1, f);
    uint64_t start = (list - (2u << 20)) & ~0xfffull;
    uint32_t sz = 10u << 20, readable = 0;
    static uint8_t zero[1 << 16];
    fwrite(&start, 8, 1, f), fwrite(&sz, 4, 1, f);
    for (uint64_t c = start; c < start + sz; c += sizeof zero) {   // what cannot be read as zeros
        int ok = plat_mem_readable(c, sizeof zero);
        readable += ok;
        fwrite(ok ? (const void *)(uintptr_t)c : zero, 1, sizeof zero, f);
    }
    rt_log("gmd probe: %u of %u 64 KiB blocks of the window readable", readable, sz >> 16);
    fclose(f);
    rt_log("gmd probe: %s: the list and %u MiB from %#llx", path, sz >> 20, (unsigned long long)start);
}

// TOTK_GMD_PROBE=index: the values by a numbering worked out offline: <profile>/gmd_index.bin holds u32 set count, u32
// clear count, then the flags' numbers (the index the game's hash map gives each hash) that are set and that are clear
// in the save. Every 4-byte-aligned start of the heap and the alias region is tried as a bitset (bit k, lowest first)
// and as a byte array (byte k nonzero) of those numbers; the starts matching nine in ten of both are logged.
static void index_search(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/gmd_index.bin", rt_config()->profile);
    uint64_t sz = 0;
    uint32_t *d = (uint32_t *)plat_file_read(path, &sz);
    if (!d || sz < 8 || 8ull + 4ull * ((uint64_t)d[0] + d[1]) > sz) {
        rt_log("gmd probe: no %s", path);
        return;
    }
    uint32_t n1 = d[0], n0 = d[1], *ones = d + 2, *zeros = d + 2 + n1, top = 0;
    for (uint32_t i = 0; i < n1 + n0; i++) top = ones[i] > top ? ones[i] : top;
    const RtGuestLayout *L = mem_layout();
    uint64_t hb, hs;
    mem_heap_range(&hb, &hs);
    uint64_t ranges[2][2] = {{hb, hs}, {L->alias.base, L->alias.size}}, t0 = plat_now_ms();
    int found = 0;
    for (int r = 0; r < 2; r++)
        for (uint64_t c = ranges[r][0]; c < ranges[r][0] + ranges[r][1]; c += 1 << 20) {
            uint64_t end = c + (1 << 20);
            if (end + 16ull * top + 32 > ranges[r][0] + ranges[r][1] || !plat_mem_readable(c, end - c + 16ull * top + 32)) continue;
            // kinds: 0 bitset lowest bit first, 1 highest first; 2.. arrays of 1, 2, 4, 8, 16-byte elements (nonzero)
            static const uint32_t es[7] = {0, 0, 1, 2, 4, 8, 16};
            for (uint64_t b = c; b < end; b++)
                for (int t = 0; t < 7; t++)
                    for (int sh = 0; sh < 2; sh++) {   // the number as given, or one less (numbered from 1)
                        if (t >= 3 && (b & (es[t] < 4 ? es[t] - 1 : 3))) continue;
                        if (t < 2 && (b & 3)) continue;
#define GET(k0) (t == 0 ? (*(const uint8_t *)(uintptr_t)(b + ((k0) - sh) / 8) >> (((k0) - sh) % 8) & 1)                               : t == 1 ? (*(const uint8_t *)(uintptr_t)(b + ((k0) - sh) / 8) >> (7 - ((k0) - sh) % 8) & 1)                         : es[t] == 1 ? *(const uint8_t *)(uintptr_t)(b + ((k0) - sh)) != 0                                                 : es[t] == 2 ? *(const uint16_t *)(uintptr_t)(b + 2ull * ((k0) - sh)) != 0                                                      : *(const uint32_t *)(uintptr_t)(b + (uint64_t)es[t] * ((k0) - sh)) != 0)
                        uint32_t q = 0;
                        while (q < 24 && q < n1 && GET(ones[q])) q++;
                        if (q < 24 && q < n1) continue;
                        uint32_t z = 0;   // and most of 64 clear ones spread over the numbers (dense memory sets them all)
                        for (uint32_t i = 0; i < 64 && n0; i++) z += !GET(zeros[(uint64_t)i * n0 / 64]);
                        if (z < 56) continue;
                        uint32_t m1 = 0, m0 = 0;
                        for (uint32_t i = 0; i < n1; i++) m1 += GET(ones[i]) != 0;
                        for (uint32_t i = 0; i < n0; i++) m0 += !GET(zeros[i]);
#undef GET
                        if (m1 * 10 < n1 * 8 || m0 * 10 < n0 * 8) continue;
                        static const char *const kn[7] = {"bitset low-first", "bitset high-first", "bytes", "u16s", "u32s",
                                                          "8-byte elements", "16-byte elements"};
                        if (found++ < 30)
                            rt_log("gmd probe: values as %s (number%s) at %#llx: %u of %u set and %u of %u clear match", kn[t],
                                   sh ? " - 1" : "", (unsigned long long)b, m1, n1, m0, n0);
                    }
        }
    rt_log("gmd probe: %d starts found in %llu ms", found, (unsigned long long)(plat_now_ms() - t0));
    free(d);
}

// TOTK_GMD_PROBE=owner: the object that holds the game's list of bool hashes, and the values beside it. The heap and
// the alias region are searched for pointers to the list's start; each holder's 64 words around the pointer are
// logged, and every one of them that points into the heap is tried as the values: a bitset (lowest or highest bit
// first) or an array of 1, 2, 4 or 8-byte elements (nonzero when set), numbered by the list's order or by the hash
// map's numbers (gmd_index.bin, as for index; that number or one less).
static int g_is1[1 << 15], g_is0[1 << 15];   // by the hash map's number: 1 set / clear in the save (from gmd_index.bin)

static int value_kind(uint64_t b, int t, int by_map, int sh, uint32_t n, uint64_t list, uint32_t *m1o, uint32_t *m0o) {
    uint32_t m1 = 0, m0 = 0, t1 = 0, t0 = 0;
    for (uint32_t i = 0; i < (by_map ? (1u << 15) : n); i++) {
        int set, known;
        if (by_map) set = g_is1[i], known = g_is1[i] || g_is0[i];
        else {
            int k = find(rd(list + 4ull * i));
            known = k >= 0, set = known && g_val[k];
        }
        if (!known) continue;
        int64_t k = (int64_t)i - sh;
        if (k < 0) continue;
        uint64_t at = t < 2 ? b + (uint64_t)k / 8 : b + (uint64_t)k * (t == 2 ? 1 : t == 3 ? 2 : t == 4 ? 4 : 8);
        uint8_t byte;
        uint64_t word = 0;
        int got;
        if (t < 2) {
            if (!plat_mem_read(at, &byte, 1)) return 0;
            got = t == 0 ? byte >> (k % 8) & 1 : byte >> (7 - k % 8) & 1;
        } else {
            if (!plat_mem_read(at, &word, t == 2 ? 1 : t == 3 ? 2 : t == 4 ? 4 : 8)) return 0;
            got = word != 0;
        }
        if (set) t1++, m1 += got;
        else t0++, m0 += !got;
        if (t1 >= 24 && m1 * 10 < t1 * 8) return 0;   // early out
    }
    *m1o = m1, *m0o = m0;
    return t1 && t0 && m1 * 10 >= t1 * 9 && m0 * 10 >= t0 * 9;
}

static void owner_search(uint64_t list, uint32_t n) {
    char path[1200];
    snprintf(path, sizeof path, "%s/gmd_index.bin", rt_config()->profile);
    uint64_t sz = 0;
    uint32_t *d = (uint32_t *)plat_file_read(path, &sz);
    if (d && sz >= 8 && 8ull + 4ull * ((uint64_t)d[0] + d[1]) <= sz) {
        for (uint32_t i = 0; i < d[0]; i++) if (d[2 + i] < (1u << 15)) g_is1[d[2 + i]] = 1;
        for (uint32_t i = 0; i < d[1]; i++) if (d[2 + d[0] + i] < (1u << 15)) g_is0[d[2 + d[0] + i]] = 1;
    }
    free(d);
    const RtGuestLayout *L = mem_layout();
    uint64_t hb, hs;
    mem_heap_range(&hb, &hs);
    uint64_t ranges[2][2] = {{hb, hs}, {L->alias.base, L->alias.size}};
    int holders = 0;
    static const char *const kn[6] = {"bitset low-first", "bitset high-first", "bytes", "u16s", "u32s", "u64s"};
    for (int r = 0; r < 2 && holders < 16; r++)
        for (uint64_t c = ranges[r][0]; c < ranges[r][0] + ranges[r][1] && holders < 16; c += 1 << 16) {
            if (!plat_mem_readable(c, 1 << 16)) continue;
            for (uint64_t a = c; a < c + (1 << 16) && holders < 16; a += 8) {
                uint64_t v = *(const uint64_t *)(uintptr_t)a;
                if (v < list - 0x40 || v > list) continue;
                holders++;
                uint64_t w[64];
                if (!plat_mem_read(a - 0x100, w, sizeof w)) continue;
                rt_log("gmd probe: %#llx holds %#llx (the list%+lld)", (unsigned long long)a, (unsigned long long)v,
                       (long long)(v - list));
                for (int k = 0; k < 64; k += 4)
                    rt_log("gmd probe:   %+4d: %016llx %016llx %016llx %016llx", k * 8 - 0x100, (unsigned long long)w[k],
                           (unsigned long long)w[k + 1], (unsigned long long)w[k + 2], (unsigned long long)w[k + 3]);
                for (int k = 0; k < 64; k++) {   // each pointer beside it, as the values
                    uint64_t b = w[k];
                    if (!b || !plat_mem_readable(b, 16)) continue;
                    for (int t = 0; t < 6; t++)
                        for (int by_map = 0; by_map < 2; by_map++)
                            for (int sh = 0; sh < 2; sh++) {
                                uint32_t m1, m0;
                                if (value_kind(b, t, by_map, sh, n, list, &m1, &m0))
                                    rt_log("gmd probe: VALUES at %#llx (word %+d of the holder): %s by %s%s, %u set and %u clear match",
                                           (unsigned long long)b, k * 8 - 0x100, kn[t], by_map ? "the hash map's number" : "the list's order",
                                           sh ? " - 1" : "", m1, m0);
                            }
                }
            }
        }
    rt_log("gmd probe: %d holders of the list", holders);
}

// TOTK_GMD_PROBE=mgr: the bool table found by owner (on 1.4.3 its vtable word, the flag count 21414, the entries of
// 24 bytes, the hash map, ... with the list of hashes 0x40 bytes on), what points at it, and the live values. Every
// 8-byte word of the heap, the alias region and the modules' range (static data) that points at the table or up to
// 0x400 bytes before it is logged. Then each entry's bytes 20..23 are watched twice a second for two minutes, and
// every change logged with the entry's number and hash: the byte that flips as the game sets a flag is the value.
static void mgr_probe(uint64_t list) {
    uint64_t holder = 0;
    const RtGuestLayout *L = mem_layout();
    uint64_t hb, hs;
    mem_heap_range(&hb, &hs);
    uint64_t ranges[3][2] = {{hb, hs}, {L->alias.base, L->alias.size}, {L->aslr.base, L->aslr.size}};
    for (int r = 0; r < 2 && !holder; r++)
        for (uint64_t c = ranges[r][0]; c < ranges[r][0] + ranges[r][1] && !holder; c += 1 << 16) {
            if (!plat_mem_readable(c, 1 << 16)) continue;
            for (uint64_t a = c; a < c + (1 << 16); a += 8) {
                uint64_t v = *(const uint64_t *)(uintptr_t)a;
                if (v >= list - 0x40 && v <= list) {
                    holder = a;
                    break;
                }
            }
        }
    if (!holder) {
        rt_log("gmd probe: no holder of the list");
        return;
    }
    uint64_t table = holder - 0x40, w[8];
    plat_mem_read(table, w, sizeof w);
    uint32_t count = (uint32_t)w[1];
    uint64_t entries = w[2], map = w[3];
    rt_log("gmd probe: bool table at %#llx: vtable %#llx, %u flags, entries %#llx, map %#llx", (unsigned long long)table,
           (unsigned long long)w[0], count, (unsigned long long)entries, (unsigned long long)map);
    int refs = 0;
    for (int r = 0; r < 3; r++)
        for (uint64_t c = ranges[r][0] & ~0xffffull; c < ranges[r][0] + ranges[r][1]; c += 1 << 16) {
            if (!plat_mem_readable(c, 1 << 16)) continue;
            for (uint64_t a = c; a < c + (1 << 16); a += 8) {
                uint64_t v = *(const uint64_t *)(uintptr_t)a;
                if (v > table || table - v > 0x400 || (a >= table - 0x400 && a < table + 0x400)) continue;
                if (refs++ < 40)
                    rt_log("gmd probe: %s %#llx points at the table%+lld", r == 0 ? "heap" : r == 1 ? "alias" : "module",
                           (unsigned long long)a, (long long)(v - table));
            }
        }
    rt_log("gmd probe: %d pointers at the table", refs);
    if (!count || count > 100000) return;
    // the entry each hash map number names: its hash, from the map's (hash, number) pairs in the save's hashes
    uint32_t *hash_of = calloc(count, 4);
    uint8_t *was = malloc(4ull * count);
    if (!hash_of || !was) return;
    for (uint64_t a = map - 0x100000; a < map + 0x100000; a += 8) {   // the pairs lie around the map's pointer
        uint32_t pr[2];
        if (!plat_mem_read(a, pr, 8)) continue;
        if (pr[1] < count && find(pr[0]) >= 0) hash_of[pr[1]] = pr[0];
    }
    for (uint32_t i = 0; i < count; i++) plat_mem_read(entries + 24ull * i + 20, was + 4ull * i, 4);
    rt_log("gmd probe: watching %u entries' bytes 20..23", count);
    for (int tick = 0; tick < 240; tick++) {
        plat_sleep_ms(500);
        for (uint32_t i = 0; i < count; i++) {
            uint8_t now[4];
            if (!plat_mem_read(entries + 24ull * i + 20, now, 4) || !memcmp(now, was + 4ull * i, 4)) continue;
            rt_log("gmd probe: %.1f s entry %u (hash %08x): %02x %02x %02x %02x -> %02x %02x %02x %02x",
                   (double)frame_step_world_us() / 1e6, i, hash_of[i], was[4 * i], was[4 * i + 1], was[4 * i + 2],
                   was[4 * i + 3], now[0], now[1], now[2], now[3]);
            memcpy(was + 4ull * i, now, 4);
        }
    }
    rt_log("gmd probe: watch over");
}

static void scan_and_watch(void *arg) {
    (void)arg;
    while (frame_step_world_us() < 20000000) plat_sleep_ms(1000);   // the world has run for 20 s: a save is loaded
    int n = load_flags();
    if (!n) return;
    if (arg == (void *)2) {
        index_search();
        return;
    }
    if (arg == (void *)4) {   // mgr: the list (as clusters finds it), then its table and the live values
        cluster_scan();
        uint64_t list;
        uint32_t len;
        if (g_list_hint && hash_array(g_list_hint, &list, &len)) mgr_probe(list);
        return;
    }
    if (arg == (void *)3) {   // owner: the list (as clusters finds it), then who holds it
        cluster_scan();
        uint64_t list;
        uint32_t len;
        if (g_list_hint && hash_array(g_list_hint, &list, &len)) owner_search(list, len);
        return;
    }
    if (arg) {   // clusters: where the hashes are, then the values of the game's list of them
        cluster_scan();
        uint64_t list;
        uint32_t len;
        if (g_list_hint && hash_array(g_list_hint, &list, &len)) dump_window(list, len);
        return;
    }
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
    if (!e || (strcmp(e, "1") && strcmp(e, "clusters") && strcmp(e, "index") && strcmp(e, "owner") && strcmp(e, "mgr"))) return;
    void *mode = !strcmp(e, "mgr") ? (void *)4 : !strcmp(e, "owner") ? (void *)3 : !strcmp(e, "index") ? (void *)2 : !strcmp(e, "clusters") ? (void *)1 : NULL;
    if (!plat_thread_spawn(scan_and_watch, mode, "gmd probe", 0)) rt_log("gmd probe: no thread");
}
