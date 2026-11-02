// What Link carries and where the game draws it, 1.4.3 only: his weapon, shield and bow, in his hand or on his back,
// and the arrow on the string while he draws the bow, for a friend's figure (net_mp.c sends it, runtime/link_figure.c
// draws it).
//
// Which: the GameData pouch (game_flags.c): Pouch.<Weapon|Shield|Bow>.EquipIndex[0] is the pouch slot in use (-1
// none) and Pouch.<kind>.Content.Name[slot] the item's actor (Weapon_Sword_103); the arrow (NormalArrow) only
// while a layer of his animation is Bow_Attack_Charge (game_player_anim_layers), by name alone: the figure holds it in
// its right hand (each draw takes a new arrow object, sooner gone than a search finds it).
// Where: each carried item is an actor with a model object of its own; the model object's +0x90 points at its name
// (Weapon_Sword_103.Weapon_Sword_103, the model file's) and its [+0x458]+0x8 at its skinning palette: per entry a
// row-major 3x4 as the game draws it this frame (the first the model's place; a bow's string bends by the others), in
// the render origin's frame like Link's body (frame_rate.c game_player_body). No fixed path from Link's actor leads to those objects (anim_probe.c TOTK_ANIM_PROBE=models found
// a different one each run), so a background search finds them by name: the name's text in the heap, then the model
// objects pointing at it, the one nearest Link kept. It runs when the item changes or the kept object stops matching.
// The model's name is the library's (link_figure_gear_model: the actor's pack's ModelInfo; Weapon_Sword_168, the
// Wooden Stick, shows Weapon_Sword_103.Weapon_Sword_103); without the library, any model named "<actor>.".
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "link_figure.h"
#include "platform/platform.h"

enum { KINDS = LF_GEAR, MAX_TEXTS = 64, CHUNK = 1 << 20, RETRY_MS = 3000 };
static const char *const kKind[KINDS] = {"Weapon", "Shield", "Bow", "Arrow"};

static struct {
    char actor[64];        // the item in use, "" none
    char model[64];        // its model's name, or "<actor>." to match as a prefix
    int entries;           // the palette entries its vertices use (link_figure_gear_model), 1 without the library
    uint64_t found;        // its model object, 0 not found yet
    uint64_t tried_ms;     // the last search for it
} slot[KINDS];
static PlatMutex lock = PLAT_MUTEX_INIT;
static volatile int32_t searching;

static int rd(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }
static uint64_t rd_ptr(uint64_t a) {
    uint64_t v = 0;
    return rd(a, &v, 8) ? v : 0;
}

// The model object's name is the one wanted (a prefix when it ends with a dot).
static int named(uint64_t model, const char *want) {
    char t[64];
    uint64_t np = rd_ptr(model + 0x90);
    size_t n = strlen(want);
    return np && n && n < 63 && rd(np, t, n + 1) && !memcmp(t, want, n) && (want[n - 1] == '.' || t[n] == 0);
}

// The model's first n palette entries (render origin frame), 0 when it has none.
static int place_of_n(uint64_t model, float *g, int n) {
    uint64_t res = rd_ptr(model + 0x458), pal = res ? rd_ptr(res + 0x8) : 0;
    if (!pal || !rd(pal, g, 48ull * (size_t)n)) return 0;
    for (int i = 0; i < 12 * n; i++)
        if (!isfinite(g[i]) || fabsf(g[i]) > 1e5f) return 0;
    return 1;
}
static int place_of(uint64_t model, float g[12]) { return place_of_n(model, g, 1); }

// Link's frame: his place in the render origin's frame (local) and his rotation, from his body's palette.
static int link_frame(float local[3], float r[9], float body0[3]) {
    uint64_t actor = game_player_actor(), unit = actor ? rd_ptr(actor + 0x238) : 0, mdl = unit ? rd_ptr(unit + 0x3e0) : 0;
    uint64_t res = mdl ? rd_ptr(mdl + 0x458) : 0, pal = res ? rd_ptr(res + 0x8) : 0;
    float p[3], b[12];
    if (!pal || !rd(actor + 0x2b4, p, 12) || !rd(actor + 0x2c0, r, 36) || !rd(pal, b, 48)) return 0;
    float best = 1e9f;
    for (int d = 0; d < 27; d++) {   // the render origin, as game_player_body finds it
        float o[3] = {floorf(p[0] / 10) * 10 + (d % 3 - 1) * 10, floorf(p[1] / 10) * 10 + (d / 3 % 3 - 1) * 10,
                      floorf(p[2] / 10) * 10 + (d / 9 - 1) * 10};
        float dx = b[3] + o[0] - p[0], dy = b[7] + o[1] - p[1], dz = b[11] + o[2] - p[2], e = dx * dx + dy * dy + dz * dz;
        if (e < best) best = e, local[0] = p[0] - o[0], local[1] = p[1] - o[1], local[2] = p[2] - o[2];
    }
    body0[0] = b[3], body0[1] = b[7], body0[2] = b[11];
    return best <= 25;
}

