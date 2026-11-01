// Achievements (achievements.h): load the compiled definitions, evaluate them on the newest progress.sav, record and
// announce the unlocks.
#include "achievements.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "host.h"
#include "platform/platform.h"

// The condition kinds of compiled.tsv (tools/achievements.py resolve).
// K_MP: a multiplayer counter (ach_mp_add / ach_mp_max), not a save flag; outside the platinum, inside 100 %.
enum { K_BOOL, K_ENUM, K_NUM, K_ARRSUM, K_COUNT, K_POUCH, K_ALL, K_MP };

typedef struct {
    char *id, *name, *desc, *item;
    int rarity, kind, hidden, list;
    uint32_t a, b, want;   // flag hashes (a; b the stock array of a pouch, want the enum value of enum and count)
    uint32_t after;        // num: a bool that must be true for the number to count, 0 none (tools/achievements.py
                           // COUNT_AFTER: the prologue's 30 hearts and three wheels are saved before Link wakes)
    double min, unit;
    uint8_t *icon;
    int unlocked;
    int blocked;   // already true in a save the game did not write here: unlocks only after it is false again
    int64_t when;
    double cur;
} Ach;

typedef struct {
    uint32_t *h;
    int n;
} HashList;

static Ach *achs;
static int nach;
// The progress figures of the S lines (shrines, lightroots...), in ACH_STAT order, and how often they were read.
static const char *const stat_names[ACH_STATS] = {"shrines", "lightroots", "towers", "koroks", "hearts"};
static Ach stats[ACH_STATS];
static int stat_found[ACH_STATS];
static unsigned stats_gen;
static int awake = -1;   // AchStats.awake, from the newest save
// SageOfSky_IsOpenShutterInWakeUpRoom (murmur3, as tools/achievements.py hashes flag names): the shutter of the room
// where Link wakes on the Great Sky Island opens; before it the save holds only the prologue.
#define FLAG_AWAKE 0x2c2ac169u

static HashList *lists;
static int nlists;
static PlatMutex lock = PLAT_MUTEX_INIT;
static PlatOnce load_once = PLAT_ONCE_INIT;
static char unlocked_path[1100], state_path[1100], mp_path[1100];

// The multiplayer counters K_MP conditions read (<profile>/achievements-mp.tsv): what net_mp.c counts in rooms.
enum { MP_COUNTERS = 32 };
static struct { char name[32]; double v; } mp_counter[MP_COUNTERS];
static int n_mp_counters;

// The saves this game wrote: a fingerprint (FNV-1a, 64-bit) of each slot's progress.sav after every commit, kept in
// <profile>/achievements-state.tsv with the blocked achievements. A slot that differs at start was replaced while the
// game was closed (a save from elsewhere, or a backup put back): what it already holds does not unlock.
enum { SLOTS = 10 };
static uint64_t slot_print[SLOTS];
static int slot_known[SLOTS];

// Announcements: unlocks in batches, one batch per evaluation.
enum { NOTICE_CAP = 256, NOTICE_NET_CAP = 64 };
static int notice[NOTICE_CAP], notice_batch[NOTICE_CAP], notice_head, notice_tail;
// Unlocks for the multiplayer room (net_mp.c), apart from the notices the menu takes.
static int net_unlock[NOTICE_NET_CAP], net_head, net_tail;

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// The next line of text at *pos (its end cut off with a NUL), NULL at the end; skips empty lines.
static char *next_line(char **pos) {
    char *p = *pos;
    while (*p == '\r' || *p == '\n') p++;
    if (!*p) return NULL;
    char *line = p;
    while (*p && *p != '\r' && *p != '\n') p++;
    if (*p) *p++ = 0;
    *pos = p;
    return line;
}

static int split(char *line, char **f, int max) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++)
        if (*p == '\t') {
            *p = 0;
            f[n++] = p + 1;
        }
    return n;
}

static int rarity_of(const char *s) {
    static const char *const names[ACH_RARITIES] = {"common", "rare", "epic", "legendary", "platinum"};
    for (int i = 0; i < ACH_RARITIES; i++)
        if (!strcmp(s, names[i])) return i;
    return ACH_COMMON;
}

