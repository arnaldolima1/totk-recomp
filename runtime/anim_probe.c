// Friends' animations, first step: where the game keeps the name of the animation Link plays. Link's animations are
// the skeletal animations of Model/Player_Animation.anim.bfres (1560 of them: Nml_Wait, Climb_*, Sword_*...); the
// loaded file keeps each one's name in its string pool, and whatever plays one holds a pointer that leads to it.
// TOTK_ANIM_PROBE=<file> (a research switch, off by default; the file lists the names, one a line, as the BFRES
// dictionary has them) walks the pointers from the Player actor (frame_rate.c game_player_actor), breadth first and a
// few levels deep, and keeps every path that ends at one of those names (a string pool entry: the name itself or its
// 2-byte length before it). Then it follows each path again ten times a second and logs the names that change, with
// the paths that show them, while the game is played: the path that tracks walking, climbing and attacking is the one
// to read. A new walk every 2 s adds paths an action opened. It also logs, while Link runs, the way he
// faces from his rotation against the way he moves, to check game_player_pose. Read only: nothing is written.
//
// Second target, the sounds: the game's sound link file (SLink2/slink2.Product.140.bslnk, "XLNK", read whole into
// memory) lists per user (the Player among them) its asset calls, 48 bytes each, whose key names (PV425: a voice,
// Pl_FrontRoll...) name the sounds. The probe finds the loaded file, and the walk also keeps paths that end at one of
// the Player's asset calls or at one of their key names: the one that changes as Link speaks or makes a sound is
// the one the friend's game can read and send. For the sounds the walk runs every 2 s, as a sound's handle lives for
// about as long as the sound.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

enum { MAX_NAMES = 4096, NAME_SLOTS = 8192, MAX_NODES = 300000, SEEN_SLOTS = 1 << 20, NODE_BYTES = 0x400,
       ROOT_BYTES = 0x1000, MAX_DEPTH = 5, MAX_PATHS = 4096, RESCAN_MS = 2000, TICK_MS = 50, SOUND = 1 << 20 };

static char *g_names[MAX_NAMES];
static int g_nnames;
static int16_t g_slot[NAME_SLOTS];   // open addressing over g_names by fnv1a, -1 empty

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int name_index(const char *s) {
    for (uint32_t h = fnv(s) & (NAME_SLOTS - 1);; h = (h + 1) & (NAME_SLOTS - 1)) {
        if (g_slot[h] < 0) return -1;
        if (!strcmp(g_names[g_slot[h]], s)) return g_slot[h];
    }
}

static int load_names(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    memset(g_slot, 0xff, sizeof g_slot);
    char line[128];
    while (g_nnames < MAX_NAMES && fgets(line, sizeof line, f)) {
        size_t n = strcspn(line, "\r\n");
        line[n] = 0;
        if (n < 3 || name_index(line) >= 0) continue;
        g_names[g_nnames] = strdup(line);
        uint32_t h = fnv(line) & (NAME_SLOTS - 1);
        while (g_slot[h] >= 0) h = (h + 1) & (NAME_SLOTS - 1);
        g_slot[h] = (int16_t)g_nnames++;
    }
    fclose(f);
    return g_nnames;
}

// The game's data lives in the heap (SetHeapSize) and in the alias region it fills with MapPhysicalMemory.
static uint64_t g_heap, g_heap_size, g_alias, g_alias_size;
static int in_heap(uint64_t a) {
    return (a >= g_heap && a < g_heap + g_heap_size) || (a >= g_alias && a < g_alias + g_alias_size);
}

static uint64_t rd_ptr(uint64_t a) {
    uint64_t p = 0;
    return a && plat_mem_read(a, &p, 8) ? p : 0;
}

