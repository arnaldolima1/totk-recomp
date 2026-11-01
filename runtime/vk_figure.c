// Friends' Link figures drawn into the game's G-buffer, so the game lights them like its own characters: its sun and
// sky, its shadows on them, its fog and post-processing.
//
// The overlay (ui/menu.cpp world_tags) hands over each friend's place, velocity, facing and animation state with
// gfx_figures_set once a presented frame; the replay moves them on to the moment it draws and poses them
// (link_figure_pose). The G-buffer pass of a frame (vk_exec_draw.c cam_take, through vk_occl_arm)
// arms a draw with its targets, viewport and camera; when the replay begins the first dispatch after it
// (shadow and occlusion compute shaders read the G-buffer) or the next pass on another depth target
// (vk_occl_before_pass), the G-buffer is complete and the lighting has not read it yet. Then one render pass of our
// own over the same targets draws every figure: our pipeline (shaders/figure_vs.vert, figure_fs.frag) writes the
// targets the way TOTK's character shaders write them, and tests and writes the scene depth, so the scene hides the
// figure and the figure hides what is behind it.
//
// The figure's vertices go to the GPU in view space (positions and smooth normals, computed here), its triangle
// corners once (vertex index, colour word, atlas place), its atlas once as an sRGB image with mipmaps.
// TOTK_FIGURE_GBUF=0 leaves the figures to the overlay.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "link_figure.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_rec.h"
#include "platform/platform.h"

#include "figure_fs_spv.h"
#include "figure_vs_spv.h"

enum { MAX_FIGURES = 8 };
enum { FLAG_UNTEXTURED = 1, FLAG_HAIR = 2, FLAG_SKIN = 4 };   // the colour word's top byte (figure_fs.frag)

// ---- the figures the overlay hands over (any thread)
static PlatMutex lock = PLAT_MUTEX_INIT;
static GfxFigure figs[MAX_FIGURES];
static int nfigs;
static uint64_t set_ms;
static int failed;   // the pipeline or the atlas could not be made: the overlay draws the figures

int gfx_figures_wanted(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("TOTK_FIGURE_GBUF");
        on = !(v && *v == '0');
    }
    return on && !__atomic_load_n(&failed, __ATOMIC_RELAXED) && link_figure() != NULL;
}

void gfx_figures_set(const GfxFigure *f, int n) {
    if (n > MAX_FIGURES) n = MAX_FIGURES;
    plat_mutex_lock(&lock);
    memcpy(figs, f, sizeof *f * (size_t)n);
    nfigs = n;
    set_ms = plat_now_ms();
    plat_mutex_unlock(&lock);
}

// ---- armed by the G-buffer pass (replay thread)
// The character tile buffer of this frame (vk_exec_draw.c tiles_note): TOTK's character shaders set a word per 16-byte
// tile entry, at byte 452 + 16 * (tile y * stride + tile x), tile x and y the pixel times the scales in c4 (x, y) and
// stride the integer bits of c4.z; the deferred lighting gives marked tiles the character treatment.
static struct {
    int seen;
    uint64_t addr, size;
    float c4[4];
} tiles;

// Where and how a pass draws: its depth target, viewport (x, y, w, h), depth range, depth compare and bias, and the
// camera block it was drawn with.
typedef struct {
    VkExecTarget depth;
    float vp[4], range[2];
    VkCompareOp compare;
    float bias[3];   // constant, clamp, slope
    float cam[84];
} View;

