// Occlusion of world points against the game's scene depth, for the friends drawn in the world (ui/menu.cpp
// world_tags): is something of the scene between the camera and the point?
//
// The G-buffer pass (vk_exec_draw.c cam_take) arms a test with its depth target, its viewport and its camera. When the
// replay begins the next pass on another depth target, the G-buffer depth is complete: one 1x1 copy per point goes
// from it into a host-visible buffer, and when the submission completes (vk_completion_add) each sampled depth becomes
// a distance along the view axis, compared with the point's own. The points come from the overlay (gfx_occlusion);
// the first is always the player's chest (game_player_pos), which the camera sees, as a check: TOTK_SPEED_PROBE=1 logs
// it every few seconds.
//
// The overlay can also ask for a rectangle of the depth (gfx_depth_want, in NDC): the friends' figures are tested
// triangle by triangle against it (gfx_depth_at), so the player's own Link, trees and rocks in front hide them. One
// region a frame, at most REG_MAX depth pixels (else it is skipped and the figures are drawn whole).
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

enum { OCCL_POINTS = 32, OCCL_SLOTS = 4, REG_MAX = 1 << 20 };

typedef struct {
    VkBuffer buf;
    VmaAllocation alloc;
    float *map;                       // OCCL_POINTS sampled depths
    volatile int32_t busy;            // copied, waiting for its submission
    int n;                            // points of this test
    uint8_t in[OCCL_POINTS];          // the point fell on the depth target, so a copy was made
    float view_z[OCCL_POINTS];        // the point's distance along the view axis (metres)
    float proj[16];                   // the camera's projection, to turn sampled depths into distances
    int gl_depth;                     // clip z runs -1..1 (OpenGL convention) rather than 0..1
    float vp_near, vp_far;            // the viewport's depth range
    VkBuffer reg_buf;                 // the depth rectangle: REG_MAX floats
    VmaAllocation reg_alloc;
    float *reg_map;
    int rx, ry, rw, rh;               // its place in the depth target (rw 0: none this time)
    float vp[4];                      // the viewport, to place NDC in the depth target
} Slot;

static PlatMutex lock = PLAT_MUTEX_INIT;
static Slot slots[OCCL_SLOTS];
static int next_slot;
static float want[OCCL_POINTS][3];    // the overlay's points (lock)
static int nwant;
static uint64_t want_ms;           // when the overlay last asked: older points are dropped
static float seen[OCCL_POINTS];       // the last results: 1 visible, 0 hidden, -1 unknown (lock)
static int nseen;
static uint64_t seen_ms;
static float want_rect[4];            // the overlay's depth rectangle in NDC: x0, y0, x1, y1 (y up) (lock)
static uint64_t want_rect_ms;
static Slot res_conv;                 // the last region: its conversion data and place (lock)
static float *res_data;               // REG_MAX floats, its depths (lock)
static uint64_t res_ms;

// armed by the G-buffer pass of this frame
static struct {
    int on;
    VkImage image;
    VkImageAspectFlags aspect;
    uint32_t w, h, level, layer;
    float vp[6];                      // x, y, w, h, near, far
    float cam[84];
} arm;

void vk_occl_arm(const VkExec *ex, const float cam[84]) {
    const VkExecTarget *d = &ex->depth;
    if (!d->image || d->samples > 1 || d->is3d || d->blocks) return;
    arm.on = 1;
    arm.image = d->image;
    arm.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    arm.w = d->width, arm.h = d->height, arm.level = d->level, arm.layer = d->base_layer;
    if (ex->has_vp) memcpy(arm.vp, ex->vp[0], sizeof arm.vp);
    else arm.vp[0] = arm.vp[1] = 0, arm.vp[2] = (float)d->width, arm.vp[3] = (float)d->height, arm.vp[4] = 0, arm.vp[5] = 1;
    memcpy(arm.cam, cam, sizeof arm.cam);
    vk_figure_arm(ex, cam, arm.vp);
}

static float clip_ndc_z(const float *p, float z) {   // NDC depth of a point at view-space z (negative ahead)
    float c = p[10] * z + p[11], w = p[14] * z + p[15];
    return w != 0 ? c / w : 0;
}

// view-space distance ahead of the camera from a depth-buffer value
static float depth_to_dist(const Slot *s, float d) {
    float span = s->vp_far - s->vp_near;
    float ndc = span != 0 ? (d - s->vp_near) / span : d;
    if (s->gl_depth) ndc = ndc * 2 - 1;
    // ndc = (P22 z + P23) / (P32 z + P33)  =>  z = (P23 - ndc P33) / (ndc P32 - P22)
    const float *p = s->proj;
    float den = ndc * p[14] - p[10];
    if (fabsf(den) < 1e-12f) return 1e9f;
    return -(p[11] - ndc * p[15]) / den;
}

static int probe_on(void) {
    const char *e = cfg_get("TOTK_SPEED_PROBE");
    return e && !strcmp(e, "1");
}

