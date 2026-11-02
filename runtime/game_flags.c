// The game's flags while it runs (GameData: an item got for the first time, a chest opened, a shrine done), 1.4.3
// only, as found with the research probe gmd.c (TOTK_GMD_PROBE). A global of the main module at kFlagsGlobal points 8
// bytes before the flag manager's bool table; the manager's other tables (other value types: counters and the like)
// follow and precede it every 0x60 bytes in the same layout: a vtable word, the entry count, the entries, the hash map
// and the map's capacity in the high half of the fifth word. Bool entries are 24 bytes, byte 22 the value (byte 23
// matches the save too). The map is an open-addressed array of (u32 hash, u32 entry number) pairs, a hash's first
// slot hash % capacity and the next ones after it; number 0 an empty slot. The bool table is checked by its vtable
// before anything is read.
//
// A flag's name is hashed with MurmurHash3 (x86, 32 bits, seed 0): "IsGet.<actor>" is set the first time Link gets that
// actor's item, the actor names being those of the RomFS's Pack/Actor files (game_flag_item).
//
// A thread watches the bool values four times a second and keeps the changes in a ring that each reader follows with
// its own cursor (game_flags_since). A reload (a death, a save loaded) changes hundreds at once: such a burst is kept as
// one reload mark instead.
//
// TOTK_FLAG_LOG=1 (a research switch, off by default) also writes every change to <profile>/flag-log.tsv: the clock and
// world time, hash, entry, old and new value, Link's place and animation and the flag's name when known; and every
// change of the other tables' entries (table, entry, hash, the 4-byte word that changed), so the flags and counters of
// an action (a chest, a kill, a shrine) can be told apart by playing it. Read only.
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"

static const uint64_t kFlagsGlobal = 0x7003ad66b0ull, kBoolVtable = 0x70039403d0ull;
enum { IA_TABLE = 3, IA_BYTES = 64, SA_TABLE = 17, SA_BYTES = 144, SA_ELEM = 0x50, INT_TABLE = 2, INT_BYTES = 40, INT_VALUE_AT = 28, ENUM_TABLE = 6, ENUM_BYTES = 56, ENUM_VALUE_AT = 28, ENTRY_BYTES = 24, VALUE_AT = 22, MAX_FLAGS = 200000, RING = 4096, BURST = 24, POLL_MS = 250, TABLE_STRIDE = 0x60,
       MAX_TABLES = 24 };

typedef struct {   // one table of the manager
    uint64_t entries, map;
    uint32_t count, cap, stride;
    uint32_t *hash_of;   // per entry its hash (0 none), from the map
    uint8_t *was;        // the entries as last seen (bool table: the value bytes; others: whole entries, for the log)
    uint8_t *runs;       // others: polls in a row each entry changed (a ticking one goes quiet in the log)
} Table;

static struct {
    int ok;
    Table b;                  // the bool table
    Table e;                  // the enum table (a quest's step: Step_<quest>), count 0 when it was not found
    Table n;                  // the int table (PlayerStatus.Life...), count 0 when it was not found
    Table ia, sa;             // the int array and String64 array tables (Pouch.Weapon.EquipIndex, ...Content.Name)
    Table other[MAX_TABLES];  // the others, for the log
    int nother;
    GameFlagChange ring[RING];
    uint64_t head;            // changes put in the ring so far
} F;
static PlatMutex lock = PLAT_MUTEX_INIT;

static int rd(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }

