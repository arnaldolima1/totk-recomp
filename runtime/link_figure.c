// The figure friends in a multiplayer room are drawn as (ui/menu.cpp world_tags): Link's mesh and skeleton with the
// curves of all his animations, built from the player's own game files by tools/link_figure.py into
// <profile>/link-figure.bin (its format is described there); a pose evaluates them at a frame. Loaded once, on the
// first call; a missing or broken file leaves the overlay's flat figure.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "host.h"
#include "link_figure.h"
#include "tex.h"
#include "platform/platform.h"

static LinkFigure fig;
static int tried;

enum { KEY_BYTES = 10 };   // u16 frame x 32, 4 halves

// Unit face normals (bind pose) for the shading, and which way the triangles wind: their normals should point out of
// the body, away from its vertical axis.
static void face_normals(LinkFigure *f, uint32_t nv, uint32_t nt) {
    for (uint32_t t = 0; t < nt; t++) {
        const float *a = f->verts + 3 * f->tris[3 * t], *b = f->verts + 3 * f->tris[3 * t + 1],
                    *c = f->verts + 3 * f->tris[3 * t + 2];
        float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float nx = u[1] * v[2] - u[2] * v[1], ny = u[2] * v[0] - u[0] * v[2], nz = u[0] * v[1] - u[1] * v[0];
        float l = sqrtf(nx * nx + ny * ny + nz * nz);
        if (l < 1e-12f) l = 1;
        f->normals[3 * t] = nx / l, f->normals[3 * t + 1] = ny / l, f->normals[3 * t + 2] = nz / l;
    }
    double cx = 0, cz = 0, out_sum = 0;
    for (uint32_t v = 0; v < nv; v++) cx += f->verts[3 * v], cz += f->verts[3 * v + 2];
    cx /= nv ? nv : 1, cz /= nv ? nv : 1;
    for (uint32_t t = 0; t < nt; t++) {
        const float *a = f->verts + 3 * f->tris[3 * t];
        out_sum += f->normals[3 * t] * (a[0] - cx) + f->normals[3 * t + 2] * (a[2] - cz);
    }
    f->winding = out_sum >= 0 ? 1 : -1;
}

// A picture the game lays over Link's skin by its alpha (the Zonai arm's, tools/link_figure.py OVER_SKIN): laid over
// the skin's tone here (the median of Link_RaulSkin_Alb), opaque.
static void over_skin(uint8_t *px, uint32_t w, uint32_t h) {
    static const uint8_t skin[3] = {153, 116, 87};
    for (uint64_t i = 0; i < 4ull * w * h; i += 4) {
        unsigned a = px[i + 3];
        for (int c = 0; c < 3; c++) px[i + c] = (uint8_t)((px[i + c] * a + skin[c] * (255 - a) + 127) / 255);
        px[i + 3] = 255;
    }
}

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
        uint32_t bw = rd32(d + at + 16), bh = rd32(d + at + 20), bytes = rd32(d + at + 24), over = bw >> 8 & 1;
        bw &= 0xff;
        at += 28;
        if (at + bytes > n || !w || !h || x + w > side || y + h > side || bw < 4 || bh < 4 || bw > 12 || bh > 12 ||
            (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16 > bytes)
            return 0;
        uint8_t *px = malloc(4ull * w * h);
        if (!px) return 0;
        astc_decode_ldr(d + at, w, h, bw, bh, false, px);
        if (over) over_skin(px, w, h);
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
        if (at + 8 <= n && !memcmp(d + at, "PALM", 4)) {   // the game's body palette as the figure's bones
            uint32_t np = rd32(d + at + 4);
            at += 8;
            if (np <= 1024 && at + 4ull * np <= n && (fig.palm_bone = malloc(2ull * np + 2)) && (fig.palm_rigid = malloc(np + 1ull))) {
                for (uint32_t k = 0; k < np; k++) {
                    uint16_t b = (uint16_t)(d[at + 4 * k] | d[at + 4 * k + 1] << 8);
                    fig.palm_bone[k] = b < nb ? b : 0xffff;
                    fig.palm_rigid[k] = d[at + 4 * k + 2] & 1;
                }
                fig.npalm = np;
            }
        }
    }
    face_normals(&fig, nv, nt);
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
            free(fig.anims), free(fig.parent), free(fig.bind), free(fig.inv_bind), free(fig.bone_names), free(fig.palm_bone),
                free(fig.palm_rigid);
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