static void view_take(View *v, const VkExec *ex, const float cam[84]) {
    v->depth = ex->depth;
    if (ex->has_vp) memcpy(v->vp, ex->vp[0], sizeof v->vp);
    else v->vp[0] = v->vp[1] = 0, v->vp[2] = (float)ex->depth.width, v->vp[3] = (float)ex->depth.height;
    v->vp[3] = fabsf(v->vp[3]);
    v->range[0] = ex->depth_range[0], v->range[1] = ex->depth_range[1];
    unsigned df = ex->pipe.ds.df;   // NVN_DEPTH_FUNC_* are NEVER = 1 .. ALWAYS = 8 in VkCompareOp order
    v->compare = df >= 1 && df <= 8 ? (VkCompareOp)(df - 1) : VK_COMPARE_OP_LESS_OR_EQUAL;
    if (v->compare == VK_COMPARE_OP_LESS) v->compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (v->compare == VK_COMPARE_OP_GREATER) v->compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    v->bias[0] = ex->poly_offset.units, v->bias[1] = ex->poly_offset.clamp, v->bias[2] = ex->poly_offset.factor;
    memcpy(v->cam, cam, sizeof v->cam);
}

static struct {
    int on;
    VkExecTarget color[4];
    View v;
} arm;

// The nearest shadow cascade (layer 0 of the sun's shadow map), armed by its first pass of the frame
// (vk_exec_draw.c shadow_take); the figures go in when the replay moves on to another target.
static struct {
    int on;
    View v;
} shadow;

void vk_figure_arm(const VkExec *ex, const float cam[84], const float vp[6]) {
    arm.on = 0;
    static int logged;
    if (!logged++)
        rt_log("figure: G-buffer pass with %u colour targets (%d %d %d %d), depth %d", ex->pass_ncolor,
               (int)ex->color[0].format, (int)ex->color[1].format, (int)ex->color[2].format, (int)ex->color[3].format,
               (int)ex->depth.format);
    if (ex->pass_ncolor < 4 || !ex->color[0].image || !ex->color[1].image || !ex->color[3].image || !ex->depth.image)
        return;
    (void)vp;
    for (int i = 0; i < 4; i++) arm.color[i] = ex->color[i];
    arm.color[2].image = VK_NULL_HANDLE;
    view_take(&arm.v, ex, cam);
    tiles.seen = 0;
    shadow.on = 0;
    arm.on = 1;
}

int vk_figure_wants_tiles(void) { return arm.on && !tiles.seen; }

static void draw_shadow(VkExec *ex);

void vk_figure_shadow_arm(const VkExec *ex, const float cam[84]) {
    if (shadow.on && shadow.v.depth.image == ex->depth.image && shadow.v.depth.base_layer == ex->depth.base_layer) return;
    // vertices go through the cascade's view-projection (camera block floats 12..27, checked on the player's place):
    // an identity view, that matrix as the projection
    float vp_cam[84];
    memcpy(vp_cam, cam, sizeof vp_cam);
    memset(vp_cam, 0, 12 * sizeof(float));
    vp_cam[0] = vp_cam[5] = vp_cam[10] = 1;
    memcpy(vp_cam + 28, cam + 12, 16 * sizeof(float));
    view_take(&shadow.v, ex, vp_cam);
    shadow.on = 1;
}

// A pass begins: the cascade's passes are over once it is on another target.
void vk_figure_before_pass(VkExec *ex) {
    if (shadow.on && (ex->depth.image != shadow.v.depth.image || ex->depth.base_layer != shadow.v.depth.base_layer))
        draw_shadow(ex);
}

// Compute shaders run between the G-buffer passes too (culling for the next one): the figures go in at the first
// dispatch after the game's characters were drawn, else when the next pass on another depth target begins.
void vk_figure_before_dispatch(VkExec *ex) {
    if (arm.on && tiles.seen) vk_figure_draw(ex);
    if (shadow.on) draw_shadow(ex);
}

void vk_figure_tiles(uint64_t addr, uint64_t size, const float c4[4]) {
    tiles.seen = 1, tiles.addr = addr, tiles.size = size;
    memcpy(tiles.c4, c4, sizeof tiles.c4);
    static int logged;
    uint32_t stride;
    memcpy(&stride, &c4[2], 4);
    if (!logged++)
        rt_log("figure: character tiles at %#llx (%llu bytes), scales %g %g, %u a row", (unsigned long long)addr,
               (unsigned long long)size, c4[0], c4[1], stride);
}