static void slot_done(void *arg) {   // completion thread
    Slot *s = arg;
    float vis[OCCL_POINTS];
    for (int i = 0; i < s->n; i++) {
        if (!s->in[i]) { vis[i] = -1; continue; }
        float scene = depth_to_dist(s, s->map[i]);
        // hidden when the scene is nearer than the point by half a metre and 3 % (depth precision far away)
        vis[i] = scene < s->view_z[i] - 0.5f - 0.03f * s->view_z[i] ? 0.0f : 1.0f;
    }
    static uint64_t logged;
    if (s->n && s->in[0] && probe_on() && plat_now_ms() - logged > 3000) {
        logged = plat_now_ms();
        rt_log("occlusion: player chest %.2f m ahead, scene there %.2f m (depth %.6f): %s", s->view_z[0],
               depth_to_dist(s, s->map[0]), s->map[0], vis[0] > 0 ? "visible" : "hidden");
        char pts[OCCL_POINTS * 24] = "";
        size_t at = 0;
        for (int i = 1; i < s->n && at < sizeof pts - 24; i++)   // each friend point: its distance and the scene's, or "-" off screen
            at += (size_t)(s->in[i] ? snprintf(pts + at, sizeof pts - at, " %.1f/%.1f", s->view_z[i], depth_to_dist(s, s->map[i]))
                                    : snprintf(pts + at, sizeof pts - at, " -"));
        if (s->n > 1) rt_log("occlusion: friend points, metres ahead / scene:%s", pts);
    }
    plat_mutex_lock(&lock);
    if (s->rw && (res_data || (res_data = malloc(sizeof(float) * REG_MAX)))) {
        memcpy(res_data, s->reg_map, sizeof(float) * (size_t)s->rw * (size_t)s->rh);
        res_conv = *s;
        res_ms = plat_now_ms();
    }
    memcpy(seen, vis, sizeof(float) * (size_t)s->n);
    nseen = s->n;
    seen_ms = plat_now_ms();
    plat_mutex_unlock(&lock);
    __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
}

static int slot_ready(Slot *s) {
    if (s->buf && s->reg_buf) return 1;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = OCCL_POINTS * sizeof(float),
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &s->buf, &s->alloc, &info) != VK_SUCCESS) return s->buf = VK_NULL_HANDLE, 0;
    s->map = info.pMappedData;
    bi.size = sizeof(float) * REG_MAX;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &s->reg_buf, &s->reg_alloc, &info) != VK_SUCCESS) return s->reg_buf = VK_NULL_HANDLE, 0;
    s->reg_map = info.pMappedData;
    return 1;
}

