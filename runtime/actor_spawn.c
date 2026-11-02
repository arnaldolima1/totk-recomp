// Actors made by the game itself next to Link, 1.4.3 only: engine::actor::ActorMgr::requestCreateActorAsync, the call
// the game places its own actors with, called from the present (nvnQueuePresentTexture, between frames). A test switch
// for now: TOTK_SPAWN=<actor>@<seconds>[~<metres>][,...] asks for that actor in front of Link (3 m unless given) once the
// world clock passes that many seconds (an enemy that hits him, a blast that knocks him down, for the combat research),
// and logs his animation and life over the next 6 s.
//
// Facts used (MacNativeGames' NativeTOTK, MIT, actor_spawn.c; addresses mapped from 1.0.0's public symbols): the call is
// main+0x599670 and ActorMgr::sInstance main+0x3ad7450; the call's first seven instructions are checked before it is
// made. Its shape: bool (ActorMgr *, const SafeString &name (one pointer to the name), const CreateArg &, watcher, priority
// (High = 1), pre_actor, actor_file, function, bool, Result *, PreActor **out): eleven arguments, the last three on
// the stack. CreateArg is 0x98 bytes: position, a row-major rotation, scale, an empty name, the blackboard's init info
// (an empty bb::InitInfo<32>, 0x1918 bytes) at +0x58 and which of the transform's parts to use at +0x90.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

static const uint64_t kMain = 0x7000000000ull, kRequestCreate = 0x599670, kActorMgr = 0x3ad7450;
static const uint32_t kEntry[7] = {0xa9ba7bfd, 0xa9016ffc, 0x910003fd, 0xa90267fa, 0xa9035ff8, 0xa90457f6, 0xa9054ff4};

typedef struct {
    float position[3], rotation[9], scale[3];
    uint32_t pad_3c;
    const char *name;              // "" (a SafeString)
    uint8_t b48[8], b50[4];
    uint32_t pad_54;
    void *blackboard, *blackboard_60, *parent, *dependent, *creator, *creator_rtti, *instance_heap;
    uint8_t transform_flags;       // 1 position, 2 rotation, 4 scale
    uint8_t pad_91[7];
} CreateArg;
_Static_assert(sizeof(CreateArg) == 0x98, "CreateArg is 0x98 bytes");
_Static_assert(__builtin_offsetof(CreateArg, blackboard) == 0x58, "the blackboard at +0x58");
_Static_assert(__builtin_offsetof(CreateArg, transform_flags) == 0x90, "the flags at +0x90");

typedef struct { int32_t size; uint32_t pad; void *params; int32_t count; uint32_t pad2; uint8_t buffer[32][0xc8]; } BlackboardInit;
_Static_assert(sizeof(BlackboardInit) == 0x1918, "bb::InitInfo<32> is 0x1918 bytes");

static struct { char name[64]; double at, dist; int done; } want[8];
static double watch_until;   // a spawn's effect on Link logged until then (world seconds)
static char watch_anim[64];
static int watch_life = -1;
static int nwant = -1;

static void parse(void) {
    nwant = 0;
    const char *e = cfg_get("TOTK_SPAWN");
    for (const char *p = e; p && *p && nwant < 8;) {
        const char *at = strchr(p, '@'), *end = strchr(p, ',');
        if (!at || (end && end < at)) break;
        size_t n = (size_t)(at - p);
        if (n >= sizeof want[0].name) break;
        memcpy(want[nwant].name, p, n), want[nwant].name[n] = 0;
        const char *tilde = strchr(at, '~');   // ~<metres>: how far in front of Link (default 3)
        want[nwant].at = atof(at + 1), want[nwant].done = 0;
        want[nwant].dist = tilde && (!end || tilde < end) ? atof(tilde + 1) : 3.0;
        nwant++;
        p = end ? end + 1 : NULL;
    }
}