// ---- the armor library (<profile>/link-armor.bin, tools/link_figure.py --armor): Link's own model and every armor
// model, each skinned to the figure's skeleton with an atlas of its own, which model each worn piece shows, and the
// game's rule for what shows: Link's model and each piece name groups of materials (G_Upper the chest's skin, G_Belt_A
// a belt), each piece names groups of its own model's materials (G_Skin its legs' skin, G_Skirt), and each piece worn
// hides some groups by name (ArmorParam): Link's model's by his groups, its own model's by its groups. A friend's figure
// is dressed from it (link_figure_dressed): Link's model, then the head, upper and lower pieces they wear, or the bare
// ones where they wear none (a head piece carries its own hair; Armor_999_Head is the bare head's), less what the
// pieces worn hide.
enum { LIB_MODEL_HEAD = 68, DRESS_W = 2048, DRESS_H = 1024, DRESS_CACHE = 8, DRESS_PARTS = 4 };
typedef struct {
    char name[48];
    uint32_t nverts, ntris, side, nastc, zsize, nmats;
    const uint8_t *mats;   // its material names (u8 length, bytes)
    const uint8_t *verts, *skin, *tris, *cls, *mtri, *uvs, *zatlas, *astc;   // in the file; mtri: u16 material a triangle
    uint8_t *atlas;        // side x side RGBA8, decoded at its first use
} ArmorModel;
typedef struct {
    char name[48];
    int model;
    const uint8_t *hides, *groups;   // the groups it hides (u8 count, names); its own model's groups
} ArmorPiece;
static struct {
    int tried, nmodels, npieces;
    uint8_t *file;
    const uint8_t *body;   // Link's own material groups
    ArmorPiece *pieces;
    ArmorModel *models;
} lib;

// The length of a string (u8 length, bytes) and of a group list (u8 count, then name, u8 count, names), 0 past end.
static uint64_t str_len(const uint8_t *p, const uint8_t *end) { return p < end && p + 1 + p[0] <= end ? 1ull + p[0] : 0; }
static uint64_t groups_len(const uint8_t *p, const uint8_t *end) {
    if (p >= end) return 0;
    const uint8_t *q = p + 1;
    for (int g = 0; g < p[0]; g++) {
        uint64_t n = str_len(q, end);
        if (!n || q + n >= end) return 0;
        q += n;
        int m = *q++;
        for (int k = 0; k < m; k++) {
            uint64_t l = str_len(q, end);
            if (!l) return 0;
            q += l;
        }
    }
    return (uint64_t)(q - p);
}

// Whether the group list holds group g with material mat.
static int group_has(const uint8_t *groups, const uint8_t *g, const uint8_t *mat) {
    const uint8_t *q = groups + 1;
    for (int i = 0; groups && i < groups[0]; i++) {
        int same = q[0] == g[0] && !memcmp(q + 1, g + 1, g[0]);
        q += 1 + q[0];
        int m = *q++;
        for (int k = 0; k < m; k++) {
            if (same && q[0] == mat[0] && !memcmp(q + 1, mat + 1, mat[0])) return 1;
            q += 1 + q[0];
        }
    }
    return 0;
}