static int kind_of(const char *s) {
    static const char *const names[] = {"bool", "enum", "num", "arrsum", "count", "pouch", "all", "mp"};
    for (int i = 0; i < (int)(sizeof names / sizeof *names); i++)
        if (!strcmp(s, names[i])) return i;
    return -1;
}

// The folder of totk.exe, with a trailing separator.
static void exe_dir(char *out, size_t cap) {
    if (!plat_exe_path(out, cap)) {
        snprintf(out, cap, "./");
        return;
    }
    char *slash = NULL;
    for (char *p = out; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    if (slash) slash[1] = 0;
}

static void load_unlocked(void) {
    uint64_t size = 0;
    char *text = (char *)plat_file_read(unlocked_path, &size);
    if (!text) return;
    char *pos = text;
    for (char *line; (line = next_line(&pos));) {
        char *f[3];
        if (*line == '#' || split(line, f, 3) < 2) continue;
        for (int i = 0; i < nach; i++)
            if (!strcmp(achs[i].id, f[0])) {
                achs[i].unlocked = 1;
                achs[i].when = strtoll(f[1], NULL, 10);
            }
    }
    free(text);
}

static void save_unlocked(void) {
    size_t cap = 64 + (size_t)nach * 64, n = 0;
    char *out = malloc(cap);
    if (!out) return;
    n += snprintf(out + n, cap - n, "# achievement id, unix time of the unlock (runtime/achievements.c)\n");
    for (int i = 0; i < nach; i++)
        if (achs[i].unlocked && n < cap)
            n += snprintf(out + n, cap - n, "%s\t%lld\n", achs[i].id, (long long)achs[i].when);
    if (!plat_file_write_atomic(unlocked_path, out, n)) rt_log("achievements: cannot write %s", unlocked_path);
    free(out);
}

static double *mp_slot(const char *name, int create) {
    for (int i = 0; i < n_mp_counters; i++)
        if (!strcmp(mp_counter[i].name, name)) return &mp_counter[i].v;
    if (!create || n_mp_counters == MP_COUNTERS) return NULL;
    snprintf(mp_counter[n_mp_counters].name, sizeof mp_counter[0].name, "%s", name);
    mp_counter[n_mp_counters].v = 0;
    return &mp_counter[n_mp_counters++].v;
}

static void load_mp(void) {
    uint64_t size = 0;
    char *text = (char *)plat_file_read(mp_path, &size);
    if (!text) return;
    char *pos = text;
    for (char *line; (line = next_line(&pos));) {
        char *f[3];
        if (*line == '#' || split(line, f, 3) < 2) continue;
        double *v = mp_slot(f[0], 1);
        if (v) *v = atof(f[1]);
    }
    free(text);
}

static void save_mp(void) {
    char out[64 + MP_COUNTERS * 64];
    size_t n = (size_t)snprintf(out, sizeof out, "# multiplayer counter, value (runtime/achievements.c ach_mp_add)\n");
    for (int i = 0; i < n_mp_counters && n < sizeof out; i++)
        n += (size_t)snprintf(out + n, sizeof out - n, "%s\t%.1f\n", mp_counter[i].name, mp_counter[i].v);
    if (!plat_file_write_atomic(mp_path, out, n)) rt_log("achievements: cannot write %s", mp_path);
}

static void load_state(void) {
    uint64_t size = 0;
    char *text = (char *)plat_file_read(state_path, &size);
    if (!text) return;
    char *pos = text;
    for (char *line; (line = next_line(&pos));) {
        char *f[3];
        int n = split(line, f, 3);
        if (!strcmp(f[0], "F") && n >= 3) {
            int slot = atoi(f[1]);
            if (slot >= 0 && slot < SLOTS) slot_print[slot] = strtoull(f[2], NULL, 16), slot_known[slot] = 1;
        } else if (!strcmp(f[0], "B") && n >= 2) {
            for (int i = 0; i < nach; i++)
                if (!strcmp(achs[i].id, f[1])) achs[i].blocked = 1;
        }
    }
    free(text);
}

static void save_state(void) {
    size_t cap = 256 + (size_t)nach * 64 + SLOTS * 40, n = 0;
    char *out = malloc(cap);
    if (!out) return;
    n += snprintf(out + n, cap - n, "# F slot fingerprint of the progress.sav this game wrote; B an achievement that "
                                    "was already done in a save from elsewhere (runtime/achievements.c)\n");
    for (int i = 0; i < SLOTS; i++)
        if (slot_known[i]) n += snprintf(out + n, cap - n, "F\t%d\t%016llx\n", i, (unsigned long long)slot_print[i]);
    for (int i = 0; i < nach && n < cap; i++)
        if (achs[i].blocked) n += snprintf(out + n, cap - n, "B\t%s\n", achs[i].id);
    if (!plat_file_write_atomic(state_path, out, n)) rt_log("achievements: cannot write %s", state_path);
    free(out);
}

static uint64_t fnv1a(const uint8_t *d, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ d[i]) * 0x100000001b3ull;
    return h;
}

