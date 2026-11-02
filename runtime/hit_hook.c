// The player's hit hook, 1.4.3 only (patches-v143.txt set hithook): a friend's duel strike goes through the game's own
// damage step, so Link plays the reaction an enemy's hit gives (Nml_Damage_M_F and the like, the knock-back, the
// sound), the hearts drop by the game's hand, and a hit that takes the last quarter knocks him down.
//
// Where it runs: Link's damage step (main+0x2f9910) gathers the attacks that touched him into a hit list and then hands
// the list to main+0x315dd4, which turns each record into damage and a reaction (main+0x1cd60f0: damage = power *
// factor * a table by attack type). The patch replaces that call with this function, which may add a record and then
// makes the call. The step runs for every actor of the player's kind on job threads at once; x19 holds the actor.
//
// The list (main+0x315dd4): [[[x0+0x78]+0x228]+0x428]+0x50; +0x8 its capacity, +0x10 the records (0x1c0 bytes each),
// +0x18 how many this frame. A record, as an enemy's hit leaves it (a Chuchu's, read with TOTK_ANIM_PROBE=lifewatch):
// +0x8 07 ff 00 00, +0xc the unit vector toward the attacker, +0x28 0x16, +0x30 0x82000000, +0x38 an attack id and
// +0x3c 2, +0x40 where it hit, +0x88 -1 and 1, +0x90 the attack type bytes 01 02 00 01, +0x98 a second id and +0x9c 2,
// +0xa0 the power (int quarters at factor 1), +0xa4 the factor, +0xac 0xc, +0xb0 the attacker's position, +0xbc..+0x1a0
// -1.0 each, +0x1b0 1, +0x1b4 0x01000000. No attacker object is needed: its pointers stay empty.
//
// TOTK_HIT_TEST=<seconds>[,...] (a test switch): a 2-quarter strike from 1.5 m in front of Link at those world times.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

enum { RECORD_SIZE = 0x1c0 };
static const uint64_t kConsume = 0x7000315dd4ull;

static int ready;   // the patch is on and points here
static PlatMutex lock_strike = PLAT_MUTEX_INIT;
static struct { float from[3]; int quarters, set; } want;   // the next strike, under lock_strike
static uint32_t next_id = 0x4000;                          // attack ids of our strikes, apart from the game's small ones
static double watch_until;   // after a strike: Link's animation logged as it changes, to see the reaction
static char watch_anim[64];
static double tests[8];
static int ntests = -1;

static void put32(uint8_t *r, int o, uint32_t v) { memcpy(r + o, &v, 4); }
static void putf(uint8_t *r, int o, float v) { memcpy(r + o, &v, 4); }

static uint64_t hit_list(uint64_t x0) {
    uint64_t a = 0;
    if (!plat_mem_read(x0 + 0x78, &a, 8) || !a || !plat_mem_read(a + 0x228, &a, 8) || !a || !plat_mem_read(a + 0x428, &a, 8) ||
        !a || !plat_mem_read(a + 0x50, &a, 8))
        return 0;
    return a;
}

// The record of a strike from `from` (world metres) for `quarters`, Link at `me`.
static void build(uint8_t *r, const float me[3], const float from[3], int quarters) {
    float dx = from[0] - me[0], dz = from[2] - me[2], len = sqrtf(dx * dx + dz * dz);
    if (!(len > 1e-3f)) dx = 0, dz = 1, len = 1;
    dx /= len, dz /= len;
    r[8] = 0x07, r[9] = 0xff, r[10] = 0, r[11] = 0;
    putf(r, 0x0c, dx), putf(r, 0x10, 0), putf(r, 0x14, dz);
    put32(r, 0x28, 0x16), put32(r, 0x30, 0x82000000u);
    put32(r, 0x38, next_id), put32(r, 0x3c, 2);
    putf(r, 0x40, me[0] + 0.3f * dx), putf(r, 0x44, me[1] + 0.9f), putf(r, 0x48, me[2] + 0.3f * dz);
    put32(r, 0x88, 0xffffffffu), put32(r, 0x8c, 1);
    put32(r, 0x90, 0x01000201u);
    put32(r, 0x98, next_id + 1), put32(r, 0x9c, 2);
    put32(r, 0xa0, (uint32_t)quarters), putf(r, 0xa4, 1.0f);
    put32(r, 0xac, 0xc);
    putf(r, 0xb0, from[0]), putf(r, 0xb4, from[1]), putf(r, 0xb8, from[2]);
    for (int o = 0xbc; o < 0x1a4; o += 4) putf(r, o, -1.0f);
    put32(r, 0x1b0, 1), put32(r, 0x1b4, 0x01000000u);
    next_id += 2;
}

