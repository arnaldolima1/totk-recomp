// Multiplayer client (net_mp.h): one thread, one TCP connection to the room, text lines (tools/mp_server.py).
#include "net_mp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Sock;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_BAD (-1)
#define sock_close close
#endif

#include "achievements.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "ui_text.h"
#include "voice.h"

#define PROTOCOL "1"
enum { DEFAULT_PORT = 5577, LOG_LINES = 200, TOASTS = 16, OUTBOX = 16, RETRY_MS = 5000, PING_MS = 2000 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static MpView view;
static MpLine log_lines[LOG_LINES];
static int log_count, log_next;
static MpLine toasts[TOASTS];
static int toast_head, toast_tail;
static char outbox[OUTBOX][320];
static int out_head, out_tail;
static volatile int32_t started, kick;
static volatile int64_t ping_sent;   // when the PING awaiting its PONG went (ms), 0 none
static char me[MP_NAME_MAX];   // this game's name as the room keeps it (room_name): its own chat lines get no notice

// Friends' positions by name (POS lines), merged into the roster's players by mp_view. A moving player sends one every
// POS_EVERY_MS stamped with their own clock; the last POS_RING of each friend are kept, and mp_view shows the friend
// POS_DELAY_MS behind the newest, between two real places, so the figure follows the path they took (up a slope, over
// a jump) instead of a guess cutting through the ground.
enum { POS_STALE_MS = 6000, POS_EVERY_MS = 50, POS_IDLE_MS = 1000, POS_RING = 24, POS_DELAY_MS = 90, POS_AHEAD_MS = 200,
       POS_VEL_MS = 250 };
typedef struct {
    float pos[3], yaw;
    float q[4];             // the whole rotation (x, y, z, w; game_player_quat); w 2 when not sent
    int64_t t;
    char anim[MP_ANIM_MAX];
    float frame;            // the frame of anim the sender's game showed (NAN when not sent)
    char sfx[MP_SFX_MAX];   // the voice clips Link started then, comma-separated ("" none)
    int sfx_taken;          // mp_take_sfx gave them out
    int has_body;           // the sender's real body pose came (link_body_decode)
    LinkBody body;          // with the gear's places (gear_parse)
    char gear[LF_GEAR][48]; // what the sender carries (LF_GEAR_* actors, "" none)
} PosSample;
// t: the sender's clock (ms); yaw NAN when not sent; anim the animation Link plays, "" when not sent
static struct {
    char name[MP_NAME_MAX];
    float pos[3];          // the newest place (radar, the meeting achievement)
    uint64_t at_ms;        // when it came
    PosSample ring[POS_RING];
    int n, head;           // samples kept; the next slot
    int64_t offset;        // this game's clock minus the sender's, the smallest seen lately (the fastest delivery)
} friend_pos[MP_MAX_PLAYERS];
// Friends' armor by name (LOOK lines: what each wears now, or at their last save from a game that cannot read it
// live), merged into the roster by mp_view.
static struct { char name[MP_NAME_MAX]; char look[3][48]; } friend_look[MP_MAX_PLAYERS];
static volatile int32_t mark_pending;   // mp_mark asked; the connection's thread sends it

static int64_t now_unix(void) { return (int64_t)time(NULL); }

// Copies s into a field of the protocol: no tab, no line break.
static void field(char *dst, size_t cap, const char *s) {
    size_t n = 0;
    for (; s && *s && n + 1 < cap; s++) dst[n++] = *s == '\t' || *s == '\r' || *s == '\n' ? ' ' : *s;
    dst[n] = 0;
}

// The name as tools/mp_server.py keeps it: surrounding spaces stripped, at most 24 characters (code points).
static void room_name(char *dst, size_t cap, const char *name) {
    while (*name == ' ') name++;
    size_t n = 0, chars = 0;
    for (const char *p = name; *p && n + 1 < cap; p++) {
        if (((unsigned char)*p & 0xc0) != 0x80 && ++chars > 24) break;
        dst[n++] = *p;
    }
    while (n && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

// Under lock.
static void add_line_at(int kind, const char *from, const char *text, const char *ach_id, const char *rarity, int toast,
                        int64_t when) {
    MpLine *l = &log_lines[log_next];
    memset(l, 0, sizeof *l);
    l->kind = kind, l->when = when;
    field(l->from, sizeof l->from, from);
    field(l->text, sizeof l->text, text);
    field(l->ach_id, sizeof l->ach_id, ach_id);
    field(l->rarity, sizeof l->rarity, rarity);
    log_next = (log_next + 1) % LOG_LINES;
    if (log_count < LOG_LINES) log_count++;
    int next = (toast_tail + 1) % TOASTS;
    if (toast && next != toast_head) {
        toasts[toast_tail] = *l;
        toast_tail = next;
    }
}

static void add_line(int kind, const char *from, const char *text, const char *ach_id, const char *rarity, int toast) {
    add_line_at(kind, from, text, ach_id, rarity, toast, now_unix());
}

// The names the game gives its items (<profile>/item-names.tsv, tools/item_names.py: actor, English, Portuguese,
// Russian), loaded at the first item event; the actor's own name, its underscores as spaces, when there is none.
typedef struct { char *actor, *name[3]; } ItemName;
static ItemName *item_names;
static int item_count = -1;

static int cmp_item(const void *a, const void *b) { return strcmp(((const ItemName *)a)->actor, ((const ItemName *)b)->actor); }

static void item_display(const char *actor, char *out, size_t cap) {
    if (item_count < 0) {
        item_count = 0;
        char path[1200];
        snprintf(path, sizeof path, "%s/item-names.tsv", rt_config()->profile);
        uint64_t n = 0;
        char *d = (char *)plat_file_read(path, &n);   // kept: the names point into it
        int lines = 0;
        for (uint64_t i = 0; d && i < n; i++) lines += d[i] == '\n';
        if (d && lines && (item_names = calloc((size_t)lines, sizeof *item_names))) {
            for (char *line = d, *end; line < d + n && item_count < lines; line = end + 1) {
                end = memchr(line, '\n', (size_t)(d + n - line));
                if (!end) break;
                *end = 0;
                ItemName *it = &item_names[item_count];
                char *f[4] = {line, NULL, NULL, NULL};
                for (int k = 1; k < 4 && f[k - 1]; k++)
                    if ((f[k] = strchr(f[k - 1], '\t'))) *f[k]++ = 0;
                if (!f[1]) continue;
                it->actor = f[0], it->name[0] = f[1], it->name[1] = f[2] ? f[2] : "", it->name[2] = f[3] ? f[3] : "";
                item_count++;
            }
            qsort(item_names, (size_t)item_count, sizeof *item_names, cmp_item);
        }
        rt_log("mp: %d item names from %s", item_count, path);
    }
    ItemName key = {(char *)actor, {NULL, NULL, NULL}};
    const ItemName *it = item_count > 0 ? bsearch(&key, item_names, (size_t)item_count, sizeof key, cmp_item) : NULL;
    const char *lang = menu_lang();
    int col = !strncmp(lang, "pt", 2) ? 1 : !strncmp(lang, "ru", 2) ? 2 : 0;
    if (it && it->name[col][0]) snprintf(out, cap, "%s", it->name[col]);
    else if (it && it->name[0][0]) snprintf(out, cap, "%s", it->name[0]);
    else {
        size_t k = 0;
        for (; actor[k] && k + 1 < cap; k++) out[k] = actor[k] == '_' ? ' ' : actor[k];
        out[k] = 0;
    }
}

// The game's quests (<profile>/quests.tsv, tools/item_names.py --quests: quest, category, steps, English, Portuguese,
// Russian titles), loaded at first use. A quest's progress is the enum flag Step_<quest>: NotReady, its steps in
// order, Complete, each value the hash of its name (game_flag_hash).
enum { QUEST_STEPS = 24 };
typedef struct {
    char *id, *title[3], *category;
    uint32_t flag;                    // hash of Step_<quest>
    int nsteps;                       // steps[0] NotReady, then the quest's, then Complete
    char *step[QUEST_STEPS];
    uint32_t step_hash[QUEST_STEPS];
    char *text[QUEST_STEPS][3];       // the adventure log's text at each step (quest-steps.tsv), NULL none
} Quest;
static Quest *quests;
static int quest_count = -1;

static PlatMutex quest_load_lock = PLAT_MUTEX_INIT;   // the menu's thread and the room's both load the list

static void load_quests(void) {
    plat_mutex_lock(&quest_load_lock);
    if (quest_count >= 0) {
        plat_mutex_unlock(&quest_load_lock);
        return;
    }
    int loaded = 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/quests.tsv", rt_config()->profile);
    uint64_t n = 0;
    char *d = (char *)plat_file_read(path, &n);   // kept: the quests point into it
    int lines = 0;
    for (uint64_t i = 0; d && i < n; i++) lines += d[i] == '\n';
    if (!d || !lines || !(quests = calloc((size_t)lines, sizeof *quests))) {
        quest_count = 0;
        plat_mutex_unlock(&quest_load_lock);
        return;
    }
    static char not_ready[] = "NotReady", complete[] = "Complete";
    for (char *line = d, *end; line < d + n && loaded < lines; line = end + 1) {
        if (!(end = memchr(line, '\n', (size_t)(d + n - line)))) break;
        *end = 0;
        char *f[6] = {line};
        for (int k = 1; k < 6 && f[k - 1]; k++)
            if ((f[k] = strchr(f[k - 1], '\t'))) *f[k]++ = 0;
        if (!f[2]) continue;
        Quest *q = &quests[loaded];
        q->id = f[0], q->category = f[1] ? f[1] : "";
        for (int k = 0; k < 3; k++) q->title[k] = f[3 + k] ? f[3 + k] : "";
        q->step[q->nsteps++] = not_ready;
        for (char *st = f[2], *comma; st && *st && q->nsteps < QUEST_STEPS - 1; st = comma ? comma + 1 : NULL) {
            if ((comma = strchr(st, ','))) *comma = 0;
            q->step[q->nsteps++] = st;
        }
        q->step[q->nsteps++] = complete;
        char name[96];
        snprintf(name, sizeof name, "Step_%s", q->id);
        q->flag = game_flag_hash(name);
        for (int k = 0; k < q->nsteps; k++) q->step_hash[k] = game_flag_hash(q->step[k]);
        loaded++;
    }
    // the adventure log's texts (quest-steps.tsv: quest, step, English, Portuguese, Russian)
    snprintf(path, sizeof path, "%s/quest-steps.tsv", rt_config()->profile);
    uint64_t tn = 0;
    char *t = (char *)plat_file_read(path, &tn);   // kept as the quests' file is
    int texts = 0;
    for (char *line = t, *end; t && line < t + tn; line = end + 1) {
        if (!(end = memchr(line, '\n', (size_t)(t + tn - line)))) break;
        *end = 0;
        char *f[5] = {line};
        for (int k = 1; k < 5 && f[k - 1]; k++)
            if ((f[k] = strchr(f[k - 1], '\t'))) *f[k]++ = 0;
        if (!f[2]) continue;
        for (int i = 0; i < loaded; i++) {
            if (strcmp(quests[i].id, f[0])) continue;
            for (int k = 0; k < quests[i].nsteps; k++)
                if (!strcmp(quests[i].step[k], f[1])) {
                    for (int l = 0; l < 3; l++) quests[i].text[k][l] = f[2 + l] && f[2 + l][0] ? f[2 + l] : NULL;
                    texts++;
                }
            break;
        }
    }
    quest_count = loaded;
    rt_log("mp: %d quests and %d step texts from %s", quest_count, texts, rt_config()->profile);
    plat_mutex_unlock(&quest_load_lock);
}

// Each player's quest steps by quest number (QUESTS lines; this game's own under its name), 0 not under way.
enum { MAX_QUESTS = 512 };
static struct { char name[MP_NAME_MAX]; uint8_t step[MAX_QUESTS]; } quest_state[MP_MAX_PLAYERS + 1];

static void set_quests(const char *name, const char *list) {   // "q:k,q:k" under the lock
    int slot = -1, free_slot = -1;
    for (int i = 0; i <= MP_MAX_PLAYERS; i++) {
        if (!strcmp(quest_state[i].name, name)) slot = i;
        if (!quest_state[i].name[0] && free_slot < 0) free_slot = i;
    }
    if (slot < 0) slot = free_slot >= 0 ? free_slot : MP_MAX_PLAYERS;
    field(quest_state[slot].name, sizeof quest_state[0].name, name);
    memset(quest_state[slot].step, 0, sizeof quest_state[0].step);
    for (const char *p = list; p && *p;) {
        char *end;
        long q = strtol(p, &end, 10);
        if (*end != ':') break;
        long k = strtol(end + 1, &end, 10);
        if (q >= 0 && q < MAX_QUESTS && k >= 0 && k < 256) quest_state[slot].step[q] = (uint8_t)k;
        p = *end == ',' ? end + 1 : NULL;
    }
}

int mp_quest_step(const char *name, int i) {
    plat_mutex_lock(&lock);
    int k = 0;
    for (int s = 0; s <= MP_MAX_PLAYERS && i >= 0 && i < MAX_QUESTS; s++)
        if (quest_state[s].name[0] && !strcmp(quest_state[s].name, name)) k = quest_state[s].step[i];
    plat_mutex_unlock(&lock);
    return k;
}

int mp_quest_count(void) {
    load_quests();
    return quest_count < MAX_QUESTS ? quest_count : MAX_QUESTS;
}

const char *mp_quest_title(int i) {
    if (i < 0 || i >= mp_quest_count()) return "";
    const char *lang = menu_lang();
    int col = !strncmp(lang, "pt", 2) ? 1 : !strncmp(lang, "ru", 2) ? 2 : 0;
    return quests[i].title[col][0] ? quests[i].title[col] : quests[i].title[0][0] ? quests[i].title[0] : quests[i].id;
}

const char *mp_quest_step_text(int i, int k) {
    if (i < 0 || i >= mp_quest_count() || k < 0 || k >= quests[i].nsteps) return "";
    const char *lang = menu_lang();
    int col = !strncmp(lang, "pt", 2) ? 1 : !strncmp(lang, "ru", 2) ? 2 : 0;
    return quests[i].text[k][col] ? quests[i].text[k][col] : quests[i].text[k][0] ? quests[i].text[k][0] : "";
}

const char *mp_quest_category(int i) { return i >= 0 && i < mp_quest_count() ? quests[i].category : ""; }
int mp_quest_steps(int i) { return i >= 0 && i < mp_quest_count() ? quests[i].nsteps - 3 : 0; }

static const Quest *quest_by_id(const char *id) {
    load_quests();
    for (int i = 0; i < quest_count; i++)
        if (!strcmp(quests[i].id, id)) return &quests[i];
    return NULL;
}

// An EVENT line's text in the menu's language for the kinds it knows (tools/mp_server.py Room.event: kind, the room's
// text, the player, the step, the total), else the room's own text.
static void event_text(const char *kind, const char *who, const char *arg, int total, const char *room_text, char *out,
                       size_t cap) {
    int step = atoi(arg);
    static const struct { const char *kind; UiTextId one, many; } kinds[] = {
        {"shrines", UI_MENU_MP_EV_SHRINES_1, UI_MENU_MP_EV_SHRINES_N},
        {"lightroots", UI_MENU_MP_EV_LIGHTROOTS_1, UI_MENU_MP_EV_LIGHTROOTS_N},
        {"towers", UI_MENU_MP_EV_TOWERS_1, UI_MENU_MP_EV_TOWERS_N},
        {"koroks", UI_MENU_MP_EV_KOROKS_1, UI_MENU_MP_EV_KOROKS_N},
        {"hearts", UI_MENU_MP_EV_HEARTS_1, UI_MENU_MP_EV_HEARTS_N},
        {"death", UI_MENU_MP_EV_DEATH, UI_MENU_MP_EV_DEATH},
        {"warp", UI_MENU_MP_EV_WARP, UI_MENU_MP_EV_WARP},
        {"join", UI_MENU_MP_EV_JOIN, UI_MENU_MP_EV_JOIN},
        {"leave", UI_MENU_MP_EV_LEAVE, UI_MENU_MP_EV_LEAVE},
        {"chest", UI_MENU_MP_EV_CHEST, UI_MENU_MP_EV_CHEST},
    };
    static const struct { const char *actor; UiTextId name; } powers[] = {   // tools/mp_server.py POWERS
        {"Obj_UltraHand", UI_MENU_MP_POWER_ULTRAHAND}, {"Obj_ReverseRecorder", UI_MENU_MP_POWER_RECALL},
        {"Obj_AutoBuilder", UI_MENU_MP_POWER_AUTOBUILD}, {"Obj_Camera", UI_MENU_MP_POWER_CAMERA},
    };
    snprintf(out, cap, "%s", room_text);
    if (!who[0]) return;
    for (size_t i = 0; !strcmp(kind, "item") && i < sizeof powers / sizeof *powers; i++)
        if (!strcmp(arg, powers[i].actor)) {
            snprintf(out, cap, menu_text(UI_MENU_MP_EV_POWER), who, menu_text(powers[i].name));
            return;
        }
    if (!strcmp(kind, "hit") && arg[0]) {   // victim:quarter hearts
        char victim[MP_NAME_MAX + 8], hearts[16];
        snprintf(victim, sizeof victim, "%s", arg);
        char *colon = strchr(victim, ':');
        int q = colon ? atoi(colon + 1) : 0;
        if (colon) *colon = 0;
        if (q % 4) snprintf(hearts, sizeof hearts, "%d,%d", q / 4, (q % 4) * 25);
        else snprintf(hearts, sizeof hearts, "%d", q / 4);
        snprintf(out, cap, menu_text(UI_MENU_MP_EV_HIT), who, victim, hearts);
        return;
    }
    if (!strcmp(kind, "block") && arg[0]) {   // the attacker whose strike the shield stopped
        snprintf(out, cap, menu_text(UI_MENU_MP_EV_BLOCK), who, arg);
        return;
    }
    if (!strcmp(kind, "duel") && arg[0]) {   // the loser
        snprintf(out, cap, menu_text(UI_MENU_MP_EV_DUEL), who, arg);
        return;
    }
    if (!strcmp(kind, "quest") && arg[0]) {   // quest:step - begun, a step further, or done
        char id[96];
        snprintf(id, sizeof id, "%s", arg);
        char *step = strchr(id, ':'), *from = NULL;   // quest:step:the step it left
        if (step) *step++ = 0;
        if (step && (from = strchr(step, ':'))) *from++ = 0;
        const Quest *q = quest_by_id(id);
        const char *lang = menu_lang();
        int col = !strncmp(lang, "pt", 2) ? 1 : !strncmp(lang, "ru", 2) ? 2 : 0;
        const char *title = q && q->title[col][0] ? q->title[col] : q && q->title[0][0] ? q->title[0] : id;
        int at = -1;
        for (int k = 0; q && step && k < q->nsteps; k++)
            if (!strcmp(q->step[k], step)) at = k;
        int fresh = !from || !strcmp(from, "NotReady") || !strcmp(from, "Ready");   // not under way before
        if (q && at == q->nsteps - 1) snprintf(out, cap, menu_text(UI_MENU_MP_EV_QUEST_DONE), who, title);
        else if (q && at >= 1 && fresh) snprintf(out, cap, menu_text(UI_MENU_MP_EV_QUEST_START), who, title);
        else if (q && at > 1) snprintf(out, cap, menu_text(UI_MENU_MP_EV_QUEST_STEP), who, title, at - 1, q->nsteps - 3);
        return;
    }
    if (!strcmp(kind, "item") && arg[0]) {   // the item's name as the game gives it
        char item[96];
        item_display(arg, item, sizeof item);
        snprintf(out, cap, menu_text(UI_MENU_MP_EV_ITEM), who, item);
        return;
    }
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++) {
        if (strcmp(kind, kinds[i].kind)) continue;
        if (kinds[i].one == kinds[i].many) snprintf(out, cap, menu_text(kinds[i].one), who);
        else if (step > 1) snprintf(out, cap, menu_text(kinds[i].many), who, step, total);
        else snprintf(out, cap, menu_text(kinds[i].one), who, total);
        return;
    }
}

void mp_local_notice(const char *text) {
    plat_mutex_lock(&lock);
    add_line(MP_LINE_EVENT, "", text, "", "", 1);
    plat_mutex_unlock(&lock);
}

// ---- the duel (TOTK_MP_DUEL): friends who have it on too take life from this game's Link with their strikes. Each
// friend's animation (POS lines) is followed: a strike begins when an attack animation starts (or starts over); 150 ms
// on, while it is still under way, it hits when the friend stands within the weapon's reach of Link, at most 2 m above
// or below, and faces him (within 70 degrees). One hit a strike; the damage by weapon. A shield raised (a layer of
// Link's animation *_Guard*, game_player_anim_layers) toward the friend (within 80 degrees of his facing) stops it:
// no damage, the room is told. The hit goes through the game's
// own damage step (hit_hook.c), so Link reacts as to an enemy's blow; without the hook the life is written. When the
// life would reach zero the strike leaves him one quarter, the life comes back full 1.2 s later and the room is told
// the friend won: nobody really dies.
static struct { char name[MP_NAME_MAX]; int on; } friend_duel[MP_MAX_PLAYERS];
static int duel_sent = -1;   // whether this game's duel is on, as the room last heard (-1: not told on this connection)

typedef struct { const char *prefix; int quarters; float reach; } Strike;
static const Strike kStrikes[] = {
    {"Lsword_Attack", 6, 2.4f}, {"Spear_Attack", 4, 2.8f}, {"Sword_Blunt_Attack", 4, 1.9f}, {"Sword_Attack", 4, 1.9f},
};

static const Strike *strike_of(const char *anim, int *quarters) {
    for (size_t i = 0; i < sizeof kStrikes / sizeof *kStrikes; i++)
        if (!strncmp(anim, kStrikes[i].prefix, strlen(kStrikes[i].prefix))) {
            int q = kStrikes[i].quarters;
            if (strstr(anim, "Charge")) q *= 2;                                    // a charged strike: twice
            else if (strstr(anim, "Jump") || strstr(anim, "Fall")) q = q * 3 / 2;   // from the air: half again
            *quarters = q;
            return &kStrikes[i];
        }
    return NULL;
}

static int duel_on(const char *name) {
    for (int i = 0; i < MP_MAX_PLAYERS; i++)
        if (friend_duel[i].name[0] && !strcmp(friend_duel[i].name, name)) return friend_duel[i].on;
    return 0;
}

static void set_duel(const char *name, int on) {   // under the lock
    int slot = -1, free_slot = -1;
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        if (!strcmp(friend_duel[i].name, name)) slot = i;
        if (!friend_duel[i].name[0] && free_slot < 0) free_slot = i;
    }
    if (slot < 0) slot = free_slot >= 0 ? free_slot : 0;
    field(friend_duel[slot].name, sizeof friend_duel[0].name, name);
    friend_duel[slot].on = on;
}

static void place_at(int k, uint64_t now, float pos[3], float vel[3], float *yaw, char anim[MP_ANIM_MAX], float rot[4],
                     float frames[3], LinkBody *body, int *has_body);

static int send_line(Sock s, int nfields, const char *const *fields);

// Each friend's strike under way: its animation, when it began, whether it has hit. The room's thread, each POS tick.
static int duel_tick(Sock s, uint64_t now) {
    static struct { char name[MP_NAME_MAX], anim[MP_ANIM_MAX]; float frame; uint64_t began; int hit, quarters; float reach; } sw[MP_MAX_PLAYERS];
    static uint64_t refill_at;   // a lost duel: when the life comes back full
    if (refill_at && now >= refill_at) {
        int life = 0, max_life = 0;
        if (game_player_life(&life, &max_life)) game_player_life_set(max_life > 0 ? max_life : 12);
        refill_at = 0;
    }
    if (!cfg_bool("TOTK_MP_DUEL")) return 1;
    float me[3], my_yaw;
    if (!game_player_pose(me, &my_yaw)) return 1;
    for (int k = 0; k < MP_MAX_PLAYERS; k++) {
        float pos[3], vel[3], yaw = NAN, rot[4] = {0, 0, 0, 1}, frames[3];
        char anim[MP_ANIM_MAX], name[MP_NAME_MAX];
        plat_mutex_lock(&lock);
        int live = friend_pos[k].name[0] && friend_pos[k].n > 0 && now - friend_pos[k].at_ms < POS_STALE_MS &&
                   duel_on(friend_pos[k].name);
        if (live) {
            place_at(k, now, pos, vel, &yaw, anim, rot, frames, NULL, NULL);
            snprintf(name, sizeof name, "%s", friend_pos[k].name);
        }
        plat_mutex_unlock(&lock);
        if (!live) continue;
        int quarters = 0;
        const Strike *st = strike_of(anim, &quarters);
        float frame = isfinite(frames[0]) ? frames[0] : 0;
        if (strcmp(sw[k].name, name)) memset(&sw[k], 0, sizeof sw[k]), snprintf(sw[k].name, sizeof sw[k].name, "%s", name);
        if (!st) {
            sw[k].anim[0] = 0;
            continue;
        }
        if (strcmp(anim, sw[k].anim) || frame + 1 < sw[k].frame) {   // a strike begins (or begins again)
            snprintf(sw[k].anim, sizeof sw[k].anim, "%s", anim);
            sw[k].began = now, sw[k].hit = 0, sw[k].quarters = quarters, sw[k].reach = st->reach;
        }
        sw[k].frame = frame;
        if (sw[k].hit || now - sw[k].began < 150) continue;
        // within reach, and facing Link: the friend's forward (their rotation's local +z) against the way to him
        float dx = me[0] - pos[0], dz = me[2] - pos[2], dist = sqrtf(dx * dx + dz * dz);
        float qx = rot[0], qy = rot[1], qz = rot[2], qw = rot[3];
        float fx = 2 * (qx * qz + qw * qy), fz = 1 - 2 * (qx * qx + qy * qy), fl = sqrtf(fx * fx + fz * fz);
        if (dist > sw[k].reach || fabsf(me[1] - pos[1]) > 2.0f || fl < 1e-3f) continue;
        if (dist > 0.3f && (fx * dx + fz * dz) / (fl * dist) < 0.34f) continue;   // cos 70 degrees
        sw[k].hit = 1;
        char lay[8][64];
        int nl = game_player_anim_layers(lay, 8), guard = 0;
        for (int i = 0; i < nl && i < 3; i++) guard |= strstr(lay[i], "_Guard") != NULL;
        if (guard && isfinite(my_yaw) && dist > 0.1f && (-dx * sinf(my_yaw) - dz * cosf(my_yaw)) / dist > 0.17f) {   // cos 80
            const char *act[] = {"ACT", "block", name};
            if (!send_line(s, 3, act)) return 0;
            rt_log("mp: duel: %s's %s blocked (%.1f m)", name, sw[k].anim, dist);
            continue;
        }
        int life = 0, max_life = 0;
        if (!game_player_life(&life, &max_life)) continue;
        char q[16];
        snprintf(q, sizeof q, "%d", sw[k].quarters);
        if (life - sw[k].quarters <= 0) {   // the duel lost: a last blow to one quarter, the life back full, the room told
            if (hit_hook_strike(pos, life > 1 ? life - 1 : 0)) refill_at = now + 1200;
            else game_player_life_set(max_life > 0 ? max_life : 12);
            const char *act[] = {"ACT", "duel", name};
            if (!send_line(s, 3, act)) return 0;
        } else {
            if (!hit_hook_strike(pos, sw[k].quarters)) game_player_life_set(life - sw[k].quarters);
            const char *act[] = {"ACT", "hit", name, q};
            if (!send_line(s, 4, act)) return 0;
        }
        rt_log("mp: duel: %s's %s hit (%.1f m): %d -> %d quarters", name, sw[k].anim, dist, life,
               life - sw[k].quarters <= 0 ? max_life : life - sw[k].quarters);
    }
    return 1;
}

// The prologue is over: Link has woken on the Great Sky Island in the newest save. Rooms are for the open game, so
// the client waits until then (MP_LOCKED). Without achievements loaded no save is read: the gate stays open.
static int island_reached(void) {
    AchStats st;
    ach_stats(&st);
    return st.awake == 1 || (st.awake < 0 && ach_count() == 0);
}

static void set_state(int state, const char *error) {
    plat_mutex_lock(&lock);
    view.state = state;
    snprintf(view.error, sizeof view.error, "%s", error ? error : "");
    if (state != MP_CONNECTED) view.nplayers = 0;
    plat_mutex_unlock(&lock);
}

// ---- the connection

static int send_all(Sock s, const char *data, size_t n) {
    while (n) {
        int k = (int)send(s, data, (int)n, 0);
        if (k <= 0) return 0;
        data += k, n -= (size_t)k;
    }
    return 1;
}

// Sends the fields as one line.
static int send_line(Sock s, int nfields, const char *const *fields) {
    char buf[2048];
    size_t n = 0;
    for (int i = 0; i < nfields && n + 2 < sizeof buf; i++) {
        if (i) buf[n++] = '\t';
        char f[1024];
        field(f, sizeof f, fields[i]);
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", f);
    }
    if (n + 1 >= sizeof buf) n = sizeof buf - 2;
    buf[n++] = '\n';
    return send_all(s, buf, n);
}

// POS x y z yaw t anim quat sfx: centimetres, the way Link faces (radians, "" when unread), this game's clock in
// milliseconds, the animation Link plays and its frame (game_player_anim: "Name:frame", "" when unread), his whole
// rotation (game_player_quat:
// "x,y,z,w", "" when unread), the voice clips he started since the last line (game_player_voices, comma-separated) and
// his real body pose (game_player_body as link_body_encode packs it, "" when unread) and what he carries (gear_text).
static int send_pos(Sock s, const float p[3], float yaw, const float q[4], int64_t t, const char *anim, float frame,
                    const char *sfx, const char *body, const char *gear) {
    char x[24], y[24], z[24], w[24], tt[24], qs[64], an[MP_ANIM_MAX + 16];
    if (anim[0] && isfinite(frame)) snprintf(an, sizeof an, "%s:%.2f", anim, frame);
    else snprintf(an, sizeof an, "%s", anim);
    snprintf(x, sizeof x, "%.2f", p[0]), snprintf(y, sizeof y, "%.2f", p[1]), snprintf(z, sizeof z, "%.2f", p[2]);
    if (isfinite(yaw)) snprintf(w, sizeof w, "%.3f", yaw);
    else w[0] = 0;
    snprintf(tt, sizeof tt, "%lld", (long long)t);
    if (q[3] <= 1) snprintf(qs, sizeof qs, "%.4f,%.4f,%.4f,%.4f", q[0], q[1], q[2], q[3]);
    else qs[0] = 0;
    const char *pos[] = {"POS", x, y, z, w, tt, an, qs, sfx, body, gear};
    return send_line(s, gear[0] ? 11 : body[0] ? 10 : 9, pos);
}

// What Link carries (game_player_gear) as a POS line's field: per LF_GEAR_* item "actor@a:e;e;..." where a is the bone
// it hangs from (link_gear_anchor, LF_ANCHOR_*) and each e a palette entry in that bone's space "qx,qy,qz,qw,x,y,z"
// (rotation, place in millimetres); "actor" alone for the arrow (the figure draws it on the string); "" for none, all
// joined by '|'. An item not found yet stays out (no guess in the wrong hand); one lost for a moment keeps its last
// place on its bone while the game looks for it again. pal: Link's body palette (npal entries, actor space).
static void gear_text(char *out, size_t cap, const float *pal, int npal) {
    static GameGear g[LF_GEAR];
    static struct { char actor[48]; int anchor, n; float rel[LF_GEAR_BONES][12]; } last[LF_GEAR];
    int n = game_player_gear(g, LF_GEAR), any = 0;
    size_t len = 0;
    out[0] = 0;
    for (int k = 0; k < LF_GEAR && len + 2 < cap; k++) {
        if (k) out[len++] = '|', out[len] = 0;
        if (k >= n || !g[k].model[0]) continue;
        if (k == LF_GEAR_ARROW) {
            len += (size_t)snprintf(out + len, cap - len, "%s", g[k].model);
            any = 1;
            continue;
        }
        float inv[12];
        int anchor = g[k].n > 0 && npal > 0 ? link_gear_anchor(pal, npal, g[k].m[0], inv) : -1;
        if (anchor >= 0) {   // into the bone's space, kept
            snprintf(last[k].actor, sizeof last[k].actor, "%s", g[k].model);
            last[k].anchor = anchor, last[k].n = g[k].n;
            for (int e = 0; e < g[k].n; e++) link_figure_mul34(inv, g[k].m[e], last[k].rel[e]);
        } else if (strcmp(last[k].actor, g[k].model) || !last[k].n)
            continue;   // not found yet
        len += (size_t)snprintf(out + len, cap - len, "%s@%d:", last[k].actor, last[k].anchor);
        for (int e = 0; e < last[k].n && len + 48 < cap; e++) {
            float qq[4];
            const float *m = last[k].rel[e];
            link_figure_quat_of(m, qq);
            len += (size_t)snprintf(out + len, cap - len, "%s%.3f,%.3f,%.3f,%.3f,%d,%d,%d", e ? ";" : "", qq[0], qq[1], qq[2], qq[3],
                                    (int)lrintf(m[3] * 1000), (int)lrintf(m[7] * 1000), (int)lrintf(m[11] * 1000));
        }
        if (len >= cap) len = cap - 1;
        any = 1;
    }
    if (!any) out[0] = 0;
}

// A POS line's gear field into names (LF_GEAR_* actors, "" none) and the body's gear places. An entry with the
// actor alone (a game or a test that sends no body pose) names the item: the figure holds it in the hand.
static void gear_parse(const char *text, char names[LF_GEAR][48], LinkBody *body) {
    memset(names, 0, LF_GEAR * 48);
    memset(body->gear_n, 0, sizeof body->gear_n);
    const char *p = text;
    for (int k = 0; k < LF_GEAR && p; k++) {
        const char *end = strchr(p, '|'), *colon = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (colon && end && colon > end) colon = NULL;
        size_t nl = colon ? (size_t)(colon - p) : len;
        const char *at = memchr(p, '@', nl);   // the bone it hangs from
        body->gear_anchor[k] = at ? (float)atoi(at + 1) : -1;
        if (at) nl = (size_t)(at - p);
        if (nl < 48) memcpy(names[k], p, nl);
        for (const char *e = colon ? colon + 1 : NULL; e && (!end || e < end) && body->gear_n[k] < LF_GEAR_BONES;) {
            float v[4];
            int mm[3], i = (int)body->gear_n[k];
            if (sscanf(e, "%f,%f,%f,%f,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &mm[0], &mm[1], &mm[2]) != 7) break;
            float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
            if (!(l > 0.5f && l < 1.5f) || abs(mm[0]) > 5000 || abs(mm[1]) > 5000 || abs(mm[2]) > 5000) break;
            for (int c = 0; c < 4; c++) body->gear_q[k][i][c] = v[c] / l;
            for (int c = 0; c < 3; c++) body->gear_t[k][i][c] = mm[c] / 1000.0f;
            body->gear_n[k] = (float)(i + 1);
            e = strchr(e, ';');
            if (e) e++;
        }
        p = end ? end + 1 : NULL;
    }
}

// host[:port] into a connected socket, or SOCK_BAD with *why set.
static Sock dial(const char *server, const char **why) {
    char host[128], port[16];
    snprintf(host, sizeof host, "%s", server);
    snprintf(port, sizeof port, "%d", DEFAULT_PORT);
    char *colon = strrchr(host, ':');
    if (colon && !strchr(colon + 1, ']')) {
        *colon = 0;
        snprintf(port, sizeof port, "%s", colon + 1);
    }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC, hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) || !res) {
        *why = "address";
        return SOCK_BAD;
    }
    Sock s = SOCK_BAD;
    for (struct addrinfo *a = res; a && s == SOCK_BAD; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == SOCK_BAD) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen)) {
            sock_close(s);
            s = SOCK_BAD;
        }
    }
    freeaddrinfo(res);
    if (s == SOCK_BAD) *why = "connect";
    else {   // lines go out as written: Nagle would hold a place or a voice frame until the last one is acknowledged,
             // and Windows delays that acknowledgement up to 200 ms
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    }
    return s;
}