// The fingerprint of each slot's progress.sav under save_dir into print (present[i]: the slot has one); returns how
// many slots have one.
static int slot_prints(const char *save_dir, uint64_t *print, int *present) {
    int found = 0;
    for (int slot = 0; slot < SLOTS; slot++) {
        char path[1200];
        uint64_t size = 0;
        snprintf(path, sizeof path, "%s/slot_%02d/progress.sav", save_dir, slot);
        uint8_t *d = plat_file_read(path, &size);
        present[slot] = d != NULL;
        print[slot] = d ? fnv1a(d, size) : 0;
        found += d != NULL;
        free(d);
    }
    return found;
}

// The condition fields of an A or S line (f[3] the kind, f[4..7] its arguments) into a; 0 for an unknown kind.
static int parse_condition(Ach *a, char **f) {
    a->kind = kind_of(f[3]);
    if (a->kind < 0) return 0;
    a->unit = 1, a->list = -1;
    switch (a->kind) {
    case K_BOOL: a->a = (uint32_t)strtoul(f[4], NULL, 16), a->min = 1; break;
    case K_ENUM: a->a = (uint32_t)strtoul(f[4], NULL, 16), a->want = (uint32_t)strtoul(f[5], NULL, 16), a->min = 1; break;
    case K_NUM:
        a->a = (uint32_t)strtoul(f[4], NULL, 16), a->min = atof(f[5]);
        a->unit = atof(f[6]) > 0 ? atof(f[6]) : 1;
        a->after = f[7][0] ? (uint32_t)strtoul(f[7], NULL, 16) : 0;
        break;
    case K_ARRSUM: a->a = (uint32_t)strtoul(f[4], NULL, 16), a->min = atof(f[5]); break;
    case K_COUNT:
        a->list = atoi(f[4] + 1), a->min = atof(f[5]), a->want = (uint32_t)strtoul(f[6], NULL, 16);
        if (!a->want) a->want = 1;   // a list of bools: true
        break;
    case K_POUCH:
        a->a = (uint32_t)strtoul(f[4], NULL, 16), a->b = (uint32_t)strtoul(f[5], NULL, 16), a->min = atof(f[6]);
        a->item = dup_str(f[7]);
        break;
    case K_MP:   // mp counter min unit
        a->item = dup_str(f[4]), a->min = atof(f[5]);
        a->unit = atof(f[6]) > 0 ? atof(f[6]) : 1;
        break;
    }
    return 1;
}

