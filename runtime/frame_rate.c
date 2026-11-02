// Adapted from MacNativeGames games/totk/runtime/game_patches.c @ ebe621d (MIT, Copyright (c) 2026 MacNativeGames
// contributors): runtime_frame_step_update (2338bb0: no 1/120 s floor) and the speed survey's player chain
// (world_survey.c player_actor).
// Frame rate: the frame limit and the real-time frame step, the host half of the patch sets fps60, realtime and cloth
// (build tables patches-v121.txt, patches-v143.txt).
//
// TotK's adaptive present-interval controller (main+0x8f26c8) turns the main thread's measured work into a number of
// 60 Hz ticks clamped to [minimum, maximum]; shipping data sets the minimum to 2 (30 fps), the set fps60 lowers it to
// 1. The controller still raises the interval when a frame does not fit. Shipping code then steps the game by whole
// vsyncs (VFR step main+0x7ede6c: ratio = interval * 0.5, seconds = interval / 60; global ratio at main+0x8f2394), so
// game speed is right only when frames take exactly their interval. With the set realtime both read the patch page
// instead: +0 ratio = seconds * 30, +4 seconds (1.4.3: +8 interval = seconds * 60, the producer's nominal interval
// before its half-step coefficient), the moving average of the last 5 display intervals, at most 1/15 s
// (slower frames slow the game down, as on the console) and at least 1/240 s, so a higher frame-rate cap only changes
// the pacer (frame_step_cap_ns, TOTK_FPS_LIMIT). The set cloth gives hair and cloth the same clock and needs realtime.
//
// The floor: with a shorter step (ratio below 0.125) 1.4.3 aborts within seconds of some save loads (issue #241, a
// 280 Hz monitor): Havok's hkDefaultMemoryImpl reports "Out of memory" for the buffer the physics pre-solve (profiler
// scope "TtPreSolve") resizes in main+0x1261630. Forced steps on the Depths save: 1/240 s plays, 1/244 s and shorter
// abort; the field, sky island and fight saves play at 1/280 s, the town save aborts after about 800 frames. The
// same short steps also crash a physics worker reading that buffer's contacts: an access violation in main+0x1292560
// or its caller main+0x1286870 (narrowphase contact setup, reached from the grid task main+0x12f1ea0), reading a
// Havok body's shape at body+0x78 as null or garbage (issues #29 and #92, 240 Hz monitors with fps_limit = display).
// Steps drawn from 1/252..1/230 s crash the Depths load there in seconds; 1/240..1/230 s plays. The frame limit
// stops at 240 too (LIMIT_MAX_HZ), so the floor only catches the average dipping under 1/240 s at that limit: a
// present late by less than a period keeps the pacer's phase (nvn_api_obj.c, pace), so the next display interval is
// shorter than the period. No limit makes the game run slower than the clock.
//
// The sample is the display time the pacer gives each present (nvnQueuePresentTexture), so the step follows the
// frames as shown, pacing waits included. With the step on, the pacer ignores the game's interval and holds a frame
// only to the frame limit (nvn_api_obj.c present_period): the controller's 2 or 3 vsyncs would only add latency. The
// vsync event stays at 60 Hz whatever the limit: the game does not wait for it each frame (1.4.3 shows its title at 144
// fps with the event at 60 Hz).
// One 64-bit and one 32-bit store per present; the guest reads the floats on its own threads, and each stays finite
// and in range whichever store it sees.
//
// The world clock (frame_step_world_us) sums the steps from the first present that finds the player actor away from
// the origin, where the title screen keeps it: the end of the first save load. Recorded and scripted input
// (input.c) runs on it, so a replay presses at the same game time at any frame rate and after a first load of any
// length; a later load (fast travel) still shifts what follows by its length.
//
// TOTK_TRACE=perf adds a line per 120 presents: fps, the game seconds stepped against wall seconds, and with
// TOTK_SPEED_PROBE=1 the player's position and ground speed (an independent check: Link runs at the same speed at
// any frame rate when game time follows wall time).
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define STEP_SAMPLES 5
#define STEP_MAX_S (1.0 / 15.0)    // longest step; a longer frame slows the game down
#define STEP_RESET_S 0.25          // a gap this long (loading, a debugger) restarts the average
#define STEP_FIRST_S (1.0 / 60.0)
#define STEP_MIN_S (1.0 / 240.0)   // shortest step; a shorter one makes the game's physics run out of memory