// ---- voice frames travel as base64 in VOICE lines

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(char *out, const uint8_t *in, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63], out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int b64_decode(uint8_t *out, int cap, const char *in) {
    uint32_t v = 0;
    int bits = 0, n = 0;
    for (; *in && *in != '='; in++) {
        const char *c = strchr(B64, *in);
        if (!c || !*in) return -1;
        v = v << 6 | (uint32_t)(c - B64), bits += 6;
        if (bits >= 8) {
            if (n >= cap) return -1;
            bits -= 8, out[n++] = (uint8_t)(v >> bits);
        }
    }
    return n;
}

static const char *rarity_name(int r) {
    static const char *const n[] = {"common", "rare", "epic", "legendary", "platinum"};
    return r >= 0 && r < 5 ? n[r] : "common";
}

// One line from the room.
static void on_line(char *line, Sock s) {
    char *f[4 + 2 * (MP_MAX_PLAYERS * (2 + MP_STATS))];
    int n = 0, cap = (int)(sizeof f / sizeof *f);
    f[n++] = line;
    for (char *p = line; *p && n < cap; p++)
        if (*p == '\t') *p = 0, f[n++] = p + 1;
    if (!strcmp(f[0], "POS") && n >= 5) {   // POS player x y z [yaw t [anim [quat [sfx]]]]: an older game, the place alone
        uint64_t now = plat_now_ms();
        PosSample smp;
        for (int k = 0; k < 3; k++) smp.pos[k] = (float)atof(f[2 + k]);
        smp.yaw = n >= 6 && f[5][0] ? (float)atof(f[5]) : NAN;
        smp.t = n >= 7 && f[6][0] ? strtoll(f[6], NULL, 10) : (int64_t)now;
        smp.anim[0] = 0, smp.sfx[0] = 0, smp.sfx_taken = 0, smp.frame = NAN;
        if (n >= 8) {   // Name or Name:frame
            field(smp.anim, sizeof smp.anim, f[7]);
            char *colon = strchr(smp.anim, ':');
            if (colon) {
                *colon = 0;
                float fr = (float)atof(colon + 1);
                if (isfinite(fr) && fr >= 0) smp.frame = fr;
            }
        }
        smp.q[3] = 2;
        float q[4], ql = 0;
        if (n >= 9 && sscanf(f[8], "%f,%f,%f,%f", &q[0], &q[1], &q[2], &q[3]) == 4 &&
            (ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])) > 0.5f && ql < 1.5f)
            for (int k = 0; k < 4; k++) smp.q[k] = q[k] / ql;
        if (n >= 10) field(smp.sfx, sizeof smp.sfx, f[9]);
        smp.has_body = n >= 11 && link_body_decode(f[10], &smp.body);
        memset(smp.gear, 0, sizeof smp.gear);
        if (n >= 12) gear_parse(f[11], smp.gear, &smp.body);
        static int body_logged;
        if (smp.has_body && !body_logged++) rt_log("mp: %s's game sends their Link's body pose", f[1]);
        static int bad_logged;
        if (n >= 11 && !smp.has_body && f[10][0] && !bad_logged++)
            rt_log("mp: %s's body pose (%zu characters) is not one this game reads", f[1], strlen(f[10]));
        if (!isfinite(smp.pos[0]) || !isfinite(smp.pos[1]) || !isfinite(smp.pos[2])) return;
        plat_mutex_lock(&lock);
        int slot = -1, oldest = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            if (!strcmp(friend_pos[i].name, f[1])) slot = i;
            if (friend_pos[i].at_ms < friend_pos[oldest].at_ms) oldest = i;
        }
        if (slot < 0) slot = oldest, friend_pos[slot].n = 0;   // a new friend in this slot
        field(friend_pos[slot].name, sizeof friend_pos[0].name, f[1]);
        int fresh = friend_pos[slot].n == 0;
        if (!fresh) {   // the sender's clock went back (their game restarted), or they travelled: start over
            const PosSample *last = &friend_pos[slot].ring[(friend_pos[slot].head + POS_RING - 1) % POS_RING];
            float d = hypotf(smp.pos[0] - last->pos[0], smp.pos[2] - last->pos[2]) + fabsf(smp.pos[1] - last->pos[1]);
            if (smp.t <= last->t || smp.t - last->t > 60000 || d > 50) fresh = 1;
        }
        if (fresh) friend_pos[slot].n = 0, friend_pos[slot].head = 0, friend_pos[slot].offset = (int64_t)now - smp.t;
        // the delivery time drifts and jitters: the offset follows the quickest delivery, and creeps back up slowly
        // so that one early line does not hold it low for good
        int64_t off = (int64_t)now - smp.t;
        friend_pos[slot].offset = off < friend_pos[slot].offset ? off : friend_pos[slot].offset + 1;
        friend_pos[slot].ring[friend_pos[slot].head] = smp;
        friend_pos[slot].head = (friend_pos[slot].head + 1) % POS_RING;
        if (friend_pos[slot].n < POS_RING) friend_pos[slot].n++;
        memcpy(friend_pos[slot].pos, smp.pos, sizeof smp.pos);
        friend_pos[slot].at_ms = now;
        plat_mutex_unlock(&lock);
        return;
    }
    if (!strcmp(f[0], "LOOK") && n >= 5) {   // LOOK player head upper lower: the armor pieces, "" for none
        plat_mutex_lock(&lock);
        int slot = -1, free_slot = -1;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            if (!strcmp(friend_look[i].name, f[1])) slot = i;
            if (!friend_look[i].name[0] && free_slot < 0) free_slot = i;
        }
        if (slot < 0) slot = free_slot >= 0 ? free_slot : 0;
        field(friend_look[slot].name, sizeof friend_look[0].name, f[1]);
        for (int k = 0; k < 3; k++)
            field(friend_look[slot].look[k], sizeof friend_look[0].look[k], strncmp(f[2 + k], "Armor_", 6) ? "" : f[2 + k]);
        plat_mutex_unlock(&lock);
        return;
    }
    if (!strcmp(f[0], "MARK") && n >= 5) {   // MARK player x y z: one marker per player, the newest kept
        plat_mutex_lock(&lock);
        int slot = -1;
        for (int i = 0; i < view.nmarkers; i++)
            if (!strcmp(view.markers[i].name, f[1])) slot = i;
        if (slot < 0 && view.nmarkers < MP_MAX_MARKERS) slot = view.nmarkers++;
        if (slot >= 0) {
            MpMarker *m = &view.markers[slot];
            field(m->name, sizeof m->name, f[1]);
            for (int k = 0; k < 3; k++) m->pos[k] = (float)atof(f[2 + k]);
            m->at_ms = plat_now_ms();
        }
        // this game's own marker, back from the room: it sits at the radar's centre under the player's dot, so a
        // notice says it went out
        if (!strcmp(f[1], me)) add_line(MP_LINE_EVENT, "", ui_text(UI_MENU_MP_MARK_PLACED), "", "", 1);
        plat_mutex_unlock(&lock);
        return;
    }
    if (!strcmp(f[0], "VOICE") && n >= 3) {   // VOICE from base64: a friend's Opus frame
        uint8_t pkt[VOICE_PACKET_MAX];
        int len = b64_decode(pkt, sizeof pkt, f[2]);
        if (len > 0) voice_receive(f[1], pkt, len);
        return;
    }
    if (!strcmp(f[0], "PONG")) {   // the room's answer to the last PING: the round trip, smoothed over a few;
        // before the lock the lines below take
        int64_t sent = plat_xchg64(&ping_sent, 0);
        if (sent) {
            int rtt = (int)((int64_t)plat_now_ms() - sent);
            plat_mutex_lock(&lock);
            view.ping_ms = view.ping_ms > 0 ? (3 * view.ping_ms + rtt + 2) / 4 : (rtt > 0 ? rtt : 1);
            plat_mutex_unlock(&lock);
        }
        return;
    }
    plat_mutex_lock(&lock);
    if (!strcmp(f[0], "WELCOME") && n >= 3) {
        view.state = MP_CONNECTED;
        voice_connected(1);
        ach_mp_add("rooms", 1);
        field(view.room, sizeof view.room, f[2]);
        view.error[0] = 0;
    } else if (!strcmp(f[0], "ERROR") && n >= 3) {
        view.state = MP_REFUSED;
        field(view.error, sizeof view.error, f[2]);
    } else if (!strcmp(f[0], "ROSTER") && n >= 2) {
        int count = atoi(f[1]), at = 2;
        view.nplayers = 0;
        for (int i = 0; i < count && i < MP_MAX_PLAYERS && at + 2 + MP_STATS <= n; i++) {
            MpPlayer *p = &view.players[view.nplayers++];
            p->slot = -1;   // the SLOTS line after the roster fills it
            field(p->name, sizeof p->name, f[at]);
            field(p->place, sizeof p->place, f[at + 1]);
            for (int k = 0; k < MP_STATS; k++) p->stats[k] = atoi(f[at + 2 + k]);
            at += 2 + MP_STATS;
        }
        ach_mp_max("players", view.nplayers);
    } else if (!strcmp(f[0], "SLOTS") && n >= 2) {   // SLOTS count (name slot)...: each player's colour slot
        for (int i = 0, at = 2; i < atoi(f[1]) && at + 1 < n; i++, at += 2) {
            char who[MP_NAME_MAX];
            field(who, sizeof who, f[at]);
            int slot = atoi(f[at + 1]);
            for (int k = 0; k < view.nplayers; k++)
                if (!strcmp(view.players[k].name, who)) view.players[k].slot = slot >= 0 && slot < MP_SLOTS ? slot : -1;
        }
    } else if (!strcmp(f[0], "CHALLENGES") && n >= 2) {   // CHALLENGES count (stat total done t1 t2 t3 t4)...
        static const char *const stat_names[MP_STATS] = {"shrines", "lightroots", "towers", "koroks", "hearts",
                                                         "achievements"};
        int count = atoi(f[1]), at = 2, tiers = 0;
        view.ntracks = 0;
        for (int i = 0; i < count && view.ntracks < MP_TRACKS && at + 3 + MP_TIERS <= n; i++, at += 3 + MP_TIERS) {
            int stat = -1;
            for (int k = 0; k < MP_STATS; k++)
                if (!strcmp(f[at], stat_names[k])) stat = k;
            if (stat < 0) continue;
            MpTrack *t = &view.tracks[view.ntracks++];
            t->stat = stat, t->total = atoi(f[at + 1]), t->done = atoi(f[at + 2]);
            for (int k = 0; k < MP_TIERS; k++) t->target[k] = atoi(f[at + 3 + k]);
            tiers += t->done;
        }
        ach_mp_max("tiers", tiers);
    } else if (!strcmp(f[0], "CHAT") && n >= 3) {
        add_line(MP_LINE_CHAT, f[1], f[2], "", "", strcmp(f[1], me) != 0);   // the room echoes our own lines
    } else if (!strcmp(f[0], "ACH") && n >= 5) {   // ACH from id name rarity
        add_line(MP_LINE_ACH, f[1], f[3], f[2], f[4], 1);
        if (strcmp(f[1], me)) ach_mp_add("friend_unlocks", 1);
    } else if (!strcmp(f[0], "EVENT") && n >= 3) {   // EVENT kind text [player step total when [old]]
        char text[320];
        const char *who = n >= 4 ? f[3] : "";
        event_text(f[1], who, n >= 5 ? f[4] : "", n >= 6 ? atoi(f[5]) : 0, f[2], text, sizeof text);
        int64_t when = n >= 7 && f[6][0] ? strtoll(f[6], NULL, 10) : now_unix();
        int old = n >= 8 && !strcmp(f[7], "old"), again = 0;
        for (int i = 0; old && i < log_count; i++)   // the room's past events again (after a reconnection): once
            again |= log_lines[i].kind == MP_LINE_EVENT && log_lines[i].when == when && !strcmp(log_lines[i].text, text);
        if (!again) add_line_at(MP_LINE_EVENT, who, text, "", "", !old, when);
    } else if (!strcmp(f[0], "DUEL") && n >= 3) {   // DUEL player 0|1: whether a friend has the duel on
        set_duel(f[1], f[2][0] == '1');
    } else if (!strcmp(f[0], "QUESTS") && n >= 3) {   // QUESTS player q:k,...: where a friend stands in the quests
        set_quests(f[1], f[2]);   // (these lines are handled under the lock)
    } else if (!strcmp(f[0], "CHALLENGE") && n >= 5) {
        view.challenges_done = !strcmp(f[1], "done");
        view.challenge_total = atoi(f[2]), view.challenge_target = atoi(f[3]);
        field(view.challenge, sizeof view.challenge, f[4]);
    }
    plat_mutex_unlock(&lock);
    (void)s;
}

