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
#include "link_figure.h"
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

// TOTK_ANIM_PROBE=bones: where the game keeps Link's final bone transforms, the pose it draws after blending its
// layers, the rag doll and the feet's placing (a friend's figure could show that pose whole instead of naming one
// animation). The walk from the actor tests each object it reaches, at each 16-byte step of its first NODE_BYTES, for
// a run of at least MIN_RUN transforms: a 3x4 rotation-and-translation (row-major with the translation last in each
// row, or column-major with it as the fourth column), alone (48 bytes) or as a 4x4 (64 bytes). Each array found is
// logged once with the path to it, its layout, its length and where its first translation lies against Link's place
// (world transforms lie about him, model-space ones about the origin); then once a second the arrays that changed,
// and how much: the animated ones are the candidates. Read only.
enum { MIN_RUN = 24, MAX_RUN = 1024, MAX_ARRAYS = 512, LAYOUTS = 4 };
static const char *const layout_name[LAYOUTS] = {"rows34", "cols43", "rows44", "cols44"};
static const int layout_stride[LAYOUTS] = {48, 48, 64, 64};

// The rotation's three axes and the translation of transform m in layout l.
static void unpack(int l, const float *m, float ax[3][3], float t[3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            ax[i][j] = l == 0 ? m[4 * j + i] : l == 1 ? m[3 * i + j] : l == 2 ? m[4 * j + i] : m[4 * i + j];
    for (int j = 0; j < 3; j++) t[j] = l == 0 ? m[4 * j + 3] : l == 1 ? m[9 + j] : l == 2 ? m[4 * j + 3] : m[12 + j];
}

// 1: a rotation with a uniform-ish scale; 2: a zero scale (a hidden bone); 0 neither.
static int is_transform(int l, const float *m) {
    for (int i = 0; i < layout_stride[l] / 4; i++)
        if (!isfinite(m[i]) || fabsf(m[i]) > 1e5f) return 0;
    if (l == 2 && (m[12] != 0 || m[13] != 0 || m[14] != 0 || fabsf(m[15] - 1) > 1e-3f)) return 0;
    if (l == 3 && (m[3] != 0 || m[7] != 0 || m[11] != 0 || fabsf(m[15] - 1) > 1e-3f)) return 0;
    float ax[3][3], t[3], len[3];
    unpack(l, m, ax, t);
    int zero = 1;
    for (int i = 0; i < 3; i++) {
        len[i] = sqrtf(ax[i][0] * ax[i][0] + ax[i][1] * ax[i][1] + ax[i][2] * ax[i][2]);
        if (len[i] > 1e-6f) zero = 0;
    }
    if (zero) return 2;
    for (int i = 0; i < 3; i++)
        if (len[i] < 0.3f || len[i] > 3.0f) return 0;
    for (int i = 0; i < 3; i++) {
        int k = (i + 1) % 3;
        float d = ax[i][0] * ax[k][0] + ax[i][1] * ax[k][1] + ax[i][2] * ax[k][2];
        if (fabsf(d) > 0.03f * len[i] * len[k]) return 0;
    }
    return 1;
}

typedef struct {
    uint64_t addr;
    Path path;
    int layout, count, zeros;
    float sample[64];   // the first four transforms' first 16 floats, to see it change
} BoneArray;
static BoneArray g_arrays[MAX_ARRAYS];
static int g_narrays;

// The run of transforms at a in layout l: its length (0 when shorter than MIN_RUN), hidden ones in *zeros. Identical
// rotations all along (an array of identities, a cleared buffer) do not count.
static int run_at(uint64_t a, int l, int *zeros) {
    static float buf[MAX_RUN * 16];
    int stride = layout_stride[l] / 4, n = 0, z = 0, varied = 0;
    if (!plat_mem_read(a, buf, (size_t)MIN_RUN * layout_stride[l])) return 0;
    for (int i = 0; i < MIN_RUN; i++) {
        int k = is_transform(l, buf + i * stride);
        if (!k) return 0;
    }
    size_t got = (size_t)MIN_RUN;
    for (; n < MAX_RUN; n++) {
        if ((size_t)n >= got) {   // read on in steps of 64 transforms
            size_t more = MAX_RUN - got < 64 ? MAX_RUN - got : 64;
            if (!plat_mem_read(a + got * layout_stride[l], buf + got * stride, more * layout_stride[l])) break;
            got += more;
        }
        int k = is_transform(l, buf + n * stride);
        if (!k) break;
        z += k == 2;
        if (n && memcmp(buf + n * stride, buf, 36)) varied = 1;
    }
    *zeros = z;
    return n >= MIN_RUN && varied && z * 4 < n ? n : 0;   // mostly zero scales: cleared memory, not a skeleton
}

static int covered(uint64_t a) {
    for (int i = 0; i < g_narrays; i++)
        if (a >= g_arrays[i].addr && a < g_arrays[i].addr + (uint64_t)g_arrays[i].count * layout_stride[g_arrays[i].layout])
            return 1;
    return 0;
}

static void bone_found(uint64_t actor, int node, uint16_t off, uint64_t a, int l, int count, int zeros) {
    if (g_narrays >= MAX_ARRAYS) return;
    BoneArray *b = &g_arrays[g_narrays];
    memset(b, 0, sizeof *b);
    int chain[MAX_DEPTH + 1], m = 0;
    for (int k = node; k > 0 && m < MAX_DEPTH; k = g_nodes[k].parent) chain[m++] = k;
    for (int i = m - 1; i >= 0; i--) b->path.off[b->path.n++] = g_nodes[chain[i]].off;
    b->path.off[b->path.n++] = off;   // the last offset: where in the object the array begins
    b->addr = a, b->layout = l, b->count = count, b->zeros = zeros;
    plat_mem_read(a, b->sample, sizeof b->sample);
    float m0[16], ax[3][3], t[3], p[3] = {0}, yaw;
    plat_mem_read(a, m0, (size_t)layout_stride[l]);
    unpack(l, m0, ax, t);
    int have = game_player_pose(p, &yaw);
    // the spread of the translations: about Link's size for a skeleton's
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}, mm[16];
    for (int i = 0; i < count; i++) {
        float ai[3][3], ti[3];
        if (!plat_mem_read(a + (uint64_t)i * layout_stride[l], mm, (size_t)layout_stride[l])) break;
        unpack(l, mm, ai, ti);
        for (int j = 0; j < 3; j++) lo[j] = fminf(lo[j], ti[j]), hi[j] = fmaxf(hi[j], ti[j]);
    }
    char text[160];
    size_t n = (size_t)snprintf(text, sizeof text, "actor");
    for (int i = 0; i + 1 < b->path.n && n < sizeof text; i++) n += (size_t)snprintf(text + n, sizeof text - n, "->%#x", b->path.off[i]);
    snprintf(text + n, sizeof text - n, " +%#x", off);
    rt_log("bone probe: array %d %#llx %s %s x%d hid %d", g_narrays, (unsigned long long)a, text, layout_name[l], count, zeros);
    rt_log("bone probe: array %d t0 %.2f %.2f %.2f box %.2f %.2f %.2f", g_narrays, t[0], t[1], t[2], hi[0] - lo[0], hi[1] - lo[1],
           hi[2] - lo[2]);
    (void)have;
    g_narrays++;
}