static void load(void *arg) {
    (void)arg;
    char dir[1024], path[1200], tsv[1200];
    exe_dir(dir, sizeof dir);
    snprintf(tsv, sizeof tsv, "%sachievements/compiled.tsv", dir);
    snprintf(unlocked_path, sizeof unlocked_path, "%s/achievements.tsv", rt_config()->profile);
    snprintf(mp_path, sizeof mp_path, "%s/achievements-mp.tsv", rt_config()->profile);
    snprintf(state_path, sizeof state_path, "%s/achievements-state.tsv", rt_config()->profile);
    uint64_t size = 0;
    char *text = (char *)plat_file_read(tsv, &size);
    if (!text) {
        rt_log("achievements: no %s (the setup or the achievements editor writes it); none this run", tsv);
        return;
    }
    int lines = 1;
    for (char *p = text; *p; p++) lines += *p == '\n';
    achs = calloc((size_t)lines, sizeof *achs);
    lists = calloc((size_t)lines, sizeof *lists);
    char *pos = text;
    for (char *line; achs && lists && (line = next_line(&pos));) {
        char *f[12];
        if (*line == '#') continue;
        int n = split(line, f, 12);
        if (f[0][0] == 'L' && n >= 2) {   // L<n>\t<hash>,<hash>,...
            int id = atoi(f[0] + 1), count = 1;
            if (id < 0 || id >= lines) continue;
            for (char *p = f[1]; *p; p++) count += *p == ',';
            lists[id].h = malloc(sizeof(uint32_t) * (size_t)count);
            for (char *p = f[1]; lists[id].h && *p;) {
                lists[id].h[lists[id].n++] = (uint32_t)strtoul(p, &p, 16);
                if (*p == ',') p++;
            }
            if (id >= nlists) nlists = id + 1;
        } else if (!strcmp(f[0], "S") && n >= 7) {   // S stat kind a b c d: a progress figure for the room
            int k = -1;
            for (int j = 0; j < ACH_STATS; j++)
                if (!strcmp(f[1], stat_names[j])) k = j;
            if (k < 0) continue;
            char *g[11] = {"A", f[1], "common", f[2], f[3], f[4], f[5], f[6], "0", "", ""};
            if (parse_condition(&stats[k], g)) stat_found[k] = 1;
        } else if (!strcmp(f[0], "A") && n >= 11) {   // A id rarity kind a b c d hidden name description
            Ach *a = &achs[nach];
            if (!parse_condition(a, f)) continue;
            a->id = dup_str(f[1]), a->name = dup_str(f[9]), a->desc = dup_str(f[10]);
            a->rarity = rarity_of(f[2]);
            a->hidden = atoi(f[8]);
            snprintf(path, sizeof path, "%sachievements/icons/%s.rgba", dir, a->id);
            uint64_t isize = 0;
            a->icon = plat_file_read(path, &isize);
            if (a->icon && isize != ACH_ICON * ACH_ICON * 4) {
                free(a->icon);
                a->icon = NULL;
            }
            nach++;
        }
    }
    free(text);
    for (int k = 0; k < ACH_STATS; k++)
        if (stat_found[k] && stats[k].kind == K_COUNT &&
            (stats[k].list < 0 || stats[k].list >= nlists || !lists[stats[k].list].h))
            stat_found[k] = 0;
    for (int i = 0; i < nach; i++)
        if (achs[i].kind == K_COUNT && (achs[i].list < 0 || achs[i].list >= nlists || !lists[achs[i].list].h)) {
            rt_log("achievements: %s names a missing flag list; it stays locked", achs[i].id);
            achs[i].kind = -1;
        }
    int others = 0;
    for (int i = 0; i < nach; i++) others += achs[i].kind != K_ALL && achs[i].kind != K_MP;
    for (int i = 0; i < nach; i++)
        if (achs[i].kind == K_ALL) achs[i].min = others;
    load_unlocked();
    load_state();
    load_mp();
    for (int i = 0; i < nach; i++)
        if (achs[i].kind == K_MP) {
            double *v = mp_slot(achs[i].item, 0);
            achs[i].cur = v ? *v : 0;
        }
    int done = 0;
    for (int i = 0; i < nach; i++) done += achs[i].unlocked;
    rt_log("achievements: %d loaded from %s, %d unlocked before", nach, tsv, done);
}

static void ensure_loaded(void) { plat_once(&load_once, load, NULL); }

// ---- progress.sav: (hash, value) pairs after a 0x20-byte header, in sections opened by (0, type index); arrays and
// strings sit at the value's offset in the file (tools/achievements.py read_save).
enum { T_BOOL = 0, T_INT = 2, T_INT_ARRAY = 3, T_FLOAT = 4, T_ENUM = 6, T_STRING64_ARRAY = 17, T_UINT = 20,
       T_UINT_ARRAY = 21 };
