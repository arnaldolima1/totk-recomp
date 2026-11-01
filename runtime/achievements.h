// Achievements: conditions over the game's saved GameData flags, checked after every save the game commits.
//
// tools/achievements.py compiles the definitions of the install folder (achievements/achievements.json, made by the
// setup or the achievements editor) into achievements/compiled.tsv next to totk.exe, every condition resolved to flag
// hashes, and each icon into achievements/icons/<id>.rgba (64x64 RGBA). This module reads them once, evaluates them
// against the newest slot's progress.sav whenever fs_host.c commits a save (and once when the save storage comes up),
// records what became true in <profile>/achievements.tsv and queues a notice for the menu (ui/menu.cpp) to show.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ACH_COMMON, ACH_RARE, ACH_EPIC, ACH_LEGENDARY, ACH_PLATINUM, ACH_RARITIES };
enum { ACH_ICON = 64 };   // icons are ACH_ICON x ACH_ICON straight RGBA

typedef struct {
    const char *id, *name, *desc;
    int rarity;
    int hidden;            // shown as a secret until unlocked
    int unlocked;
    int blocked;           // already done in a save from elsewhere: counts only once earned again
    int64_t when;          // unix time of the unlock, 0 when locked
    double cur, target;    // progress in shown units (target 1 for a yes/no condition)
    const uint8_t *icon;   // ACH_ICON * ACH_ICON * 4 bytes, NULL when the file is missing
    int multiplayer;       // a multiplayer achievement: outside the platinum, inside 100 %
} AchView;

// The save storage's newest save directory (save/<uid> or the finished commit in save-tx): after a commit, or once at
// start. Evaluates the slot whose progress.sav was written last. A commit is the game's own save; at start the save
// counts as the game's only when every slot matches the fingerprint taken after the last commit, else what it already
// holds is blocked (a save copied in, a backup put back). Runs in the caller's thread.
void ach_save_committed(const char *save_dir);
void ach_save_opened(const char *save_dir);
int ach_blocked_count(void);         // locked achievements a save from elsewhere already held

// The progress figures the multiplayer room shows, read with the achievements after each save: shrines done,
// lightroots lit, towers opened, Korok seeds in the pouch, heart containers; -1 for one the compiled file lacks.
enum { ACH_STAT_SHRINES, ACH_STAT_LIGHTROOTS, ACH_STAT_TOWERS, ACH_STAT_KOROKS, ACH_STAT_HEARTS, ACH_STATS };
typedef struct {
    int v[ACH_STATS];
    int achievements;   // unlocked
    int awake;          // 1 once Link woke on the Great Sky Island (the prologue is over), 0 before, -1 no save read
} AchStats;
unsigned ach_stats(AchStats *out);   // returns a count that grows with every evaluation (0: no save read yet)
unsigned ach_look(char out[3][64]);   // the armor pieces worn at the last save read (head, upper, lower; "" none);
                                      // the result grows when they change (0: none read yet)
int ach_take_net_unlock(void);       // the next unlock to tell the room (an index for ach_get), -1 when none
int ach_find(const char *id);        // the index of an achievement id, -1 when this install has none

// Multiplayer counters (net_mp.c): what happens in rooms, kept in <profile>/achievements-mp.tsv; the multiplayer
// achievements over them unlock at once. add: count up by delta; max: the largest value seen.
void ach_mp_add(const char *name, double delta);
void ach_mp_max(const char *name, double value);

int ach_count(void);                 // achievements loaded (0 without compiled.tsv); loads them on the first call
int ach_get(int i, AchView *out);    // 1 when i is in range; the strings live until exit
int ach_unlocked_count(void);
int ach_take_notice(int *count);     // the next unlock to announce (its index), -1 when none; *count: how many waited
                                     // in the same batch (a large batch is announced as one summary)

#ifdef __cplusplus
}
#endif