// Every object the walk reaches, each 16-byte step of its first NODE_BYTES, in each layout.
static void bone_walk(uint64_t actor) {
    memset(g_seen, 0, sizeof *g_seen * SEEN_SLOTS);
    int n = 0, before = g_narrays;
    g_nodes[n++] = (Node){actor, -1, 0, 0};
    seen_add(actor);
    uint64_t buf[ROOT_BYTES / 8];
    for (int i = 0; i < n; i++) {
        Node nd = g_nodes[i];
        int words = (nd.depth ? NODE_BYTES : ROOT_BYTES) / 8;
        if (!plat_mem_read(nd.addr, buf, (size_t)words * 8)) continue;
        for (int w = 0; w < words; w += 2) {   // arrays inside this object
            uint64_t a = nd.addr + (uint64_t)w * 8;
            if (covered(a)) continue;
            for (int l = 0; l < LAYOUTS; l++) {
                int fits = (size_t)(w * 8 + layout_stride[l]) <= (size_t)words * 8;   // else run_at reads it
                if (fits && !is_transform(l, (const float *)&buf[w])) continue;
                int z, c = run_at(a, l, &z);
                if (c) {
                    bone_found(actor, i, (uint16_t)(w * 8), a, l, c, z);
                    break;
                }
            }
        }
        for (int w = 0; w < words; w++) {   // the objects it points at
            uint64_t v = buf[w];
            if (!in_heap(v) || (v & 7) || nd.depth + 1 >= MAX_DEPTH || n >= MAX_NODES || !seen_add(v)) continue;
            g_nodes[n++] = (Node){v, i, (uint16_t)(w * 8), (uint8_t)(nd.depth + 1)};
        }
    }
    rt_log("bone probe: walked %d objects from the actor %#llx, %d new arrays (%d in all)", n, (unsigned long long)actor,
           g_narrays - before, g_narrays);
}

// The second search, over the whole heap and alias region: arrays of transforms whose translations all lie within
// NEAR metres of Link (world transforms: the pose the game draws him with, whatever the path to it). Each float
// within NEAR of his x is tried as the x of a translation in each layout; a hit is followed back to its array's start
// and on to its end. Each array is logged once; the watch then logs which of them change, and how much.
enum { MAX_SCAN = 256 };
#define NEAR 3.0f
typedef struct { uint64_t addr; int layout, count; float sample[64]; } ScanArray;
static ScanArray g_scan[MAX_SCAN];
static int g_nscan;