static uint32_t mmh3(const char *s) {   // MurmurHash3 x86 32, seed 0
    uint32_t h = 0, n = (uint32_t)strlen(s), k;
    const uint8_t *p = (const uint8_t *)s;
    for (uint32_t i = 0; i + 4 <= n; i += 4) {
        memcpy(&k, p + i, 4);
        k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u;
        h ^= k, h = h << 13 | h >> 19, h = h * 5 + 0xe6546b64u;
    }
    k = 0;
    for (uint32_t i = n & ~3u, s8 = 0; i < n; i++, s8 += 8) k |= (uint32_t)p[i] << s8;
    if (n & 3) k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u, h ^= k;
    h ^= n, h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

// A table at rec: its entries, map and the map's (hash, number) pairs read into hash_of; 0 when rec is not one.
static int table_at(uint64_t rec, Table *t, int whole) {
    uint64_t w[5];
    if (!rd(rec, w, sizeof w) || (w[0] >> 32) != 0x70) return 0;
    uint32_t count = (uint32_t)w[1], cap = (uint32_t)(w[4] >> 32);
    if (!count || count > MAX_FLAGS || cap < count || cap > 4 * MAX_FLAGS || !w[2] || !w[3]) return 0;
    uint32_t stride = ENTRY_BYTES;
    if (whole) {   // the entries' size: each starts with the same vtable word
        uint64_t v0, v;
        if (!rd(w[2], &v0, 8)) return 0;
        for (stride = 8; stride <= 256 && !(rd(w[2] + stride, &v, 8) && v == v0); stride += 8) {}
        if (stride > 256) return 0;
    }
    uint32_t *slots = malloc(8ull * cap);
    t->hash_of = calloc(count, 4);
    t->was = malloc((uint64_t)count * (whole ? stride : 1));
    t->runs = whole ? calloc(count, 1) : NULL;
    if (!slots || !t->hash_of || !t->was || !rd(w[3], slots, 8ull * cap)) {
        free(slots), free(t->hash_of), free(t->was);
        return 0;
    }
    for (uint32_t i = 0; i < cap; i++)
        if (slots[2 * i + 1] && slots[2 * i + 1] < count) t->hash_of[slots[2 * i + 1]] = slots[2 * i];
    free(slots);
    t->entries = w[2], t->map = w[3], t->count = count, t->cap = cap, t->stride = stride;
    if (whole) rd(t->entries, t->was, (uint64_t)count * stride);
    else
        for (uint32_t i = 0; i < count; i++) rd(t->entries + (uint64_t)ENTRY_BYTES * i + VALUE_AT, &t->was[i], 1);
    return 1;
}

static uint32_t number_of(const Table *t, uint32_t hash);

// The bool table (and, for the log, the others), found and checked once a save runs; 0 until then or on another version.
static int ready(int with_others) {
    if (F.ok) return 1;
    if (!RT_V143) return 0;
    uint64_t p = 0, vt = 0;
    if (!rd(kFlagsGlobal, &p, 8) || !p || !rd(p + 8, &vt, 8) || vt != kBoolVtable || !table_at(p + 8, &F.b, 0)) return 0;
    // the enums six tables on: 56-byte entries whose value (the hash of its name) is the word at +28
    if (!table_at(p + 8 + ENUM_TABLE * TABLE_STRIDE, &F.e, 1) || F.e.stride != ENUM_BYTES) {
        rt_log("flags: no enum table where 1.4.3 has it; quests are not followed");
        memset(&F.e, 0, sizeof F.e);
    }
    if (!table_at(p + 8 + INT_TABLE * TABLE_STRIDE, &F.n, 1) || F.n.stride != INT_BYTES) {
        rt_log("flags: no int table where 1.4.3 has it");
        memset(&F.n, 0, sizeof F.n);
    }
    // the arrays: int arrays three tables on (64-byte entries: +0x20 the element count, +0x28 the elements), String64
    // arrays 17 on (144-byte entries: +0x70 the count, +0x78 the elements, 0x50 bytes each with the text at +0xc)
    if (!table_at(p + 8 + IA_TABLE * TABLE_STRIDE, &F.ia, 1) || F.ia.stride != IA_BYTES) memset(&F.ia, 0, sizeof F.ia);
    if (!table_at(p + 8 + SA_TABLE * TABLE_STRIDE, &F.sa, 1) || F.sa.stride != SA_BYTES) memset(&F.sa, 0, sizeof F.sa);
    for (int k = -8; with_others && k <= 16 && F.nother < MAX_TABLES; k++)
        if (k && table_at(p + 8 + (int64_t)k * TABLE_STRIDE, &F.other[F.nother], 1)) {
            rt_log("flags: table %+d: %u entries of %u bytes", k, F.other[F.nother].count, F.other[F.nother].stride);
            F.nother++;
        }
    rt_log("flags: %u bool flags at %#llx (map %#llx, %u slots)", F.b.count, (unsigned long long)F.b.entries,
           (unsigned long long)F.b.map, F.b.cap);
    return F.ok = 1;
}

// A flag's entry number in table t, 0 when its map has none.
static uint32_t number_of(const Table *t, uint32_t hash) {
    for (uint32_t i = 0, s = t->cap ? hash % t->cap : 0; i < t->cap; i++, s = s + 1 == t->cap ? 0 : s + 1) {
        uint32_t pair[2];
        if (!rd(t->map + 8ull * s, pair, 8) || !pair[1]) return 0;
        if (pair[0] == hash) return pair[1] < t->count ? pair[1] : 0;
    }
    return 0;
}

uint32_t game_flag_hash(const char *name) { return mmh3(name); }

// The value byte written straight into guest memory (the game's pages are the host's, as the reads use them); the
// watcher's copy too, so the write is no change of the game's own. Only the value: the byte after it, which matches the
// save as well, is left to the game.
int game_flag_set(uint32_t hash, int value) {
    plat_mutex_lock(&lock);
    uint32_t k;
    uint8_t was = 0;
    int ok = F.ok && (k = number_of(&F.b, hash)) != 0;
    uint64_t at = ok ? F.b.entries + (uint64_t)ENTRY_BYTES * k + VALUE_AT : 0;
    if (ok && (ok = plat_mem_readable(at, 1) && rd(at, &was, 1))) {
        *(volatile uint8_t *)(uintptr_t)at = (uint8_t)(value != 0);
        F.b.was[k] = (uint8_t)(value != 0);
    }
    plat_mutex_unlock(&lock);
    if (ok) rt_log("flags: %08x set to %d (was %d)", hash, value != 0, was);
    return ok;
}

// An int flag (the int table: 40-byte entries, the default at +24, the value at +28 and a copy at +32;
// PlayerStatus.Life in quarter hearts follows Link's hearts live).
int game_int_get(uint32_t hash, int32_t *value) {
    plat_mutex_lock(&lock);
    uint32_t k;
    int ok = F.ok && F.n.count && (k = number_of(&F.n, hash)) && rd(F.n.entries + (uint64_t)INT_BYTES * k + INT_VALUE_AT, value, 4);
    plat_mutex_unlock(&lock);
    return ok;
}

int game_int_set(uint32_t hash, int32_t value) {
    plat_mutex_lock(&lock);
    uint32_t k;
    int32_t was = 0;
    int ok = F.ok && F.n.count && (k = number_of(&F.n, hash)) != 0;
    uint64_t at = ok ? F.n.entries + (uint64_t)INT_BYTES * k + INT_VALUE_AT : 0;
    if (ok && (ok = plat_mem_readable(at, 8) && rd(at, &was, 4))) {
        *(volatile int32_t *)(uintptr_t)at = value;
        *(volatile int32_t *)(uintptr_t)(at + 4) = value;   // its copy too
    }
    plat_mutex_unlock(&lock);
    if (ok) rt_log("flags: int %08x set to %d (was %d)", hash, value, was);
    return ok;
}

// TOTK_ARRAY_PROBE=<name>[,<name>...] (research): which of the manager's tables holds those flags, and their entries'
// words, with what the pointer-like words lead to, once after a save loads.
static void array_probe(void) {
    const char *e = cfg_get("TOTK_ARRAY_PROBE");
    uint64_t p = 0;
    if (!e || !*e || !rd(kFlagsGlobal, &p, 8) || !p) return;
    char names[512];
    snprintf(names, sizeof names, "%s", e);
    for (char *n = strtok(names, ","); n; n = strtok(NULL, ",")) {
        uint32_t h = mmh3(n);
        int found = 0;
        for (int k = -8; k <= 30; k++) {
            Table t = {0};
            if (!table_at(p + 8 + (int64_t)k * TABLE_STRIDE, &t, 1)) continue;
            uint32_t i = number_of(&t, h);
            if (i) {
                found = 1;
                uint64_t w[24] = {0};
                rd(t.entries + (uint64_t)t.stride * i, w, t.stride < 192 ? t.stride : 192);
                for (int j = 8; j * 8 < (int)t.stride && j < 24; j += 4)
                    rt_log("array probe:   words %d..: %llx %llx %llx %llx", j, (unsigned long long)w[j], (unsigned long long)w[j + 1],
                           (unsigned long long)w[j + 2], (unsigned long long)w[j + 3]);
                rt_log("array probe: %s (%08x) table %d (%u entries of %u bytes) entry %u: %llx %llx %llx %llx %llx %llx %llx %llx", n, h,
                       k, t.count, t.stride, i, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2],
                       (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6],
                       (unsigned long long)w[7]);
                for (int j = 1; j < 24 && j * 8 < (int)t.stride; j++) {
                    if (w[j] >> 32 != 1 && w[j] >> 32 != 2) continue;   // heap-like
                    uint8_t b[160];
                    if (!rd(w[j] & ~7ull, b, sizeof b)) continue;
                    char hex[3 * 64 + 1], txt[161];
                    for (int q = 0; q < 64; q++) snprintf(hex + 3 * q, 4, "%02x ", b[q]);
                    for (int q = 0; q < 160; q++) txt[q] = b[q] >= 32 && b[q] < 127 ? (char)b[q] : '.';
                    txt[160] = 0;
                    rt_log("array probe:   word %d -> %s| %s", j, hex, txt);
                }
            }
            free(t.hash_of), free(t.was), free(t.runs);
        }
        if (!found) rt_log("array probe: %s (%08x) in no table", n, h);
    }
}

