#version 450
// A friend's Link figure into the game's G-buffer (vk_figure.c). Each vertex is a triangle corner: the index of its
// posed vertex, its colour word and its place in the atlas. The posed vertices (view-space position, then view-space
// normal, a vec4 each) are written by the CPU every frame. The depth goes through the game's own conventions: its
// projection, then NDC z from -1..1 when the projection is GL-style, then the viewport's depth range.

layout(location = 0) in uint a_vertex;
layout(location = 1) in uint a_colour;
layout(location = 2) in vec2 a_uv;

layout(std430, binding = 0) readonly buffer Posed { vec4 v[]; } posed;

layout(push_constant) uniform Push {
    mat4 proj;     // the game's projection
    vec4 depth;    // x: 1 for a -1..1 NDC depth, y, z: the viewport's depth range
    vec4 hair;     // the friend's colour (linear)
    vec4 tiles;    // the character tile buffer: x, y scales, stride and first word (integer bits) (figure_fs.frag)
} pc;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uint v_colour;
layout(location = 3) out vec3 v_pos;

void main() {
    vec3 p = posed.v[a_vertex * 2u].xyz;
    vec4 c = pc.proj * vec4(p, 1.0);
    float z = c.z / c.w;
    if (pc.depth.x > 0.5) z = z * 0.5 + 0.5;
    z = pc.depth.y + z * (pc.depth.z - pc.depth.y);
    gl_Position = vec4(c.x, -c.y, z * c.w, c.w);   // the game's NDC y points up, Vulkan's down
    v_normal = posed.v[a_vertex * 2u + 1u].xyz;
    v_uv = a_uv;
    v_colour = a_colour;
    v_pos = p;
}