static _Atomic uint64_t *step_word;   // patch page +0: ratio (low float), seconds (high float)
static double step_now = STEP_FIRST_S;   // the step the patch page holds: what the game steps each frame
static _Atomic uint32_t *step_interval;   // patch page +8: seconds * 60 (patches-v143.txt realtime)
static uint64_t last_ns;
static double samples[STEP_SAMPLES];
static unsigned sample_count, sample_next;

static void step_store(float seconds) {
    step_now = seconds;
    float ratio = seconds * 30.0f, interval = seconds * 60.0f;
    uint32_t lo, hi, iv;
    memcpy(&lo, &ratio, 4);
    memcpy(&hi, &seconds, 4);
    memcpy(&iv, &interval, 4);
    atomic_store_explicit(step_word, (uint64_t)lo | (uint64_t)hi << 32, memory_order_release);
    atomic_store_explicit(step_interval, iv, memory_order_release);
}

static void frame_limit_live(void);

void frame_step_init(void) {
    int fps60 = rt_patch_set("fps60"), realtime = rt_patch_set("realtime"), cloth = rt_patch_set("cloth");
    if (realtime < 0) {
        rt_log("frame rate: no patch table, shipping 30 fps whole-vsync steps");
        return;
    }
    if (!rt_patch_enabled(realtime) && rt_patch_enabled(cloth)) {
        rt_patch_disable(cloth);
        rt_log("frame rate: cloth clock off, it needs the real-time step (TOTK_REALTIME=0)");
    }
    if (rt_patch_enabled(realtime)) {
        step_word = (_Atomic uint64_t *)rt_patch_page();
        step_interval = (_Atomic uint32_t *)((uint8_t *)rt_patch_page() + 8);
        step_store((float)STEP_FIRST_S);
    }
    frame_limit_live();   // TOTK_FPS_LIMIT decides the set fps60 from the start
    rt_log("frame rate: present interval %s, %s", rt_patch_enabled(fps60) ? "1..3 (above 30 fps)" : "2..3 (shipping 30 fps)",
           step_word ? "real-time step (5-frame average of display intervals, at most 1/15 s)" : "whole-vsync steps");
}

int frame_step_realtime(void) { return step_word != NULL; }

// The frame limit (menu, TOTK_FPS_LIMIT, live): 30, 60, 90, 120, 144, any whole rate from 30 to 240, or display (the
// refresh rate of the window's monitor, at most 240). A higher number gives 240, the rate of the shortest step the
// game survives (STEP_MIN_S). Unset, the older TOTK_FPS60=0 still means 30. A limit of 30 switches the set
// fps60 off (the shipping minimum interval 2), every other limit switches it on; the controller reads its minimum
// interval from the patched sites each frame. With the real-time step the pacer holds frames to the limit; above 60
// only the pacer changes, as the step's floor is the limit's ceiling. Without it the game steps whole 60 Hz vsyncs and the pacer
// follows the game's interval, so the limit stops at 60.
#define LIMIT_MIN_HZ 30
#define LIMIT_MAX_HZ 240   // 1 / STEP_MIN_S
static int fps60_set = -2;
static unsigned limit_seen;
static int limit_read;
static int limit_hz = -1;   // 0: display; -1 before the first read

static int limit_parse(void) {
    const char *e = cfg_get("TOTK_FPS_LIMIT"), *old = cfg_get("TOTK_FPS60");
    if (!e || !*e) return old && !strcmp(old, "0") ? 30 : 60;
    if (!strcmp(e, "display")) return 0;
    char *end;
    long n = strtol(e, &end, 10);
    if (!*end && n > LIMIT_MAX_HZ) {
        rt_log("frame rate: TOTK_FPS_LIMIT=%s is above %d, where the game's physics runs out of memory; %d", e,
               LIMIT_MAX_HZ, LIMIT_MAX_HZ);
        return LIMIT_MAX_HZ;
    }
    if (!*end && n >= LIMIT_MIN_HZ) return (int)n;
    rt_log("frame rate: TOTK_FPS_LIMIT=%s is not %d..%d or display; 60", e, LIMIT_MIN_HZ, LIMIT_MAX_HZ);
    return 60;
}

