// Win32 adapter of the in-game menu: window messages to menu events (menu.h). The only Win32 code of the menu; the SDL3
// window has its own (ui/menu_sdl.cpp). Window thread.
//
// F1 opens and closes the menu, Esc opens it (with the menu open, Esc goes to the menu, which closes). Esc also releases
// the cursor (input_window_msg runs first; closing the menu gives the capture back). While the menu is open, its keys,
// the typed characters, the mouse and focus changes go to it; the game polls the keyboard on its own (input_win.c) and gets nothing while the
// menu is open (input_menu).
#include <string.h>
#include <windows.h>
#include <windowsx.h>

#include "imgui.h"

extern "C" {
#include "cfg.h"
#include "input.h"
#include "input_win.h"
#include "ui/menu.h"
#include "voice.h"
}

// The multiplayer keys of totk.ini (TOTK_MP_CHAT_KEY, TOTK_MP_VOICE_KEY): a letter, enter, capslock or off.
static bool is_key(const char *setting, WPARAM vk) {
    const char *k = cfg_value(setting);
    if (!strcmp(k, "enter")) return vk == VK_RETURN;
    if (!strcmp(k, "capslock")) return vk == VK_CAPITAL;
    return k[0] >= 'a' && k[0] <= 'z' && !k[1] && vk == (WPARAM)(k[0] - 'a' + 'A');
}
static bool g_skip_char;   // the chat key's own character, which TranslateMessage sends after the box opened

static ImGuiKey key_of(WPARAM vk) {
    switch (vk) {
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_TAB: return ImGuiKey_Tab;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_DELETE: return ImGuiKey_Delete;   // clears a cell of the remapping rows
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_SHIFT: return ImGuiKey_LeftShift;
    case VK_CONTROL: return ImGuiKey_LeftCtrl;
    case VK_MENU: return ImGuiKey_LeftAlt;   // with Ctrl: AltGr, which types characters
    case 'Q': return ImGuiKey_Q;   // previous tab
    case 'E': return ImGuiKey_E;   // next tab
    // the text fields' Ctrl shortcuts: select all, copy, paste, cut, undo
    case 'A': return ImGuiKey_A;
    case 'C': return ImGuiKey_C;
    case 'V': return ImGuiKey_V;
    case 'X': return ImGuiKey_X;
    case 'Z': return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

extern "C" int menu_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) {
    (void)h;
    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // bit 30: auto-repeat
        // Esc with the menu open (opened by F1 too) is not a toggle here: it goes to the menu, which closes itself
        if (!(l & (1 << 30)) && (w == VK_F1 || (w == VK_ESCAPE && !menu_is_open()))) {
            menu_toggle();
            *res = 0;
            return 1;
        }
        if (w == VK_F10) {   // also keeps F10 from opening the window menu
            if (!(l & (1 << 30))) menu_perf_cycle();
            *res = 0;
            return 1;
        }
        if (!(l & (1 << 30)) && !menu_is_open() && is_key("TOTK_MP_CHAT_KEY", w) && menu_chat_open()) {
            g_skip_char = true;
            *res = 0;
            return 1;
        }
        [[fallthrough]];
    case WM_KEYUP:
    case WM_SYSKEYUP:
        // push to talk: down only while playing (not typing), up always; the key goes on to the game as well
        if (is_key("TOTK_MP_VOICE_KEY", w) && (!menu_is_open() || m == WM_KEYUP || m == WM_SYSKEYUP))
            voice_ptt(m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
        if (!menu_is_open()) return 0;
        if (ImGuiKey k = key_of(w)) menu_event_key(k, m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
        return 0;   // F11 and the system keys keep working
    case WM_CHAR:   // the typed text, for the menu's text fields (the window loop translates key messages)
        if (g_skip_char) {
            g_skip_char = false;
            return 0;
        }
        if (menu_is_open()) menu_event_char16((unsigned)w);
        return 0;
    case WM_MOUSEMOVE:
        if (menu_is_open()) menu_event_mouse_pos((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        if (!menu_is_open()) return 0;
        int b = m == WM_LBUTTONDOWN || m == WM_LBUTTONUP ? 0 : m == WM_RBUTTONDOWN || m == WM_RBUTTONUP ? 1 : 2;
        int down = m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN;
        menu_event_mouse_pos((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        menu_event_mouse_button(b, down);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (menu_is_open()) menu_event_wheel(0, (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (m == WM_KILLFOCUS) voice_ptt(0);   // a key released in another window never comes here
        menu_event_focus(m == WM_SETFOCUS);
        return 0;
    }
    return 0;
}