// The settings a connection is made with: a change to one of them reconnects, other settings (the voice's, the rest of
// totk.ini) do not.
static void conn_settings(char *out, size_t cap) {
    snprintf(out, cap, "%d\t%s\t%s\t%s", cfg_bool("TOTK_MP"), cfg_value("TOTK_MP_SERVER"), cfg_value("TOTK_MP_NAME"),
             cfg_value("TOTK_MP_PASSWORD"));
}

static void session(const char *server) {
    const char *why = NULL;
    set_state(MP_CONNECTING, NULL);
    Sock s = dial(server, &why);
    if (s == SOCK_BAD) {
        set_state(MP_RETRYING, why);
        return;
    }
    char name[MP_NAME_MAX];
    field(name, sizeof name, cfg_value("TOTK_MP_NAME"));
    if (!*name) {
        const char *u = getenv("USERNAME");
        field(name, sizeof name, u && *u ? u : "Link");
    }
    const char *hello[] = {"HELLO", PROTOCOL, name, cfg_value("TOTK_MP_PASSWORD")};
    if (!send_line(s, 4, hello)) {
        sock_close(s);
        set_state(MP_RETRYING, "connect");
        return;
    }
    plat_mutex_lock(&lock);
    room_name(me, sizeof me, name);
    snprintf(view.me, sizeof view.me, "%s", me);
    plat_mutex_unlock(&lock);
    rt_log("mp: connected to %s as %s", server, name);
    duel_sent = -1;   // the new connection is told
    unsigned sent_gen = 0, sent_look = 0, ver = cfg_version();
    char settings[512], now_settings[512];
    conn_settings(settings, sizeof settings);
    uint64_t last_ping = plat_now_ms(), minute_at = 0, pos_at = 0, sent_at = 0, gate_at = plat_now_ms();
    float sent_pos[3] = {0, 0, 0}, sent_yaw = 0;
    PosSample prev = {{0, 0, 0}, NAN, {0, 0, 0, 2}, -1, "", NAN, "", 0};   // the place read on the tick before, sent or not
    char sent_anim[MP_ANIM_MAX] = "";
    float sent_q[4] = {0, 0, 0, 1};
    uint64_t voice_slot[16] = {0};
    char held_voice[16] = "";   // a voice named without its variant, held a moment for the variant to show
    uint64_t held_at = 0;
    char said[4][16] = {"", "", "", ""};   // the voices sent lately (one handle can sit in two slots at once)
    uint64_t said_at[4] = {0, 0, 0, 0};
    int voice_frames = 0;
    char buf[16384];
    size_t have = 0;
    int alive = 1;
    while (alive && plat_cas32(&kick, 0, 0) == 0) {
        if (plat_now_ms() - gate_at > 2000) {   // a new game went back to the prologue: leave the room
            gate_at = plat_now_ms();
            if (!island_reached()) break;
        }
        if (cfg_version() != ver) {
            ver = cfg_version();
            conn_settings(now_settings, sizeof now_settings);
            if (strcmp(now_settings, settings)) break;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = {0, 10000};   // 10 ms: voice frames leave every 20 ms
        int r = select((int)s + 1, &rd, NULL, NULL, &tv);
        if (r < 0) break;
        if (r > 0) {
            int k = (int)recv(s, buf + have, (int)(sizeof buf - 1 - have), 0);
            if (k <= 0) break;
            have += (size_t)k;
            char *start = buf, *nl;
            while ((nl = memchr(start, '\n', (size_t)(buf + have - start)))) {
                *nl = 0;
                if (nl > start && nl[-1] == '\r') nl[-1] = 0;
                on_line(start, s);
                start = nl + 1;
            }
            have = (size_t)(buf + have - start);
            memmove(buf, start, have);
            if (have >= sizeof buf - 1) have = 0;   // an overlong line: dropped
        }
        plat_mutex_lock(&lock);
        int refused = view.state == MP_REFUSED, joined = view.state == MP_CONNECTED, others = view.nplayers - 1;
        plat_mutex_unlock(&lock);
        if (refused) break;
        if (!joined) continue;
        if (!minute_at) minute_at = plat_now_ms();
        if (plat_now_ms() - minute_at >= 60000) {   // time in the room, for the multiplayer achievements
            minute_at += 60000;
            if (others > 0) ach_mp_add("minutes", 1);
        }
        AchStats st;   // the progress after each evaluated save
        unsigned gen = ach_stats(&st);
        if (gen && gen != sent_gen) {
            char v[MP_STATS][16];
            for (int k = 0; k < ACH_STATS; k++) snprintf(v[k], sizeof v[k], "%d", st.v[k] < 0 ? 0 : st.v[k]);
            snprintf(v[MP_STAT_ACHIEVEMENTS], sizeof v[0], "%d", st.achievements);
            const char *status[] = {"STATUS", "", v[0], v[1], v[2], v[3], v[4], v[5]};
            alive = send_line(s, 8, status);
            sent_gen = gen;
        }
        // the items Link gets for the first time (game_flags.c: their "IsGet.<actor>" flags), for the room's log; what
        // happened before joining is not news, nor what a reload sets again (a burst, left out by the watcher)
        static uint64_t flag_cursor;
        static int flag_drained;
        GameFlagChange fch[64];
        int nf;
        if (!flag_drained) {
            while (game_flags_since(&flag_cursor, fch, 64) == 64) {}
            flag_drained = 1, nf = 0;
        } else {
            nf = game_flags_since(&flag_cursor, fch, 64);
        }
        for (int i = 0, sent = 0; alive && i < nf && sent < 4; i++) {
            const char *item = fch[i].hash && !fch[i].old && fch[i].now ? game_flag_item(fch[i].hash) : NULL;
            if (!item) continue;
            const char *act[] = {"ACT", "item", item};
            alive = send_line(s, 3, act), sent++;
        }
        // the quests' steps (their Step_<quest> enums), each second: a change is a quest begun, a step further or
        // a quest done; the first reading, and a reload's burst of changes, are where the save stands, not news
        static uint64_t quest_at;
        static uint32_t *quest_was;
        static int quest_known;
        if (alive && plat_now_ms() - quest_at >= 1000) {
            quest_at = plat_now_ms();
            load_quests();
            if (quest_count > 0 && !quest_was) quest_was = calloc((size_t)quest_count, 4);
            uint32_t now_v[512];
            int read = 0, changed = 0;
            for (int i = 0; quest_was && i < quest_count && i < 512; i++) {
                now_v[i] = quest_was[i];
                if (game_enum_get(quests[i].flag, &now_v[i])) read++;
                changed += now_v[i] != quest_was[i];
            }
            if (read && quest_known && changed <= 8)
                for (int i = 0; i < quest_count && i < 512 && alive; i++) {
                    if (now_v[i] == quest_was[i]) continue;
                    int from = 0, to = -1;   // the step it left and the one it reached
                    for (int k = 0; k < quests[i].nsteps; k++) {
                        if (quests[i].step_hash[k] == quest_was[i]) from = k;
                        if (quests[i].step_hash[k] == now_v[i]) to = k;
                    }
                    if (to < 2) continue;   // back to NotReady, or offered (Ready): not news
                    const char *act[] = {"ACT", "quest", quests[i].id, quests[i].step[to], quests[i].step[from]};
                    alive = send_line(s, 5, act);
                }
            if (read && !quest_known) {   // once: where the save stands
                char begun[600] = "";
                size_t m = 0;
                for (int i = 0; i < quest_count && i < 512 && m + 80 < sizeof begun; i++)
                    for (int k = 2; k < quests[i].nsteps; k++)   // past Ready (offered, not taken)
                        if (quests[i].step_hash[k] == now_v[i])
                            m += (size_t)snprintf(begun + m, sizeof begun - m, " %s:%s", quests[i].id, quests[i].step[k]);
                rt_log("mp: %d of %d quests read; under way or done:%s", read, quest_count, begun);
            }
            if (read) memcpy(quest_was, now_v, 4ull * (quest_count < 512 ? quest_count : 512)), quest_known = 1;
            // where this game stands, for the room's quest page: q:k of the quests past Ready
            static char sent_list[3000];
            static uint64_t list_at;
            char list[3000];
            size_t m = 0;
            list[0] = 0;
            for (int i = 0; read && i < quest_count && i < 512 && m + 12 < sizeof list; i++)
                for (int k = 2; k < quests[i].nsteps; k++)
                    if (quests[i].step_hash[k] == now_v[i]) m += (size_t)snprintf(list + m, sizeof list - m, "%s%d:%d", m ? "," : "", i, k);
            if (read && strcmp(list, sent_list) && plat_now_ms() - list_at >= 2000) {
                const char *ql[] = {"QUESTS", list};
                alive = send_line(s, 2, ql);
                snprintf(sent_list, sizeof sent_list, "%s", list), list_at = plat_now_ms();
                char me_name[MP_NAME_MAX];
                plat_mutex_lock(&lock);
                snprintf(me_name, sizeof me_name, "%s", view.me);
                set_quests(me_name, list);
                plat_mutex_unlock(&lock);
            }
        }
        // the armor Link wears: read live from the game each second (game_player_armor), sent when it changes; where
        // that cannot be read (another game version), what the last save read says
        static uint64_t look_at;
        static char live_sent[3][48];
        static int live_ok;
        char live[3][48] = {{0}};   // zeroed: compared whole with what went last
        if (alive && plat_now_ms() - look_at >= 1000) look_at = plat_now_ms(), live_ok = game_player_armor(live);
        else live_ok = 0;
        if (live_ok) {
            if (memcmp(live, live_sent, sizeof live) || !sent_look) {
                const char *line[] = {"LOOK", live[0], live[1], live[2]};
                alive = send_line(s, 4, line);
                rt_log("mp: wearing head '%s' upper '%s' lower '%s'", live[0], live[1], live[2]);
                memcpy(live_sent, live, sizeof live), sent_look = ~0u;
            }
        } else if (sent_look != ~0u) {
            char worn[3][64];   // the armor worn at the last save read
            unsigned look_gen = ach_look(worn);
            if (alive && look_gen && look_gen != sent_look) {
                const char *line[] = {"LOOK", worn[0], worn[1], worn[2]};
                alive = send_line(s, 4, line);
                sent_look = look_gen;
            }
        }
        for (int idx; alive && (idx = ach_take_net_unlock()) >= 0;) {
            AchView a;
            if (!ach_get(idx, &a)) continue;
            const char *ach[] = {"ACH", a.id, a.name, rarity_name(a.rarity)};
            alive = send_line(s, 4, ach);
            if (others > 0 && !a.multiplayer) ach_mp_add("group_unlocks", 1);   // earned with friends in the room
        }
        for (;;) {
            char text[320];
            plat_mutex_lock(&lock);
            int any = out_head != out_tail;
            if (any) {
                snprintf(text, sizeof text, "%s", outbox[out_head]);
                out_head = (out_head + 1) % OUTBOX;
            }
            plat_mutex_unlock(&lock);
            if (!any || !alive) break;
            const char *chat[] = {"CHAT", text};
            alive = send_line(s, 2, chat);
            ach_mp_add("chat", 1);
        }
        uint8_t pkt[VOICE_PACKET_MAX];
        for (int len; alive && (len = voice_take_packet(pkt, sizeof pkt)) > 0;) {
            char vline[8 + VOICE_PACKET_MAX * 4 / 3 + 8];
            memcpy(vline, "VOICE\t", 6);
            size_t k = 6 + b64_encode(vline + 6, pkt, (size_t)len);
            vline[k++] = '\n';
            alive = send_all(s, vline, k);
            if (++voice_frames == 50) {   // a second of voice sent
                voice_frames = 0;
                ach_mp_add("voice", 1);
            }
        }
        if (alive && cfg_bool("TOTK_MP_DUEL") != duel_sent) {
            duel_sent = cfg_bool("TOTK_MP_DUEL");
            const char *dl[] = {"DUEL", duel_sent ? "1" : "0"};
            alive = send_line(s, 2, dl);
        }
        if (alive && plat_now_ms() - pos_at >= POS_EVERY_MS) alive = duel_tick(s, plat_now_ms());
        if (alive && plat_now_ms() - pos_at >= POS_EVERY_MS) {   // this game's place while it moves (or each second)
            float p[3], yaw = NAN;
            uint64_t now = plat_now_ms();
            pos_at = now;
            int have = game_player_pose(p, &yaw);
            char anim[MP_ANIM_MAX];
            float frame = NAN;
            if (!have || !game_player_anim(anim, sizeof anim, &frame)) anim[0] = 0;
            float q[4] = {0, 0, 0, 2};
            if (!have || !game_player_quat(q)) q[3] = 2;
            // the voices Link started: a variant goes at once; a name without one (the game picks the variant a
            // moment later) waits up to 300 ms for it, then goes as it is (the friend's game picks one then)
            char sfx[MP_SFX_MAX] = "", got[8][16];
            int ng = have ? game_player_voices(got, 8, voice_slot) : 0;
            for (int g = 0; g < ng; g++) {
                // the same voice, or its name without the variant after the variant went, within 600 ms: once
                int again = 0;
                for (int q = 0; q < 4; q++)
                    again |= now - said_at[q] < 600 && !strncmp(said[q], got[g], strlen(got[g]) == 5 ? 5 : 16);
                if (again) continue;
                if (strlen(got[g]) == 5) {
                    if (!held_voice[0]) snprintf(held_voice, sizeof held_voice, "%s", got[g]), held_at = now;
                    continue;
                }
                if (held_voice[0] && !strncmp(got[g], held_voice, 5)) held_voice[0] = 0;
                if (strlen(sfx) + 17 < sizeof sfx) snprintf(sfx + strlen(sfx), sizeof sfx - strlen(sfx), "%s%s", sfx[0] ? "," : "", got[g]);
                int old = 0;
                for (int q = 1; q < 4; q++)
                    if (said_at[q] < said_at[old]) old = q;
                snprintf(said[old], sizeof said[old], "%s", got[g]), said_at[old] = now;
            }
            if (held_voice[0] && now - held_at >= 300) {
                if (strlen(sfx) + 17 < sizeof sfx) snprintf(sfx + strlen(sfx), sizeof sfx - strlen(sfx), "%s%s", sfx[0] ? "," : "", held_voice);
                held_voice[0] = 0;
            }
            plat_mutex_lock(&lock);
            view.me_has_pos = have;
            if (have) memcpy(view.me_pos, p, sizeof p);
            plat_mutex_unlock(&lock);
            float turned = isfinite(yaw) ? fabsf(remainderf(yaw - sent_yaw, 6.2831853f)) : 0;
            int moved = have && (fabsf(p[0] - sent_pos[0]) + fabsf(p[1] - sent_pos[1]) + fabsf(p[2] - sent_pos[2]) > 0.02f ||
                                 turned > 0.03f || strcmp(anim, sent_anim) || sfx[0] ||
                                 (q[3] <= 1 && fabsf(q[0] * sent_q[0] + q[1] * sent_q[1] + q[2] * sent_q[2] + q[3] * sent_q[3]) < 0.9998f));
            // with the animation's frame known, every tick goes: the friend's figure then plays the frames this game
            // shows (a flip, a fall, an attack in place), not a guess at their pace
            if (have && (moved || isfinite(frame) || now - sent_at >= POS_IDLE_MS || !sent_at)) {
                // after standing still, the place of the tick before goes first with its own time: the friend's game
                // then sees the stop last until the step began, not a slow slide over the whole pause
                if (sent_at && now - sent_at > 2 * POS_EVERY_MS && prev.t > (int64_t)sent_at)
                    alive = send_pos(s, prev.pos, prev.yaw, prev.q, prev.t, prev.anim, prev.frame, "", "", "");
                static float pal[89 * 12];   // the body's palette up to the last entry sent (LF_BODY_ENTRY)
                char body[(LF_BODY_BYTES + 2) / 3 * 4 + 1];
                link_body_encode(pal, game_player_body(pal, 89), body, sizeof body);
                char gear[1000];
                gear_text(gear, sizeof gear, pal, body[0] ? 89 : 0);
                if (alive) alive = send_pos(s, p, yaw, q, (int64_t)now, anim, frame, sfx, body, body[0] ? gear : "");
                snprintf(sent_anim, sizeof sent_anim, "%s", anim);
                if (q[3] <= 1) memcpy(sent_q, q, sizeof q);
                memcpy(sent_pos, p, sizeof p);
                if (isfinite(yaw)) sent_yaw = yaw;
                sent_at = now;
                // close to a friend in the same layer: the meeting achievement
                plat_mutex_lock(&lock);
                int met = 0;
                for (int i = 0; i < MP_MAX_PLAYERS; i++)
                    if (friend_pos[i].name[0] && now - friend_pos[i].at_ms < POS_STALE_MS &&
                        mp_layer(friend_pos[i].pos[1]) == mp_layer(p[1]) &&
                        hypotf(friend_pos[i].pos[0] - p[0], friend_pos[i].pos[2] - p[2]) < 30)
                        met = 1;
                plat_mutex_unlock(&lock);
                if (met) ach_mp_max("meet", 1);
            }
            if (have) {
                memcpy(prev.pos, p, sizeof p), prev.yaw = yaw, prev.t = (int64_t)now;
                memcpy(prev.q, q, sizeof q);
                snprintf(prev.anim, sizeof prev.anim, "%s", anim);
                prev.frame = frame;
            }
        }
        if (alive && plat_xchg32(&mark_pending, 0)) {
            float p[3];
            if (game_player_pos(p)) {
                char x[24], y[24], z[24];
                snprintf(x, sizeof x, "%.1f", p[0]), snprintf(y, sizeof y, "%.1f", p[1]), snprintf(z, sizeof z, "%.1f", p[2]);
                const char *mark[] = {"MARK", x, y, z};
                alive = send_line(s, 4, mark);
                // the Guia de Hyrule achievement counts markers at least 50 m from the last one it counted, so
                // pressing the key over and over in one spot does not earn it
                static float counted[3];
                static int have_counted;
                if (!have_counted || hypotf(p[0] - counted[0], p[2] - counted[2]) >= 50 || fabsf(p[1] - counted[1]) >= 50) {
                    ach_mp_add("marks", 1);
                    memcpy(counted, p, sizeof counted);
                    have_counted = 1;
                }
            }
        }
        if (alive && plat_now_ms() - last_ping > PING_MS) {   // keeps the connection and measures it (the ping shown)
            const char *ping[] = {"PING"};
            plat_xchg64(&ping_sent, (int64_t)plat_now_ms());
            alive = send_line(s, 1, ping);
            last_ping = plat_now_ms();
        }
    }
    sock_close(s);
    voice_connected(0);
    plat_mutex_lock(&lock);
    if (view.state != MP_REFUSED) view.state = MP_RETRYING, snprintf(view.error, sizeof view.error, "lost");
    view.ping_ms = 0;
    view.nplayers = 0;
    plat_mutex_unlock(&lock);
    rt_log("mp: disconnected from %s", server);
}

static void thread_main(void *arg) {
    (void)arg;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        set_state(MP_REFUSED, "winsock");
        return;
    }
#endif
    unsigned refused_ver = 0;
    for (;;) {
        char server[128];
        snprintf(server, sizeof server, "%s", cfg_value("TOTK_MP_SERVER"));
        plat_mutex_lock(&lock);
        snprintf(view.server, sizeof view.server, "%s", server);
        int refused = view.state == MP_REFUSED;
        plat_mutex_unlock(&lock);
        plat_xchg32(&kick, 0);
        if (!cfg_bool("TOTK_MP") || !*server) {
            set_state(MP_OFF, NULL);
            plat_sleep_ms(500);
            continue;
        }
        if (refused && cfg_version() == refused_ver) {   // the room said no: wait for other settings or a retry
            plat_sleep_ms(500);
            continue;
        }
        if (!island_reached()) {
            set_state(MP_LOCKED, NULL);
            plat_sleep_ms(1000);
            continue;
        }
        session(server);
        plat_mutex_lock(&lock);
        refused = view.state == MP_REFUSED;
        plat_mutex_unlock(&lock);
        if (refused) {
            refused_ver = cfg_version();
            continue;
        }
        for (int t = 0; t < RETRY_MS / 100 && !plat_cas32(&kick, 0, 0); t++) plat_sleep_ms(100);
    }
}