static void frame_limit_live(void) {
    unsigned v = cfg_version();
    if (limit_read && v == limit_seen) return;
    limit_read = 1;
    limit_seen = v;
    int hz = limit_parse();
    if (hz == limit_hz) return;
    limit_hz = hz;
    if (fps60_set == -2) fps60_set = rt_patch_set("fps60");
    if (fps60_set >= 0) rt_patch_switch(fps60_set, hz != 30);
    if ((hz > 60 || hz == 0) && !step_word)
        rt_log("frame rate: limits above 60 need the real-time step (TOTK_REALTIME=1); 60 fps");
    else if (hz) rt_log("frame rate: limit %d fps", hz);
    else if (gfx_window_refresh_hz() > 0) rt_log("frame rate: limit at the monitor's refresh rate (%.3f Hz now)", gfx_window_refresh_hz());
    else rt_log("frame rate: limit at the monitor's refresh rate (60 until the window measures it)");
}

uint64_t frame_step_cap_ns(void) {
    frame_limit_live();
    double hz = limit_hz ? limit_hz : gfx_window_refresh_hz();
    if (hz < LIMIT_MIN_HZ) hz = 60;   // display rate unknown (no window yet)
    if (hz > LIMIT_MAX_HZ) hz = LIMIT_MAX_HZ;
    return (uint64_t)(1e9 / hz + 0.5);
}

// The frame rate the limit gives, for the performance overlay: the setting, the monitor's rate for "display", 60 at
// most without the real-time step; 0 before the first frame read the setting.
double frame_step_limit_hz(void) {
    int hz = limit_hz;
    if (hz < 0) return 0;
    double r = hz ? hz : gfx_window_refresh_hz();
    if (r < LIMIT_MIN_HZ) r = 60;
    return !step_word && r > 60 ? 60 : r > LIMIT_MAX_HZ ? LIMIT_MAX_HZ : r;
}

// ---- TOTK_TRACE=perf speed lines
static uint64_t win_t0, win_frames, win_hist[4];   // win_hist: frames by present interval 1, 2, 3+ ([0] unused)
static double win_game;
static int probe = -1;
static float last_pos[3];
static uint64_t last_pos_ns;

static int read_guest(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }
static uint64_t read_ptr(uint64_t a) {
    uint64_t p = 0;
    return read_guest(a, &p, 8) ? p : 0;
}
// 1.2.1: [main+0x4728538] (1.1.0: [main+0x4630a68], 1.4.3: [main+0x3add238]) +0x1e8 scene, its component 13 lists the
// resident actors; the one named "Player" has its position at +0x2b4 (MacNativeGames world_survey.c, game_version.h
// TOTK_GAME_V121/V143). The 1.1.0 slot was measured from its own dereference chain: 181 scene accesses in that build
// read it, and one of them reproduces this walk (the 14-component check, component 13, the 0x70 stride and the name
// compare) instruction for instruction.
// The names of the resident actors (the list player_actor walks), up to max, each cut to 48 bytes; how many.
int game_resident_actors(char out[][48], int max) {
    uint64_t module = read_ptr(MOD_MAIN_BASE + (RT_V143 ? 0x3add238 : RT_V110 ? 0x4630a68 : 0x4728538)),
             scene = module ? read_ptr(module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !read_guest(scene + 0x50, &count, 4) || count < 14 || count > 256) return 0;
    uint64_t comps = read_ptr(scene + 0x58), resident = comps ? read_ptr(comps + 13 * 8) : 0;
    if (!resident || !read_guest(resident + 0x20, &count, 4) || count < 1 || count > 64) return 0;
    uint64_t links = read_ptr(resident + 0x28);
    int n = 0;
    for (int i = 0; links && i < count && n < max; i++) {
        uint64_t data = read_ptr(links + (uint64_t)i * 0x70 + 8), actor = data ? read_ptr(data + 0x40) : 0;
        uint64_t name = actor ? read_ptr(actor + 0x218) : 0;
        if (name && read_guest(name, out[n], 48)) out[n][47] = 0, n++;
    }
    return n;
}

static uint64_t player_actor(void) {
    uint64_t module = read_ptr(MOD_MAIN_BASE + (RT_V143 ? 0x3add238 : RT_V110 ? 0x4630a68 : 0x4728538)),
             scene = module ? read_ptr(module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !read_guest(scene + 0x50, &count, 4) || count < 14 || count > 256) return 0;
    uint64_t comps = read_ptr(scene + 0x58), resident = comps ? read_ptr(comps + 13 * 8) : 0;
    if (!resident || !read_guest(resident + 0x20, &count, 4) || count < 1 || count > 64) return 0;
    uint64_t links = read_ptr(resident + 0x28);
    for (int i = 0; links && i < count; i++) {
        uint64_t data = read_ptr(links + (uint64_t)i * 0x70 + 8), actor = data ? read_ptr(data + 0x40) : 0;
        uint64_t name = actor ? read_ptr(actor + 0x218) : 0;
        char text[7];
        if (name && read_guest(name, text, 7) && !memcmp(text, "Player", 7)) return actor;
    }
    return 0;
}