static int lib_load(void) {
    if (lib.tried) return lib.nmodels > 0;
    lib.tried = 1;
    char path[1200];
    snprintf(path, sizeof path, "%s/link-armor.bin", rt_config()->profile);
    uint64_t n = 0;
    uint8_t *d = (uint8_t *)plat_file_read(path, &n);
    if (!d || n < 16 || memcmp(d, "LNKA", 4) || rd32(d + 4) != 2) {
        rt_log("link armor: %s %s; friends show in the Hylian outfit", path, d ? "is not an armor file this build reads" : "not found");
        free(d);
        return 0;
    }
    const uint8_t *end = d + n, *p = d + 16;
    uint32_t nm = rd32(d + 8), na = rd32(d + 12);
    ArmorModel *m = calloc(nm ? nm : 1, sizeof *m);
    ArmorPiece *pc = calloc(na ? na : 1, sizeof *pc);
    uint64_t gl = groups_len(p, end);
    if (!m || !pc || nm > 4096 || na > 8192 || !gl) return free(d), free(m), free(pc), 0;
    lib.body = p, p += gl;
    for (uint32_t i = 0; i < na; i++) {   // the pieces
        if (p + 53 > end) return free(d), free(m), free(pc), 0;
        memcpy(pc[i].name, p, 47);
        int32_t mi;
        memcpy(&mi, p + 48, 4);
        pc[i].model = mi;
        p += 52;
        pc[i].hides = p;
        const uint8_t *q = p + 1;
        for (int k = 0; k < p[0]; k++) {
            uint64_t l = str_len(q, end);
            if (!l) return free(d), free(m), free(pc), 0;
            q += l;
        }
        pc[i].groups = q;
        if (!(gl = groups_len(q, end))) return free(d), free(m), free(pc), 0;
        p = q + gl;
    }
    int ok = 0;
    for (uint32_t i = 0; i < nm; i++, ok++) {   // the models
        ArmorModel *a = &m[i];
        if (p + LIB_MODEL_HEAD + 2 > end) break;
        memcpy(a->name, p, 47);
        a->nverts = rd32(p + 48), a->ntris = rd32(p + 52), a->side = rd32(p + 56), a->nastc = rd32(p + 60);
        a->zsize = rd32(p + 64), a->nmats = (uint32_t)(p[68] | p[69] << 8);
        p += LIB_MODEL_HEAD + 2;
        a->mats = p;
        for (uint32_t k = 0; k < a->nmats; k++) {
            uint64_t l = str_len(p, end);
            if (!l) break;
            p += l;
        }
        uint64_t need = 28ull * a->nverts + 39ull * a->ntris + a->zsize;
        if (a->side < 8 || a->side > 2048 || a->nverts > 1000000 || a->ntris > 1000000 || (uint64_t)(end - p) < need) break;
        a->verts = p, a->skin = a->verts + 12ull * a->nverts, a->tris = a->skin + 16ull * a->nverts;
        a->cls = a->tris + 12ull * a->ntris, a->mtri = a->cls + a->ntris, a->uvs = a->mtri + 2ull * a->ntris;
        a->zatlas = a->uvs + 24ull * a->ntris, a->astc = a->zatlas + a->zsize;
        p += need;
        for (uint32_t k = 0; k < a->nastc && p + 28 <= end; k++) p += 28 + rd32(p + 24);   // the ASTC pictures
        if (p > end) break;
    }
    if (ok < (int)nm) {
        rt_log("link armor: %s is cut short after %d of %u models", path, ok, nm);
        nm = (uint32_t)ok;
    }
    lib.file = d, lib.models = m, lib.nmodels = (int)nm, lib.pieces = pc, lib.npieces = (int)na;
    rt_log("link armor: %d models for %d pieces from %s", lib.nmodels, lib.npieces, path);
    return lib.nmodels > 0;
}

static int lib_model(const char *name) {
    for (int i = 0; name && *name && i < lib.nmodels; i++)
        if (!strcmp(lib.models[i].name, name)) return i;
    return -1;
}

static const ArmorPiece *lib_piece(const char *actor) {   // a worn piece with a model, NULL none
    for (int i = 0; actor && *actor && i < lib.npieces; i++)
        if (!strncmp(lib.pieces[i].name, actor, 47))
            return lib.pieces[i].model >= 0 && lib.pieces[i].model < lib.nmodels ? &lib.pieces[i] : NULL;
    return NULL;
}

// A model's material k (u8 length, bytes).
static const uint8_t *model_mat(const ArmorModel *a, uint32_t k) {
    const uint8_t *p = a->mats;
    for (uint32_t i = 0; i < k && i < a->nmats; i++) p += 1 + p[0];
    return p;
}

// Whether the pieces worn hide material mat of a part: of Link's model (own NULL) by his groups, of a piece's model by
// that piece's groups.
static int hidden(const ArmorPiece *const worn[3], const ArmorPiece *own, const uint8_t *mat) {
    for (int w = 0; w < 3; w++) {
        if (!worn[w]) continue;
        const uint8_t *h = worn[w]->hides + 1;
        for (int k = 0; k < worn[w]->hides[0]; k++, h += 1 + h[0])
            if (group_has(own ? own->groups : lib.body, h, mat)) return 1;
    }
    return 0;
}

