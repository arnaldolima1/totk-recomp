// Player mods: the scan of the mods folder, its order and switches (mods.h).
//
// One subfolder per mod, as emulators take them. The RomFS root is the first of <mod>/romfs, <mod>/romfslite (TKMM's
// name for it when it bypasses Atmosphere's RomFS build), <mod>/atmosphere/contents/<title>/romfs,
// <mod>/contents/<title>/romfs and <mod>/<title>/romfs that exists; the ExeFS and cheats folders likewise. A mod is
// listed whatever it holds; what the port cannot use is its problem, shown in the menu and logged, and never stops the
// start. romfs.c serves the RomFS files of the enabled mods, the top one winning a path.
//
// The order and the switches are totk.ini keys (cfg.c): TOTK_MODS_ORDER lists names top first, separated by '|' (a
// Windows file name cannot hold '|'; a folder whose name does on Linux is listed as unusable); a folder it does not name
// goes below the named ones, by name. TOTK_MODS_DISABLED lists the switched-off names. The menu changes the list in
// place and writes both keys; the scan's state is kept to tell the menu a restart is due.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"

#define TITLE "0100F2C0115B6000"
#define MAX_MODS 512

static ModInfo *mods;
static int nmods;
static char dir[2048];
static char start_order[MAX_MODS][256];   // names in the scan's order and their switches, for mods_changed
static int start_enabled[MAX_MODS];

// 1 when a file lies anywhere under path.
static int has_file_cb(const char *name, int kind, uint64_t size, void *ctx);
static int has_file(const char *path) {
    struct { const char *path; int found; } c = {path, 0};
    plat_fs_list(path, has_file_cb, &c);
    return c.found;
}
static int has_file_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    struct { const char *path; int found; } *c = ctx;
    if (kind == PLAT_FS_FILE) {
        c->found = 1;
    } else if (kind == PLAT_FS_DIR) {
        char sub[2100];
        if (snprintf(sub, sizeof sub, "%s/%s", c->path, name) < (int)sizeof sub) c->found = has_file(sub);
    }
    return !c->found;
}

// The first of the emulator and Atmosphere places of <mod>/<part> that is a folder, into out ("" none).
static void find_root(const char *mod, const char *part, char *out, size_t cap) {
    static const char *const forms[] = {"%s/%s", "%s/atmosphere/contents/" TITLE "/%s", "%s/contents/" TITLE "/%s",
                                        "%s/" TITLE "/%s"};
    for (size_t i = 0; i < sizeof forms / sizeof *forms; i++) {
        if (snprintf(out, cap, forms[i], mod, part) < (int)cap && plat_dir_exists(out)) return;
    }
    out[0] = 0;
}

static int ends_with_ci(const char *s, const char *tail) {
    size_t n = strlen(s), t = strlen(tail);
    return n >= t && !strcasecmp(s + n - t, tail);
}

// What the ExeFS folder holds: MOD_EXEFS for patches (.ips, .pchtxt), MOD_SUBSDK for code modules (subsdk* plugins, a
// replaced main). main.npdm alone is the process's metadata, not code: it counts as neither.
static int exefs_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    int *k = ctx;
    if (kind != PLAT_FS_FILE) return 1;
    if (ends_with_ci(name, ".ips") || ends_with_ci(name, ".pchtxt")) *k |= MOD_EXEFS;
    else if (!strncasecmp(name, "subsdk", 6) || !strcasecmp(name, "main")) *k |= MOD_SUBSDK;
    return 1;
}

// MOD_CHEATS when the cheats folder holds an Atmosphere cheat file (<build id>.txt).
static int cheats_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    if (kind == PLAT_FS_FILE && ends_with_ci(name, ".txt")) *(int *)ctx |= MOD_CHEATS;
    return 1;
}

// 1 when name is one of the '|'-separated names of list.
static int listed(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len == n && !memcmp(p, name, n)) return 1;
        p = bar ? bar + 1 : NULL;
    }
    return 0;
}

// The position of name in the '|'-separated list, or -1.
static int list_pos(const char *list, const char *name) {
    size_t n = strlen(name);
    int k = 0;
    for (const char *p = list; p && *p; k++) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len == n && !memcmp(p, name, n)) return k;
        p = bar ? bar + 1 : NULL;
    }
    return -1;
}

static const char *order_list;   // TOTK_MODS_ORDER while sorting
static int by_order(const void *a, const void *b) {
    const ModInfo *x = a, *y = b;
    int px = list_pos(order_list, x->name), py = list_pos(order_list, y->name);
    if (px >= 0 && py >= 0) return px - py;
    if (px >= 0 || py >= 0) return px >= 0 ? -1 : 1;
    return strcmp(x->name, y->name);
}

static const char *problem_name(ModProblem p) {
    switch (p) {
    case MOD_EMPTY: return "empty";
    case MOD_PLUGIN: return "has a code module (subsdk or main), not supported; the whole mod stays off";
    case MOD_EXEFS_LATER: return "ExeFS patches need a game code rebuild, not applied; the whole mod stays off";
    case MOD_BAD_NAME: return "its folder name holds '|', which the order list uses; rename the folder";
    default: return "ok";
    }
}