static int near_link(int l, uint64_t a, const float p[3]) {
    float m[16], ax[3][3], t[3];
    if (!plat_mem_read(a, m, (size_t)layout_stride[l]) || is_transform(l, m) != 1) return 0;
    unpack(l, m, ax, t);
    return fabsf(t[0] - p[0]) < NEAR && fabsf(t[1] - p[1]) < NEAR && fabsf(t[2] - p[2]) < NEAR;
}

static void scan_hit(uint64_t a, int l, const float p[3]) {
    for (int i = 0; i < g_nscan; i++)
        if (g_scan[i].layout == l && a >= g_scan[i].addr && a < g_scan[i].addr + (uint64_t)g_scan[i].count * layout_stride[l]) return;
    if (g_nscan >= MAX_SCAN || !near_link(l, a, p)) return;
    uint64_t st = layout_stride[l], b = a, e = a + st;
    while (near_link(l, b - st, p)) b -= st;
    while ((e - b) / st < MAX_RUN && near_link(l, e, p)) e += st;
    int count = (int)((e - b) / st);
    if (count < 8) return;
    ScanArray *s = &g_scan[g_nscan];
    s->addr = b, s->layout = l, s->count = count;
    plat_mem_read(b, s->sample, sizeof s->sample);
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}, m[16], ax[3][3], t[3];
    for (uint64_t q = b; q < e; q += st) {
        plat_mem_read(q, m, st);
        unpack(l, m, ax, t);
        for (int j = 0; j < 3; j++) lo[j] = fminf(lo[j], t[j]), hi[j] = fmaxf(hi[j], t[j]);
    }
    rt_log("bone scan: %d at %#llx %s x %d, about Link %.2f..%.2f %.2f..%.2f %.2f..%.2f", g_nscan, (unsigned long long)b,
           layout_name[l], count, lo[0] - p[0], hi[0] - p[0], lo[1] - p[1], hi[1] - p[1], lo[2] - p[2], hi[2] - p[2]);
    g_nscan++;
}

static void bone_scan(void) {
    float p[3], yaw;
    if (!game_player_pose(p, &yaw)) return;
    uint64_t ranges[2][2] = {{g_heap, g_heap_size}, {g_alias, g_alias_size}};
    const uint64_t chunk = 1 << 20;
    int before = g_nscan;
    uint64_t t0 = plat_now_ms();
    for (int r = 0; r < 2; r++)
        for (uint64_t c = ranges[r][0]; c + chunk <= ranges[r][0] + ranges[r][1]; c += chunk) {
            if (!plat_mem_readable(c, chunk)) continue;
            const float *f = (const float *)(uintptr_t)c;
            for (uint64_t i = 16; i + 16 < chunk / 4; i++) {
                if (!(fabsf(f[i] - p[0]) < NEAR)) continue;
                uint64_t a = c + 4 * i;
                if (fabsf(f[i + 1] - p[1]) < NEAR && fabsf(f[i + 2] - p[2]) < NEAR) {
                    scan_hit(a - 36, 1, p);   // cols43: the translation after the three axes
                    scan_hit(a - 48, 3, p);   // cols44
                }
                if (fabsf(f[i + 4] - p[1]) < NEAR && fabsf(f[i + 8] - p[2]) < NEAR) {
                    scan_hit(a - 12, 0, p);   // rows34: the last of each row
                    scan_hit(a - 12, 2, p);   // rows44
                }
            }
        }
    rt_log("bone scan: Link at %.2f %.2f %.2f; %d new arrays (%d in all) in %llu ms", p[0], p[1], p[2], g_nscan - before,
           g_nscan, (unsigned long long)(plat_now_ms() - t0));
}