// The animation name at a (or after its 2-byte length at a), as an index into g_names; -1 when it is none.
static int anim_at(uint64_t a) {
    if (!in_heap(a)) return -1;
    char s[66];
    for (int skip = 0; skip <= 2; skip += 2) {
        if (!plat_mem_read(a + skip, s, 64)) continue;
        s[64] = 0;
        int n = 0;
        while (n < 64 && (s[n] == '_' || (s[n] >= '0' && s[n] <= '9') || (s[n] >= 'A' && s[n] <= 'Z') ||
                          (s[n] >= 'a' && s[n] <= 'z')))
            n++;
        if (n < 3 || s[n] != 0) continue;
        int i = name_index(s);
        if (i >= 0) return i;
    }
    return -1;
}

// The loaded sound link file: the Player's asset call table (file offsets measured on 1.4.3's slink2.Product.140,
// 0x812364 bytes: user "Player" at 0x4c86f0, its 0x6ef calls from 0x4c95d0, the name table at 0x78983c).
enum { XLNK_SIZE = 0x812364, CALLS_AT = 0x4c95d0, NCALLS = 0x6ef, CALL_BYTES = 48, NAMES_AT = 0x78983c };
static uint64_t g_xlnk;
static uint64_t g_key[NCALLS];   // each call's key name, as an address in the loaded file

static void find_xlnk(void) {
    uint64_t ranges[2][2] = {{g_heap, g_heap_size}, {g_alias, g_alias_size}};
    const uint64_t chunk = 1 << 20;
    for (int r = 0; r < 2 && !g_xlnk; r++)
        for (uint64_t c = ranges[r][0]; c < ranges[r][0] + ranges[r][1] && !g_xlnk; c += chunk) {
            if (!plat_mem_readable(c, chunk)) continue;
            for (uint64_t k = 0; k + 12 <= chunk; k += 4) {
                const uint32_t *w = (const uint32_t *)(uintptr_t)(c + k);
                if (w[0] == 0x4b4e4c58u && w[1] == XLNK_SIZE && w[2] == 0x21) {
                    g_xlnk = c + k;
                    break;
                }
            }
        }
    if (!g_xlnk) {
        rt_log("anim probe: the sound link file is not in memory");
        return;
    }
    // a call's first field is its key name: an offset into the name table in the file, a pointer once loaded
    for (int i = 0; i < NCALLS; i++) {
        uint64_t pos = 0;
        plat_mem_read(g_xlnk + CALLS_AT + (uint64_t)i * CALL_BYTES, &pos, 8);
        g_key[i] = pos >= g_xlnk && pos < g_xlnk + XLNK_SIZE ? pos : g_xlnk + NAMES_AT + (uint32_t)pos;
    }
    char first[16] = {0};
    plat_mem_read(g_key[6], first, 15);
    rt_log("anim probe: sound link file at %#llx; Player call 6 is %s", (unsigned long long)g_xlnk, first);
}

// What a pointer leads to: an animation name (its index), one of the Player's sound calls or its key name (SOUND +
// the call), else -1.
// TOTK_ANIM_PROBE=armor: the target is an armor model's name instead (Armor_001_Upper, Armor_001_RaulSkin_Upper,
// Armor_Default_Head...): where the game keeps what Link wears now, which the save only has as of its last save.
enum { ARMOR = 1 << 21, MAX_ARMOR = 512 };
static int g_armor_mode;
static char g_armor[MAX_ARMOR][48];
static int g_narmor;

static int armor_at(uint64_t a) {
    if (!in_heap(a)) return -1;
    char s[50];
    for (int skip = 0; skip <= 2; skip += 2) {
        if (!plat_mem_read(a + skip, s, 48)) continue;
        s[48] = 0;
        if (strncmp(s, "Armor_", 6)) continue;
        int n = 6;
        while (n < 47 && (s[n] == '_' || (s[n] >= '0' && s[n] <= '9') || (s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= 'a' && s[n] <= 'z'))) n++;
        if (n < 9 || s[n] != 0) continue;
        for (int i = 0; i < g_narmor; i++)
            if (!strcmp(g_armor[i], s)) return ARMOR + i;
        if (g_narmor == MAX_ARMOR) return -1;
        memcpy(g_armor[g_narmor], s, (size_t)n + 1);
        return ARMOR + g_narmor++;
    }
    return -1;
}