// TOTK_FLAG_SET=<hash>=<0|1>[,...] (a test switch): those bool flags written once the table is found after a save loads.
static void test_sets(void) {
    const char *ie = cfg_get("TOTK_INT_SET");   // TOTK_INT_SET=<name or hash>=<value>: an int flag, the same way
    if (ie && *ie) {
        char name[96];
        const char *eq = strchr(ie, '=');
        if (eq && eq - ie < (int)sizeof name) {
            memcpy(name, ie, (size_t)(eq - ie)), name[eq - ie] = 0;
            char *end;
            unsigned long h = strtoul(name, &end, 16);
            if (!game_int_set(*end ? mmh3(name) : (uint32_t)h, (int32_t)atoi(eq + 1))) rt_log("flags: %s is no int flag", name);
        }
    }
    const char *le = cfg_get("TOTK_LIFE_SET");   // TOTK_LIFE_SET=<quarters>: Link's own life, the same way
    if (le && *le) rt_log("flags: Link's life set to %d: %s", atoi(le), game_player_life_set(atoi(le)) ? "done" : "not found");
    const char *e = cfg_get("TOTK_FLAG_SET");
    for (const char *p = e; p && *p;) {
        char *end;
        unsigned long h = strtoul(p, &end, 16);
        if (*end != '=') break;
        int v = (int)strtol(end + 1, &end, 10);
        if (!game_flag_set((uint32_t)h, v)) rt_log("flags: %08lx is no bool flag of this game", h);
        p = *end == ',' ? end + 1 : NULL;
    }
}