// ---- the menu's side

void mp_tick(void) {
    // plat_cas32(p, new, expected) returns the old value: 0 only for the first call
    if (plat_cas32(&started, 1, 0) == 0) {
        if (!plat_thread_spawn(thread_main, NULL, "multiplayer", 0)) rt_log("mp: no thread for the multiplayer client");
        gmd_start();   // gmd.c: the shared-progress research probe, when TOTK_GMD_PROBE asks for it
        game_flags_start();   // game_flags.c: the flags' watcher (items got for the first time; TOTK_FLAG_LOG)
        anim_probe_start();   // anim_probe.c: where Link's animation is named, when TOTK_ANIM_PROBE asks for it
    }
}

// Friend k's place at t on the sender's clock: between the two samples around t; past the newest, on along the last
// step for at most POS_AHEAD_MS, then held (*held set). yaw is the way they face (NAN when their game does not send
// it), anim the animation of the last sample before t. Under the lock.
// A sample's rotation: as sent, or about the vertical alone from its yaw (an older game).
static void sample_quat(const PosSample *s, float q[4]) {
    if (s->q[3] <= 1) {
        memcpy(q, s->q, 4 * sizeof *q);
        return;
    }
    float y = isfinite(s->yaw) ? s->yaw : 0;
    q[0] = 0, q[1] = sinf(y / 2), q[2] = 0, q[3] = cosf(y / 2);
}