enum { MAP_BITS = 17, MAP_CAP = 1 << MAP_BITS };

typedef struct {
    const uint8_t *d;
    uint64_t size;
    uint32_t *key, *val;
    uint8_t *type;
} Save;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int save_find(const Save *s, uint32_t h, uint32_t *val, int *type) {
    for (uint32_t i = (h * 2654435761u) >> (32 - MAP_BITS);; i = (i + 1) & (MAP_CAP - 1)) {
        if (!s->key[i]) return 0;
        if (s->key[i] == h) {
            *val = s->val[i], *type = s->type[i];
            return 1;
        }
    }
}

static int save_parse(Save *s) {
    if (s->size < 0x20 || rd32(s->d) != 0x01020304u) return 0;
    uint64_t end = rd32(s->d + 8);
    if (end > s->size) end = s->size;
    s->key = calloc(MAP_CAP, sizeof *s->key), s->val = calloc(MAP_CAP, sizeof *s->val), s->type = calloc(MAP_CAP, 1);
    if (!s->key || !s->val || !s->type) return 0;
    int type = -1, n = 0;
    for (uint64_t p = 0x20; p + 8 <= end; p += 8) {
        uint32_t h = rd32(s->d + p), v = rd32(s->d + p + 4);
        if (!h) {
            type = (int)v;
            continue;
        }
        if (++n > MAP_CAP * 3 / 4) break;
        uint32_t i = (h * 2654435761u) >> (32 - MAP_BITS);
        while (s->key[i] && s->key[i] != h) i = (i + 1) & (MAP_CAP - 1);
        s->key[i] = h, s->val[i] = v, s->type[i] = (uint8_t)type;
    }
    return 1;
}

static void save_free(Save *s) {
    free(s->key), free(s->val), free(s->type);
    free((void *)s->d);
}

// An int array: its count and first item, NULL when the hash is not one or it runs past the file.
static const uint8_t *int_array(const Save *s, uint32_t h, uint32_t *count) {
    uint32_t off;
    int t;
    if (!save_find(s, h, &off, &t) || (t != T_INT_ARRAY && t != T_UINT_ARRAY) || (uint64_t)off + 4 > s->size) return NULL;
    *count = rd32(s->d + off);
    if ((uint64_t)off + 4 + 4ull * *count > s->size) return NULL;
    return s->d + off + 4;
}

static double value_of(const Ach *a, const Save *s) {
    uint32_t v, n;
    int t;
    switch (a->kind) {
    case K_BOOL: return save_find(s, a->a, &v, &t) && t == T_BOOL && v == 1;
    case K_ENUM: return save_find(s, a->a, &v, &t) && t == T_ENUM && v == a->want;
    case K_NUM:
        if (a->after && !(save_find(s, a->after, &v, &t) && t == T_BOOL && v == 1)) return 0;
        if (!save_find(s, a->a, &v, &t)) return 0;
        if (t == T_INT) return (int32_t)v;
        if (t == T_UINT) return v;
        if (t == T_FLOAT) {
            float f;
            memcpy(&f, &v, 4);
            return f;
        }
        return 0;
    case K_ARRSUM: {
        const uint8_t *p = int_array(s, a->a, &n);
        double sum = 0;
        for (uint32_t i = 0; p && i < n; i++)
            if ((int32_t)rd32(p + 4 * i) > 0) sum += (int32_t)rd32(p + 4 * i);
        return sum;
    }
    case K_COUNT: {
        const HashList *l = &lists[a->list];
        int c = 0;
        for (int i = 0; i < l->n; i++) c += save_find(s, l->h[i], &v, &t) && v == a->want;
        return c;
    }
    case K_POUCH: {
        uint32_t off;
        const uint8_t *stock = int_array(s, a->b, &n);
        if (!stock || !save_find(s, a->a, &off, &t) || t != T_STRING64_ARRAY || (uint64_t)off + 4 > s->size) return 0;
        uint32_t names = rd32(s->d + off);
        double have = 0;
        for (uint32_t i = 0; i < names && i < n && (uint64_t)off + 4 + 64ull * (i + 1) <= s->size; i++)
            if (!strncmp((const char *)s->d + off + 4 + 64 * i, a->item, 64) && (int32_t)rd32(stock + 4 * i) > 0)
                have += (int32_t)rd32(stock + 4 * i);
        return have;
    }
    }
    return 0;
}

