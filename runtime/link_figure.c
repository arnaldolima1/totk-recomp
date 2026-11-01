// The figure friends in a multiplayer room are drawn as (ui/menu.cpp world_tags): Link's mesh posed with the arms
// down, built from the player's own game files by tools/link_figure.py into <profile>/link-figure.bin (its format is
// described there). Loaded once, on the first call; a missing or broken file leaves the overlay's flat figure.
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

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int parse(const uint8_t *d, uint64_t n) {
    if (n < 24 || memcmp(d, "LNKF", 4) || rd32(d + 4) != 3) return 0;
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
    // skinning: bones, four (bone, weight) per vertex, and the animations' baked matrices
    if (at + 8 > n) return 0;
    uint32_t nb = rd32(d + at), na = rd32(d + at + 4);
    at += 8;
    if (!nb || nb > 1024 || na > LF_MAX_ANIMS || at + 16ull * nv > n) return 0;
    fig.nbones = nb;
    fig.skin = malloc(16ull * nv);   // four (u16 bone, u16 weight) a vertex
    if (!fig.skin) return 0;
    memcpy(fig.skin, d + at, 16ull * nv), at += 16ull * nv;
    for (uint32_t v = 0; v < nv; v++)
        for (int k = 0; k < 4; k++)
            if (fig.skin[v * 8 + k * 2] >= nb) return 0;
    for (uint32_t a = 0; a < na; a++) {
        if (at + 40 > n) return 0;
        LinkAnim *an = &fig.anims[a];
        memcpy(an->name, d + at, 31), an->name[31] = 0;
        an->frames = rd32(d + at + 32);
        memcpy(&an->speed, d + at + 36, 4);
        at += 40;
        uint64_t bytes = 48ull * nb * an->frames;
        if (!an->frames || an->frames > 10000 || at + bytes > n || !(an->mats = malloc(bytes))) return 0;
        memcpy(an->mats, d + at, bytes), at += bytes;
        fig.nanims++;
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
        if (d && parse(d, n)) rt_log("link figure: %u vertices, %u triangles from %s", fig.nverts, fig.ntris, path);
        else {
            rt_log("link figure: %s %s; friends are drawn flat", path, d ? "is not a figure this build reads" : "not found");
            free(fig.verts), free(fig.tris), free(fig.cls), free(fig.normals), free(fig.uvs), free(fig.atlas), free(fig.albedo), free(fig.skin);
            for (int a = 0; a < LF_MAX_ANIMS; a++) free(fig.anims[a].mats);
            memset(&fig, 0, sizeof fig);
        }
        free(d);
    }
    return fig.ntris ? &fig : NULL;
}

// Skins every vertex for animation anim at frame (fractional, wrapped), blending the two baked frames around it.
void link_figure_pose(const LinkFigure *f, int anim, float frame, float *out) {
    if (anim < 0 || anim >= f->nanims) {
        memcpy(out, f->verts, 12ull * f->nverts);
        return;
    }
    const LinkAnim *a = &f->anims[anim];
    float fr = fmodf(frame, (float)a->frames);
    if (fr < 0) fr += (float)a->frames;
    uint32_t f0 = (uint32_t)fr % a->frames, f1 = (f0 + 1) % a->frames;
    float t = fr - floorf(fr);
    const float *m0 = a->mats + 12ull * f->nbones * f0, *m1 = a->mats + 12ull * f->nbones * f1;
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
                float rb = B[4 * r] * p[0] + B[4 * r + 1] * p[1] + B[4 * r + 2] * p[2] + B[4 * r + 3];
                acc[r] += w * (ra + (rb - ra) * t);
            }
        }
        out[3 * v] = acc[0], out[3 * v + 1] = acc[1], out[3 * v + 2] = acc[2];
    }
}