// Element i of an int array flag (Pouch.Weapon.EquipIndex: the pouch slot of the weapon in hand, -1 none).
int game_int_array_get(uint32_t hash, int i, int32_t *value) {
    plat_mutex_lock(&lock);
    uint32_t k, count = 0;
    uint64_t at = 0;
    int ok = F.ok && F.ia.count && i >= 0 && (k = number_of(&F.ia, hash)) &&
             rd(F.ia.entries + (uint64_t)IA_BYTES * k + 0x20, &count, 4) && (uint32_t)i < count &&
             rd(F.ia.entries + (uint64_t)IA_BYTES * k + 0x28, &at, 8) && rd(at + 4ull * (uint64_t)i, value, 4);
    plat_mutex_unlock(&lock);
    return ok;
}

// Element i of a String64 array flag (Pouch.Weapon.Content.Name: the actor in each pouch slot).
int game_str_array_get(uint32_t hash, int i, char out[64]) {
    plat_mutex_lock(&lock);
    uint32_t k, count = 0;
    uint64_t at = 0;
    int ok = F.ok && F.sa.count && i >= 0 && (k = number_of(&F.sa, hash)) &&
             rd(F.sa.entries + (uint64_t)SA_BYTES * k + 0x70, &count, 4) && (uint32_t)i < count &&
             rd(F.sa.entries + (uint64_t)SA_BYTES * k + 0x78, &at, 8) && rd(at + (uint64_t)SA_ELEM * (uint64_t)i + 0xc, out, 64);
    plat_mutex_unlock(&lock);
    if (ok) out[63] = 0;
    return ok;
}