// The slot directory under save_dir whose progress.sav was written last.
static int newest_progress(const char *save_dir, char *out, size_t cap) {
    uint64_t best = 0;
    int found = 0;
    for (int slot = 0; slot < 10; slot++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/slot_%02d/progress.sav", save_dir, slot);
        PlatFile *f = plat_file_open(path);
        PlatFileId id;
        if (!f) continue;
        if (plat_file_id(f, &id) && (!found || id.write_time > best)) {
            best = id.write_time, found = 1;
            snprintf(out, cap, "%s", path);
        }
        plat_file_close(f);
    }
    return found;
}

// trusted: the newest save is one this game wrote (a commit just now, or the save at start matching the fingerprints);
// otherwise what it already holds is blocked instead of unlocked.
// Under lock: the fresh unlocks to the menu's notices and to the room.
static void announce(const int *fresh, int nfresh) {
    for (int k = 0; k < nfresh; k++) {
        int next = (notice_tail + 1) % NOTICE_CAP;
        if (next == notice_head) break;
        notice[notice_tail] = fresh[k], notice_batch[notice_tail] = nfresh;
        notice_tail = next;
        rt_log("achievements: unlocked %s (%s)", achs[fresh[k]].id, achs[fresh[k]].name);
    }
    for (int k = 0; k < nfresh; k++) {
        int next = (net_tail + 1) % NOTICE_NET_CAP;
        if (next == net_head) break;
        net_unlock[net_tail] = fresh[k];
        net_tail = next;
    }
}

// ---- what the player wears (ach_look), for the friends' figures (net_mp.c LOOK lines): the armor pieces the save's
// Pouch.Armor.EquipIndexes.Head / .Upper / .Bottom (int arrays, item 0 the index, -1 none) point at in
// Pouch.Armor.Content.Name (names as the TotK savegame editor's hash list gives them, checked against GameDataList).
enum { H_ARMOR_NAMES = 0x754e8549u, H_EQUIP_HEAD = 0xc88d0a1eu, H_EQUIP_UPPER = 0x632017b5u, H_EQUIP_BOTTOM = 0x94ce9bbfu };
static char look[3][64];
static unsigned look_gen;

static void read_look(const Save *s, char out[3][64]) {
    static const uint32_t slots[3] = {H_EQUIP_HEAD, H_EQUIP_UPPER, H_EQUIP_BOTTOM};
    uint32_t off, n;
    int t;
    memset(out, 0, 3 * 64);
    if (!save_find(s, H_ARMOR_NAMES, &off, &t) || t != T_STRING64_ARRAY || (uint64_t)off + 4 > s->size) return;
    uint32_t names = rd32(s->d + off);
    for (int k = 0; k < 3; k++) {
        const uint8_t *p = int_array(s, slots[k], &n);
        int32_t i = p && n ? (int32_t)rd32(p) : -1;
        if (i < 0 || (uint32_t)i >= names || (uint64_t)off + 4 + 64ull * ((uint64_t)i + 1) > s->size) continue;
        memcpy(out[k], s->d + off + 4 + 64 * (uint32_t)i, 63);
        if (strncmp(out[k], "Armor_", 6)) out[k][0] = 0;
    }
}

unsigned ach_look(char out[3][64]) {
    plat_mutex_lock(&lock);
    memcpy(out, look, sizeof look);
    unsigned g = look_gen;
    plat_mutex_unlock(&lock);
    return g;
}

