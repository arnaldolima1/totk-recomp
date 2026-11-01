// Link's figure for friends in a multiplayer room (link_figure.c, tools/link_figure.py).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Material classes of the triangles, as tools/link_figure.py CLASSES numbers them.
enum { LF_UNTEXTURED = 0x80 };
enum { LF_SKIN, LF_HAIR, LF_TUNIC, LF_TROUSERS, LF_LEATHER, LF_EYE, LF_DARK, LF_METAL, LF_RAURU, LF_CLASSES };
enum { LF_MAX_ANIMS = 8, LF_WAIT = 0, LF_WALK, LF_RUN, LF_DASH };   // the animations, in tools/link_figure.py ANIMS order
typedef struct {
    char name[32];
    uint32_t frames;   // at 30 a second
    float speed;       // ground speed it was made for (m/s), 0 standing
    float *mats;       // frames x bones x 12: row-major 3x4 skinning matrices
} LinkAnim;
typedef struct {
    uint32_t nverts, ntris;
    float *verts;      // 3 per vertex: metres, x right, y up, z forward (the way the figure faces)
    uint32_t *tris;    // 3 vertex indices per triangle
    uint8_t *cls;      // LF_* per triangle; bit 7 (LF_UNTEXTURED): coloured by class, not by the atlas
    float *normals;    // 3 per triangle, unit length (bind pose)
    int winding;       // 1: (b - a) x (c - a) points out of the body, -1: in
    float *uvs;        // 6 per triangle: its corners in the atlas (0..1)
    uint8_t *atlas;    // atlas_side x atlas_side RGBA8, lifted for the overlay
    uint8_t *albedo;   // the same as stored (sRGB)
    uint32_t atlas_side;
    uint32_t nbones;
    uint16_t *skin;    // per vertex four (bone, weight of 65535)
    int nanims;
    LinkAnim anims[LF_MAX_ANIMS];
} LinkFigure;
const LinkFigure *link_figure(void);   // NULL when <profile>/link-figure.bin is missing or unreadable
// The vertices (3 floats each, model space) of animation anim at a fractional frame; the bind pose for a bad anim.
void link_figure_pose(const LinkFigure *f, int anim, float frame, float *out);
#ifdef __cplusplus
}
#endif
