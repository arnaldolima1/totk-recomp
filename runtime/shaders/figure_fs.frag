#version 450
// A friend's Link figure into the game's G-buffer (vk_figure.c), written the way TOTK's character shaders write it
// (read from their translated SPIR-V; docs/maxwell.md "G-buffer"):
//   target 0: (12, material 0x1fd low byte, 0, high byte) / 255: the character material
//   target 1: albedo (an sRGB target), alpha 4 / 255
//   target 3: the view-space normal folded into two channels, |n.xy| * sqrt((1 - n.z) / 2 / |n.xy|^2), then a byte of
//             224 | 8 | 2 when n.x > 0 | 1 when n.y > 0, then a byte the lighting reads as subsurface strength
//             (high on skin)
// Target 2 is not written. Like them it marks its screen tiles in the character tile buffer.

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_colour;   // sRGB bytes r, g, b; flags in the top byte (vk_figure.c FLAG_*)
layout(location = 3) in vec3 v_pos;

layout(binding = 1) uniform sampler2D atlas;

layout(push_constant) uniform Push {
    mat4 proj;
    vec4 depth;
    vec4 hair;
    vec4 tiles;    // x, y: pixel to tile scales; z: tiles a row, w: the buffer's first word (integer bits); x 0: none
} pc;

// TOTK's character tile buffer (vk_figure.c): a word set per 16-byte tile entry marks a tile as holding a character.
layout(std430, binding = 2) buffer Tiles { uint w[]; } tiles;

layout(location = 0) out vec4 o_material;
layout(location = 1) out vec4 o_albedo;
layout(location = 3) out vec4 o_normal;

const float HAIR_LUM = 0.30;   // about the mean linear luminance of Link's hair texture: it maps to the friend's colour

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    uint flags = v_colour >> 24;
    vec3 albedo;
    if ((flags & 2u) != 0u) {   // hair: its texture's light and shade, in the friend's colour
        vec4 h = texture(atlas, v_uv);
        if (h.a < 0.5) discard;
        float lum = dot(h.rgb, vec3(0.2126, 0.7152, 0.0722));
        albedo = pc.hair.rgb * clamp(lum / HAIR_LUM, 0.25, 1.6);
    } else if ((flags & 1u) != 0u) {
        albedo = srgb_to_linear(unpackUnorm4x8(v_colour).rgb);
    } else {
        vec4 t = texture(atlas, v_uv);
        if (t.a < 0.5) discard;   // cut-out parts (eyelashes, strands)
        albedo = t.rgb;
    }
    if (pc.tiles.x > 0.0) {
        uint tx = uint(int(gl_FragCoord.x * pc.tiles.x)), ty = uint(int(gl_FragCoord.y * pc.tiles.y));
        uint at = floatBitsToUint(pc.tiles.w) + (ty * floatBitsToUint(pc.tiles.z) + tx) * 4u + 113u;
        if (at < uint(tiles.w.length())) tiles.w[at] = 1u;
    }
    vec3 n = normalize(v_normal);
    if (dot(n, -v_pos) < 0.0) n = -n;   // a face seen from behind: its normal towards the camera
    float r = sqrt(max(0.0, 0.5 - 0.5 * n.z) / max(dot(n.xy, n.xy), 1e-8));
    uint signs = 232u | (n.x > 0.0 ? 2u : 0u) | (n.y > 0.0 ? 1u : 0u);
    float sss = (flags & 4u) != 0u ? 254.0 : 18.0;
    o_material = vec4(12.0, 253.0, 0.0, 1.0) / 255.0;
    o_albedo = vec4(albedo, 4.0 / 255.0);
    o_normal = vec4(abs(n.xy) * r, float(signs) / 255.0, sss / 255.0);
}
