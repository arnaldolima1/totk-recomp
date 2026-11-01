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
enum { DEFAULT_PORT = 5577, LOG_LINES = 80, TOASTS = 16, OUTBOX = 16, RETRY_MS = 5000, PING_MS = 30000 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static MpView view;
static MpLine log_lines[LOG_LINES];
static int log_count, log_next;
static MpLine toasts[TOASTS];
static int toast_head, toast_tail;
static char outbox[OUTBOX][320];
static int out_head, out_tail;
static volatile int32_t started, kick;
static char me[MP_NAME_MAX];   // this game's name as the room keeps it (room_name): its own chat lines get no notice

// Friends' positions by name (POS lines), merged into the roster's players by mp_view.
enum { POS_STALE_MS = 6000, POS_EVERY_MS = 200 };
static struct { char name[MP_NAME_MAX]; float pos[3]; uint64_t at_ms; } friend_pos[MP_MAX_PLAYERS];
// Friends' armor by name (LOOK lines: what each wore at their last save), merged into the roster by mp_view.
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
static void add_line(int kind, const char *from, const char *text, const char *ach_id, const char *rarity, int toast) {
    MpLine *l = &log_lines[log_next];
    memset(l, 0, sizeof *l);
    l->kind = kind, l->when = now_unix();
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
    char buf[1024];
    size_t n = 0;
    for (int i = 0; i < nfields && n + 2 < sizeof buf; i++) {
        if (i) buf[n++] = '\t';
        char f[512];
        field(f, sizeof f, fields[i]);
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", f);
    }
    if (n + 1 >= sizeof buf) n = sizeof buf - 2;
    buf[n++] = '\n';
    return send_all(s, buf, n);
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
    if (!strcmp(f[0], "POS") && n >= 5) {   // POS player x y z
        plat_mutex_lock(&lock);
        int slot = -1, oldest = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            if (!strcmp(friend_pos[i].name, f[1])) slot = i;
            if (friend_pos[i].at_ms < friend_pos[oldest].at_ms) oldest = i;
        }
        if (slot < 0) slot = oldest;
        field(friend_pos[slot].name, sizeof friend_pos[0].name, f[1]);
        for (int k = 0; k < 3; k++) friend_pos[slot].pos[k] = (float)atof(f[2 + k]);
        friend_pos[slot].at_ms = plat_now_ms();
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
    } else if (!strcmp(f[0], "EVENT") && n >= 3) {
        add_line(MP_LINE_EVENT, "", f[2], "", "", 1);
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
    unsigned sent_gen = 0, sent_look = 0, ver = cfg_version();
    char settings[512], now_settings[512];
    conn_settings(settings, sizeof settings);
    uint64_t last_ping = plat_now_ms(), minute_at = 0, pos_at = 0, gate_at = plat_now_ms();
    float sent_pos[3] = {0, 0, 0};
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
        char worn[3][64];   // the armor worn at the last save read, for the friends' figures of it
        unsigned look_gen = ach_look(worn);
        if (alive && look_gen && look_gen != sent_look) {
            const char *line[] = {"LOOK", worn[0], worn[1], worn[2]};
            alive = send_line(s, 4, line);
            sent_look = look_gen;
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
        if (alive && plat_now_ms() - pos_at >= POS_EVERY_MS) {   // this game's position, when it moved (or each 2 s)
            float p[3];
            uint64_t now = plat_now_ms();
            int have = game_player_pos(p);
            plat_mutex_lock(&lock);
            view.me_has_pos = have;
            if (have) memcpy(view.me_pos, p, sizeof p);
            plat_mutex_unlock(&lock);
            if (have && (fabsf(p[0] - sent_pos[0]) + fabsf(p[1] - sent_pos[1]) + fabsf(p[2] - sent_pos[2]) > 0.3f ||
                         now - pos_at >= 2000 || !pos_at)) {
                char x[24], y[24], z[24];
                snprintf(x, sizeof x, "%.1f", p[0]), snprintf(y, sizeof y, "%.1f", p[1]), snprintf(z, sizeof z, "%.1f", p[2]);
                const char *pos[] = {"POS", x, y, z};
                alive = send_line(s, 4, pos);
                memcpy(sent_pos, p, sizeof p);
                pos_at = now;
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
            if (!have) pos_at = now;
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
        if (alive && plat_now_ms() - last_ping > PING_MS) {
            const char *ping[] = {"PING"};
            alive = send_line(s, 1, ping);
            last_ping = plat_now_ms();
        }
    }
    sock_close(s);
    voice_connected(0);
    plat_mutex_lock(&lock);
    if (view.state != MP_REFUSED) view.state = MP_RETRYING, snprintf(view.error, sizeof view.error, "lost");
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
    }
}

void mp_view(MpView *out) {
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&lock);
    *out = view;
    for (int i = 0; i < out->nplayers; i++) {   // the friends' positions, while fresh
        MpPlayer *p = &out->players[i];
        p->has_pos = 0;
        for (int k = 0; k < MP_MAX_PLAYERS; k++)
            if (friend_pos[k].name[0] && !strcmp(friend_pos[k].name, p->name) && now - friend_pos[k].at_ms < POS_STALE_MS) {
                memcpy(p->pos, friend_pos[k].pos, sizeof p->pos);
                p->has_pos = 1;
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