static void watch_bones(void *arg) {
    (void)arg;
    while (frame_step_world_us() < 20000000) plat_sleep_ms(1000);
    g_nodes = malloc(sizeof *g_nodes * MAX_NODES), g_seen = malloc(sizeof *g_seen * SEEN_SLOTS);
    if (!g_nodes || !g_seen) return;
    const LinkFigure *lf = link_figure();
    rt_log("bone probe: the figure's skeleton has %u bones", lf ? lf->nbones : 0);
    uint64_t walked = 0;
    for (;;) {
        uint64_t actor = game_player_actor(), now = plat_now_ms();
        mem_heap_range(&g_heap, &g_heap_size);
        g_alias = mem_layout()->alias.base, g_alias_size = mem_layout()->alias.size;
        if (!actor) {
            plat_sleep_ms(500);
            continue;
        }
        if (!walked || (now - walked >= 15000 && g_narrays < MAX_ARRAYS)) bone_walk(actor), bone_scan(), walked = plat_now_ms();
        {   // the scanned arrays that changed
            char sl[1800];
            size_t sm = 0;
            for (int i = 0; i < g_nscan && sm + 40 < sizeof sl; i++) {
                float now_s[64];
                if (!plat_mem_read(g_scan[i].addr, now_s, sizeof now_s)) continue;
                float d = 0;
                for (int k = 0; k < 64; k++) d += fabsf(now_s[k] - g_scan[i].sample[k]);
                if (d > 1e-4f && d < 1e6f) sm += (size_t)snprintf(sl + sm, sizeof sl - sm, " %d:%.2f", i, d);
                memcpy(g_scan[i].sample, now_s, sizeof now_s);
            }
            if (sm) rt_log("bone scan: %.1f s changed:%s", (double)frame_step_world_us() / 1e6, sl);
        }
        // the arrays that changed in the last second (by the path again: an array may move), with how much
        char line[1800];
        size_t m = 0;
        int changed = 0;
        for (int i = 0; i < g_narrays && m + 40 < sizeof line; i++) {
            BoneArray *b = &g_arrays[i];
            uint64_t a = actor;
            for (int k = 0; k + 1 < b->path.n && a; k++) a = rd_ptr(a + b->path.off[k]);
            if (!a) continue;
            a += b->path.off[b->path.n - 1];
            float now_s[64];
            if (!plat_mem_read(a, now_s, sizeof now_s)) continue;
            float d = 0;
            for (int k = 0; k < 64; k++) d += fabsf(now_s[k] - b->sample[k]);
            if (d > 1e-4f) m += (size_t)snprintf(line + m, sizeof line - m, " %d:%.2f%s", i, d, a != b->addr ? "(moved)" : ""), changed++;
            memcpy(b->sample, now_s, sizeof now_s);
        }
        if (changed) rt_log("bone probe: %.1f s changed:%s", (double)frame_step_world_us() / 1e6, line);
        plat_sleep_ms(1000);
    }
}

// TOTK_ANIM_PROBE=bonedump: the candidate the bone probe found on 1.4.3 (actor->0xa98->0x1d8->0x3a8 +0x3c8, 161
// rows34 transforms that move with Link's animation) written every 2 s to <profile>/bonedump.bin, with Link's place
// and rotation, for study offline: u32 count, float place[3], float rot[9], then count x 12 floats. First the objects
// on the way to it are searched two levels deep for BFRES skeleton or model headers (FSKL, FMDL), which name the bones.

// Arrays of BODY_MIN..MAX_RUN transforms (rows34 or cols43) whose translations all fit in a box of BODY_BOX metres,
// wherever it lies (world, model or camera space), over the whole heap and alias region: a skeleton's pose. Mostly
// identical rotations (a cleared or unused array) do not count.
enum { MAX_BODY = 2048 };
static int BODY_MIN = 100;
#define BODY_BOX 50.0f
typedef struct { uint64_t addr; int layout, count; } BodyArray;
static BodyArray g_body[MAX_BODY];
static int g_nbody;

static int body_run(uint64_t a, int l) {
    static float m[MAX_RUN * 12];
    int st = layout_stride[l] / 4, n = 0, same = 0, hidden = 0;
    if (!plat_mem_read(a, m, (size_t)BODY_MIN * layout_stride[l])) return 0;
    float lo[3], hi[3], ax[3][3], t[3], ax0[3][3];
    for (; n < MAX_RUN; n++) {
        if (n >= BODY_MIN && !plat_mem_read(a + (uint64_t)n * layout_stride[l], m + n * st, (size_t)layout_stride[l])) break;
        int k = is_transform(l, m + n * st);
        if (!k) break;
        if (k == 2) {   // a hidden bone (zero scale): part of the array, no place of its own
            if (!n) break;
            hidden++;
            continue;
        }
        unpack(l, m + n * st, ax, t);
        if (!n) memcpy(lo, t, 12), memcpy(hi, t, 12), memcpy(ax0, ax, sizeof ax);
        int out = 0;
        for (int j = 0; j < 3; j++) {
            float l2 = fminf(lo[j], t[j]), h2 = fmaxf(hi[j], t[j]);
            if (h2 - l2 > BODY_BOX) out = 1;
        }
        if (out) break;
        for (int j = 0; j < 3; j++) lo[j] = fminf(lo[j], t[j]), hi[j] = fmaxf(hi[j], t[j]);
        same += !memcmp(ax, ax0, sizeof ax);
    }
    return n >= BODY_MIN && same * 2 < n && hidden * 4 < n ? n : 0;
}