// While a save loads the Player actor is already listed but still at the origin (0, 0, 0); a player standing within a
// metre of the map's exact centre is rare enough to count as no position yet too.
static int player_pos(float xyz[3]) {
    uint64_t actor = player_actor();
    return actor && read_guest(actor + 0x2b4, xyz, 12) && isfinite(xyz[0]) && isfinite(xyz[2]) &&
           fabsf(xyz[0]) + fabsf(xyz[2]) > 1.0f;
}

int game_player_pos(float xyz[3]) { return player_pos(xyz); }
uint64_t game_player_actor(void) { return player_actor(); }

// The actor's rotation, a row-major 3x3 at +0x2c0 next to the position (MacNativeGames world_survey.c reads it the
// same way): Link faces his local +z, so the third column is his forward in the world, and the yaw is its angle from
// +z towards +x.
int game_player_pose(float xyz[3], float *yaw) {
    uint64_t actor = player_actor();
    float rot[9];
    if (!actor || !read_guest(actor + 0x2b4, xyz, 12) || !isfinite(xyz[0]) || !isfinite(xyz[2]) ||
        fabsf(xyz[0]) + fabsf(xyz[2]) <= 1.0f)
        return 0;
    *yaw = NAN;
    if (read_guest(actor + 0x2c0, rot, sizeof rot) && isfinite(rot[2]) && isfinite(rot[8]) &&
        rot[2] * rot[2] + rot[8] * rot[8] > 0.01f)
        *yaw = atan2f(rot[2], rot[8]);
    return 1;
}

// Link's whole rotation as a unit quaternion (x, y, z, w): the actor's row-major 3x3 at +0x2c0, whose columns are his
// right, up and forward in the world. A back flip or a side jump turns the whole actor over, not only the animation,
// and goes upside down on the way: a quaternion carries that through, where angles about each axis flip over at 90
// degrees.
int game_player_quat(float q[4]) {
    uint64_t actor = player_actor();
    float m[9];
    if (!actor || !read_guest(actor + 0x2c0, m, sizeof m)) return 0;
    for (int i = 0; i < 9; i++)
        if (!isfinite(m[i]) || fabsf(m[i]) > 2) return 0;
    float tr = m[0] + m[4] + m[8], s;
    if (tr > 0) {
        s = sqrtf(tr + 1) * 2;
        q[3] = 0.25f * s, q[0] = (m[7] - m[5]) / s, q[1] = (m[2] - m[6]) / s, q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        s = sqrtf(1 + m[0] - m[4] - m[8]) * 2;
        q[3] = (m[7] - m[5]) / s, q[0] = 0.25f * s, q[1] = (m[1] + m[3]) / s, q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        s = sqrtf(1 + m[4] - m[0] - m[8]) * 2;
        q[3] = (m[2] - m[6]) / s, q[0] = (m[1] + m[3]) / s, q[1] = 0.25f * s, q[2] = (m[5] + m[7]) / s;
    } else {
        s = sqrtf(1 + m[8] - m[0] - m[4]) * 2;
        q[3] = (m[3] - m[1]) / s, q[0] = (m[2] + m[6]) / s, q[1] = (m[5] + m[7]) / s, q[2] = 0.25f * s;
    }
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(l > 0.5f && l < 1.5f)) return 0;
    for (int i = 0; i < 4; i++) q[i] /= l;
    return 1;
}