int game_enum_get(uint32_t hash, uint32_t *value) {
    plat_mutex_lock(&lock);
    uint32_t k;
    int ok = F.ok && F.e.count && (k = number_of(&F.e, hash)) && rd(F.e.entries + (uint64_t)ENUM_BYTES * k + ENUM_VALUE_AT, value, 4);
    plat_mutex_unlock(&lock);
    return ok;
}

int game_flag_get(uint32_t hash) {
    plat_mutex_lock(&lock);
    int v = -1;
    uint32_t k;
    uint8_t b;
    if (F.ok && (k = number_of(&F.b, hash)) && rd(F.b.entries + (uint64_t)ENTRY_BYTES * k + VALUE_AT, &b, 1)) v = b != 0;
    plat_mutex_unlock(&lock);
    return v;
}

// ---- names: "IsGet.<actor>" for every actor of the RomFS, sorted by hash
typedef struct { uint32_t hash; char name[52]; } Named;
static Named *g_items;
static int g_nitems;

static int cmp_named(const void *a, const void *b) {
    uint32_t x = ((const Named *)a)->hash, y = ((const Named *)b)->hash;
    return x < y ? -1 : x > y;
}

static void load_names(void) {
    uint64_t n = romfs_file_count(), off, size;
    if (!n || !(g_items = malloc(sizeof *g_items * 20000))) return;
    static const char pre[] = "Pack/Actor/", suf[] = ".pack.zs";
    for (uint64_t i = 0; i < n && g_nitems < 20000; i++) {
        const char *p = romfs_entry(i, &off, &size);
        size_t len = p ? strlen(p) : 0;
        if (len <= sizeof pre + sizeof suf - 2 || strncmp(p, pre, sizeof pre - 1) || strcmp(p + len - (sizeof suf - 1), suf)) continue;
        size_t nl = len - (sizeof pre - 1) - (sizeof suf - 1);
        if (nl >= sizeof g_items[0].name || memchr(p + sizeof pre - 1, '/', nl)) continue;
        Named *e = &g_items[g_nitems++];
        memcpy(e->name, p + sizeof pre - 1, nl), e->name[nl] = 0;
        char flag[80];
        snprintf(flag, sizeof flag, "IsGet.%s", e->name);
        e->hash = mmh3(flag);
    }
    qsort(g_items, (size_t)g_nitems, sizeof *g_items, cmp_named);
    rt_log("flags: %d actor names for the item flags", g_nitems);
}

const char *game_flag_item(uint32_t hash) {
    int lo = 0, hi = g_nitems - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_items[mid].hash == hash) return g_items[mid].name;
        if (g_items[mid].hash < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

int game_flags_since(uint64_t *cursor, GameFlagChange *out, int max) {
    plat_mutex_lock(&lock);
    if (*cursor + RING < F.head) *cursor = F.head - RING;   // fell behind: the oldest kept
    int n = 0;
    for (; *cursor < F.head && n < max; ++*cursor) out[n++] = F.ring[*cursor % RING];
    plat_mutex_unlock(&lock);
    return n;
}

// ---- the watcher, and the research log
static FILE *g_log;

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...) {
    if (!g_log) return;
    char clock[16] = "";
    time_t now = time(NULL);   // the wall clock, for matching notes taken while playing
    struct tm tm;
#ifdef _WIN32
    if (!localtime_s(&tm, &now)) strftime(clock, sizeof clock, "%H:%M:%S", &tm);
#else
    if (localtime_r(&now, &tm)) strftime(clock, sizeof clock, "%H:%M:%S", &tm);
#endif
    float p[3] = {NAN, NAN, NAN}, frame;
    char anim[64] = "";
    game_player_pos(p);
    game_player_anim(anim, sizeof anim, &frame);
    fprintf(g_log, "%s\t%lld\t%.1f\t%.1f\t%.1f\t%s\t", clock, (long long)(frame_step_world_us() / 1000), p[0], p[1], p[2], anim);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
}