static void body_scan(void) {
    uint64_t ranges[2][2] = {{g_heap, g_heap_size}, {g_alias, g_alias_size}};
    const uint64_t chunk = 1 << 20;
    uint64_t t0 = plat_now_ms();
    int before = g_nbody;
    for (int r = 0; r < 2; r++)
        for (uint64_t c = ranges[r][0]; c + chunk <= ranges[r][0] + ranges[r][1] && g_nbody < MAX_BODY; c += chunk) {
            if (!plat_mem_readable(c, chunk)) continue;
            const float *f = (const float *)(uintptr_t)c;
            for (uint64_t i = 0; i + 12 <= chunk / 4 && g_nbody < MAX_BODY; i += 4) {
                // a unit first axis: rows34's m0 m4 m8 (its first column) or cols43's m0 m1 m2
                float r0 = f[i] * f[i] + f[i + 4] * f[i + 4] + f[i + 8] * f[i + 8];
                float c0 = f[i] * f[i] + f[i + 1] * f[i + 1] + f[i + 2] * f[i + 2];
                int l = fabsf(r0 - 1) < 0.05f ? 0 : fabsf(c0 - 1) < 0.05f ? 1 : -1;
                if (l < 0) continue;
                uint64_t a = c + 4 * i;
                int dup = 0;
                for (int k = 0; k < g_nbody; k++)
                    if (a >= g_body[k].addr && a < g_body[k].addr + (uint64_t)g_body[k].count * 48) dup = 1;
                if (dup) continue;
                int n = body_run(a, l);
                if (!n && l == 0 && fabsf(c0 - 1) < 0.05f) n = body_run(a, l = 1);
                if (!n) continue;
                g_body[g_nbody++] = (BodyArray){a, l, n};
                float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}, mm[12], ax[3][3], t[3];
                for (int k = 0; k < n; k++) {
                    plat_mem_read(a + (uint64_t)k * 48, mm, 48);
                    unpack(l, mm, ax, t);
                    for (int j = 0; j < 3; j++) lo[j] = fminf(lo[j], t[j]), hi[j] = fmaxf(hi[j], t[j]);
                }
                if (BODY_MIN >= 100) rt_log("body scan: %d %#llx %s x%d box %.1f %.1f %.1f at %.1f %.1f %.1f", g_nbody - 1, (unsigned long long)a,
                       layout_name[l], n, hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], lo[0], lo[1], lo[2]);
                i += (uint64_t)n * 12 - 4;   // past it
            }
        }
    rt_log("body scan: %d new arrays (%d in all) in %llu ms", g_nbody - before, g_nbody, (unsigned long long)(plat_now_ms() - t0));
}

// TOTK_ANIM_PROBE=bonedump: the body scan at 25 s and 45 s of play, then every array it found written every 2 s to
// <profile>/bonedump.bin with Link's place and rotation: u32 index, count, layout, 12 floats of Link, count x 12 floats.
static void bone_dump(void *arg) {
    (void)arg;
    while (frame_step_world_us() < 25000000) plat_sleep_ms(1000);
    g_nodes = malloc(sizeof *g_nodes * MAX_NODES), g_seen = malloc(sizeof *g_seen * SEEN_SLOTS);
    if (!g_nodes || !g_seen) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/bonedump.bin", rt_config()->profile);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int scans = 0;
    for (;;) {
        mem_heap_range(&g_heap, &g_heap_size);
        g_alias = mem_layout()->alias.base, g_alias_size = mem_layout()->alias.size;
        static uint64_t scanned_at;
        if (scans < 8 && plat_now_ms() - scanned_at >= 20000) {   // every 20 s: the save may load at any time
            uint64_t actor0 = game_player_actor();
            rt_log("body scan: scan %d, Link %s", scans, actor0 ? "in the world" : "not in the world");
            body_scan(), scans++, scanned_at = plat_now_ms();
            if (actor0) bone_walk(actor0);
        }
        uint64_t actor = game_player_actor();
        float pr[12];
        if (actor && plat_mem_read(actor + 0x2b4, pr, sizeof pr)) {
            static float m[MAX_RUN * 12];
            for (int i = 0; i < g_nbody; i++) {
                if (!plat_mem_read(g_body[i].addr, m, (size_t)g_body[i].count * 48)) continue;
                uint32_t hd[4] = {(uint32_t)i, (uint32_t)g_body[i].count, (uint32_t)g_body[i].layout, (uint32_t)(frame_step_world_us() / 1000)};
                fwrite(hd, 4, 4, f), fwrite(pr, 4, 12, f), fwrite(m, 48, (size_t)g_body[i].count, f);
            }
            for (int i = 0; i < g_narrays; i++) {   // the walk's, by their path, as 1000 + index
                BoneArray *b = &g_arrays[i];
                if (b->count < 100 || b->layout > 1) continue;
                uint64_t a = actor;
                for (int k = 0; k + 1 < b->path.n && a; k++) a = rd_ptr(a + b->path.off[k]);
                if (!a || !plat_mem_read(a + b->path.off[b->path.n - 1], m, (size_t)b->count * 48)) continue;
                uint32_t hd[4] = {1000u + (uint32_t)i, (uint32_t)b->count, (uint32_t)b->layout, (uint32_t)(frame_step_world_us() / 1000)};
                fwrite(hd, 4, 4, f), fwrite(pr, 4, 12, f), fwrite(m, 48, (size_t)b->count, f);
            }
            fflush(f);
            rt_log("bone dump: %.1f s %d arrays written", (double)frame_step_world_us() / 1e6, g_nbody);
        }
        plat_sleep_ms(2000);
    }
}

