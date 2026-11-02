// Multiplayer, first layer: a room on a small server (tools/mp_server.py) where friends' games share their progress,
// chat and achievement unlocks. No game state is shared: each player plays their own world.
//
// TOTK_MP (0/1) turns it on; TOTK_MP_SERVER is host[:port] (default port 5577), TOTK_MP_NAME the name shown in the
// room, TOTK_MP_PASSWORD the room's password. A thread of its own connects, reconnects, sends the progress
// achievements.c reads after each save (ach_stats) and each unlock, and keeps what the menu shows. docs/multiplayer.md.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MP_REFUSED: the room said no; no retry. MP_LOCKED: the save is still in the prologue (Link has not woken on the
// Great Sky Island yet, achievements.h AchStats.awake): the game joins once it is over.
enum { MP_OFF, MP_CONNECTING, MP_CONNECTED, MP_RETRYING, MP_REFUSED, MP_LOCKED };
// A room holds four players, as in Four Swords (tools/mp_server.py): each has a slot, 0..3, that picks their colour.
enum { MP_MAX_PLAYERS = 32, MP_NAME_MAX = 32, MP_SLOTS = 4, MP_ANIM_MAX = 48, MP_SFX_MAX = 72 };
enum { MP_STAT_SHRINES, MP_STAT_LIGHTROOTS, MP_STAT_TOWERS, MP_STAT_KOROKS, MP_STAT_HEARTS, MP_STAT_ACHIEVEMENTS,
       MP_STATS };

typedef struct {
    char name[MP_NAME_MAX], place[64];
    int stats[MP_STATS];
    int has_pos;            // a position came from the room in the last few seconds
    float pos[3];           // x east, y up, z south (metres; game_player_pos); a friend's along their path (net_mp.c)
    float vel[3];           // a friend's velocity there (m/s), 0 standing
    float yaw;              // the way a friend faces (game_player_pose), NAN when their game does not send it
    char anim[MP_ANIM_MAX]; // the animation a friend's Link plays (game_player_anim), "" when their game does not send it
    float frames[3];        // its frame: from + (to - from) u, to below from when a loop started over (net_mp.c place_on)
    float rot[4];           // a friend's Link's whole rotation (x, y, z, w; game_player_quat), from the yaw when not sent
    char look[3][48];       // the armor pieces the player wears (head, upper, lower; "" none), from LOOK lines
    int slot;               // 0..MP_SLOTS-1 from the room's SLOTS line, -1 before it came (an older server)
} MpPlayer;

// A shared marker: a point a player marked (the mark key), one per player, for MP_MARK_MS.
enum { MP_MAX_MARKERS = MP_MAX_PLAYERS, MP_MARK_MS = 15 * 60 * 1000 };
typedef struct {
    char name[MP_NAME_MAX];
    float pos[3];
    uint64_t at_ms;
} MpMarker;

// A group challenge track (tools/mp_server.py TRACKS): one figure, four tiers (bronze, silver, gold, legendary) over
// the room's total.
enum { MP_TIERS = 4, MP_TRACKS = MP_STATS };
typedef struct {
    int stat;               // MP_STAT_*
    int total, done;        // the room's total; tiers reached (0..MP_TIERS)
    int target[MP_TIERS];
} MpTrack;

typedef struct {
    int state;
    char room[64], error[200], server[128];
    char me[MP_NAME_MAX];   // this game's name in the room
    int me_has_pos;         // this game's own position, read with the ones it sends
    float me_pos[3];
    int nmarkers;
    MpMarker markers[MP_MAX_MARKERS];
    int ntracks;
    MpTrack tracks[MP_TRACKS];
    int nplayers;
    MpPlayer players[MP_MAX_PLAYERS];
    char challenge[160];
    int challenge_total, challenge_target, challenges_done;   // challenges_done: every group challenge is reached
    int ping_ms;            // the round trip to the room (PING to PONG, every 2 s, smoothed), 0 not measured yet
} MpView;

enum { MP_LINE_CHAT, MP_LINE_ACH, MP_LINE_EVENT };
typedef struct {
    int kind;
    char from[MP_NAME_MAX], text[320], ach_id[48], rarity[16];
    int64_t when;   // unix time
} MpLine;

void mp_tick(void);                    // starts the thread on the first call; cheap afterwards (the menu's frame)
void mp_view(MpView *out);
int mp_lines(MpLine *out, int max);    // the chat log, oldest first; returns how many
int mp_take_toast(MpLine *out);        // the next line to show as a notice, 1 when there was one
int mp_send_chat(const char *text);    // queued for the thread; 0 when not connected
void mp_reconnect(void);               // after the settings changed: drop the connection and try again now
int mp_mark(void);                     // marks where the player stands, for everyone in the room; 0 when not possible
int mp_slot_of(const char *name);      // that player's slot in the room, -1 when unknown
void mp_local_notice(const char *text);   // a notice of this game's own, shown with the room's (a screenshot taken)
// The voice clips that friend's Link started (comma-separated, PV000_02), each once, when the friend's shown place
// (mp_view, POS_DELAY_MS behind) reaches the moment it started; the length, 0 none.
int mp_take_sfx(const char *name, char *out, size_t cap);

// The layer of the world a height is in, for the radar and the player cards.
enum { MP_LAYER_SURFACE, MP_LAYER_SKY, MP_LAYER_DEPTHS };
static inline int mp_layer(float y) { return y > 900 ? MP_LAYER_SKY : y < -200 ? MP_LAYER_DEPTHS : MP_LAYER_SURFACE; }

#ifdef __cplusplus
}
#endif