static int spawn(Cpu *c, const char *name, float dist) {
    uint32_t entry[7];
    uint64_t mgr = 0, actor = game_player_actor();
    if (!plat_mem_read(kMain + kRequestCreate, entry, sizeof entry) || memcmp(entry, kEntry, sizeof entry)) {
        rt_log("spawn: requestCreateActorAsync is not where 1.4.3 has it");
        return -1;
    }
    float pos[3], rot[9];
    if (!actor || !plat_mem_read(kMain + kActorMgr, &mgr, 8) || !mgr || !plat_mem_read(actor + 0x2b4, pos, 12) ||
        !plat_mem_read(actor + 0x2c0, rot, 36))
        return 0;   // no world yet: later
    float fx = rot[2], fz = rot[8], len = sqrtf(fx * fx + fz * fz);   // his facing, flat
    if (!(len > 1e-3f)) fx = 0, fz = 1, len = 1;
    static CreateArg arg;
    static BlackboardInit bb;
    static const char *name_ref;
    memset(&arg, 0, sizeof arg), memset(&bb, 0, sizeof bb);
    arg.position[0] = pos[0] + dist * fx / len, arg.position[1] = pos[1] + 0.5f, arg.position[2] = pos[2] + dist * fz / len;
    arg.rotation[0] = arg.rotation[4] = arg.rotation[8] = 1;
    arg.scale[0] = arg.scale[1] = arg.scale[2] = 1;
    arg.name = "";
    bb.size = 32, bb.params = bb.buffer;
    arg.blackboard = &bb;
    arg.transform_flags = 1 | 2 | 4;
    name_ref = name;
    static uint32_t result;
    static uint64_t pre_actor;
    result = 0, pre_actor = 0;
    // the last three arguments on the guest's stack, 16-byte aligned below its pointer
    uint64_t sp = c->sp, base = (sp - 64) & ~15ull;
    uint64_t stack_args[3] = {0, (uint64_t)(uintptr_t)&result, (uint64_t)(uintptr_t)&pre_actor};   // bool, Result *, out
    memcpy((void *)(uintptr_t)base, stack_args, sizeof stack_args);
    c->sp = base;
    uint64_t ok = rt_call(c, kMain + kRequestCreate, 8, mgr, (uint64_t)(uintptr_t)&name_ref, (uint64_t)(uintptr_t)&arg,
                          (uint64_t)0, (uint64_t)1, (uint64_t)0, (uint64_t)0, (uint64_t)0);
    c->sp = sp;
    rt_log("spawn: %s at %.1f %.1f %.1f: %s (result %u, pre-actor %#llx)", name, arg.position[0], arg.position[1],
           arg.position[2], (ok & 1) ? "requested" : "refused", result, (unsigned long long)pre_actor);
    return 1;
}

void actor_spawn_tick(Cpu *c) {
    static double gear_at;   // TOTK_GEAR_LOG=1 (research): what Link carries and where, once a second
    double t = (double)frame_step_world_us() / 1e6;
    if (t > gear_at + 1 && cfg_get("TOTK_GEAR_LOG") && *cfg_get("TOTK_GEAR_LOG") == '1') {
        gear_at = t;
        GameGear g[4];
        char anim[64];
        float fr;
        int n = game_player_gear(g, 4);
        if (!game_player_anim(anim, sizeof anim, &fr)) anim[0] = 0;
        char lay[8][64], all[600] = "";
        int nl = game_player_anim_layers(lay, 8);
        for (int i = 0; i < nl; i++) snprintf(all + strlen(all), sizeof all - strlen(all), " %s", lay[i]);
        rt_log("gear: %.1f s layers:%s", t, all);
        for (int i = 0; i < n; i++)
            if (g[i].model[0]) rt_log("gear: %.1f s %s: %s at %.2f %.2f %.2f, x axis %.2f %.2f %.2f", t, anim, g[i].model, g[i].m[3], g[i].m[7],
                   g[i].m[11], g[i].m[0], g[i].m[4], g[i].m[8]);
    }
    if (nwant < 0) parse();
    if (!nwant || !RT_V143) return;
    double now = (double)frame_step_world_us() / 1e6;
    for (int i = 0; i < nwant; i++)
        if (!want[i].done && now >= want[i].at && (want[i].done = spawn(c, want[i].name, (float)want[i].dist) != 0)) {
            watch_until = now + 6, watch_anim[0] = 0, watch_life = -1;
            const char *god = cfg_get("TOTK_SPAWN_LIFE");   // research: Link's life set to this first, so a blast shows its reaction, not a death
            if (god && *god) game_player_life_set(atoi(god));
        }
    if (now < watch_until) {   // what the spawn did to Link: his animation and life as they change
        char anim[64];
        float frame;
        int life = -1, max_life;
        if (!game_player_anim(anim, sizeof anim, &frame)) anim[0] = 0;
        game_player_life(&life, &max_life);
        if (strcmp(anim, watch_anim) || life != watch_life) {
            rt_log("spawn: %.2f s Link %s, life %d", now, anim, life);
            snprintf(watch_anim, sizeof watch_anim, "%s", anim), watch_life = life;
        }
    }
}