// One mod folder of the scan.
static int scan_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    const char *disabled = ctx;
    if (kind != PLAT_FS_DIR) return 1;
    if (nmods == MAX_MODS) {
        rt_log("mods: more than %d folders in %s; the rest are left out", MAX_MODS, dir);
        return 0;
    }
    ModInfo *m = &mods[nmods];
    memset(m, 0, sizeof *m);
    char path[2100];
    if (snprintf(m->name, sizeof m->name, "%s", name) >= (int)sizeof m->name ||
        snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) {
        rt_log("mods: %s: name too long, left out", name);
        return 1;
    }
    find_root(path, "romfs", m->romfs, sizeof m->romfs);
    if (!m->romfs[0]) find_root(path, "romfslite", m->romfs, sizeof m->romfs);
    find_root(path, "exefs", m->exefs, sizeof m->exefs);
    find_root(path, "cheats", m->cheats, sizeof m->cheats);
    if (m->romfs[0] && has_file(m->romfs)) m->kind |= MOD_ROMFS;
    if (m->exefs[0]) plat_fs_list(m->exefs, exefs_cb, &m->kind);
    if (m->cheats[0]) plat_fs_list(m->cheats, cheats_cb, &m->kind);
    // a mod whose code part the port cannot use is off as a whole: its RomFS files may expect that code
    m->problem = strchr(m->name, '|')               ? MOD_BAD_NAME
               : m->kind & MOD_SUBSDK               ? MOD_PLUGIN
               : m->kind & MOD_EXEFS                ? MOD_EXEFS_LATER
               : m->kind & (MOD_ROMFS | MOD_CHEATS) ? MOD_OK
                                                    : MOD_EMPTY;
    m->enabled = !listed(disabled, m->name);
    nmods++;
    return 1;
}

void mods_scan(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *d = cfg_get("TOTK_MODS_DIR");
    if (d && *d) {
        snprintf(dir, sizeof dir, "%s", d);
    } else {
        char exe[2048];
        if (!plat_exe_path(exe, sizeof exe)) return;
        char *cut = strrchr(exe, '/'), *back = strrchr(exe, '\\');
        if (back > cut) cut = back;
        if (!cut) return;
        *cut = 0;
        snprintf(dir, sizeof dir, "%s/mods", exe);
    }
    if (!plat_dir_exists(dir)) {
        rt_log("mods: no folder %s", dir);
        return;
    }
    mods = calloc(MAX_MODS, sizeof *mods);
    if (!mods) return;
    plat_fs_list(dir, scan_cb, (void *)cfg_get("TOTK_MODS_DISABLED"));
    order_list = cfg_get("TOTK_MODS_ORDER");
    qsort(mods, nmods, sizeof *mods, by_order);
    order_list = NULL;
    for (int i = 0; i < nmods; i++) {
        snprintf(start_order[i], sizeof start_order[i], "%s", mods[i].name);
        start_enabled[i] = mods[i].enabled;
        const ModInfo *m = &mods[i];
        rt_log("mods: %s: %s,%s%s%s%s%s, %s", m->name, m->enabled ? "on" : "off", m->kind & MOD_ROMFS ? " romfs" : "",
               m->kind & MOD_EXEFS ? " exefs" : "", m->kind & MOD_SUBSDK ? " subsdk" : "",
               m->kind & MOD_CHEATS ? " cheats" : "", m->kind ? "" : " no files", problem_name(m->problem));
    }
    rt_log("mods: %d folders in %s", nmods, dir);
}

const char *mods_dir(void) { return dir; }
int mods_count(void) { return nmods; }
ModInfo *mods_at(int i) { return i >= 0 && i < nmods ? &mods[i] : NULL; }

// TOTK_MODS_ORDER as every listed name top first; TOTK_MODS_DISABLED as the switched-off names. A name holding '|'
// would split into two; it is left out of both (its mod is unusable anyway).
static void write_keys(void) {
    size_t cap = (size_t)nmods * 257 + 1;
    char *order = calloc(cap, 1), *off = calloc(cap, 1);
    if (order && off) {
        for (int i = 0; i < nmods; i++) {
            if (mods[i].problem == MOD_BAD_NAME) continue;
            if (*order) strcat(order, "|");
            strcat(order, mods[i].name);
            if (!mods[i].enabled) {
                if (*off) strcat(off, "|");
                strcat(off, mods[i].name);
            }
        }
        cfg_set("TOTK_MODS_ORDER", order);
        cfg_set("TOTK_MODS_DISABLED", off);
    }
    free(order);
    free(off);
}

void mods_set_enabled(int i, int on) {
    if (i < 0 || i >= nmods) return;
    mods[i].enabled = on != 0;
    write_keys();
}

void mods_move(int i, int delta) {
    int j = i + delta;
    if (i < 0 || i >= nmods || j < 0 || j >= nmods || i == j) return;
    ModInfo t = mods[i];
    mods[i] = mods[j];
    mods[j] = t;
    write_keys();
}

int mods_changed(void) {
    for (int i = 0; i < nmods; i++)
        if (strcmp(mods[i].name, start_order[i]) || mods[i].enabled != start_enabled[i]) return 1;
    return 0;
}