static const uint8_t *lib_atlas(ArmorModel *a) {
    if (a->atlas) return a->atlas;
    uint64_t bytes = 4ull * a->side * a->side;
    uint8_t *px = malloc(bytes);
    if (!px || ZSTD_decompress(px, bytes, a->zatlas, a->zsize) != bytes) {
        free(px);
        return NULL;
    }
    const uint8_t *q = a->astc;
    for (uint32_t k = 0; k < a->nastc; k++) {   // ASTC pictures into their place
        uint32_t x = rd32(q), y = rd32(q + 4), w = rd32(q + 8), h = rd32(q + 12), bw = rd32(q + 16), bh = rd32(q + 20);
        uint32_t nb = rd32(q + 24), over = bw >> 8 & 1;
        bw &= 0xff;
        if (w && h && x + w <= a->side && y + h <= a->side && bw >= 4 && bh >= 4 && bw <= 12 && bh <= 12 &&
            (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16 <= nb) {
            uint8_t *t = malloc(4ull * w * h);
            if (t) {
                astc_decode_ldr(q + 28, w, h, bw, bh, false, t);
                if (over) over_skin(t, w, h);
                for (uint32_t r = 0; r < h; r++) memcpy(px + 4ull * ((y + r) * a->side + x), t + 4ull * r * w, 4ull * w);
                free(t);
            }
        }
        q += 28 + nb;
    }
    return a->atlas = px;
}

const LinkFigure *link_figure_dressed(const char look[3][48]) {
    const LinkFigure *base = link_figure();
    if (!base || !base->parent || (!look[0][0] && !look[1][0] && !look[2][0]) || !lib_load()) return base;
    static struct {
        char key[3][48];
        LinkFigure *fig;
    } cache[DRESS_CACHE];
    static int ncache, next;
    for (int i = 0; i < ncache; i++)
        if (!memcmp(cache[i].key, look, sizeof cache[i].key)) return cache[i].fig;
    // the pieces worn, a bare one where none (the ponytail, the chest's strap, the underwear), as the game wears them
    static const char *const bare[3] = {"Armor_999_Head", "Armor_Default_Upper", "Armor_Default_Lower"};
    const ArmorPiece *worn[3];
    for (int k = 0; k < 3; k++) {
        worn[k] = lib_piece(look[k]);
        if (!worn[k]) worn[k] = lib_piece(bare[k]);
    }
    // the parts: Link's model, then the pieces
    const ArmorPiece *own[DRESS_PARTS] = {NULL, worn[0], worn[1], worn[2]};
    int part[DRESS_PARTS] = {lib_model("Link_Body"), -1, -1, -1};
    for (int k = 1; k < DRESS_PARTS; k++) part[k] = own[k] ? own[k]->model : -1;
    uint32_t nv = 0, nt = 0;
    for (int k = 0; k < DRESS_PARTS; k++)
        if (part[k] >= 0) nv += lib.models[part[k]].nverts, nt += lib.models[part[k]].ntris;
    LinkFigure *f = calloc(1, sizeof *f);
    if (!f || !nv || !nt) return free(f), base;
    *f = *base;   // the skeleton and the animations are the base figure's
    f->verts = malloc(12ull * nv), f->skin = malloc(16ull * nv), f->tris = malloc(12ull * nt), f->cls = malloc(nt);
    f->uvs = malloc(24ull * nt), f->normals = malloc(12ull * nt), f->albedo = calloc(1, 4ull * DRESS_W * DRESS_H);
    if (!f->verts || !f->skin || !f->tris || !f->cls || !f->uvs || !f->normals || !f->albedo) {
        free(f->verts), free(f->skin), free(f->tris), free(f->cls), free(f->uvs), free(f->normals), free(f->albedo), free(f);
        return base;
    }
    // the atlas: Link's model's 1024 on the left, then the head, upper and lower 512 each in the right half's
    // quadrants (x, y, side in texels)
    static const uint32_t place[DRESS_PARTS][3] = {{0, 0, 1024}, {1024, 0, 512}, {1536, 0, 512}, {1024, 512, 512}};
    uint32_t v0 = 0, t0 = 0;
    for (int k = 0; k < DRESS_PARTS; k++) {
        if (part[k] < 0) continue;
        ArmorModel *a = &lib.models[part[k]];
        memcpy(f->verts + 3ull * v0, a->verts, 12ull * a->nverts);
        memcpy(f->skin + 8ull * v0, a->skin, 16ull * a->nverts);
        for (uint32_t i = 0; i < 8 * a->nverts; i += 2)   // a bone beyond the skeleton: the root
            if (f->skin[8ull * v0 + i] >= f->nbones) f->skin[8ull * v0 + i] = 0;
        uint32_t side = a->side < place[k][2] ? a->side : place[k][2];   // a larger one is cut to its slot
        float qx = (float)place[k][0] / DRESS_W, qy = (float)place[k][1] / DRESS_H;
        float sx = (float)a->side / DRESS_W, sy = (float)a->side / DRESS_H;
        uint8_t hide[1024] = {0};   // per material: hidden by what is worn
        for (uint32_t m = 0; m < a->nmats && m < 1024; m++) hide[m] = (uint8_t)hidden(worn, own[k], model_mat(a, m));
        uint32_t kept = 0;
        for (uint32_t i = 0; i < a->ntris; i++) {
            uint32_t mat = (uint32_t)(a->mtri[2 * i] | a->mtri[2 * i + 1] << 8);
            if (mat < 1024 && hide[mat]) continue;
            uint32_t o = t0 + kept++;
            for (int c = 0; c < 3; c++) {   // its corners, their place in its slot
                float uv[2];
                memcpy(uv, a->uvs + 24ull * i + 8 * c, 8);
                f->tris[3ull * o + c] = rd32(a->tris + 12ull * i + 4 * c) + v0;
                f->uvs[6ull * o + 2 * c] = qx + uv[0] * sx, f->uvs[6ull * o + 2 * c + 1] = qy + uv[1] * sy;
            }
            f->cls[o] = a->cls[i];
        }
        const uint8_t *px = lib_atlas(a);
        for (uint32_t r = 0; px && r < side; r++)
            memcpy(f->albedo + 4ull * ((uint64_t)(place[k][1] + r) * DRESS_W + place[k][0]), px + 4ull * r * a->side, 4ull * side);
        v0 += a->nverts, t0 += kept;
    }
    nt = t0;
    f->nverts = nv, f->ntris = nt, f->atlas_side = DRESS_W, f->atlas_rows = DRESS_H;
    f->atlas = f->albedo;   // the overlay draws the base figure
    face_normals(f, nv, nt);
    // a slot reused: its old figure stays (the renderer may still hold it this frame), at most DRESS_CACHE kept
    int slot = ncache < DRESS_CACHE ? ncache++ : next++ % DRESS_CACHE;
    memcpy(cache[slot].key, look, sizeof cache[slot].key);
    cache[slot].fig = f;
    rt_log("link armor: dressed a figure in '%s' '%s' '%s' (%u triangles)", look[0], look[1], look[2], nt);
    return f;
}

// ---- a friend's real body pose (net_mp.c POS lines, frame_rate.c game_player_body)
// The palette entries sent: Link_RaulSkin's (tools/link_figure.py body_palette) spine, arms, hands and head (0..33)
// and waist, belts and legs (65..88); the face's entries between are left out (the figure's face keeps its animation).
const uint8_t LF_BODY_ENTRY[LF_BODY_N] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
    29, 30, 31, 32, 33, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88};