static float dist2(const float g[12], const float c[3]) {
    float dx = g[3] - c[0], dy = g[7] - c[1], dz = g[11] - c[2];
    return dx * dx + dy * dy + dz * dz;
}

// The background search: for each item wanted, the addresses of its name's text ("<actor>."), then every model object
// pointing at one of them; the nearest to Link's body within 4 m is kept.
static void search(void *arg) {
    (void)arg;
    char want[KINDS][64];
    plat_mutex_lock(&lock);
    for (int k = 0; k < KINDS; k++)   // the arrow is never looked for (game_player_gear)
        snprintf(want[k], sizeof want[k], "%s", slot[k].found || k == LF_GEAR_ARROW ? "" : slot[k].model);
    plat_mutex_unlock(&lock);
    uint64_t hb = 0, hs = 0;
    mem_heap_range(&hb, &hs);
    const RtGuestLayout *L = mem_layout();
    // the heap first (the names and the model objects found so far all lived there); the alias region (animation data)
    // only when the heap has no text
    uint64_t ranges[2][2] = {{hb, hs}, {L->alias.base, L->alias.size}};
    static uint64_t texts[KINDS][MAX_TEXTS];
    int ntexts[KINDS] = {0};
    char pat[KINDS][66];
    size_t plen[KINDS];
    for (int k = 0; k < KINDS; k++)   // a whole name with its end, or the prefix
        if ((plen[k] = strlen(want[k])) != 0) memcpy(pat[k], want[k], plen[k] + 1), plen[k] += want[k][plen[k] - 1] != '.';
    uint64_t t0 = plat_now_ms();
    int any = 0;
    for (int r = 0; r < 2 && !(r == 1 && any); r++)   // the texts
        for (uint64_t c = ranges[r][0]; c + 64 < ranges[r][0] + ranges[r][1]; c += CHUNK) {
            uint64_t n = ranges[r][0] + ranges[r][1] - c < CHUNK + 64 ? ranges[r][0] + ranges[r][1] - c : CHUNK + 64;
            if (!plat_mem_readable(c, n)) continue;
            const uint8_t *p = (const uint8_t *)(uintptr_t)c;
            for (int k = 0; k < KINDS; k++) {
                if (!plen[k]) continue;
                for (const uint8_t *q = p; (q = memchr(q, pat[k][0], (size_t)(p + n - plen[k] - q))) != NULL; q++) {
                    if (memcmp(q, pat[k], plen[k]) || (q > p && q[-1] >= '0' && q[-1] <= 'z')) continue;
                    if (ntexts[k] < MAX_TEXTS && (uint64_t)(q - p) < CHUNK) texts[k][ntexts[k]++] = (uint64_t)(uintptr_t)q, any = 1;
                }
            }
        }
    uint64_t t1 = plat_now_ms(), lo = UINT64_MAX, hi = 0;
    for (int k = 0; k < KINDS; k++)
        for (int t = 0; t < ntexts[k]; t++) lo = texts[k][t] < lo ? texts[k][t] : lo, hi = texts[k][t] > hi ? texts[k][t] : hi;
    float local[3], rot[9], body0[3];
    uint64_t best[KINDS] = {0};
    float bestd[KINDS];
    for (int k = 0; k < KINDS; k++) bestd[k] = 16;   // within 4 m of the body
    int have_frame = link_frame(local, rot, body0);
    for (int r = 0; r < 1 && have_frame && hi; r++)   // the model objects pointing at them (+0x90), in the heap
        for (uint64_t c = ranges[r][0]; c < ranges[r][0] + ranges[r][1]; c += CHUNK) {
            uint64_t n = ranges[r][0] + ranges[r][1] - c < CHUNK ? ranges[r][0] + ranges[r][1] - c : CHUNK;
            if (!plat_mem_readable(c, n)) continue;
            const uint64_t *w = (const uint64_t *)(uintptr_t)c;
            for (uint64_t i = 0; i < n / 8; i++) {
                uint64_t v = w[i];
                if (v < lo || v > hi) continue;
                for (int k = 0; k < KINDS; k++)
                    for (int t = 0; t < ntexts[k]; t++) {
                        if (v != texts[k][t]) continue;
                        uint64_t m = c + 8 * i - 0x90;
                        float g[12];
                        if (place_of(m, g) && dist2(g, body0) < bestd[k]) bestd[k] = dist2(g, body0), best[k] = m;
                    }
            }
        }
    plat_mutex_lock(&lock);
    for (int k = 0; k < KINDS; k++)
        if (best[k] && !strcmp(slot[k].model, want[k])) slot[k].found = best[k];
    plat_mutex_unlock(&lock);
    for (int k = 0; k < KINDS; k++)
        if (want[k][0])
            rt_log("gear: %s %s: %d texts, model %#llx (%.1f m), %llu + %llu ms", kKind[k], want[k], ntexts[k],
                   (unsigned long long)best[k], sqrtf(bestd[k]), (unsigned long long)(t1 - t0), (unsigned long long)(plat_now_ms() - t1));
    __atomic_store_n(&searching, 0, __ATOMIC_RELEASE);
}