static void watch(void *arg) {
    (void)arg;
    while (frame_step_world_us() < 5000000) plat_sleep_ms(500);
    load_names();
    static GameFlagChange batch[RING];
    static uint8_t *buf;
    static uint64_t cap;
    for (;; plat_sleep_ms(POLL_MS)) {
        plat_mutex_lock(&lock);
        int was_ok = F.ok, ok = ready(g_log != NULL);
        plat_mutex_unlock(&lock);
        if (!ok) continue;
        if (!was_ok) test_sets(), array_probe();
        int n = 0;
        for (uint32_t k = 1; k < F.b.count; k++) {
            uint8_t b;
            if (!rd(F.b.entries + (uint64_t)ENTRY_BYTES * k + VALUE_AT, &b, 1) || b == F.b.was[k]) continue;
            if (n < RING) batch[n++] = (GameFlagChange){F.b.hash_of[k], k, F.b.was[k], b};
            F.b.was[k] = b;
        }
        if (n) {
            plat_mutex_lock(&lock);
            if (n > BURST) F.ring[F.head++ % RING] = (GameFlagChange){0, 0, 0, 0};   // a reload: one mark
            else
                for (int i = 0; i < n; i++) F.ring[F.head++ % RING] = batch[i];
            plat_mutex_unlock(&lock);
            for (int i = 0; i < n && g_log; i++) {
                const char *name = game_flag_item(batch[i].hash);
                log_line("bool\t%08x\t%u\t%u\t%u\t%s%s", batch[i].hash, batch[i].entry, batch[i].old, batch[i].now,
                         name ? "IsGet." : "", name ? name : "");
            }
        }
        for (int t = 0; g_log && t < F.nother; t++) {   // the other tables: each entry's 4-byte words after the vtable
            Table *T = &F.other[t];
            uint64_t bytes = (uint64_t)T->count * T->stride;
            if (cap < bytes && !(buf = realloc(buf, cap = bytes))) cap = 0;
            if (!buf || !rd(T->entries, buf, bytes)) continue;
            int logged = 0;
            for (uint32_t k = 0; k < T->count; k++) {
                const uint8_t *was = T->was + (uint64_t)k * T->stride, *now = buf + (uint64_t)k * T->stride;
                if (!memcmp(was, now, T->stride)) {
                    if (T->runs && T->runs[k] < 8) T->runs[k] = 0;   // a quiet poll; a ticking entry stays left out
                    continue;
                }
                if (T->runs && T->runs[k] < 255) T->runs[k]++;
                if (T->runs && T->runs[k] == 8) log_line("table%d\t%08x\t%u\tticking: left out from now on", t, T->hash_of[k], k);
                if ((T->runs && T->runs[k] >= 8) || logged >= 64) continue;
                for (uint32_t o = 8; o + 4 <= T->stride; o += 4) {
                    uint32_t a, b;
                    memcpy(&a, was + o, 4), memcpy(&b, now + o, 4);
                    if (a == b) continue;
                    log_line("table%d\t%08x\t%u\t+%u\t%d\t%d", t, T->hash_of[k], k, o, (int32_t)a, (int32_t)b);
                    logged++;
                }
            }
            memcpy(T->was, buf, bytes);
        }
        if (g_log) fflush(g_log);
    }
}

void game_flags_start(void) {
    static volatile int32_t started;
    if (plat_cas32(&started, 1, 0) != 0 || !RT_V143) return;
    const char *e = cfg_get("TOTK_FLAG_LOG");
    if (e && !strcmp(e, "1")) {
        char path[1200];
        snprintf(path, sizeof path, "%s/flag-log.tsv", rt_config()->profile);
        if ((g_log = fopen(path, "a")))
            fprintf(g_log, "# clock\tworld_ms\tx\ty\tz\tanim\tkind\thash\tentry\told/offset\tnew/old\t(new)/name\n");
    }
    if (!plat_thread_spawn(watch, NULL, "flags", 0)) rt_log("flags: no thread for the watcher");
}