enum { BODY_T0 = 0, BODY_T1 = 34 };   // the slots whose translations go too: entries 0 (Spine_1) and 65 (Waist)

static void quat_of(const float *m, float q[4]) {   // a 3x4's rotation (its axes made unit) as a unit quaternion
    float r[9];
    for (int j = 0; j < 3; j++) {
        float l = sqrtf(m[j] * m[j] + m[4 + j] * m[4 + j] + m[8 + j] * m[8 + j]);
        if (l < 1e-6f) l = 1;
        for (int i = 0; i < 3; i++) r[3 * i + j] = m[4 * i + j] / l;
    }
    float tr = r[0] + r[4] + r[8];
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[3] = s / 4, q[0] = (r[7] - r[5]) / s, q[1] = (r[2] - r[6]) / s, q[2] = (r[3] - r[1]) / s;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        float s = sqrtf(1 + r[0] - r[4] - r[8]) * 2;
        q[3] = (r[7] - r[5]) / s, q[0] = s / 4, q[1] = (r[1] + r[3]) / s, q[2] = (r[2] + r[6]) / s;
    } else if (r[4] > r[8]) {
        float s = sqrtf(1 + r[4] - r[0] - r[8]) * 2;
        q[3] = (r[2] - r[6]) / s, q[0] = (r[1] + r[3]) / s, q[1] = s / 4, q[2] = (r[5] + r[7]) / s;
    } else {
        float s = sqrtf(1 + r[8] - r[0] - r[4]) * 2;
        q[3] = (r[3] - r[1]) / s, q[0] = (r[2] + r[6]) / s, q[1] = (r[5] + r[7]) / s, q[2] = s / 4;
    }
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= l > 0 ? l : 1;
    if (q[3] < 0)
        for (int i = 0; i < 4; i++) q[i] = -q[i];
}

