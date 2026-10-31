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
    int64_t when;          // unix time of the unlock, 0 when locked
    double cur, target;    // progress in shown units (target 1 for a yes/no condition)
    const uint8_t *icon;   // ACH_ICON * ACH_ICON * 4 bytes, NULL when the file is missing
} AchView;

// The save storage's newest save directory (save/<uid> or the finished commit in save-tx) after a commit or at start:
// evaluates the slot whose progress.sav was written last. Runs in the caller's thread (the commit thread).
void ach_save_committed(const char *save_dir);

int ach_count(void);                 // achievements loaded (0 without compiled.tsv); loads them on the first call
int ach_get(int i, AchView *out);    // 1 when i is in range; the strings live until exit
int ach_unlocked_count(void);
int ach_take_notice(int *count);     // the next unlock to announce (its index), -1 when none; *count: how many waited
                                     // in the same batch (a large batch is announced as one summary)

#ifdef __cplusplus
}
#endif