// ---- GPU objects, made on the first draw
static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe, shadow_pipe;
    VkFormat fmt[4], dfmt, shadow_fmt;
    int shadow_tried;
    VkSampler sampler;
    VkImage atlas;
    VmaAllocation atlas_mem;
    VkImageView atlas_view;
    uint32_t levels;
    int atlas_ready;
    VkBuffer corners;
    VmaAllocation corners_mem;
} G;

typedef struct {
    uint32_t vertex, colour;
    float uv[2];
} Corner;

static void fail(const char *why) {
    rt_log("figure: %s; friends are drawn by the overlay", why);
    __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
}

static VkShaderModule module(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

// Depth only, for the shadow map: the vertex stage alone, the game's depth bias.
static int make_shadow_pipeline(VkCtx *c, VkFormat dfmt) {
    VkShaderModule vs = module(c, figure_vs_spv, sizeof figure_vs_spv);
    VkPipelineShaderStageCreateInfo st = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"};
    VkVertexInputBindingDescription vb = {0, sizeof(Corner), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R32_UINT, 0}, {1, 0, VK_FORMAT_R32_UINT, 4},
                                               {2, 0, VK_FORMAT_R32G32_SFLOAT, 8}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
                                               .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .depthBiasEnable = VK_TRUE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                             VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 4, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .depthAttachmentFormat = dfmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 1, .pStages = &st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                       .pDynamicState = &dy, .layout = G.layout};
    VkResult r = vs ? vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &G.shadow_pipe) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(c->device, vs, NULL);
    if (r != VK_SUCCESS) G.shadow_pipe = VK_NULL_HANDLE;
    return r == VK_SUCCESS;
}

static int make_pipeline(VkCtx *c) {
    VkShaderModule vs = module(c, figure_vs_spv, sizeof figure_vs_spv), fs = module(c, figure_fs_spv, sizeof figure_fs_spv);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
         .pName = "main"}};
    VkVertexInputBindingDescription vb = {0, sizeof(Corner), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R32_UINT, 0}, {1, 0, VK_FORMAT_R32_UINT, 4},
                                               {2, 0, VK_FORMAT_R32G32_SFLOAT, 8}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
                                               .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
    VkPipelineColorBlendAttachmentState ba[4] = {{.colorWriteMask = 0xf}, {.colorWriteMask = 0xf}, {.colorWriteMask = 0},
                                                 {.colorWriteMask = 0xf}};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 4, .pAttachments = ba};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 3, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 4, .pColorAttachmentFormats = G.fmt,
                                        .depthAttachmentFormat = G.dfmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                       .pDynamicState = &dy, .layout = G.layout};
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &G.pipe) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(c->device, vs, NULL);
    if (fs) vkDestroyShaderModule(c->device, fs, NULL);
    return r == VK_SUCCESS;
}

