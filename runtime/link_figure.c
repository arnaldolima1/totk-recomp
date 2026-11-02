// The figure friends in a multiplayer room are drawn as (ui/menu.cpp world_tags): Link's mesh and skeleton with the
// curves of all his animations, built from the player's own game files by tools/link_figure.py into
// <profile>/link-figure.bin (its format is described there); a pose evaluates them at a frame. Loaded once, on the
// first call; a missing or broken file leaves the overlay's flat figure.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "link_figure.h"
#include "tex.h"
#include "platform/platform.h"

static LinkFigure fig;
static int tried;

enum { KEY_BYTES = 10 };   // u16 frame x 32, 4 halves

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int parse(const uint8_t *d, uint64_t n) {
    uint32_t version = n >= 8 ? rd32(d + 4) : 0;
    if (n < 24 || memcmp(d, "LNKF", 4) || (version != 3 && version != 4)) return 0;
    uint32_t nv = rd32(d + 8), nt = rd32(d + 12), side = rd32(d + 16), nastc = rd32(d + 20);
    if (!nv || !nt || nv > 1000000 || nt > 1000000 || side < 8 || side > 4096) return 0;
    uint64_t at = 24, need = at + 12ull * nv + 12ull * nt + nt + 24ull * nt + 4ull * side * side;
    if (need > n) return 0;
    fig.verts = malloc(12ull * nv), fig.tris = malloc(12ull * nt), fig.cls = malloc(nt), fig.normals = malloc(12ull * nt);
    fig.uvs = malloc(24ull * nt), fig.atlas = malloc(4ull * side * side);
    if (!fig.verts || !fig.tris || !fig.cls || !fig.normals || !fig.uvs || !fig.atlas) return 0;
    memcpy(fig.verts, d + at, 12ull * nv), at += 12ull * nv;
    memcpy(fig.tris, d + at, 12ull * nt), at += 12ull * nt;
    memcpy(fig.cls, d + at, nt), at += nt;
    memcpy(fig.uvs, d + at, 24ull * nt), at += 24ull * nt;
    memcpy(fig.atlas, d + at, 4ull * side * side), at += 4ull * side * side;
    fig.atlas_side = side;
    for (uint32_t i = 0; i < 3 * nt; i++)
        if (fig.tris[i] >= nv) return 0;
    for (uint32_t k = 0; k < nastc; k++) {   // ASTC pictures, decoded into their place in the atlas
        if (at + 28 > n) return 0;
        uint32_t x = rd32(d + at), y = rd32(d + at + 4), w = rd32(d + at + 8), h = rd32(d + at + 12);
        uint32_t bw = rd32(d + at + 16), bh = rd32(d + at + 20), bytes = rd32(d + at + 24);
        at += 28;
        if (at + bytes > n || !w || !h || x + w > side || y + h > side || bw < 4 || bh < 4 || bw > 12 || bh > 12 ||
            (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16 > bytes)
            return 0;
        uint8_t *px = malloc(4ull * w * h);
        if (!px) return 0;
        astc_decode_ldr(d + at, w, h, bw, bh, false, px);
        for (uint32_t r = 0; r < h; r++) memcpy(fig.atlas + 4ull * ((y + r) * side + x), px + 4ull * r * w, 4ull * w);
        free(px);
        at += bytes;
    }
    // skinning: bones, four (bone, weight) per vertex, then the animations (version 3: baked matrices; version 4: the
    // skeleton and every animation's curves)
    if (at + 8 > n) return 0;
    uint32_t nb = rd32(d + at), na = rd32(d + at + 4);
    at += 8;
    if (!nb || nb > 1024 || na > 4096 || at + 16ull * nv > n) return 0;
    fig.nbones = nb;
    fig.skin = malloc(16ull * nv);   // four (u16 bone, u16 weight) a vertex
    if (!fig.skin) return 0;
    memcpy(fig.skin, d + at, 16ull * nv), at += 16ull * nv;
    for (uint32_t v = 0; v < nv; v++)
        for (int k = 0; k < 4; k++)
            if (fig.skin[v * 8 + k * 2] >= nb) return 0;
    if (!(fig.anims = calloc(na ? na : 1, sizeof *fig.anims))) return 0;
    if (version == 3) {
        for (uint32_t a = 0; a < na; a++) {
            if (at + 40 > n) return 0;
            LinkAnim *an = &fig.anims[a];
            memcpy(an->name, d + at, 31), an->name[31] = 0;
            an->frames = rd32(d + at + 32), an->loop = 1;
            memcpy(&an->speed, d + at + 36, 4);
            at += 40;
            uint64_t bytes = 48ull * nb * an->frames;
            if (!an->frames || an->frames > 10000 || at + bytes > n || !(an->mats = malloc(bytes))) return 0;
            memcpy(an->mats, d + at, bytes), at += bytes;
            fig.nanims++;
        }
    } else {
        if (at + 4 + 88ull * nb > n) return 0;
        uint32_t root = rd32(d + at);
        at += 4;
        fig.root = root < nb ? (int)root : -1;
        fig.parent = malloc(4ull * nb), fig.bind = malloc(36ull * nb), fig.inv_bind = malloc(48ull * nb);
        if (!fig.parent || !fig.bind || !fig.inv_bind) return 0;
        for (uint32_t b = 0; b < nb; b++, at += 88) {
            memcpy(&fig.parent[b], d + at, 4);
            memcpy(fig.bind + 9 * b, d + at + 4, 36);
            memcpy(fig.inv_bind + 12 * b, d + at + 40, 48);
            if (fig.parent[b] >= (int32_t)b) return 0;   // parents first: the pose walks the bones in order
        }
        if (at + 32ull * nb > n || !(fig.bone_names = malloc(32ull * nb))) return 0;
        memcpy(fig.bone_names, d + at, 32ull * nb), at += 32ull * nb;
        for (uint32_t b = 0; b < nb; b++) fig.bone_names[32 * b + 31] = 0;
        for (uint32_t a = 0; a < na; a++) {
            if (at + 72 > n) return 0;
            LinkAnim *an = &fig.anims[a];
            memcpy(an->name, d + at, 47), an->name[47] = 0;
            an->frames = rd32(d + at + 48), an->loop = rd32(d + at + 52) & 1;
            memcpy(&an->speed, d + at + 56, 4);
            an->nbones = rd32(d + at + 60), an->ncurves = rd32(d + at + 64);
            uint32_t nkeys = rd32(d + at + 68);
            at += 72;
            uint64_t bytes = 40ull * an->nbones + 4ull * an->ncurves + (uint64_t)KEY_BYTES * nkeys;
            if (!an->frames || an->frames > 100000 || an->nbones > nb || at + bytes > n) return 0;
            an->bones = d + at, an->curves = d + at + 40ull * an->nbones;
            const uint8_t *key = an->curves + 4ull * an->ncurves;
            if (!(an->keys = malloc(sizeof *an->keys * (an->ncurves ? an->ncurves : 1)))) return 0;
            uint32_t c = 0, seen = 0;
            for (uint32_t b = 0; b < an->nbones; b++) {
                const uint8_t *bo = an->bones + 40ull * b;
                if ((uint32_t)(bo[0] | bo[1] << 8) >= nb) return 0;
                seen += (uint32_t)(bo[2] | bo[3] << 8);
            }
            if (seen != an->ncurves) return 0;
            for (uint64_t k = 0; c < an->ncurves; c++) {
                const uint8_t *cu = an->curves + 4ull * c;
                uint32_t cn = (uint32_t)(cu[2] | cu[3] << 8);
                if (cu[0] > 8 || !cn || k + cn > nkeys) return 0;
                an->keys[c] = key + (uint64_t)KEY_BYTES * k;
                k += cn;
            }
            at += bytes;
            fig.nanims++;
        }
    }
    for (uint32_t t = 0; t < nt; t++) {   // unit face normals, for the shading
        const float *a = fig.verts + 3 * fig.tris[3 * t], *b = fig.verts + 3 * fig.tris[3 * t + 1],
                    *c = fig.verts + 3 * fig.tris[3 * t + 2];
        float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float nx = u[1] * v[2] - u[2] * v[1], ny = u[2] * v[0] - u[0] * v[2], nz = u[0] * v[1] - u[1] * v[0];
        float l = sqrtf(nx * nx + ny * ny + nz * nz);
        if (l < 1e-12f) l = 1;
        fig.normals[3 * t] = nx / l, fig.normals[3 * t + 1] = ny / l, fig.normals[3 * t + 2] = nz / l;
    }
    // which way the triangles wind: their normals should point out of the body (away from its vertical axis)
    double cx = 0, cz = 0, out_sum = 0;
    for (uint32_t v = 0; v < nv; v++) cx += fig.verts[3 * v], cz += fig.verts[3 * v + 2];
    cx /= nv, cz /= nv;
    for (uint32_t t = 0; t < nt; t++) {
        const float *a = fig.verts + 3 * fig.tris[3 * t];
        out_sum += fig.normals[3 * t] * (a[0] - cx) + fig.normals[3 * t + 2] * (a[2] - cz);
    }
    fig.winding = out_sum >= 0 ? 1 : -1;
    // the atlas as stored (the G-buffer figure, vk_figure.c, is lit by the game); the overlay's copy is lifted, as the
    // albedo is darker than the lit picture the game shows and the overlay can only darken it
    if (!(fig.albedo = malloc(4ull * side * side))) return 0;
    memcpy(fig.albedo, fig.atlas, 4ull * side * side);
    for (uint64_t i = 0; i < 4ull * side * side; i += 4)
        for (int c = 0; c < 3; c++) {
            unsigned v = fig.atlas[i + c] * 135u / 100u;
            fig.atlas[i + c] = (uint8_t)(v > 255 ? 255 : v);
        }
    fig.nverts = nv, fig.ntris = nt;
    return 1;
}

const LinkFigure *link_figure(void) {
    if (!tried) {
        tried = 1;
        char path[1200];
        snprintf(path, sizeof path, "%s/link-figure.bin", rt_config()->profile);
        uint64_t n = 0;
        uint8_t *d = (uint8_t *)plat_file_read(path, &n);
        if (d && parse(d, n)) {
            rt_log("link figure: %u vertices, %u triangles, %d animations from %s", fig.nverts, fig.ntris, fig.nanims, path);
            if (fig.parent) d = NULL;   // version 4: the animations point into the file, which stays for good
        } else {
            rt_log("link figure: %s %s; friends are drawn flat", path, d ? "is not a figure this build reads" : "not found");
            free(fig.verts), free(fig.tris), free(fig.cls), free(fig.normals), free(fig.uvs), free(fig.atlas), free(fig.albedo), free(fig.skin);
            for (int a = 0; fig.anims && a < fig.nanims; a++) free(fig.anims[a].mats), free((void *)fig.anims[a].keys);
            free(fig.anims), free(fig.parent), free(fig.bind), free(fig.inv_bind), free(fig.bone_names);
            memset(&fig, 0, sizeof fig);
        }
        free(d);
    }
    return fig.ntris ? &fig : NULL;
}

int link_figure_find(const LinkFigure *f, const char *name) {
    for (int a = 0; name && *name && a < f->nanims; a++)
        if (!strcmp(f->anims[a].name, name)) return a;
    return -1;
}

static float half(const uint8_t *p) {
    uint32_t h = (uint32_t)(p[0] | p[1] << 8), sign = (h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 0x3ff, bits;
    if (!e) {   // zero or subnormal
        if (!m) bits = sign;
        else {
            e = 113;
            while (!(m & 0x400)) m <<= 1, e--;
            bits = sign | e << 23 | (m & 0x3ff) << 13;
        }
    } else if (e == 31) bits = sign | 0x7f800000 | m << 13;
    else bits = sign | (e + 112) << 23 | m << 13;
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

// A curve's value at frame f: the cubic of the last key at or before f, over that key's segment to the next.
static float curve_at(const uint8_t *keys, uint32_t n, float f) {
    float f32 = f * 32.0f;
    uint32_t lo = 0, hi = n - 1;
    if (f32 <= (float)(keys[0] | keys[1] << 8)) hi = 0;
    while (lo < hi) {   // the last key with frame <= f
        uint32_t mid = (lo + hi + 1) / 2;
        const uint8_t *k = keys + (uint64_t)KEY_BYTES * mid;
        if ((float)(k[0] | k[1] << 8) <= f32) lo = mid;
        else hi = mid - 1;
    }
    const uint8_t *k = keys + (uint64_t)KEY_BYTES * lo;
    float t = 0, a = (float)(k[0] | k[1] << 8);
    if (lo + 1 < n) {
        float b = (float)(k[KEY_BYTES] | k[KEY_BYTES + 1] << 8);
        if (b > a) t = (f32 - a) / (b - a);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
    }
    return half(k + 2) + t * (half(k + 4) + t * (half(k + 6) + t * half(k + 8)));
}

// Scale, euler XYZ and translation as a row-major 3x4 (tools/link_figure.py trs and euler_xyz).
static void trs(const float *v, float *m) {
    float cx = cosf(v[3]), sx = sinf(v[3]), cy = cosf(v[4]), sy = sinf(v[4]), cz = cosf(v[5]), sz = sinf(v[5]);
    float r[9] = {cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz,
                  cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz,
                  -sy,     sx * cy,                cx * cy};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[4 * i + j] = r[3 * i + j] * v[j];
        m[4 * i + 3] = v[6 + i];
    }
}

static void mul34(const float *a, const float *b, float *out) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + (j == 3 ? a[4 * i + 3] : 0);
}

// Version 4: each bone's posed world matrix (model space) for animation a at frame fr.
static void pose_worlds(const LinkFigure *f, const LinkAnim *a, float fr, float *loc, float *world) {
    uint32_t nb = f->nbones;
    memcpy(loc, f->bind, 36ull * nb);
    uint32_t c = 0;
    for (uint32_t i = 0; i < a->nbones; i++) {
        const uint8_t *bo = a->bones + 40ull * i;
        uint32_t b = (uint32_t)(bo[0] | bo[1] << 8), nc = (uint32_t)(bo[2] | bo[3] << 8);
        float *v = loc + 9 * b;
        memcpy(v, bo + 4, 36);
        // The root bone's curves are the animation's own motion (a turn's 180 degrees, a climb's step up, a run's
        // stride): the game moves and turns the actor by it, and the friend's place and facing (net_mp.c) are the
        // actor's, so the root holds where its curves start, or the figure would turn and move twice. Its still
        // transform stays: swimming keeps the body 1.4 m below the actor's place that way.
        float at = (int)b == f->root ? 0 : fr;
        for (uint32_t k = 0; k < nc; k++, c++) {
            const uint8_t *cu = a->curves + 4ull * c;
            v[cu[0]] = curve_at(a->keys[c], (uint32_t)(cu[2] | cu[3] << 8), at);
        }
    }
    for (uint32_t b = 0; b < nb; b++) {
        float m[12];
        trs(loc + 9 * b, m);
        if (f->parent[b] >= 0) mul34(world + 12 * f->parent[b], m, world + 12 * b);
        else memcpy(world + 12 * b, m, 48);
    }
}

// The frame an animation shows at frame: a looping one wraps, another holds its last frame.
static float anim_frame(const LinkAnim *a, float frame) {
    if (a->loop || a->mats) {
        float fr = fmodf(frame, (float)a->frames);
        return fr < 0 ? fr + (float)a->frames : fr;
    }
    float last = a->frames > 1 ? (float)(a->frames - 1) : 0;
    return frame < 0 ? 0 : frame > last ? last : frame;
}

// Version 4: each bone's skinning matrix (posed world x inverse bind world) for animation a at frame fr.
static void skin_mats(const LinkFigure *f, const LinkAnim *a, float fr, float *out) {
    uint32_t nb = f->nbones;
    float *loc = malloc(36ull * nb), *world = malloc(48ull * nb);
    if (!loc || !world) {
        free(loc), free(world);
        for (uint32_t b = 0; b < nb; b++) memset(out + 12 * b, 0, 48), out[12 * b] = out[12 * b + 5] = out[12 * b + 10] = 1;
        return;
    }
    pose_worlds(f, a, fr, loc, world);
    for (uint32_t b = 0; b < nb; b++) mul34(world + 12 * b, f->inv_bind + 12 * b, out + 12 * b);
    free(loc), free(world);
}

float link_figure_root_yaw(const LinkFigure *f, int anim, float frame) {
    if (anim < 0 || anim >= f->nanims || f->root < 0 || f->anims[anim].mats) return 0;
    const LinkAnim *a = &f->anims[anim];
    float fr = anim_frame(a, frame);
    uint32_t c = 0;
    for (uint32_t i = 0; i < a->nbones; i++) {
        const uint8_t *bo = a->bones + 40ull * i;
        uint32_t b = (uint32_t)(bo[0] | bo[1] << 8), nc = (uint32_t)(bo[2] | bo[3] << 8);
        for (uint32_t k = 0; k < nc; k++, c++) {
            const uint8_t *cu = a->curves + 4ull * c;
            if ((int)b == f->root && cu[0] == 4) return curve_at(a->keys[c], (uint32_t)(cu[2] | cu[3] << 8), fr);
        }
        if ((int)b == f->root) {   // no curve: its still turn
            float y;
            memcpy(&y, bo + 4 + 4 * 4, 4);
            return y;
        }
    }
    return 0;
}

void link_figure_rotate(const LinkFigure *f, float *verts, const float m[9]) {
    for (uint32_t i = 0; i < f->nverts; i++) {
        float *v = verts + 3 * i, x = v[0], y = v[1], z = v[2];
        v[0] = m[0] * x + m[1] * y + m[2] * z, v[1] = m[3] * x + m[4] * y + m[5] * z, v[2] = m[6] * x + m[7] * y + m[8] * z;
    }
}

void link_figure_orient(const float q[4], float turn, float m[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                  2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                  2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    float c = cosf(turn), s = sinf(turn);   // then about the figure's own vertical: r Ry(turn)
    for (int i = 0; i < 3; i++) {
        m[3 * i] = r[3 * i] * c - r[3 * i + 2] * s;
        m[3 * i + 1] = r[3 * i + 1];
        m[3 * i + 2] = r[3 * i] * s + r[3 * i + 2] * c;
    }
}

int link_figure_bone(const LinkFigure *f, const char *name) {
    for (uint32_t b = 0; f->bone_names && b < f->nbones; b++)
        if (!strcmp(f->bone_names + 32 * b, name)) return (int)b;
    return -1;
}

int link_figure_bones_at(const LinkFigure *f, int anim, float frame, const int *bones, int n, float (*pos)[3]) {
    if (anim < 0 || anim >= f->nanims || f->anims[anim].mats || !f->parent) return 0;
    uint32_t nb = f->nbones;
    float *loc = malloc(36ull * nb), *world = malloc(48ull * nb);
    if (!loc || !world) {
        free(loc), free(world);
        return 0;
    }
    pose_worlds(f, &f->anims[anim], anim_frame(&f->anims[anim], frame), loc, world);
    for (int i = 0; i < n; i++) {
        int b = bones[i];
        for (int k = 0; k < 3; k++) pos[i][k] = b >= 0 && (uint32_t)b < nb ? world[12 * b + 4 * k + 3] : 0;
    }
    free(loc), free(world);
    return 1;
}

static void skin_verts(const LinkFigure *f, const float *m0, const float *m1, float t, float *out) {
    for (uint32_t v = 0; v < f->nverts; v++) {
        const float *p = f->verts + 3 * v;
        const uint16_t *s = f->skin + 8 * v;
        float acc[3] = {0, 0, 0};
        for (int k = 0; k < 4; k++) {
            float w = s[2 * k + 1] * (1.0f / 65535.0f);
            if (w <= 0) continue;
            const float *A = m0 + 12 * s[2 * k], *B = m1 + 12 * s[2 * k];
            for (int r = 0; r < 3; r++) {
                float ra = A[4 * r] * p[0] + A[4 * r + 1] * p[1] + A[4 * r + 2] * p[2] + A[4 * r + 3];
                if (t > 0) {
                    float rb = B[4 * r] * p[0] + B[4 * r + 1] * p[1] + B[4 * r + 2] * p[2] + B[4 * r + 3];
                    ra += (rb - ra) * t;
                }
                acc[r] += w * ra;
            }
        }
        out[3 * v] = acc[0], out[3 * v + 1] = acc[1], out[3 * v + 2] = acc[2];
    }
}

// Skins every vertex for animation anim at frame: version 3 blends the two baked frames around it, version 4
// evaluates the curves there.
void link_figure_pose(const LinkFigure *f, int anim, float frame, float *out) {
    if (anim < 0 || anim >= f->nanims) {
        memcpy(out, f->verts, 12ull * f->nverts);
        return;
    }
    const LinkAnim *a = &f->anims[anim];
    float fr = anim_frame(a, frame);
    if (!a->mats) {
        float *m = malloc(48ull * f->nbones);
        if (!m) {
            memcpy(out, f->verts, 12ull * f->nverts);
            return;
        }
        skin_mats(f, a, fr, m);
        skin_verts(f, m, m, 0, out);
        free(m);
        return;
    }
    uint32_t f0 = (uint32_t)fr % a->frames, f1 = (f0 + 1) % a->frames;
    skin_verts(f, a->mats + 12ull * f->nbones * f0, a->mats + 12ull * f->nbones * f1, fr - floorf(fr), out);
}