// frames: the frame of the animation shown at t from the two samples around it, as (from, to, u): from + (to - from)
// u, with to below from when a loop started over between them (the reader, knowing its length, adds it); NAN from
// when not sent.
static void place_on(int k, int64_t t, float pos[3], float *yaw, int *held, const char **anim, float rot[4], float frames[3],
                     LinkBody *body, int *has_body) {
    int n = friend_pos[k].n, head = friend_pos[k].head;
    const PosSample *ring = friend_pos[k].ring;
#define SAMPLE(i) (&ring[(head + POS_RING - n + (i)) % POS_RING])   // 0 the oldest kept, n - 1 the newest
    const PosSample *a = SAMPLE(0), *b = SAMPLE(n > 1 ? 1 : 0);
    for (int i = n - 1; i > 0; i--)
        if (SAMPLE(i - 1)->t <= t || i == 1) {
            a = SAMPLE(i - 1), b = SAMPLE(i);
            break;
        }
#undef SAMPLE
    float span = (float)(b->t - a->t), u = 0;
    if (span > 0) {
        u = (float)(t - a->t) / span;
        if (u < 0) u = 0;
        float most = 1.0f + POS_AHEAD_MS / span;   // past the newest: on along the step, not for ever
        if (u > most) u = most;
    }
    if (held) *held = t - b->t > POS_AHEAD_MS;
    if (anim) *anim = u >= 1 ? b->anim : a->anim;   // the animation of the last sample before t
    if (frames) {
        const PosSample *at = u >= 1 ? b : a;
        frames[0] = at->frame, frames[1] = NAN, frames[2] = 0;
        if (span > 0 && !strcmp(a->anim, b->anim) && isfinite(a->frame) && isfinite(b->frame))
            frames[0] = a->frame, frames[1] = b->frame, frames[2] = u;   // past b too: on at the same pace
        else if (isfinite(at->frame))   // one sample to go by: on at 30 a second from it
            frames[1] = at->frame + 30.0f, frames[2] = (float)(t - at->t) / 1000.0f;
    }
    if (rot) {   // between the two rotations the shorter way (a back flip turns a third of a turn a sample)
        float qa[4], qb[4], w = fminf(u, 1.0f), d = 0, l = 0;
        sample_quat(a, qa), sample_quat(b, qb);
        for (int c = 0; c < 4; c++) d += qa[c] * qb[c];
        float sb = d < 0 ? -1.0f : 1.0f;
        for (int c = 0; c < 4; c++) rot[c] = qa[c] + (sb * qb[c] - qa[c]) * w, l += rot[c] * rot[c];
        l = sqrtf(l);
        for (int c = 0; c < 4; c++) rot[c] = l > 1e-6f ? rot[c] / l : (c == 3);
    }
    if (body) {   // the real pose between the two, or the one there is
        const PosSample *at = u >= 1 ? b : a;
        *has_body = 1;
        if (a->has_body && b->has_body) link_body_lerp(&a->body, &b->body, fminf(u, 1.0f), body);
        else if (at->has_body) *body = at->body;
        else *has_body = 0;
    }
    for (int c = 0; c < 3; c++) pos[c] = a->pos[c] + (b->pos[c] - a->pos[c]) * u;
    if (!yaw) return;
    if (isfinite(a->yaw) && isfinite(b->yaw))
        *yaw = a->yaw + remainderf(b->yaw - a->yaw, 6.2831853f) * fminf(u, 1.0f);
    else
        *yaw = isfinite(b->yaw) ? b->yaw : NAN;
}