static void evaluate(const char *save_dir, int trusted) {
    char path[1200];
    if (!newest_progress(save_dir, path, sizeof path)) return;
    Save s = {0};
    s.d = plat_file_read(path, &s.size);
    if (!s.d || !save_parse(&s)) {
        rt_log("achievements: %s is not a progress.sav this build reads", path);
        save_free(&s);
        return;
    }
    double vals[1024];
    int n = nach < 1024 ? nach : 1024;
    for (int i = 0; i < n; i++)
        vals[i] = achs[i].kind == K_ALL || achs[i].kind == K_MP || achs[i].kind < 0 ? 0 : value_of(&achs[i], &s);
    double stat_vals[ACH_STATS];
    for (int k = 0; k < ACH_STATS; k++) stat_vals[k] = stat_found[k] ? value_of(&stats[k], &s) / stats[k].unit : 0;
    char worn[3][64];
    read_look(&s, worn);
    uint32_t av;
    int at, woke = save_find(&s, FLAG_AWAKE, &av, &at) && at == T_BOOL && av == 1;
    save_free(&s);

    plat_mutex_lock(&lock);
    if (memcmp(worn, look, sizeof look)) memcpy(look, worn, sizeof look), look_gen++;
    int fresh[1024], nfresh = 0, others = 0, others_done = 0, changed = 0, blocked = 0;
    int64_t now = (int64_t)time(NULL);
    for (int k = 0; k < ACH_STATS; k++) stats[k].cur = stat_vals[k];
    awake = woke;
    stats_gen++;
    for (int i = 0; i < n; i++) {
        Ach *a = &achs[i];
        if (a->kind == K_ALL || a->kind == K_MP || a->kind < 0) continue;
        a->cur = vals[i];
        others++;
        int done = vals[i] >= a->min;
        if (!done && a->blocked) {   // false again (a new game): it can be earned
            a->blocked = 0, changed = 1;
        } else if (done && !a->unlocked && !a->blocked && !trusted) {
            a->blocked = 1, changed = 1;
            blocked++;
        } else if (done && !a->unlocked && !a->blocked) {
            a->unlocked = 1, a->when = now;
            fresh[nfresh++] = i;
        }
        others_done += a->unlocked;
    }
    for (int i = 0; i < n; i++) {   // the platinum one: every other achievement
        Ach *a = &achs[i];
        if (a->kind != K_ALL) continue;
        a->cur = others_done, a->min = others;
        if (!a->unlocked && others && others_done >= others) {
            a->unlocked = 1, a->when = now;
            fresh[nfresh++] = i;
        }
    }
    announce(fresh, nfresh);
    if (nfresh) save_unlocked();
    if (blocked) rt_log("achievements: the save at start was not written by this game here; %d achievements it already "
                        "holds do not unlock until they are earned again", blocked);
    if (changed) save_state();
    plat_mutex_unlock(&lock);
}

// After evaluating: the slots as they are now are the ones this game wrote.
static void remember_slots(const char *save_dir) {
    uint64_t print[SLOTS];
    int present[SLOTS];
    slot_prints(save_dir, print, present);
    plat_mutex_lock(&lock);
    for (int i = 0; i < SLOTS; i++) slot_print[i] = print[i], slot_known[i] = present[i];
    save_state();
    plat_mutex_unlock(&lock);
}

void ach_save_committed(const char *save_dir) {
    ensure_loaded();
    if (!nach || !save_dir) return;
    evaluate(save_dir, 1);
    remember_slots(save_dir);
}

void ach_save_opened(const char *save_dir) {
    ensure_loaded();
    if (!nach || !save_dir) return;
    uint64_t print[SLOTS];
    int present[SLOTS], same = 1;
    if (!slot_prints(save_dir, print, present)) return;   // no save yet: a new game
    for (int i = 0; i < SLOTS; i++)
        if (present[i] && (!slot_known[i] || slot_print[i] != print[i])) same = 0;
    if (!same) rt_log("achievements: the save at start differs from the one this game last wrote");
    evaluate(save_dir, same);
    remember_slots(save_dir);
}

