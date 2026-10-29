// The software keyboard's front end for SDL windows (swkbd.h). Until the in-game text field exists (an ImGui entry in
// the menu overlay, which SDL_StartTextInput can pair with the Steam Deck's on-screen keyboard), the text entry takes
// the starting text at once, as TOTK_SWKBD_ANSWER=<initial> does: the game's initial text, else the default name from
// the catalog. TotK asks only for a horse's name.
#include <string.h>

#include "host.h"
#include "swkbd.h"

int text_entry_run(const SwkbdRequest *req, uint16_t *out, size_t cap, size_t *n) {
    size_t k = 0;
    while (k < cap && req->initial[k]) k++;
    memcpy(out, req->initial, k * 2);
    *n = k;
    rt_log("swkbd: no text entry window on this host yet: the %s name is taken", req->game_initial ? "game's" : "default");
    return k > 0;
}