static int target(uint64_t v) {
    if (g_armor_mode) return armor_at(v);
    if (g_xlnk) {
        uint64_t t = g_xlnk + CALLS_AT;
        if (v >= t && v < t + (uint64_t)NCALLS * CALL_BYTES && (v - t) % CALL_BYTES == 0) return SOUND + (int)((v - t) / CALL_BYTES);
        if (v >= g_xlnk + NAMES_AT && v < g_xlnk + XLNK_SIZE)
            for (int i = 0; i < NCALLS; i++)
                if (g_key[i] == v) return SOUND + i;
    }
    return anim_at(v);
}

static const char *label(int code, char *buf, size_t cap) {
    if (code >= ARMOR) return g_armor[code - ARMOR];
    if (code < SOUND) return g_names[code];
    int n = snprintf(buf, cap, "sound#%d:", code - SOUND);
    if (n > 0 && (size_t)n < cap - 1) {
        plat_mem_read(g_key[code - SOUND], buf + n, cap - 1 - (size_t)n);
        buf[cap - 1] = 0;
    }
    return buf;
}

typedef struct { uint64_t addr; int32_t parent; uint16_t off; uint8_t depth; } Node;
typedef struct { uint8_t n; uint16_t off[MAX_DEPTH + 1]; int last; uint32_t changes; } Path;

static Node *g_nodes;
static uint64_t *g_seen;
static Path g_paths[MAX_PATHS];
static int g_npaths;

static int seen_add(uint64_t a) {   // 1 when a was new
    for (uint64_t h = (a >> 3) * 0x9e3779b97f4a7c15ull >> 44;; h = (h + 1) & (SEEN_SLOTS - 1)) {
        if (g_seen[h] == a) return 0;
        if (!g_seen[h]) return g_seen[h] = a, 1;
    }
}

// Follows a path from the actor: each offset but the last leads to the next object; the last holds the name pointer.
static int follow(uint64_t actor, const Path *p) {
    uint64_t a = actor;
    for (int i = 0; i < p->n && a; i++) a = rd_ptr(a + p->off[i]);
    return a ? target(a) : -1;
}

static void path_text(const Path *p, char *out, size_t cap) {
    size_t n = (size_t)snprintf(out, cap, "actor");
    for (int i = 0; i < p->n && n < cap; i++) n += (size_t)snprintf(out + n, cap - n, "%s%#x", i + 1 < p->n ? "->" : ".", p->off[i]);
}

static void add_path(uint64_t actor, int node, uint16_t off, int name) {
    if (g_npaths >= MAX_PATHS) return;
    Path p = {0};
    int chain[MAX_DEPTH + 1], m = 0;
    for (int k = node; k > 0 && m < MAX_DEPTH; k = g_nodes[k].parent) chain[m++] = k;
    for (int i = m - 1; i >= 0; i--) p.off[p.n++] = g_nodes[chain[i]].off;
    p.off[p.n++] = off;
    for (int i = 0; i < g_npaths; i++)
        if (g_paths[i].n == p.n && !memcmp(g_paths[i].off, p.off, p.n * sizeof *p.off)) return;
    if (follow(actor, &p) != name) return;   // the walk took a pointer that does not lead there again
    p.last = name;
    g_paths[g_npaths] = p;
    char text[160], lb[64];
    path_text(&p, text, sizeof text);
    rt_log("anim probe: path %d %s = %s", g_npaths, text, label(name, lb, sizeof lb));
    g_npaths++;
}