static void mat_of(const float q[4], float *m) {   // a unit quaternion as a row-major 3x4's rotation (translation 0)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float r[12] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),     0,
                   2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),     0,
                   2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y), 0};
    memcpy(m, r, sizeof r);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int16_t q16(float v, float scale) {
    float s = v * scale;
    return (int16_t)(s > 32767 ? 32767 : s < -32767 ? -32767 : lrintf(s));
}

void link_body_encode(const float *pal, int count, char *out, size_t cap) {
    out[0] = 0;
    if (count <= LF_BODY_ENTRY[LF_BODY_N - 1] || cap < (LF_BODY_BYTES + 2) / 3 * 4 + 1) return;
    uint8_t raw[(LF_BODY_BYTES + 2) / 3 * 3] = {0};
    int16_t v[LF_BODY_BYTES / 2];
    for (int i = 0; i < LF_BODY_N; i++) {
        float q[4];
        quat_of(pal + 12 * LF_BODY_ENTRY[i], q);
        for (int k = 0; k < 4; k++) v[4 * i + k] = q16(q[k], 32767);
    }
    const int ts[2] = {LF_BODY_ENTRY[BODY_T0], LF_BODY_ENTRY[BODY_T1]};
    for (int j = 0; j < 2; j++)
        for (int k = 0; k < 3; k++) v[4 * LF_BODY_N + 3 * j + k] = q16(pal[12 * ts[j] + 4 * k + 3], 1000);
    memcpy(raw, v, sizeof v);
    size_t o = 0;
    for (size_t i = 0; i < sizeof raw; i += 3) {
        uint32_t w = (uint32_t)raw[i] << 16 | (uint32_t)raw[i + 1] << 8 | raw[i + 2];
        for (int k = 0; k < 4; k++) out[o++] = B64[w >> (18 - 6 * k) & 63];
    }
    out[o] = 0;
}

int link_body_decode(const char *text, LinkBody *out) {
    uint8_t raw[(LF_BODY_BYTES + 2) / 3 * 3];
    size_t len = strlen(text), o = 0;
    if (len != sizeof raw / 3 * 4) return 0;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t w = 0;
        for (int k = 0; k < 4; k++) {
            const char *c = text[i + k] ? strchr(B64, text[i + k]) : NULL;
            if (!c) return 0;
            w = w << 6 | (uint32_t)(c - B64);
        }
        raw[o++] = (uint8_t)(w >> 16), raw[o++] = (uint8_t)(w >> 8), raw[o++] = (uint8_t)w;
    }
    int16_t v[LF_BODY_BYTES / 2];
    memcpy(v, raw, sizeof v);
    for (int i = 0; i < LF_BODY_N; i++) {
        float l = 0;
        for (int k = 0; k < 4; k++) out->q[i][k] = v[4 * i + k] / 32767.0f, l += out->q[i][k] * out->q[i][k];
        if (l < 0.5f || l > 1.5f) return 0;
        for (int k = 0; k < 4; k++) out->q[i][k] /= sqrtf(l);
    }
    for (int j = 0; j < 2; j++)
        for (int k = 0; k < 3; k++) out->t[j][k] = v[4 * LF_BODY_N + 3 * j + k] / 1000.0f;
    return 1;
}