// What Link wears now (1.4.3, measured with anim_probe.c): the actor's [+0x168]+0x2e0 holds his model's parts, a
// pointer each at +0x20 + 0x18 k, and each part's name at its +0x18 (Armor_1043_Lower, Armor_Default_Upper when the
// slot is bare, Armor_999_Head with no headgear, Armor_Default_Extra_00 for the body's own pieces). out[0..2] get the
// head, upper and lower pieces' names, "" for none or a default; 1 when the list was read.
int game_player_armor(char out[3][48]) {
    static const char *const kind[3] = {"_Head", "_Upper", "_Lower"};
    uint64_t actor = RT_V143 ? player_actor() : 0, a = actor ? read_ptr(actor + 0x168) : 0, list = a ? read_ptr(a + 0x2e0) : 0;
    for (int k = 0; k < 3; k++) out[k][0] = 0;
    if (!list) return 0;
    int parts = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t part = read_ptr(list + 0x20 + 0x18 * (uint64_t)i), name = part ? read_ptr(part + 0x18) : 0;
        char t[48];
        if (!name || !read_guest(name, t, sizeof t)) break;
        t[47] = 0;
        if (strncmp(t, "Armor_", 6)) break;
        parts++;
        if (!strncmp(t, "Armor_Default", 13) || !strncmp(t, "Armor_999_", 10)) continue;   // bare
        size_t len = strlen(t);
        for (int k = 0; k < 3; k++) {
            size_t kl = strlen(kind[k]);
            if (len > kl && !strcmp(t + len - kl, kind[k])) memcpy(out[k], t, len + 1);
        }
    }
    return parts > 0;
}

// The sounds Link starts (1.4.3, measured with anim_probe.c): his sound link handles sit in small blocks reached from
// the actor's +0x608 ([+0x1b0]+0x30, [+0x1b0]+0x28, [[+0x1a8]+0xa0]+0x30 and +0x38), four a block at +0x20 + 0x110 k;
// each holds a pointer to the asset call it plays, in the game's sound link file (SLink2), whose first field points at
// the call's key name. Only the names of Link's voice are given (PV000_02, or PV103 when the game picks the variant
// later): Player_Voice holds a clip of each. out gets up to max names; slot[] (16, kept by the caller) remembers
// each slot's handle, so a name is given once per time it starts.
int game_player_voices(char out[][16], int max, uint64_t slot[16]) {
    uint64_t actor = RT_V143 ? player_actor() : 0, s = actor ? read_ptr(actor + 0x608) : 0;
    if (!s) return 0;
    uint64_t b1 = read_ptr(s + 0x1b0), b2 = read_ptr(s + 0x1a8), b2a = b2 ? read_ptr(b2 + 0xa0) : 0;
    uint64_t blocks[4] = {b1 ? read_ptr(b1 + 0x30) : 0, b1 ? read_ptr(b1 + 0x28) : 0, b2a ? read_ptr(b2a + 0x30) : 0,
                          b2a ? read_ptr(b2a + 0x38) : 0};
    int n = 0;
    for (int b = 0; b < 4; b++)
        for (int k = 0; k < 4; k++) {
            uint64_t call = blocks[b] ? read_ptr(blocks[b] + 0x20 + 0x110 * k) : 0, *seen = &slot[b * 4 + k];
            if (call == *seen) continue;
            *seen = call;
            uint64_t key = call ? read_ptr(call) : 0;
            char t[16];
            if (!key || !read_guest(key, t, sizeof t)) continue;
            t[15] = 0;
            int len = (int)strnlen(t, 15), digits = 0;
            for (int i = 2; i < len; i++) digits += t[i] >= '0' && t[i] <= '9';
            bool voice = t[0] == 'P' && t[1] == 'V' && (len == 5 || (len == 8 && t[5] == '_')) && digits == len - 2 - (len == 8);
            if (voice && n < max) memcpy(out[n++], t, 16);
        }
    return n;
}

// The animation Link plays, 1.4.3 only (measured with anim_probe.c): the actor's model unit [actor+0x238], its
// [+0xc8], that one's animation player [+0x118]; the player's first slot, at +0x80, points at the header of the
// skeletal animation it plays (a BFRES FSKA of Player_Animation, "FSKA" first) whose name pointer at +8 leads to the
// string pool entry: a u16 length, then the name. That slot follows every action (Climb_Wall_Move_U, Sword_*, Fall,
// Swim_*); the slots after it at a 0xb0 stride hold blended layers (the upper body's pose, the face, the voice). The
// frame that slot plays is the float at the player's +0x58: the game starts some animations past their first frame
// (a back flip at 5.3), plays them at its own rate (a run at 1.5 times) and leaves some before their last (the flip
// at 19.5 of 30, when the landing takes over). *frame gets it, NAN when unread.
// The FSKA name a slot of the anim player points at (u16 length then text, or the text), 0 none.
static int fska_name(uint64_t fska, char *out, size_t cap) {
    char magic[4], text[66];
    uint64_t name = 0;
    out[0] = 0;
    if (!fska || !read_guest(fska, magic, 4) || memcmp(magic, "FSKA", 4) || !(name = read_ptr(fska + 8)) || !read_guest(name, text, 64))
        return 0;
    text[64] = text[65] = 0;
    for (int skip = 2; skip >= 0; skip -= 2) {
        const char *t = text + skip;
        size_t len = 0;
        while (len < 62 && (t[len] == '_' || (t[len] >= '0' && t[len] <= '9') || (t[len] >= 'A' && t[len] <= 'Z') ||
                            (t[len] >= 'a' && t[len] <= 'z')))
            len++;
        if (len < 3 || t[len] || len >= cap || (skip && (uint8_t)text[0] + ((uint8_t)text[1] << 8) != len)) continue;
        memcpy(out, t, len), out[len] = 0;
        return 1;
    }
    return 0;
}