// TOTK_ANIM_PROBE=palpath: where the actor leads to the skinning palettes. The body scan (from 24 transforms, to take
// the right hand's 30 too) finds them; then a walk from the actor, PAL_DEPTH levels and PAL_BYTES of each object,
// logs every path whose pointer lands at a palette's start or up to 0x60 bytes before it (a header). The palettes are
// written to bonedump.bin as in bonedump, for telling them apart offline.
enum { PAL_DEPTH = 6, PAL_BYTES = 0x800, PAL_PATHS = 4 };
static void pal_walk(uint64_t actor) {
    memset(g_seen, 0, sizeof *g_seen * SEEN_SLOTS);
    static int found[MAX_BODY];
    memset(found, 0, sizeof found);
    int n = 0;
    g_nodes[n++] = (Node){actor, -1, 0, 0};
    seen_add(actor);
    static uint64_t buf[PAL_BYTES / 8];
    for (int i = 0; i < n; i++) {
        Node nd = g_nodes[i];
        int words = (nd.depth ? PAL_BYTES : ROOT_BYTES) / 8;
        if (!plat_mem_read(nd.addr, buf, (size_t)words * 8)) {
            words = NODE_BYTES / 8;
            if (!plat_mem_read(nd.addr, buf, (size_t)words * 8)) continue;
        }
        for (int w = 0; w < words; w++) {
            uint64_t v = buf[w];
            if (!in_heap(v)) continue;
            int lo = 0, hi = g_nbody;   // the first palette starting at or after v (g_body is sorted by address)
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (g_body[mid].addr < v) lo = mid + 1;
                else hi = mid;
            }
            for (int k = lo; k < g_nbody && g_body[k].addr - v <= 0x60; k++) {
                if (found[k] >= PAL_PATHS) continue;
                found[k]++;
                char text[200];
                size_t m = (size_t)snprintf(text, sizeof text, "actor");
                int chain[PAL_DEPTH + 1], c = 0;
                for (int q = i; q > 0 && c < PAL_DEPTH; q = g_nodes[q].parent) chain[c++] = q;
                for (int q = c - 1; q >= 0 && m < sizeof text; q--) m += (size_t)snprintf(text + m, sizeof text - m, "->%#x", g_nodes[chain[q]].off);
                rt_log("pal path: %d x%d %s->%#x (+%#llx)", k, g_body[k].count, text, w * 8, (unsigned long long)(g_body[k].addr - v));
            }
            if ((v & 7) || nd.depth + 1 >= PAL_DEPTH || n >= MAX_NODES || !seen_add(v)) continue;
            g_nodes[n++] = (Node){v, i, (uint16_t)(w * 8), (uint8_t)(nd.depth + 1)};
        }
    }
    rt_log("pal path: walked %d objects", n);
}

static int body_cmp(const void *a, const void *b) {
    uint64_t x = ((const BodyArray *)a)->addr, y = ((const BodyArray *)b)->addr;
    return x < y ? -1 : x > y;
}

static void pal_probe(void *arg) {
    (void)arg;
    BODY_MIN = 24;
    while (frame_step_world_us() < 25000000) plat_sleep_ms(1000);
    g_nodes = malloc(sizeof *g_nodes * MAX_NODES), g_seen = malloc(sizeof *g_seen * SEEN_SLOTS);
    char path[1200];
    snprintf(path, sizeof path, "%s/bonedump.bin", rt_config()->profile);
    FILE *f = fopen(path, "wb");
    if (!f || !g_nodes || !g_seen) return;
    mem_heap_range(&g_heap, &g_heap_size);
    g_alias = mem_layout()->alias.base, g_alias_size = mem_layout()->alias.size;
    body_scan();
    qsort(g_body, (size_t)g_nbody, sizeof *g_body, body_cmp);
    for (int i = 0; i < g_nbody; i++)
        if (g_body[i].count >= 24) rt_log("pal: %d %#llx x%d", i, (unsigned long long)g_body[i].addr, g_body[i].count);
    uint64_t actor = game_player_actor();
    if (actor) pal_walk(actor);
    for (;;) {
        actor = game_player_actor();
        float pr[12];
        if (actor && plat_mem_read(actor + 0x2b4, pr, sizeof pr)) {
            static float m[MAX_RUN * 12];
            for (int i = 0; i < g_nbody; i++) {
                if (!plat_mem_read(g_body[i].addr, m, (size_t)g_body[i].count * 48)) continue;
                uint32_t hd[4] = {(uint32_t)i, (uint32_t)g_body[i].count, (uint32_t)g_body[i].layout, (uint32_t)(frame_step_world_us() / 1000)};
                fwrite(hd, 4, 4, f), fwrite(pr, 4, 12, f), fwrite(m, 48, (size_t)g_body[i].count, f);
            }
            fflush(f);
        }
        plat_sleep_ms(2000);
    }
}