// Breadth first from the actor over pointers into the heap, NODE_BYTES of each object (ROOT_BYTES of the actor),
// MAX_DEPTH levels.
static void walk(uint64_t actor) {
    memset(g_seen, 0, sizeof *g_seen * SEEN_SLOTS);
    int n = 0, before = g_npaths;
    g_nodes[n++] = (Node){actor, -1, 0, 0};
    seen_add(actor);
    uint64_t buf[ROOT_BYTES / 8];
    for (int i = 0; i < n; i++) {
        Node nd = g_nodes[i];
        int words = (nd.depth ? NODE_BYTES : ROOT_BYTES) / 8;   // the actor is a large object of its own
        if (!plat_mem_read(nd.addr, buf, (size_t)words * 8)) continue;
        for (int w = 0; w < words; w++) {
            uint64_t v = buf[w];
            if (!in_heap(v)) continue;
            int name = target(v);
            if (name >= 0) {
                add_path(actor, i, (uint16_t)(w * 8), name);
                continue;
            }
            if ((v & 7) || nd.depth + 1 >= MAX_DEPTH || n >= MAX_NODES || !seen_add(v)) continue;
            g_nodes[n++] = (Node){v, i, (uint16_t)(w * 8), (uint8_t)(nd.depth + 1)};
        }
    }
    rt_log("anim probe: walked %d objects from the actor %#llx, %d new paths (%d in all)", n, (unsigned long long)actor,
           g_npaths - before, g_npaths);
}

static void run(void *arg) {
    (void)arg;
    while (frame_step_world_us() < 20000000) plat_sleep_ms(1000);   // the world has run for 20 s: a save is loaded
    g_nodes = malloc(sizeof *g_nodes * MAX_NODES), g_seen = malloc(sizeof *g_seen * SEEN_SLOTS);
    if (!g_nodes || !g_seen) return;
    uint64_t walked = 0, faced = 0;
    int scanned = 0;
    float was[3] = {0};
    for (;;) {
        uint64_t actor = game_player_actor(), now = plat_now_ms();
        mem_heap_range(&g_heap, &g_heap_size);
        g_alias = mem_layout()->alias.base, g_alias_size = mem_layout()->alias.size;
        if (!scanned) scanned = 1, g_armor_mode ? (void)0 : find_xlnk();
        if (!actor) {
            plat_sleep_ms(500);
            continue;
        }
        if (!walked || now - walked >= RESCAN_MS) walk(actor), walked = plat_now_ms();
        // the names that changed this tick, each with the paths that show it
        int shown[64], nshown = 0;
        for (int i = 0; i < g_npaths; i++) {
            int name = follow(actor, &g_paths[i]);
            if (name < 0 || name == g_paths[i].last) continue;
            g_paths[i].last = name, g_paths[i].changes++;
            int j = 0;
            while (j < nshown && shown[j] != name) j++;
            if (j < nshown || nshown == 64) continue;
            shown[nshown++] = name;
            char ids[200];
            size_t m = 0;
            for (int q = i; q < g_npaths && m < sizeof ids - 8; q++)
                if (follow(actor, &g_paths[q]) == name) m += (size_t)snprintf(ids + m, sizeof ids - m, " %d", q);
            char lb[64];
            rt_log("anim probe: %.1f s %s (paths%s)", (double)frame_step_world_us() / 1e6, label(name, lb, sizeof lb), ids);
        }
        // the facing check, once a second while Link moves on the ground at a run
        float p[3], yaw;
        if (now - faced >= 1000 && game_player_pose(p, &yaw)) {
            float dx = p[0] - was[0], dz = p[2] - was[2], dt = faced ? (float)(now - faced) / 1000.0f : 0;
            if (dt > 0 && hypotf(dx, dz) / dt > 2.5f && hypotf(dx, dz) / dt < 20 && isfinite(yaw))
                rt_log("anim probe: facing %.2f rad, moving %.2f rad (%.1f m/s)", yaw, atan2f(dx, dz), hypotf(dx, dz) / dt);
            memcpy(was, p, sizeof was);
            faced = now;
        }
        plat_sleep_ms(TICK_MS);
    }
}

