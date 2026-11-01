// The Cheats tab of the in-game menu: the cheats of cheats\<build id>.txt (cheats.c), a row per cheat in file order and
// a heading per group label; a cheat that cannot be switched here is greyed out with the reason.
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "cheats.h"
#include "ui_text.h"
}

extern "C" void tab_cheats(float scale) {
    opt_begin("##cheats", scale);
    static char line[512], desc[768], buttons[192];
    int n = cheats_count();
    if (!n) {
        snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_NONE),
                 cheats_file()[0] ? cheats_file() : "cheats\\<build id>.txt");
        opt_text_str(line);
        opt_end();
        return;
    }
    int listed = 0;
    for (int i = 0; i < n; i++) listed += cheats_info(i)->kind != CHEAT_HEADING;
    snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_FILE), cheats_file(), listed);
    opt_text_str(line);
    for (int i = 0; i < n; i++) {
        const CheatInfo *c = cheats_info(i);
        UiTextId text = UI_MENU_CHEAT_CODE_DESC;
        switch (c->kind) {
        case CHEAT_HEADING:
            if (!strstr(c->name, "TID=")) opt_group_text(c->name);   // the file's title line only names the game
            continue;
        case CHEAT_CODE: break;
        case CHEAT_KEY: text = UI_MENU_CHEAT_KEY_DESC; break;
        case CHEAT_DATA: text = UI_MENU_CHEAT_DATA_DESC; break;
        case CHEAT_MASTER: text = UI_MENU_CHEAT_MASTER_DESC, opt_disable_next(UI_MENU_CHEAT_MASTER); break;
        case CHEAT_RESTORE: text = UI_MENU_CHEAT_RESTORE_DESC, opt_disable_next(UI_MENU_CHEAT_RESTORE); break;
        case CHEAT_REBUILD: text = UI_MENU_CHEAT_REBUILD_DESC, opt_disable_next(UI_MENU_CHEAT_REBUILD); break;
        case CHEAT_UNSUPPORTED:
            text = UI_MENU_CHEAT_UNSUPPORTED_DESC, opt_disable_next(UI_MENU_CHEAT_UNSUPPORTED);
            break;
        }
        if (c->kind == CHEAT_KEY)
            snprintf(desc, sizeof desc, menu_text(text), cheats_buttons(c->buttons, buttons, sizeof buttons));
        else if (c->kind == CHEAT_UNSUPPORTED)
            snprintf(desc, sizeof desc, "%s %s", menu_text(text), c->note);
        else
            snprintf(desc, sizeof desc, "%s", menu_text(text));
        if (opt_toggle_text(c->key, c->name, desc, cheats_on(i) != 0)) {
            cheats_set(i, !cheats_on(i));
            opt_save_soon();
        }
    }
    opt_end();
}