// TOTK_ANIM_PROBE=models: every model object (one whose [+0x458]+0x8 leads to a run of transforms, a skinning palette)
// up to three steps from the actor, each palette once with the path to it and its length. On 1.4.3 Link's body
// (Link_RaulSkin) is actor->0x238->0x3e0 (frame_rate.c game_player_body); the other paths are other models.
static void model_probe(void *arg) {
    (void)arg;
    while (frame_step_world_us() < 25000000) plat_sleep_ms(1000);
    g_nodes = malloc(sizeof *g_nodes * MAX_NODES), g_seen = malloc(sizeof *g_seen * SEEN_SLOTS);
    if (!g_nodes || !g_seen) return;
    mem_heap_range(&g_heap, &g_heap_size);
    g_alias = mem_layout()->alias.base, g_alias_size = mem_layout()->alias.size;
    uint64_t actor = game_player_actor(), unit = actor ? rd_ptr(actor + 0x238) : 0;
    if (!actor || !unit) return;
    // every model object (palette by +0x458, +0x8) up to three steps from the actor, each palette once
    static uint64_t seen_pal[256];
    int nseen = 0;
    memset(g_seen, 0, sizeof *g_seen * SEEN_SLOTS);
    int n = 0;
    g_nodes[n++] = (Node){actor, -1, 0, 0};
    seen_add(actor);
    for (int i = 0; i < n; i++) {
        Node nd = g_nodes[i];
        uint64_t buf[0x1000 / 8];
        int words = (nd.depth ? 0x800 : 0x1000) / 8;
        if (!plat_mem_read(nd.addr, buf, (size_t)words * 8)) continue;
        for (int w = 0; w < words; w++) {
            uint64_t v = buf[w];
            if (!in_heap(v) || (v & 7)) continue;
            uint64_t pal = rd_ptr(rd_ptr(v + 0x458) + 0x8);
            if (in_heap(pal)) {
                float m[12];
                int c = 0;
                while (c < 512 && plat_mem_read(pal + 48 * (uint64_t)c, m, 48) && is_transform(0, m)) c++;
                int dup = 0;
                for (int k = 0; k < nseen; k++) dup |= seen_pal[k] == pal;
                if (c >= 4 && !dup && nseen < 256) {
                    seen_pal[nseen++] = pal;
                    char text[200];
                    size_t m2 = (size_t)snprintf(text, sizeof text, "actor");
                    int chain[8], cc = 0;
                    for (int q = i; q > 0 && cc < 8; q = g_nodes[q].parent) chain[cc++] = q;
                    for (int q = cc - 1; q >= 0; q--) m2 += (size_t)snprintf(text + m2, sizeof text - m2, "->%#x", g_nodes[chain[q]].off);
                    rt_log("models: %s->%#x model, palette %#llx x%d", text, w * 8, (unsigned long long)pal, c);
                }
            }
            if (nd.depth + 1 >= 3 || n >= MAX_NODES || !seen_add(v)) continue;
            g_nodes[n++] = (Node){v, i, (uint16_t)(w * 8), (uint8_t)(nd.depth + 1)};
        }
    }
    rt_log("models: %d objects, %d palettes", n, nseen);
}

// TOTK_ANIM_PROBE=life: where Link's own life is (the GameData flag PlayerStatus.Life only mirrors it: writing the flag
// leaves the hearts as they are). The walk from the actor keeps every 4-byte word of the objects it reaches (their first
// NODE_BYTES) that holds the life as the flag shows it (quarter hearts, as an int or a float, or hearts as a float);
// each time the flag changes, the words that did not change to the new value the same way drop out, and the ones left
// are logged with the path to them.
enum { LIFE_MAX = 200000 };
typedef struct { int node; uint16_t off; uint8_t kind; } LifeCand;   // kind 0 int quarters, 1 float quarters, 2 float hearts
static LifeCand *g_life;
static int g_nlife;

static int life_matches(uint64_t a, int kind, int32_t q) {
    uint32_t w;
    if (!plat_mem_read(a, &w, 4)) return 0;
    float f;
    memcpy(&f, &w, 4);
    return kind == 0 ? (int32_t)w == q : kind == 1 ? f == (float)q : f == (float)q / 4.0f;
}

static void life_path(int node, uint16_t off, char *out, size_t cap) {
    int chain[MAX_DEPTH + 1], m = 0;
    for (int k = node; k > 0 && m < MAX_DEPTH; k = g_nodes[k].parent) chain[m++] = k;
    size_t n = (size_t)snprintf(out, cap, "actor");
    for (int i = m - 1; i >= 0 && n < cap; i--) n += (size_t)snprintf(out + n, cap - n, "->%#x", g_nodes[chain[i]].off);
    snprintf(out + n, cap - n, " +%#x", off);
}