static int init(VkCtx *c, const LinkFigure *lf) {
    G.tried = 1;
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!G.push) return fail("no push descriptors"), 0;
    VkDescriptorSetLayoutBinding b[3] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 3, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 112};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &G.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &G.layout) != VK_SUCCESS)
        return fail("the pipeline layout could not be made"), 0;
    for (int i = 0; i < 4; i++) G.fmt[i] = i == 2 ? VK_FORMAT_UNDEFINED : arm.color[i].format;
    G.dfmt = arm.v.depth.format;
    if (!make_pipeline(c)) return fail("the pipeline could not be made"), 0;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 16};
    if (vkCreateSampler(c->device, &si, NULL, &G.sampler) != VK_SUCCESS) return fail("no sampler"), 0;
    // the atlas: sRGB with a full mip chain (made by blits on the first draw)
    uint32_t side = lf->atlas_side;
    G.levels = 1;
    while ((side >> G.levels) >= 1 && G.levels < 16) G.levels++;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_SRGB, .extent = {side, side, 1}, .mipLevels = G.levels,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &G.atlas, &G.atlas_mem, NULL) != VK_SUCCESS) return fail("no atlas image"), 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = G.atlas,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, G.levels, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &G.atlas_view) != VK_SUCCESS) return fail("no atlas view"), 0;
    // the triangle corners
    static const uint8_t base[LF_CLASSES][3] = {
        {232, 189, 153},   // skin
        {0, 0, 0},         // hair: the friend's colour
        {61, 133, 69},     // tunic
        {199, 179, 140},   // trousers
        {115, 77, 48},     // leather
        {235, 235, 235},   // eye
        {38, 31, 31},      // dark
        {153, 168, 184},   // metal
        {89, 199, 161},    // rauru
    };
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof(Corner) * 3ull * lf->ntris,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VmaAllocationCreateInfo bai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &bai, &G.corners, &G.corners_mem, &info) != VK_SUCCESS)
        return fail("no corner buffer"), 0;
    Corner *co = info.pMappedData;
    for (uint32_t t = 0; t < lf->ntris; t++) {
        uint8_t cls = lf->cls[t] & 0x7f;
        if (cls >= LF_CLASSES) cls = LF_LEATHER;
        uint32_t flags = cls == LF_SKIN ? FLAG_SKIN : 0, colour = 0;
        if (cls == LF_HAIR) flags |= FLAG_HAIR;
        else if (lf->cls[t] & LF_UNTEXTURED)
            flags |= FLAG_UNTEXTURED, colour = base[cls][0] | (uint32_t)base[cls][1] << 8 | (uint32_t)base[cls][2] << 16;
        for (int k = 0; k < 3; k++) {
            int kk = lf->winding > 0 ? k : 2 - k;
            co[3 * t + k] = (Corner){lf->tris[3 * t + kk], colour | flags << 24,
                                     {lf->uvs[6 * t + 2 * kk], lf->uvs[6 * t + 2 * kk + 1]}};
        }
    }
    vmaFlushAllocation(c->vma, G.corners_mem, 0, VK_WHOLE_SIZE);
    return G.ok = 1;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, uint32_t level, uint32_t n, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, n, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// The atlas into its image, then each mip level blitted from the one above; left in SHADER_READ_ONLY_OPTIMAL.