// Friend k's place at this game's time now: POS_DELAY_MS behind the newest sample on the sender's clock. vel (m/s) is
// the way it went over the last POS_VEL_MS, not over one step: a step is 50 ms and its length jitters with the
// sender's timing, which would make the figure's gait flicker. 0 once held.
static void place_at(int k, uint64_t now, float pos[3], float vel[3], float *yaw, char anim[MP_ANIM_MAX], float rot[4],
                     float frames[3], LinkBody *body, int *has_body) {
    int64_t t = (int64_t)now - friend_pos[k].offset - POS_DELAY_MS;
    int held = 0;
    float before[3];
    const char *playing = "";
    place_on(k, t, pos, yaw, &held, &playing, rot, frames, body, has_body);
    snprintf(anim, MP_ANIM_MAX, "%s", playing);
    place_on(k, t - POS_VEL_MS, before, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    for (int c = 0; c < 3; c++) vel[c] = held ? 0 : (pos[c] - before[c]) * 1000.0f / POS_VEL_MS;
}

int mp_take_sfx(const char *name, char *out, size_t cap) {
    uint64_t now = plat_now_ms();
    size_t len = 0;
    out[0] = 0;
    plat_mutex_lock(&lock);
    for (int k = 0; k < MP_MAX_PLAYERS; k++) {
        if (!friend_pos[k].name[0] || strcmp(friend_pos[k].name, name)) continue;
        int64_t t = (int64_t)now - friend_pos[k].offset - POS_DELAY_MS;   // as place_at shows them
        for (int i = 0; i < friend_pos[k].n; i++) {
            PosSample *smp = &friend_pos[k].ring[(friend_pos[k].head + POS_RING - friend_pos[k].n + i) % POS_RING];
            if (smp->sfx_taken || !smp->sfx[0] || smp->t > t) continue;
            smp->sfx_taken = 1;
            if (t - smp->t > 1000) continue;   // long past (the friend came into view, a stall): not news
            size_t add = strlen(smp->sfx);
            if (len + add + 2 >= cap) break;
            if (len) out[len++] = ',';
            memcpy(out + len, smp->sfx, add + 1), len += add;
        }
    }
    plat_mutex_unlock(&lock);
    return (int)len;
}

void mp_view(MpView *out) {
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&lock);
    *out = view;
    for (int i = 0; i < out->nplayers; i++) {   // the friends' positions, while fresh
        MpPlayer *p = &out->players[i];
        p->has_pos = 0, p->yaw = NAN, p->anim[0] = 0, p->rot[0] = p->rot[1] = p->rot[2] = 0, p->rot[3] = 1, p->has_body = 0;
        memset(p->gear, 0, sizeof p->gear);
        p->frames[0] = p->frames[1] = NAN, p->frames[2] = 0;
        memset(p->vel, 0, sizeof p->vel);
        for (int k = 0; k < MP_MAX_PLAYERS; k++)
            if (friend_pos[k].name[0] && !strcmp(friend_pos[k].name, p->name) && now - friend_pos[k].at_ms < POS_STALE_MS &&
                friend_pos[k].n > 0) {
                p->has_pos = 1, place_at(k, now, p->pos, p->vel, &p->yaw, p->anim, p->rot, p->frames, &p->body, &p->has_body);
                const PosSample *last = &friend_pos[k].ring[(friend_pos[k].head + POS_RING - 1) % POS_RING];
                memcpy(p->gear, last->gear, sizeof p->gear);
            }
        if (!strcmp(p->name, me) && view.me_has_pos) memcpy(p->pos, view.me_pos, sizeof p->pos), p->has_pos = 1;
        memset(p->look, 0, sizeof p->look);
        for (int k = 0; k < MP_MAX_PLAYERS; k++)
            if (friend_look[k].name[0] && !strcmp(friend_look[k].name, p->name)) memcpy(p->look, friend_look[k].look, sizeof p->look);
    }
    int w = 0;   // markers past their time, or of players gone, drop out
    for (int i = 0; i < out->nmarkers; i++) {
        int here = 0;
        for (int k = 0; k < out->nplayers; k++) here |= !strcmp(out->players[k].name, out->markers[i].name);
        if (here && now - out->markers[i].at_ms < MP_MARK_MS) out->markers[w++] = out->markers[i];
    }
    out->nmarkers = w;
    plat_mutex_unlock(&lock);
}