void link_body_lerp(const LinkBody *a, const LinkBody *b, float t, LinkBody *out) {
    for (int i = 0; i < LF_BODY_N; i++) {   // normalised lerp, the short way round
        float d = a->q[i][0] * b->q[i][0] + a->q[i][1] * b->q[i][1] + a->q[i][2] * b->q[i][2] + a->q[i][3] * b->q[i][3];
        float sb = d < 0 ? -t : t, l = 0;
        for (int k = 0; k < 4; k++) out->q[i][k] = a->q[i][k] * (1 - t) + b->q[i][k] * sb, l += out->q[i][k] * out->q[i][k];
        l = sqrtf(l);
        for (int k = 0; k < 4; k++) out->q[i][k] /= l > 1e-6f ? l : 1;
    }
    for (int j = 0; j < 2; j++)
        for (int k = 0; k < 3; k++) out->t[j][k] = a->t[j][k] + (b->t[j][k] - a->t[j][k]) * t;
}

void link_figure_pose_body(const LinkFigure *f, int anim, float frame, const LinkBody *body, float *out) {
    if (!f->npalm || !f->parent || anim < 0 || anim >= f->nanims || f->anims[anim].mats) {
        link_figure_pose(f, anim, frame, out);
        return;
    }
    uint32_t nb = f->nbones;
    float *loc = malloc(36ull * nb), *world = malloc(48ull * nb), *bindw = malloc(48ull * nb), *mats = malloc(48ull * nb);
    int *slot = malloc(sizeof *slot * nb);
    if (!loc || !world || !bindw || !mats || !slot) {
        free(loc), free(world), free(bindw), free(mats), free(slot);
        link_figure_pose(f, anim, frame, out);
        return;
    }
    const LinkAnim *a = &f->anims[anim];
    pose_worlds(f, a, anim_frame(a, frame), loc, world);
    for (uint32_t b = 0; b < nb; b++) {   // the bind world (the figure's own rest), and which slot moves each bone
        float m[12];
        trs(f->bind + 9 * b, m);
        if (f->parent[b] >= 0) mul34(bindw + 12 * f->parent[b], m, bindw + 12 * b);
        else memcpy(bindw + 12 * b, m, 48);
        slot[b] = -1;
    }
    for (int i = LF_BODY_N - 1; i >= 0; i--)   // the first entry for a bone wins
        if (LF_BODY_ENTRY[i] < f->npalm && f->palm_bone[LF_BODY_ENTRY[i]] < nb && !f->palm_rigid[LF_BODY_ENTRY[i]])
            slot[f->palm_bone[LF_BODY_ENTRY[i]]] = i;
    for (uint32_t b = 0; b < nb; b++) {
        float m[12];
        int s = slot[b], p = f->parent[b];
        trs(loc + 9 * b, m);
        if (s < 0) {   // the animation's, under its parent as posed here
            if (p >= 0) mul34(world + 12 * p, m, world + 12 * b);
            continue;
        }
        float r[12], w[12];
        mat_of(body->q[s], r);
        mul34(r, bindw + 12 * b, w);   // the bone's rotation in the friend's actor space
        if (s == BODY_T0 || s == BODY_T1) {   // its place as sent: the entry's translation moves the bind place
            for (int i = 0; i < 3; i++) w[4 * i + 3] += body->t[s == BODY_T1][i];
        } else if (p >= 0) {   // its place hangs from its parent by the animation's offset
            for (int i = 0; i < 3; i++)
                w[4 * i + 3] = world[12 * p + 4 * i] * m[3] + world[12 * p + 4 * i + 1] * m[7] + world[12 * p + 4 * i + 2] * m[11] +
                               world[12 * p + 4 * i + 3];
        }
        memcpy(world + 12 * b, w, 48);
    }
    for (uint32_t b = 0; b < nb; b++) mul34(world + 12 * b, f->inv_bind + 12 * b, mats + 12 * b);
    skin_verts(f, mats, mats, 0, out);
    free(loc), free(world), free(bindw), free(mats), free(slot);
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