static void upload_atlas(VkExec *ex, const LinkFigure *lf) {
    uint32_t side = lf->atlas_side;
    VkBuffer buf;
    VkDeviceSize off;
    uint8_t *p = vk_exec_scratch(ex, 4ull * side * side, &buf, &off);
    memcpy(p, lf->albedo, 4ull * side * side);
    VkCommandBuffer cb = ex->cb;
    image_barrier(cb, G.atlas, 0, G.levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy rc = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {side, side, 1}};
    vkCmdCopyBufferToImage(cb, buf, G.atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    for (uint32_t l = 1; l < G.levels; l++) {
        image_barrier(cb, G.atlas, l - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT);
        int32_t s0 = (int32_t)(side >> (l - 1)), s1 = (int32_t)(side >> l);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1}, .srcOffsets = {{0, 0, 0}, {s0, s0, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, .dstOffsets = {{0, 0, 0}, {s1, s1, 1}}};
        vkCmdBlitImage(cb, G.atlas, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G.atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &bl, VK_FILTER_LINEAR);
    }
    if (G.levels > 1)
        image_barrier(cb, G.atlas, 0, G.levels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    image_barrier(cb, G.atlas, G.levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    ex->recorded++;
    G.atlas_ready = 1;
}

static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

// The posed figure f in view space: per vertex its position and its smooth normal (the posed faces' normals summed,
// outward), a vec4 each.
static void to_view(const LinkFigure *lf, const float *pose, const float feet[3], float yaw, const float *view, float *out,
                    float *acc) {
    memset(acc, 0, 12ull * lf->nverts);
    for (uint32_t t = 0; t < lf->ntris; t++) {
        const uint32_t *ix = lf->tris + 3 * t;
        const float *a = pose + 3 * ix[0], *b = pose + 3 * ix[1], *c = pose + 3 * ix[2];
        float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        for (int k = 0; k < 3; k++)
            for (int q = 0; q < 3; q++) acc[3 * ix[k] + q] += n[q] * (float)lf->winding;
    }
    float cy = cosf(yaw), sy = sinf(yaw);
    for (uint32_t i = 0; i < lf->nverts; i++) {
        const float *m = pose + 3 * i, *n = acc + 3 * i;
        float w[3] = {feet[0] + cy * m[0] + sy * m[2], feet[1] + m[1], feet[2] - sy * m[0] + cy * m[2]};
        float wn[3] = {cy * n[0] + sy * n[2], n[1], -sy * n[0] + cy * n[2]};
        float *o = out + 8ull * i;
        for (int r = 0; r < 3; r++) {
            o[r] = view[4 * r] * w[0] + view[4 * r + 1] * w[1] + view[4 * r + 2] * w[2] + view[4 * r + 3];
            o[4 + r] = view[4 * r] * wn[0] + view[4 * r + 1] * wn[1] + view[4 * r + 2] * wn[2];
        }
        o[3] = 1, o[7] = 0;
    }
}

// The figures of this frame, posed in model space by the G-buffer draw and drawn again into the shadow cascade.
static struct {
    int n;
    float *pose[MAX_FIGURES];
    float feet[MAX_FIGURES][3], yaw[MAX_FIGURES], hair[MAX_FIGURES][3];
} posed;

static int pose_figures(const LinkFigure *lf) {
    GfxFigure fig[MAX_FIGURES];
    plat_mutex_lock(&lock);
    uint64_t now = plat_now_ms();
    int n = now - set_ms < 500 ? nfigs : 0;   // the overlay stopped handing figures over: none
    memcpy(fig, figs, sizeof *fig * (size_t)n);
    // each figure moved on to this moment: the overlay sets them once a presented frame and the replay draws its
    // frames at its own pace, so a pose taken as it was set would be shown twice or skipped (a stutter)
    float ahead = fminf(0.1f, (float)(now - set_ms) / 1000.0f);
    plat_mutex_unlock(&lock);
    static float *pose2;
    if (!pose2 && !(pose2 = malloc(12ull * lf->nverts))) return 0;
    for (int i = 0; i < n; i++) {
        GfxFigure *g = &fig[i];
        if (!posed.pose[i] && !(posed.pose[i] = malloc(12ull * lf->nverts))) return 0;
        float frame = g->frame + g->fps * ahead, prev = g->prev_frame + g->fps * ahead;
        float blend = fminf(1.0f, g->blend + 4.0f * ahead);
        link_figure_pose(lf, g->anim, frame, posed.pose[i]);
        if (blend < 1) {   // still blending out of the previous animation
            float *p = posed.pose[i];
            link_figure_pose(lf, g->prev_anim, prev, pose2);
            for (uint64_t q = 0; q < 3ull * lf->nverts; q++) p[q] = pose2[q] + (p[q] - pose2[q]) * blend;
        }
        for (int k = 0; k < 3; k++) posed.feet[i][k] = g->feet[k] + g->vel[k] * ahead, posed.hair[i][k] = srgb_to_linear(g->hair[k]);
        posed.yaw[i] = g->yaw;
    }
    return posed.n = n;
}

// The push constants of a view: its projection (row-major in the camera block) as GLSL's column-major mat4, and its
// depth conventions: NDC depth from -1 when the game draws with NVN's near-is-minus-w mode, then the depth range.
static void view_push(const View *v, float push[28]) {
    const float *P = v->cam + 28;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) push[4 * k + r] = P[4 * r + k];
    push[16] = nvn_depth_minus_w() ? 1.0f : 0.0f, push[17] = v->range[0], push[18] = v->range[1], push[19] = 0;
    push[20] = push[21] = push[22] = 0, push[23] = 1;
    push[24] = push[25] = push[26] = push[27] = 0;
}

static void view_state(VkCommandBuffer cb, const View *v) {
    VkViewport vp = {v->vp[0], v->vp[1], v->vp[2], v->vp[3], 0, 1};
    VkRect2D sc = {{0, 0}, {v->depth.width, v->depth.height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetDepthCompareOp(cb, v->compare);
}

// Each posed figure's vertices in view space of v, into scratch memory.
static void figures_in_view(VkExec *ex, const LinkFigure *lf, const View *v, VkBuffer *vbuf, VkDeviceSize *voff) {
    static float *acc;
    if (!acc && !(acc = malloc(12ull * lf->nverts))) return;
    for (int i = 0; i < posed.n; i++) {
        float *o = (float *)vk_exec_scratch(ex, 32ull * lf->nverts, &vbuf[i], &voff[i]);
        to_view(lf, posed.pose[i], posed.feet[i], posed.yaw[i], v->cam, o, acc);
    }
}

void vk_figure_draw(VkExec *ex) {
    if (!arm.on) return;
    arm.on = 0;
    posed.n = 0;
    const LinkFigure *lf = link_figure();
    if (!lf || __atomic_load_n(&failed, __ATOMIC_RELAXED) || !gfx_figures_wanted()) return;
    int n = pose_figures(lf);
    static int logged;
    if (!logged && n) logged = 1, rt_log("figure: drawing %d figure(s) into the G-buffer", n);
    if (!n) return;
    VkCtx *c = vk_ctx();
    if (!G.tried && !init(c, lf)) return;
    if (!G.ok) return;
    if (arm.color[0].format != G.fmt[0] || arm.color[1].format != G.fmt[1] || arm.color[3].format != G.fmt[3] ||
        arm.v.depth.format != G.dfmt)
        return;   // not the G-buffer this pipeline was made for
    uint64_t per = 32ull * lf->nverts;
    VkBuffer vbuf[MAX_FIGURES];
    VkDeviceSize voff[MAX_FIGURES];
    figures_in_view(ex, lf, &arm.v, vbuf, voff);

    vk_exec_barrier(ex);   // the G-buffer pass's writes; its targets back in the GENERAL layout
    if (!G.atlas_ready) upload_atlas(ex, lf);
    VkRenderingAttachmentInfo ca[4];
    for (int i = 0; i < 4; i++) {
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (i == 2) continue;
        ca[i].imageView = vk_exec_attach_view(ex, &arm.color[i]);
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = vk_exec_attach_view(ex, &arm.v.depth),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    uint32_t w = arm.v.depth.width, h = arm.v.depth.height;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 4, .pColorAttachments = ca, .pDepthAttachment = &da};
    VkCommandBuffer cb = ex->cb;
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pipe);
    view_state(cb, &arm.v);
    static int debug = -1;
    if (debug < 0) { const char *e = cfg_get("TOTK_FIGURE_DEBUG"); debug = e && *e == '1'; }
    if (debug) vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    float push[28];
    view_push(&arm.v, push);
    // the tile buffer: bound from an aligned offset, the words before the buffer's start skipped by the shader. In
    // mirror mode the game's draws and the lighting use the pool's device-local mirror (vk_exec_draw.c bind_range), so
    // the marks go there too and are noted as GPU writes; else they land in the imported copy, which the lighting never
    // reads and the mirror's write-back overwrites at the end of the submission.
    VkDescriptorBufferInfo tb = {0};
    uint32_t tile_on = 0, tile_base = 0;
    VkMemSpan ts;
    if (tiles.seen && tiles.size >= 512 && vk_mem_resolve(tiles.addr, tiles.size, &ts)) {
        VK_MEM_USE(ts);
        vk_mem_gpu_wrote(&ts, tiles.size);
        VkDeviceSize al = 256, start = ts.offset / al * al;
        tb = (VkDescriptorBufferInfo){ts.buffer, start, ts.offset - start + tiles.size};
        tile_base = (uint32_t)((ts.offset - start) / 4);
        tile_on = 1;
    } else {   // none seen this frame: a scratch word stands in, unmarked
        VkBuffer sb;
        VkDeviceSize so;
        memset(vk_exec_scratch(ex, 256, &sb, &so), 0, 256);
        tb = (VkDescriptorBufferInfo){sb, so, 256};
    }
    uint32_t stride;
    memcpy(&stride, &tiles.c4[2], 4);
    push[24] = tile_on ? tiles.c4[0] : 0, push[25] = tile_on ? tiles.c4[1] : 0;
    memcpy(&push[26], &stride, 4);
    memcpy(&push[27], &tile_base, 4);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &G.corners, &zero);
    VkDescriptorImageInfo ii = {G.sampler, G.atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (int i = 0; i < n; i++) {
        VkDescriptorBufferInfo bi = {vbuf[i], voff[i], per};
        VkWriteDescriptorSet wr[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &tb}};
        rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, G.layout, 0, 3, wr);
        push[20] = posed.hair[i][0], push[21] = posed.hair[i][1], push[22] = posed.hair[i][2], push[23] = 1;
        vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
        vkCmdDraw(cb, 3 * lf->ntris, 1, 0, 0);
    }
    vkCmdEndRendering(cb);
    ex->recorded += 8;
    // the replay's own state is no longer what the command buffer holds: its next draw binds everything again
    vk_exec_state_lost(ex);
}

// The figures posed for this frame's G-buffer into the nearest shadow cascade, so they cast the sun's shadow (on the
// ground and on themselves) like the game's own characters.
static void draw_shadow(VkExec *ex) {
    shadow.on = 0;
    const LinkFigure *lf = link_figure();
    if (!lf || !G.ok || !posed.n) return;
    VkCtx *c = vk_ctx();
    if (!G.shadow_tried) {
        G.shadow_tried = 1, G.shadow_fmt = shadow.v.depth.format;
        if (!make_shadow_pipeline(c, G.shadow_fmt)) rt_log("figure: the shadow pipeline could not be made; friends cast no shadow");
    }
    if (!G.shadow_pipe || shadow.v.depth.format != G.shadow_fmt) return;
    VkBuffer vbuf[MAX_FIGURES];
    VkDeviceSize voff[MAX_FIGURES];
    figures_in_view(ex, lf, &shadow.v, vbuf, voff);
    vk_exec_barrier(ex);
    VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = vk_exec_attach_view(ex, &shadow.v.depth),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    uint32_t w = shadow.v.depth.width, h = shadow.v.depth.height;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .pDepthAttachment = &da};
    VkCommandBuffer cb = ex->cb;
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, G.shadow_pipe);
    view_state(cb, &shadow.v);
    vkCmdSetDepthBias(cb, shadow.v.bias[0], shadow.v.bias[1], shadow.v.bias[2]);
    float push[28];
    view_push(&shadow.v, push);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &G.corners, &zero);
    VkDescriptorImageInfo ii = {G.sampler, G.atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkBuffer sb;
    VkDeviceSize so;
    memset(vk_exec_scratch(ex, 256, &sb, &so), 0, 256);
    VkDescriptorBufferInfo tb = {sb, so, 256};
    for (int i = 0; i < posed.n; i++) {
        VkDescriptorBufferInfo bi = {vbuf[i], voff[i], 32ull * lf->nverts};
        VkWriteDescriptorSet wr[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &tb}};
        rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, G.layout, 0, 3, wr);
        vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
        vkCmdDraw(cb, 3 * lf->ntris, 1, 0, 0);
    }
    vkCmdEndRendering(cb);
    ex->recorded += 8;
    vk_exec_state_lost(ex);
    static int logged;
    if (!logged++) rt_log("figure: friends cast shadows into the sun's nearest cascade (%ux%u)", w, h);
}
