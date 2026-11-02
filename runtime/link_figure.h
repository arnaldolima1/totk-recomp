// Link's figure for friends in a multiplayer room (link_figure.c, tools/link_figure.py).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Material classes of the triangles, as tools/link_figure.py CLASSES numbers them.
enum { LF_UNTEXTURED = 0x80 };
enum { LF_SKIN, LF_HAIR, LF_TUNIC, LF_TROUSERS, LF_LEATHER, LF_EYE, LF_DARK, LF_METAL, LF_RAURU, LF_CLASSES };
// The animations: the four the speed picks lead (tools/link_figure.py ANIMS order), every other one of Link's follows.
enum { LF_WAIT = 0, LF_WALK, LF_RUN, LF_DASH };
typedef struct {
    char name[48];
    uint32_t frames;   // at 30 a second
    int loop;          // 1: it loops; else it holds its last frame
    float speed;       // ground speed it was made for (m/s), 0 standing or not one of the four
    float *mats;       // a version 3 file: frames x bones x 12, row-major 3x4 skinning matrices
    // a version 4 file: curves evaluated at the frame (link_figure_pose), pointing into the file kept in memory
    uint32_t nbones, ncurves;
    const uint8_t *bones;    // per animated bone u16 bone, u16 curve count, 9 floats (its still local transform)
    const uint8_t *curves;   // per curve u8 channel, u8 0, u16 key count
    const uint8_t **keys;    // per curve its first key: u16 frame x 32, 4 halves
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
    uint32_t atlas_rows;   // the albedo's rows when not square (a dressed figure, 2048 x 1024), 0 square
    uint32_t nbones;
    uint16_t *skin;    // per vertex four (bone, weight of 65535)
    int nanims;
    LinkAnim *anims;
    // version 4: the skeleton the animations move
    int root;          // the bone whose ground travel is left out, -1 none
    int32_t *parent;   // per bone, -1 none; a parent comes before its children
    float *bind;       // per bone 9 floats: scale xyz, euler XYZ, translation xyz
    float *inv_bind;   // per bone its inverse bind world, row-major 3x4
    char *bone_names;  // per bone 32 bytes
    // Link_RaulSkin's skinning palette (the game's, game_player_body) as the figure's bones: per entry its bone
    // (0xffff none) and 1 when rigid (the pose itself, not the pose times the inverse bind); npalm 0 in an older file
    uint32_t npalm;
    uint16_t *palm_bone;
    uint8_t *palm_rigid;
} LinkFigure;
// A friend's real body pose (net_mp.c POS lines): the rotations of the palette entries LF_BODY_ENTRY lists, each
// entry's skinning matrix in the friend's actor space (x right, y up, z forward about their feet), and the
// translations of the two whose bones head the body's chains (Spine_1 and Waist), in metres.
enum { LF_BODY_N = 58, LF_BODY_BYTES = LF_BODY_N * 8 + 12 };
typedef struct {
    float q[LF_BODY_N][4];   // unit quaternions x, y, z, w
    float t[2][3];           // entries 0 and 65
} LinkBody;
extern const uint8_t LF_BODY_ENTRY[LF_BODY_N];
// The palette entries (count x 12 floats, row-major 3x4, actor space) as the POS line's text: LF_BODY_BYTES packed
// (int16 quaternions, millimetres) in base64url; "" when there are too few entries.
void link_body_encode(const float *pal, int count, char *out, size_t cap);
int link_body_decode(const char *text, LinkBody *out);   // 1 when it was a whole pose
void link_body_lerp(const LinkBody *a, const LinkBody *b, float t, LinkBody *out);
// As link_figure_pose, with the bones the body pose has (and their children) in their real place: those without
// (the Zonai forearm, the hair) take animation anim's local transforms under them. The plain pose when the figure
// has no palette table.
void link_figure_pose_body(const LinkFigure *f, int anim, float frame, const LinkBody *body, float *out);
const LinkFigure *link_figure(void);   // NULL when <profile>/link-figure.bin is missing or unreadable
// The vertices (3 floats each, model space) of animation anim at a fractional frame (a looping one wraps, another
// holds its last frame); the bind pose for a bad anim.
void link_figure_pose(const LinkFigure *f, int anim, float frame, float *out);
int link_figure_find(const LinkFigure *f, const char *name);   // the animation of that name, -1 none
// The figure dressed in a friend's worn pieces (head, upper, lower actor names, Armor_001_Upper; "" none) from
// <profile>/link-armor.bin: the base figure's skeleton and animations with the pieces' meshes and an atlas of their
// own (2048 x 1024); the base figure when none are named or there is no library. Kept for the session (up to 8 at a time).
const LinkFigure *link_figure_dressed(const char look[3][48]);
// Turns posed vertices (model space, about the feet) by the row-major 3x3 m into the world's axes.
void link_figure_rotate(const LinkFigure *f, float *verts, const float m[9]);
// The figure's rotation: the friend's (a unit quaternion x, y, z, w; net_mp.h MpPlayer rot), then turn radians about
// its own vertical (the animation's own turn, ui/menu.cpp figure_animate), as a row-major 3x3.
void link_figure_orient(const float q[4], float turn, float m[9]);
int link_figure_bone(const LinkFigure *f, const char *name);   // the bone of that name (version 4), -1 none
// The root bone's turn about the vertical (radians, the yaw's sense) in animation anim at frame: the turn the animation
// makes of its own, which the pose leaves out (version 4; 0 without one).
float link_figure_root_yaw(const LinkFigure *f, int anim, float frame);
// The model-space positions of n bones in animation anim at frame (as link_figure_pose poses it); 0 without a
// version 4 skeleton.
int link_figure_bones_at(const LinkFigure *f, int anim, float frame, const int *bones, int n, float (*pos)[3]);
#ifdef __cplusplus
}
#endif