// The animations of all of Link's layers (the anim player's slots every 0xb0 from +0x80: the main one, then the
// upper body's pose, the face, the voice...), as game_player_anim reads the first; the count read into out.
int game_player_anim_layers(char out[][64], int max) {
    uint64_t actor = RT_V143 ? player_actor() : 0, m = actor ? read_ptr(actor + 0x238) : 0;
    uint64_t c = m ? read_ptr(m + 0xc8) : 0, player = c ? read_ptr(c + 0x118) : 0;
    int n = 0;
    for (int k = 0; player && k < 8 && n < max; k++)
        if (fska_name(read_ptr(player + 0x80 + 0xb0 * (uint64_t)k), out[n], 64)) n++;
    return n;
}

int game_player_anim(char *out, size_t cap, float *frame) {
    out[0] = 0;
    *frame = NAN;
    uint64_t actor = RT_V143 ? player_actor() : 0, m = actor ? read_ptr(actor + 0x238) : 0;
    uint64_t c = m ? read_ptr(m + 0xc8) : 0, player = c ? read_ptr(c + 0x118) : 0, fska = player ? read_ptr(player + 0x80) : 0;
    char magic[4], text[66];
    uint64_t name = 0;
    if (!fska || !read_guest(fska, magic, 4) || memcmp(magic, "FSKA", 4) || !(name = read_ptr(fska + 8))) return 0;
    float f;
    if (read_guest(player + 0x58, &f, 4) && isfinite(f) && f >= 0 && f < 100000) *frame = f;
    if (!read_guest(name, text, 64)) return 0;
    text[64] = text[65] = 0;
    // the length first (the BFRES string pool); a pointer straight at the text is taken too
    for (int skip = 2; skip >= 0; skip -= 2) {
        const char *t = text + skip;
        size_t len = 0;
        while (len < 62 && (t[len] == '_' || (t[len] >= '0' && t[len] <= '9') || (t[len] >= 'A' && t[len] <= 'Z') ||
                            (t[len] >= 'a' && t[len] <= 'z')))
            len++;
        if (len < 3 || t[len] || len >= cap || (skip && (uint8_t)text[0] + ((uint8_t)text[1] << 8) != len)) continue;
        memcpy(out, t, len), out[len] = 0;
        return 1;
    }
    return 0;
}