// A multiplayer counter changed: its K_MP achievements unlock at once. max: keep the larger value instead of adding.
static void mp_update(const char *name, double v, int max) {
    ensure_loaded();
    if (!nach) return;   // no achievements loaded: the counters file was not read either, so it is left alone
    plat_mutex_lock(&lock);
    double *c = mp_slot(name, 1);
    if (!c || (max && v <= *c) || (!max && v == 0)) {
        plat_mutex_unlock(&lock);
        return;
    }
    *c = max ? v : *c + v;
    int fresh[64], nfresh = 0;
    int64_t now = (int64_t)time(NULL);
    for (int i = 0; i < nach && nfresh < 64; i++) {
        Ach *a = &achs[i];
        if (a->kind != K_MP || strcmp(a->item, name)) continue;
        a->cur = *c;
        if (!a->unlocked && a->cur >= a->min) a->unlocked = 1, a->when = now, fresh[nfresh++] = i;
    }
    announce(fresh, nfresh);
    if (nfresh) save_unlocked();
    save_mp();
    plat_mutex_unlock(&lock);
}

void ach_mp_add(const char *name, double delta) { mp_update(name, delta, 0); }
void ach_mp_max(const char *name, double value) { mp_update(name, value, 1); }

int ach_count(void) {
    ensure_loaded();
    return nach;
}

int ach_get(int i, AchView *out) {
    ensure_loaded();
    if (i < 0 || i >= nach) return 0;
    plat_mutex_lock(&lock);
    const Ach *a = &achs[i];
    double unit = a->unit > 0 ? a->unit : 1;
    *out = (AchView){a->id, a->name, a->desc, a->rarity, a->hidden, a->unlocked, a->blocked, a->when,
                     a->unlocked && a->cur < a->min ? a->min / unit : a->cur / unit, a->min / unit, a->icon,
                     a->kind == K_MP};
    plat_mutex_unlock(&lock);
    return 1;
}

unsigned ach_stats(AchStats *out) {
    ensure_loaded();
    plat_mutex_lock(&lock);
    for (int k = 0; k < ACH_STATS; k++) out->v[k] = stat_found[k] ? (int)stats[k].cur : -1;
    int done = 0;
    for (int i = 0; i < nach; i++) done += achs[i].unlocked;
    out->achievements = done;
    out->awake = awake;
    unsigned gen = stats_gen;
    plat_mutex_unlock(&lock);
    return gen;
}

int ach_take_net_unlock(void) {
    plat_mutex_lock(&lock);
    int idx = -1;
    if (net_head != net_tail) {
        idx = net_unlock[net_head];
        net_head = (net_head + 1) % NOTICE_NET_CAP;
    }
    plat_mutex_unlock(&lock);
    return idx;
}

int ach_find(const char *id) {
    ensure_loaded();
    for (int i = 0; i < nach; i++)
        if (!strcmp(achs[i].id, id)) return i;
    return -1;
}

int ach_blocked_count(void) {
    ensure_loaded();
    plat_mutex_lock(&lock);
    int c = 0;
    for (int i = 0; i < nach; i++) c += achs[i].blocked && !achs[i].unlocked;
    plat_mutex_unlock(&lock);
    return c;
}

int ach_unlocked_count(void) {
    ensure_loaded();
    plat_mutex_lock(&lock);
    int c = 0;
    for (int i = 0; i < nach; i++) c += achs[i].unlocked;
    plat_mutex_unlock(&lock);
    return c;
}

int ach_take_notice(int *count) {
    plat_mutex_lock(&lock);
    int idx = -1;
    *count = 0;
    if (notice_head != notice_tail) {
        int batch = notice_batch[notice_head];
        if (batch > 3) {   // a large batch (the first run on an old save): one summary with its rarest achievement
            idx = notice[notice_head];
            for (int k = 0; k < batch && notice_head != notice_tail; k++) {
                int j = notice[notice_head];
                if (achs[j].rarity > achs[idx].rarity) idx = j;
                notice_head = (notice_head + 1) % NOTICE_CAP;
            }
            *count = batch;
        } else {
            idx = notice[notice_head];
            notice_head = (notice_head + 1) % NOTICE_CAP;
            *count = 1;
        }
    }
    plat_mutex_unlock(&lock);
    return idx;
}