// TOTK_ANIM_PROBE=actors: only the resident actors' names, logged when they change (does the armor Link wears show
// there as actors of its own?).
static void list_actors(void *arg) {
    (void)arg;
    static char was[64][48], now[64][48];
    int nwas = -1;
    while (frame_step_world_us() < 20000000) plat_sleep_ms(1000);   // as the walk: once a save has run for 20 s
    for (;;) {
        plat_sleep_ms(1000);
        int n = game_resident_actors(now, 64);
        if (n == nwas && !memcmp(now, was, sizeof now[0] * (size_t)n)) continue;
        char line[3200];
        size_t m = 0;
        line[0] = 0;
        for (int i = 0; i < n && m + 50 < sizeof line; i++) m += (size_t)snprintf(line + m, sizeof line - m, " %s", now[i]);
        rt_log("anim probe: %.1f s resident actors (%d):%s", (double)frame_step_world_us() / 1e6, n, line);
        memcpy(was, now, sizeof now), nwas = n;
    }
}

// TOTK_ANIM_PROBE=frame: where the animation player keeps the frame its first slot plays. Ten times a second it logs
// that slot's animation (frame_rate.c game_player_anim) and the floats near the slot (the player object's +0x40 to
// +0x1c0) that changed since the last tick and lie in 0..1000: the one that climbs at 30 a second (or the rate the
// game plays at) and starts over with each animation is the frame.
static void watch_frame(void *arg) {
    (void)arg;
    while (frame_step_world_us() < 20000000) plat_sleep_ms(1000);
    float was[96] = {0};
    for (;;) {
        plat_sleep_ms(100);
        uint64_t actor = game_player_actor();
        uint64_t mm = 0, c = 0, player = 0;
        if (actor && plat_mem_read(actor + 0x238, &mm, 8) && mm && plat_mem_read(mm + 0xc8, &c, 8) && c &&
            plat_mem_read(c + 0x118, &player, 8) && player) {
            float v[96];
            if (!plat_mem_read(player + 0x40, v, sizeof v)) continue;
            char name[64], line[1600];
            float fr;
            if (!game_player_anim(name, sizeof name, &fr)) name[0] = 0;
            size_t n = (size_t)snprintf(line, sizeof line, "%s:", name);
            for (int i = 0; i < 96 && n + 24 < sizeof line; i++)
                if (v[i] != was[i] && isfinite(v[i]) && v[i] >= 0 && v[i] <= 1000)
                    n += (size_t)snprintf(line + n, sizeof line - n, " %x=%.2f", 0x40 + 4 * i, v[i]);
            memcpy(was, v, sizeof v);
            rt_log("anim probe: %.2f s frame %s", (double)frame_step_world_us() / 1e6, line);
        }
    }
}

void anim_probe_start(void) {
    static volatile int32_t started;
    if (plat_cas32(&started, 1, 0) != 0) return;
    const char *e = cfg_get("TOTK_ANIM_PROBE");
    if (!e || !*e || !strcmp(e, "0")) return;
    if (!strcmp(e, "frame")) {
        if (!plat_thread_spawn(watch_frame, NULL, "anim frame", 0)) rt_log("anim probe: no thread");
        return;
    }
    if (!strcmp(e, "actors")) {
        if (!plat_thread_spawn(list_actors, NULL, "actor list", 0)) rt_log("anim probe: no thread");
        return;
    }
    g_armor_mode = !strcmp(e, "armor");
    if (!g_armor_mode && !load_names(e)) {
        rt_log("anim probe: no names in %s", e);
        return;
    }
    rt_log("anim probe: %d animation names from %s", g_nnames, e);
    if (!plat_thread_spawn(run, NULL, "anim probe", 0)) rt_log("anim probe: no thread");
}