// Link's body as the game draws it, 1.4.3 only (measured with anim_probe.c, TOTK_ANIM_PROBE=bonedump and models):
// the model unit [actor+0x238] holds his body's model (Link_RaulSkin) at +0x3e0, whose [+0x458]+0x8 is that model's
// skinning palette for this frame: per matrix_to_bone entry a row-major 3x4, the bone's pose times its inverse bind
// (smooth entries) or the pose itself (rigid ones, after the smooth). It is the final pose: the animation's layers
// blended (the shield held up while walking, the bow drawn), the rag doll of a knock-down, the feet set on the ground.
// The palette is in world axes about a render origin on a 10 m grid (each axis a multiple of 10 m near Link); the
// origin is the cell whose corner puts the body's first entry nearest his place. *out gets each entry in his actor
// space (his place the origin, his right, up and forward the axes), for a friend's figure (link_figure_pose_body).
int game_player_body(float *out, int max) {
    uint64_t actor = RT_V143 ? player_actor() : 0, unit = actor ? read_ptr(actor + 0x238) : 0;
    uint64_t mdl = unit ? read_ptr(unit + 0x3e0) : 0, res = mdl ? read_ptr(mdl + 0x458) : 0, pal = res ? read_ptr(res + 0x8) : 0;
    float p[3], r[9];
    static int why_logged;   // once: why the first read failed (a different game layout shows here)
#define BODY_FAIL(why) return (!why_logged++ ? rt_log("body: %s", why) : (void)0), 0
    if (!pal) BODY_FAIL("no palette at actor->0x238->0x3e0->0x458->0x8");
    if (max <= 0 || max > 128 || !read_guest(actor + 0x2b4, p, 12) || !read_guest(actor + 0x2c0, r, 36) ||
        !read_guest(pal, out, 48ull * (size_t)max))
        BODY_FAIL("palette unreadable");
    for (int i = 0; i < 12 * max; i++)
        if (!isfinite(out[i]) || fabsf(out[i]) > 1e5f) BODY_FAIL("palette holds no transforms");
    float best = 1e9f, local[3] = {0};
    for (int d = 0; d < 27; d++) {   // the render origin: the 10 m cell corner by Link, or one next to it
        float o[3] = {floorf(p[0] / 10) * 10 + (d % 3 - 1) * 10, floorf(p[1] / 10) * 10 + (d / 3 % 3 - 1) * 10,
                      floorf(p[2] / 10) * 10 + (d / 9 - 1) * 10};
        float dx = out[3] + o[0] - p[0], dy = out[7] + o[1] - p[1], dz = out[11] + o[2] - p[2], e = dx * dx + dy * dy + dz * dz;
        if (e < best) best = e, local[0] = p[0] - o[0], local[1] = p[1] - o[1], local[2] = p[2] - o[2];
    }
    if (best > 25) BODY_FAIL("palette more than 5 m from Link");   // not his
#undef BODY_FAIL
    if (!why_logged) why_logged = 1, rt_log("body: Link's palette read (%d entries)", max);
    for (int k = 0; k < max; k++) {   // R^T (S - [0 | place]): rotation and translation into his actor space
        float *m = out + 12 * k, n[12];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) n[4 * i + j] = r[i] * m[j] + r[3 + i] * m[4 + j] + r[6 + i] * m[8 + j];
            n[4 * i + 3] = r[i] * (m[3] - local[0]) + r[3 + i] * (m[7] - local[1]) + r[6 + i] * (m[11] - local[2]);
        }
        memcpy(m, n, sizeof n);
    }
    return max;
}

// Link's life, 1.4.3 only (found with anim_probe.c, TOTK_ANIM_PROBE=life, an enemy made to hit him): a 32-bit count of
// quarter hearts at [actor+0x600]+0x248, +0x298 (the same word several paths lead to). The GameData flag
// PlayerStatus.Life only mirrors it: the game copies the life there as it changes, never back, so writing the flag
// leaves the hearts as they are. The word is taken only while it agrees with the mirror (a sign the path still holds).
static uint64_t life_word(void) {
    uint64_t actor = RT_V143 ? player_actor() : 0, o = actor ? read_ptr(actor + 0x600) : 0, l = o ? read_ptr(o + 0x248) : 0;
    int32_t q, mirror;
    if (!l || !read_guest(l + 0x298, &q, 4) || !game_int_get(game_flag_hash("PlayerStatus.Life"), &mirror) || q != mirror)
        return 0;
    return l + 0x298;
}

uint64_t game_player_life_addr(void) { return life_word(); }

int game_player_life(int *quarters, int *max_quarters) {
    uint64_t a = life_word();
    int32_t q = 0, m = 0;
    if (!a || !read_guest(a, &q, 4)) return 0;
    *quarters = q;
    if (max_quarters) *max_quarters = game_int_get(game_flag_hash("PlayerStatus.MaxLife"), &m) ? m : 0;
    return 1;
}

int game_player_life_set(int quarters) {
    uint64_t a = life_word();
    if (!a || !plat_mem_readable(a, 4) || quarters < 0) return 0;
    *(volatile int32_t *)(uintptr_t)a = quarters;   // the game's pages are the host's
    return 1;
}