int game_player_gear(GameGear *out, int max) {
    if (!RT_V143) return 0;
    uint64_t now = plat_now_ms();
    int need = 0, n = 0;
    float local[3], r[9], body0[3];
    int framed = link_frame(local, r, body0), drawing = 0;
    char lay[8][64];
    for (int i = 0, nl = game_player_anim_layers(lay, 8); i < nl && i < 3; i++) drawing |= !strncmp(lay[i], "Bow_Attack_Charge", 17);
    plat_mutex_lock(&lock);
    for (int k = 0; k < KINDS; k++) {
        char key[64], name[64] = "";
        int32_t idx = -1;
        if (k == LF_GEAR_ARROW) {   // named alone while he draws: each draw takes a new arrow, quicker than a search
            if (drawing) snprintf(name, sizeof name, "NormalArrow");
        } else {
            snprintf(key, sizeof key, "Pouch.%s.EquipIndex", kKind[k]);
            if (game_int_array_get(game_flag_hash(key), 0, &idx) && idx >= 0) {
                snprintf(key, sizeof key, "Pouch.%s.Content.Name", kKind[k]);
                if (!game_str_array_get(game_flag_hash(key), idx, name)) name[0] = 0;
            }
        }
        if (strcmp(name, slot[k].actor)) {
            memcpy(slot[k].actor, name, sizeof name), slot[k].found = 0, slot[k].tried_ms = 0;
            char m[48];
            slot[k].entries = 1;
            if (link_figure_gear_model(name, m, &slot[k].entries)) snprintf(slot[k].model, sizeof slot[k].model, "%s", m);
            else snprintf(slot[k].model, sizeof slot[k].model, "%.62s.", name);
        }
        if (k >= max) continue;
        out[k].model[0] = 0, out[k].n = 0;
        n = k + 1;
        if (!name[0]) continue;
        float g[LF_GEAR_BONES * 12];
        int ne = slot[k].entries;
        snprintf(out[k].model, sizeof out[k].model, "%s", name);   // named while it is looked for: held in the hand
        if (k == LF_GEAR_ARROW) continue;   // the figure holds it in the right hand, by the body's pose
        if (slot[k].found) {
            int nm = named(slot[k].found, slot[k].model), pl = nm && place_of_n(slot[k].found, g, ne);
            if (!(nm && pl && framed && dist2(g, body0) < 16)) {   // gone or moved away: found again
                static int why;
                if (why++ < 20)
                    rt_log("gear: %s lost (named %d, placed %d, framed %d, %.1f m)", kKind[k], nm, pl, framed,
                           pl ? sqrtf(dist2(g, body0)) : -1.0f);
                slot[k].found = 0;
            }
        }
        if (!slot[k].found) {
            if (now - slot[k].tried_ms >= RETRY_MS) need = 1, slot[k].tried_ms = now;
            continue;
        }
        out[k].n = ne;
        for (int e = 0; e < ne; e++) {
            const float *ge = g + 12 * e;
            float *o = out[k].m[e];
            for (int i = 0; i < 3; i++) {   // R^T (G - [0 | place]), as game_player_body
                for (int j = 0; j < 3; j++) o[4 * i + j] = r[i] * ge[j] + r[3 + i] * ge[4 + j] + r[6 + i] * ge[8 + j];
                o[4 * i + 3] = r[i] * (ge[3] - local[0]) + r[3 + i] * (ge[7] - local[1]) + r[6 + i] * (ge[11] - local[2]);
            }
        }
    }
    plat_mutex_unlock(&lock);
    if (need && framed && plat_cas32(&searching, 1, 0) == 0 && !plat_thread_spawn(search, NULL, "gear search", 0))
        __atomic_store_n(&searching, 0, __ATOMIC_RELEASE);
    return n;
}