static void life_probe(void *arg) {
    (void)arg;
    while (frame_step_world_us() < 15000000) plat_sleep_ms(1000);
    g_nodes = malloc(sizeof *g_nodes * MAX_NODES), g_seen = malloc(sizeof *g_seen * SEEN_SLOTS);
    g_life = malloc(sizeof *g_life * LIFE_MAX);
    if (!g_nodes || !g_seen || !g_life) return;
    uint32_t flag = game_flag_hash("PlayerStatus.Life");
    int32_t q = 0;
    while (!game_int_get(flag, &q)) plat_sleep_ms(500);
    uint64_t actor = game_player_actor();
    mem_heap_range(&g_heap, &g_heap_size);
    g_alias = mem_layout()->alias.base, g_alias_size = mem_layout()->alias.size;
    // the walk, as walk() does, keeping the words that match
    memset(g_seen, 0, sizeof *g_seen * SEEN_SLOTS);
    int n = 0;
    g_nodes[n++] = (Node){actor, -1, 0, 0};
    seen_add(actor);
    static uint64_t buf[ROOT_BYTES / 8];
    for (int i = 0; i < n; i++) {
        Node nd = g_nodes[i];
        int words = (nd.depth ? NODE_BYTES : ROOT_BYTES) / 8;
        if (!plat_mem_read(nd.addr, buf, (size_t)words * 8)) continue;
        for (int w = 0; w < 2 * words && g_nlife < LIFE_MAX; w++)
            for (int kind = 0; kind < 3; kind++)
                if (life_matches(nd.addr + 4ull * w, kind, q)) g_life[g_nlife++] = (LifeCand){i, (uint16_t)(4 * w), (uint8_t)kind};
        for (int w = 0; w < words; w++) {
            uint64_t v = buf[w];
            if (!in_heap(v) || (v & 7) || nd.depth + 1 >= MAX_DEPTH || n >= MAX_NODES || !seen_add(v)) continue;
            g_nodes[n++] = (Node){v, i, (uint16_t)(w * 8), (uint8_t)(nd.depth + 1)};
        }
    }
    rt_log("life probe: life %d quarters; %d objects walked, %d words hold it", q, n, g_nlife);
    for (int round = 0; round < 12 && g_nlife > 0;) {
        if (round >= 3 && g_nlife <= 12) {   // few left: which one is the life itself? Write each, watch the flag follow
            for (int c = 0; c < g_nlife; c++) {
                int32_t before = 0, after = 0, v = 0;
                game_int_get(flag, &before);
                v = before == 3 ? 6 : 3;
                uint64_t a = g_nodes[g_life[c].node].addr + g_life[c].off;
                if (g_life[c].kind || !plat_mem_readable(a, 4)) continue;
                *(volatile int32_t *)(uintptr_t)a = v;
                plat_sleep_ms(800);
                game_int_get(flag, &after);
                char path[200];
                life_path(g_life[c].node, g_life[c].off, path, sizeof path);
                rt_log("life probe: wrote %d at %s: the flag went %d -> %d%s", v, path, before, after,
                       after == v ? "  <- the life" : "");
            }
            break;
        }
        plat_sleep_ms(100);
        int32_t now;
        if (!game_int_get(flag, &now) || now == q) continue;
        int keep = 0;
        for (int c = 0; c < g_nlife; c++)
            if (life_matches(g_nodes[g_life[c].node].addr + g_life[c].off, g_life[c].kind, now)) g_life[keep++] = g_life[c];
        g_nlife = keep, q = now, round++;
        rt_log("life probe: life now %d: %d words followed it", q, g_nlife);
        for (int c = 0; c < g_nlife && c < 12; c++) {
            char path[200];
            life_path(g_life[c].node, g_life[c].off, path, sizeof path);
            rt_log("life probe:   %s (%s)", path, g_life[c].kind == 0 ? "int quarters" : g_life[c].kind == 1 ? "float quarters" : "float hearts");
        }
    }
}

void anim_probe_start(void) {
    static volatile int32_t started;
    if (plat_cas32(&started, 1, 0) != 0) return;
    const char *e = cfg_get("TOTK_ANIM_PROBE");
    if (!e || !*e || !strcmp(e, "0")) return;
    if (!strcmp(e, "life")) {
        if (!plat_thread_spawn(life_probe, NULL, "life probe", 0)) rt_log("life probe: no thread");
        return;
    }
    if (!strcmp(e, "models")) {
        if (!plat_thread_spawn(model_probe, NULL, "model probe", 0)) rt_log("model probe: no thread");
        return;
    }
    if (!strcmp(e, "palpath")) {
        if (!plat_thread_spawn(pal_probe, NULL, "pal probe", 0)) rt_log("pal probe: no thread");
        return;
    }
    if (!strcmp(e, "bonedump")) {
        if (!plat_thread_spawn(bone_dump, NULL, "bone dump", 0)) rt_log("bone dump: no thread");
        return;
    }
    if (!strcmp(e, "bones")) {
        if (!plat_thread_spawn(watch_bones, NULL, "bone probe", 0)) rt_log("bone probe: no thread");
        return;
    }
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