void vk_occl_before_pass(VkExec *ex) {
    if (!arm.on || ex->depth.image == arm.image) return;   // still drawing into the G-buffer depth
    arm.on = 0;
    vk_figure_draw(ex);   // the friends' figures into the complete G-buffer, before the lighting reads it
    plat_mutex_lock(&lock);
    int asked = nwant && plat_now_ms() - want_ms < 1000;
    int region = plat_now_ms() - want_rect_ms < 500;
    float rect[4];
    memcpy(rect, want_rect, sizeof rect);
    asked |= region;
    plat_mutex_unlock(&lock);
    if (!asked && !probe_on()) return;   // no friends drawn: no copy, no extra barrier
    Slot *s = &slots[next_slot];
    if (__atomic_load_n(&s->busy, __ATOMIC_ACQUIRE) || !slot_ready(s)) return;   // the GPU is far behind: skip a frame
    next_slot = (next_slot + 1) % OCCL_SLOTS;

    float pts[OCCL_POINTS][3];
    int n = 0;
    float me[3];
    if (game_player_pos(me)) pts[n][0] = me[0], pts[n][1] = me[1] + 1.0f, pts[n][2] = me[2], n++;
    else pts[n][0] = pts[n][1] = pts[n][2] = 0, n++;   // keeps the overlay's points from index 1
    plat_mutex_lock(&lock);
    for (int i = 0; i < nwant && n < OCCL_POINTS && plat_now_ms() - want_ms < 1000; i++) memcpy(pts[n++], want[i], sizeof pts[0]);
    plat_mutex_unlock(&lock);

    const float *vp = arm.cam + 12, *view = arm.cam;
    memcpy(s->proj, arm.cam + 28, sizeof s->proj);
    float zn = arm.cam[56], zf = arm.cam[57];
    float a = clip_ndc_z(s->proj, -zn), b = clip_ndc_z(s->proj, -zf);
    s->gl_depth = fabsf(a + 1) < 0.05f || fabsf(b + 1) < 0.05f;   // one end of the range at -1
    s->vp_near = arm.vp[4], s->vp_far = arm.vp[5];
    VkBufferImageCopy reg[OCCL_POINTS];
    uint32_t nreg = 0;
    for (int i = 0; i < n; i++) {
        const float *w = pts[i];
        float clip[4];
        for (int r = 0; r < 4; r++) clip[r] = vp[r * 4] * w[0] + vp[r * 4 + 1] * w[1] + vp[r * 4 + 2] * w[2] + vp[r * 4 + 3];
        // distance along the view axis: minus the view-space z (the view's third row)
        s->view_z[i] = -(view[8] * w[0] + view[9] * w[1] + view[10] * w[2] + view[11]);
        s->in[i] = 0;
        if (clip[3] <= 0.01f || s->view_z[i] <= 0) continue;
        float x = arm.vp[0] + (clip[0] / clip[3] * 0.5f + 0.5f) * arm.vp[2];
        float y = arm.vp[1] + (0.5f - clip[1] / clip[3] * 0.5f) * fabsf(arm.vp[3]);
        if (!(x >= 0 && y >= 0 && x < (float)arm.w && y < (float)arm.h)) continue;
        s->in[i] = 1;
        reg[nreg++] = (VkBufferImageCopy){
            .bufferOffset = (VkDeviceSize)i * sizeof(float),
            .imageSubresource = {arm.aspect, arm.level, arm.layer, 1},
            .imageOffset = {(int32_t)x, (int32_t)y, 0},
            .imageExtent = {1, 1, 1}};
    }
    s->n = n;
    memcpy(s->vp, arm.vp, sizeof s->vp);
    s->rw = s->rh = 0;
    if (region) {   // the rectangle in depth pixels (NDC y up, depth rows down)
        float x0 = arm.vp[0] + (rect[0] * 0.5f + 0.5f) * arm.vp[2], x1 = arm.vp[0] + (rect[2] * 0.5f + 0.5f) * arm.vp[2];
        float y0 = arm.vp[1] + (0.5f - rect[3] * 0.5f) * fabsf(arm.vp[3]), y1 = arm.vp[1] + (0.5f - rect[1] * 0.5f) * fabsf(arm.vp[3]);
        int ix0 = (int)floorf(fmaxf(x0, 0)), iy0 = (int)floorf(fmaxf(y0, 0));
        int ix1 = (int)ceilf(fminf(x1, (float)arm.w)), iy1 = (int)ceilf(fminf(y1, (float)arm.h));
        if (ix1 > ix0 && iy1 > iy0 && (int64_t)(ix1 - ix0) * (iy1 - iy0) <= REG_MAX)
            s->rx = ix0, s->ry = iy0, s->rw = ix1 - ix0, s->rh = iy1 - iy0;
    }
    if (!nreg && !s->rw) {   // nothing on screen: the results say so at once
        __atomic_store_n(&s->busy, 1, __ATOMIC_RELEASE);
        slot_done(s);
        return;
    }
    vk_exec_barrier(ex);   // the G-buffer pass's writes, and its depth back in the GENERAL layout
    if (nreg) vkCmdCopyImageToBuffer(ex->cb, arm.image, VK_IMAGE_LAYOUT_GENERAL, s->buf, nreg, reg);
    if (s->rw) {
        VkBufferImageCopy rc = {.bufferOffset = 0, .bufferRowLength = (uint32_t)s->rw,
                                .imageSubresource = {arm.aspect, arm.level, arm.layer, 1},
                                .imageOffset = {s->rx, s->ry, 0}, .imageExtent = {(uint32_t)s->rw, (uint32_t)s->rh, 1}};
        vkCmdCopyImageToBuffer(ex->cb, arm.image, VK_IMAGE_LAYOUT_GENERAL, s->reg_buf, 1, &rc);
    }
    ex->recorded++;
    __atomic_store_n(&s->busy, 1, __ATOMIC_RELEASE);
    vk_completion_add(ex->fence, slot_done, s);
}

int gfx_occlusion(const float (*pts)[3], int n, float *visible) {
    if (n > OCCL_POINTS - 1) n = OCCL_POINTS - 1;
    plat_mutex_lock(&lock);
    memcpy(want, pts, sizeof want[0] * (size_t)n);   // results are by index: the overlay keeps its order
    nwant = n;
    want_ms = plat_now_ms();
    int fresh = seen_ms && plat_now_ms() - seen_ms < 500 && nseen == n + 1;
    for (int i = 0; i < n; i++) visible[i] = fresh ? seen[i + 1] : -1;
    plat_mutex_unlock(&lock);
    return fresh;
}

void gfx_depth_want(const float ndc_rect[4]) {
    plat_mutex_lock(&lock);
    memcpy(want_rect, ndc_rect, sizeof want_rect);
    want_rect_ms = plat_now_ms();
    plat_mutex_unlock(&lock);
}

static int depth_held;

int gfx_depth_begin(void) {
    plat_mutex_lock(&lock);
    depth_held = res_data && res_ms && plat_now_ms() - res_ms < 300 && res_conv.rw;
    return depth_held;
}

float gfx_depth_at(float ndc_x, float ndc_y) {
    if (!depth_held) return -1;
    const Slot *s = &res_conv;
    int x = (int)(s->vp[0] + (ndc_x * 0.5f + 0.5f) * s->vp[2]) - s->rx;
    int y = (int)(s->vp[1] + (0.5f - ndc_y * 0.5f) * fabsf(s->vp[3])) - s->ry;
    if (x < 0 || y < 0 || x >= s->rw || y >= s->rh) return -1;
    return depth_to_dist(s, res_data[(size_t)y * (size_t)s->rw + (size_t)x]);
}

void gfx_depth_end(void) {
    depth_held = 0;
    plat_mutex_unlock(&lock);
}