int mp_slot_of(const char *name) {
    int slot = -1;
    plat_mutex_lock(&lock);
    for (int i = 0; i < view.nplayers; i++)
        if (!strcmp(view.players[i].name, name)) slot = view.players[i].slot;
    plat_mutex_unlock(&lock);
    return slot;
}

int mp_mark(void) {
    plat_mutex_lock(&lock);
    int ok = view.state == MP_CONNECTED && view.me_has_pos;
    plat_mutex_unlock(&lock);
    if (ok) plat_xchg32(&mark_pending, 1);
    return ok;
}

int mp_lines(MpLine *out, int max) {
    plat_mutex_lock(&lock);
    int n = log_count < max ? log_count : max;
    for (int i = 0; i < n; i++) out[i] = log_lines[(log_next - n + i + LOG_LINES) % LOG_LINES];
    plat_mutex_unlock(&lock);
    return n;
}

int mp_take_toast(MpLine *out) {
    plat_mutex_lock(&lock);
    int any = toast_head != toast_tail;
    if (any) {
        *out = toasts[toast_head];
        toast_head = (toast_head + 1) % TOASTS;
    }
    plat_mutex_unlock(&lock);
    return any;
}

int mp_send_chat(const char *text) {
    plat_mutex_lock(&lock);
    int ok = view.state == MP_CONNECTED && text && *text;
    int next = (out_tail + 1) % OUTBOX;
    if (ok && next != out_head) {
        field(outbox[out_tail], sizeof outbox[0], text);
        out_tail = next;
    } else {
        ok = 0;
    }
    plat_mutex_unlock(&lock);
    return ok;
}

void mp_reconnect(void) {
    plat_mutex_lock(&lock);
    if (view.state == MP_REFUSED) view.state = MP_RETRYING;
    plat_mutex_unlock(&lock);
    plat_xchg32(&kick, 1);
}