static void speed_report(uint64_t now, double step, int interval) {
    if (!win_t0) {
        win_t0 = now;
        return;
    }
    win_frames++;
    win_game += step;
    win_hist[interval < 1 ? 1 : interval > 3 ? 3 : interval]++;
    if (win_frames < 120) return;
    if (probe < 0) {
        const char *e = cfg_get("TOTK_SPEED_PROBE");
        probe = e && !strcmp(e, "1");
    }
    double wall = (double)(now - win_t0) * 1e-9;
    char pos[256] = "";
    float p[3];
    if (probe && player_pos(p)) {
        double d = last_pos_ns ? hypot(p[0] - last_pos[0], p[2] - last_pos[2]) : 0,
               s = last_pos_ns ? (double)(now - last_pos_ns) * 1e-9 : 0;
        float scr[2], cd = 0;   // where the camera puts the player on the window (gfx_world_to_screen's check)
        int on = gfx_world_to_screen(p, scr, &cd);
        snprintf(pos, sizeof pos, "; player %.1f %.1f %.1f, %.2f m in %.3f s = %.2f m/s; on screen %s%.3f %.3f, %.1f m from the camera",
                 p[0], p[1], p[2], d, s, s > 0 ? d / s : 0, on ? "" : "(no camera) ", on ? scr[0] : 0, on ? scr[1] : 0, cd);
        memcpy(last_pos, p, sizeof p);
        last_pos_ns = now;
    }
    if (step_word)
        rt_log("fps: %llu frames in %.3f s (%.1f fps), %.3f s game stepped, speed %.3f; interval 1/2/3+ %llu/%llu/%llu%s",
               (unsigned long long)win_frames, wall, (double)win_frames / wall, win_game, win_game / wall,
               (unsigned long long)win_hist[1], (unsigned long long)win_hist[2], (unsigned long long)win_hist[3], pos);
    else
        rt_log("fps: %llu frames in %.3f s (%.1f fps), whole-vsync steps; interval 1/2/3+ %llu/%llu/%llu%s",
               (unsigned long long)win_frames, wall, (double)win_frames / wall, (unsigned long long)win_hist[1],
               (unsigned long long)win_hist[2], (unsigned long long)win_hist[3], pos);
    win_t0 = now;
    win_frames = 0;
    win_game = 0;
    memset(win_hist, 0, sizeof win_hist);
}

static _Atomic int64_t world_us = -1;
static _Atomic uint64_t game_us;   // game time stepped since the first present (frame_step_ticks)
static uint64_t presents;

int64_t frame_step_world_us(void) { return atomic_load_explicit(&world_us, memory_order_relaxed); }
uint64_t frame_step_ticks(void) { return atomic_load_explicit(&game_us, memory_order_relaxed) * 60 / 1000000; }

static void world_clock(double dt, double step) {
    presents++;
    int64_t w = atomic_load_explicit(&world_us, memory_order_relaxed);
    if (w < 0) {
        float p[3];
        if (!player_pos(p) || fabsf(p[0]) + fabsf(p[2]) < 1.0f) return;   // a few guest reads, until the load ends
        rt_log("frame step: world clock starts at present %llu, %.2f s after the guest started (the player at %.1f %.1f %.1f)",
               (unsigned long long)presents, rt_guest_start_ns ? (kern_now_ns() - rt_guest_start_ns) / 1e9 : 0.0, p[0],
               p[1], p[2]);
        atomic_store_explicit(&world_us, 0, memory_order_relaxed);
        return;
    }
    if (!step_word) step = dt > STEP_MAX_S ? STEP_MAX_S : dt;   // whole-vsync steps: the display time stands in
    atomic_store_explicit(&world_us, w + (int64_t)(step * 1e6 + 0.5), memory_order_relaxed);
}

void frame_step_present(uint64_t display_ns, int interval) {
    frame_limit_live();
    double step = 0, dt = last_ns ? (double)(display_ns - last_ns) * 1e-9 : 0;
    if (step_word && last_ns) {
        if (dt > STEP_RESET_S || dt < 0.001) {
            sample_count = sample_next = 0;
        } else {
            if (dt > STEP_MAX_S) dt = STEP_MAX_S;
            samples[sample_next++ % STEP_SAMPLES] = dt;
            if (sample_count < STEP_SAMPLES) sample_count++;
            double sum = 0;
            for (unsigned i = 0; i < sample_count; i++) sum += samples[i];
            step = sum / sample_count;
            if (step < STEP_MIN_S) step = STEP_MIN_S;
            step_store((float)step);
        }
    }
    last_ns = display_ns;
    // The game clock of untimed script lines: what the game stepped this frame, the patch page's step with the
    // real-time step (kept through a reset), whole vsyncs without it.
    double game = step_word ? step_now : (interval > 0 ? interval : 1) / 60.0;
    atomic_store_explicit(&game_us, atomic_load_explicit(&game_us, memory_order_relaxed) + (uint64_t)(game * 1e6 + 0.5),
                          memory_order_relaxed);
    world_clock(dt, step);
    if (rt_trace & RT_TRACE_PERF) speed_report(display_ns, step, interval);
}