static void test_tick(void) {
    if (ntests < 0) {
        ntests = 0;
        const char *e = cfg_get("TOTK_HIT_TEST");
        for (const char *p = e; p && *p && ntests < 8;) {
            tests[ntests++] = atof(p);
            p = strchr(p, ',');
            if (p) p++;
        }
    }
    double now = (double)frame_step_world_us() / 1e6;
    for (int i = 0; i < ntests; i++) {
        float me[3], yaw;
        if (tests[i] > 0 && now >= tests[i] && game_player_pose(me, &yaw)) {
            float from[3] = {me[0] + 1.5f * sinf(yaw), me[1], me[2] + 1.5f * cosf(yaw)};
            tests[i] = 0;
            rt_log("hit hook: test strike at %.2f s", now);
            hit_hook_strike(from, 2);
        }
    }
}

static void add_strike(Cpu *c) {
    plat_mutex_lock(&lock_strike);
    int set = want.set, quarters = want.quarters;
    float from[3] = {want.from[0], want.from[1], want.from[2]};
    plat_mutex_unlock(&lock_strike);
    if (!set) return;
    uint64_t list = hit_list(c->x[0]), recs = 0;
    uint32_t cap = 0, n = 0;
    float me[3];
    if (!list || !plat_mem_read(list + 0x8, &cap, 4) || !plat_mem_read(list + 0x10, &recs, 8) ||
        !plat_mem_read(list + 0x18, &n, 4) || !recs || n >= cap || cap > 64 || !game_player_pos(me))
        return;   // no room this frame: the next one
    build((uint8_t *)(uintptr_t)(recs + (uint64_t)n * RECORD_SIZE), me, from, quarters);
    n++;
    memcpy((void *)(uintptr_t)(list + 0x18), &n, 4);
    watch_until = (double)frame_step_world_us() / 1e6 + 1.5, watch_anim[0] = 0;
    rt_log("hit hook: strike of %d quarters from %.1f m", quarters, sqrtf((from[0] - me[0]) * (from[0] - me[0]) +
                                                                           (from[2] - me[2]) * (from[2] - me[2])));
    plat_mutex_lock(&lock_strike);
    want.set = 0;
    plat_mutex_unlock(&lock_strike);
}

static void hit_hook(Cpu *c) {
    if (c->x[19] && c->x[19] == game_player_actor()) {
        test_tick();
        add_strike(c);
        char anim[64];
        float frame;
        if ((double)frame_step_world_us() / 1e6 < watch_until && game_player_anim(anim, sizeof anim, &frame) &&
            strcmp(anim, watch_anim)) {
            rt_log("hit hook:   Link %s", anim);
            snprintf(watch_anim, sizeof watch_anim, "%s", anim);
        }
    }
    c->pc = kConsume;   // the call the patch replaced, with the same arguments and return address
    rt_dispatch(c);
}

int hit_hook_strike(const float from[3], int quarters) {
    if (!ready || quarters < 0) return 0;
    plat_mutex_lock(&lock_strike);
    memcpy(want.from, from, sizeof want.from);
    want.quarters = quarters, want.set = 1;
    plat_mutex_unlock(&lock_strike);
    return 1;
}

void hit_hook_init(void) {
    int set = rt_patch_set("hithook");
    uint8_t *page = rt_patch_page();
    if (set < 0 || !page) return;
    uint64_t a = hostfn_register("<player hit hook>", hit_hook);
    memcpy(page + 104, &a, 8);
    rt_patch_switch(set, 1);
    ready = 1;
}
