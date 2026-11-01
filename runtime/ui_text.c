// Catalog of the text the host shows the player (ui_text.h). One array per catalog language, indexed by UiTextId: a new
// string gets an id in ui_text.h and an entry in every language that has one (a missing entry falls back to English).
// The software keyboard has every language; the menu has English, Russian and Brazilian Portuguese. "pt" is the
// Switch's European Portuguese and "pt-BR" the Brazilian one.
#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui_text.h"

typedef struct { const char *lang; const char *const *text; } Row;

// The language names are the same in every row.
#define LANG_NAMES \
    [UI_MENU_LANGNAME_EN] = "English", [UI_MENU_LANGNAME_JA] = "日本語", [UI_MENU_LANGNAME_FR] = "Français", \
    [UI_MENU_LANGNAME_DE] = "Deutsch", [UI_MENU_LANGNAME_IT] = "Italiano", [UI_MENU_LANGNAME_ES] = "Español", \
    [UI_MENU_LANGNAME_NL] = "Nederlands", [UI_MENU_LANGNAME_RU] = "Русский", \
    [UI_MENU_LANGNAME_ZH_HANS] = "简体中文", [UI_MENU_LANGNAME_ZH_HANT] = "繁體中文", [UI_MENU_LANGNAME_KO] = "한국어", \
    [UI_MENU_LANGNAME_PT_BR] = "Português (Brasil)"

// SWKBD: TITLE, PROMPT, DEFAULT_NAME (at most 9 UTF-16 units, no '@' '%' '\'), OK, CANCEL, PAD_HINT.
#define SWKBD(title, prompt, name, ok, cancel, hint) \
    [UI_SWKBD_TITLE] = title, [UI_SWKBD_PROMPT] = prompt, [UI_SWKBD_DEFAULT_NAME] = name, [UI_SWKBD_OK] = ok, \
    [UI_SWKBD_CANCEL] = cancel, [UI_SWKBD_PAD_HINT] = hint, LANG_NAMES

static const char *const text_en[UI_TEXT_COUNT] = {
    SWKBD("Text entry", "Enter a name", "Horse", "OK", "Cancel", "Controller: A = OK, B = Cancel"),

    [UI_MENU_TITLE] = "Settings",
    [UI_MENU_TAB_SCREEN] = "Screen", [UI_MENU_TAB_GAME] = "Game", [UI_MENU_TAB_SOUND] = "Sound",
    [UI_MENU_TAB_CONTROLS] = "Controls", [UI_MENU_TAB_AMIIBO] = "Amiibo", [UI_MENU_TAB_MENU] = "Menu", [UI_MENU_TAB_ADVANCED] = "Advanced",
    [UI_MENU_CONTINUE] = "Continue", [UI_MENU_QUIT] = "Quit game",
    [UI_MENU_ON] = "On", [UI_MENU_OFF] = "Off", [UI_MENU_AUTO] = "Automatic", [UI_MENU_DEFAULT_MARK] = "default",
    [UI_MENU_AFTER_RESTART] = "after restart", [UI_MENU_SET_BY_ENV] = "set by environment variable",
    [UI_MENU_SAVE_FAILED] = "could not save settings",
    [UI_MENU_VERSION] = "TotK Recomp %s, game %s",
    [UI_MENU_HINT] = "B or Esc closes  ·  L / R or Q / E change tab  ·  F10 overlay  ·  F11 fullscreen",

    [UI_MENU_FULLSCREEN] = "Fullscreen", [UI_MENU_FULLSCREEN_DESC] = "Fills the screen with the game; F11 toggles it.",
    [UI_MENU_BORDERLESS] = "Borderless window",
    [UI_MENU_BORDERLESS_DESC] = "The window without its title bar and edges, centred on the screen, at the window size. Outside fullscreen only.",
    [UI_MENU_WINDOW_SIZE] = "Window size", [UI_MENU_WINDOW_SIZE_DESC] = "Size of the game window when not fullscreen.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Custom",
    [UI_MENU_PRESENT] = "Presentation", [UI_MENU_PRESENT_DESC] = "How finished frames reach the screen.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Immediate",
    [UI_MENU_PRESENT_IN_USE] = "in use: %s",
    [UI_MENU_GPU] = "Graphics card", [UI_MENU_GPU_DESC] = "The Vulkan device that draws the game.",
    [UI_MENU_GPU_AUTO] = "Automatic",
    [UI_MENU_SHADER_TOAST] = "Shader notice",
    [UI_MENU_SHADER_TOAST_DESC] = "A small notice in the bottom right corner while shaders and pipelines are being "
                                  "compiled, which is when the game can stutter for a moment.",
    [UI_MENU_TOAST_SHADERS] = "Compiling shaders (%u)",
    [UI_MENU_AMIIBO_DIR] = "Amiibo folder:",
    [UI_MENU_AMIIBO_SET_DIR] = "Use this folder",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Default (./amiibo)",
    [UI_MENU_AMIIBO_REFRESH] = "Refresh",
    [UI_MENU_AMIIBO_PICK] = "Pick an Amiibo file (.bin / .nfc):",
    [UI_MENU_AMIIBO_FOLDER] = "[folder]",
    [UI_MENU_AMIIBO_ACTIONS] = "NFC reader",
    [UI_MENU_AMIIBO_READER] = "Reader:",
    [UI_MENU_AMIIBO_IDLE] = "Ready",
    [UI_MENU_AMIIBO_SEARCHING] = "The game is looking for a figure...",
    [UI_MENU_AMIIBO_FOUND] = "Figure detected",
    [UI_MENU_AMIIBO_MOUNTED] = "Figure being read",
    [UI_MENU_AMIIBO_DISABLED] = "NFC reader off",
    [UI_MENU_AMIIBO_SELECTED] = "Selected file:",
    [UI_MENU_AMIIBO_SCAN] = "Scan Amiibo",
    [UI_MENU_AMIIBO_NONE_SELECTED] = "(no file selected)",
    [UI_MENU_AMIIBO_REMOVE] = "Take the figure away",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Take the figure away by itself after a scan",
    [UI_MENU_AMIIBO_HOLD] = "Time on the reader",
    [UI_MENU_AMIIBO_INFO] = "Figure on the reader:",
    [UI_MENU_AMIIBO_NO_TAG] = "No figure on the reader.",
    [UI_MENU_AMIIBO_STATUS] = "Status: %s",
    [UI_MENU_AMIIBO_READY] = "Ready",
    [UI_MENU_AMIIBO_CANNOT_OPEN] = "Cannot open the folder %s",
    [UI_MENU_AMIIBO_TAPPED] = "Read %s",
    [UI_MENU_AMIIBO_LOAD_ERROR] = "Could not read %s",
    [UI_MENU_AMIIBO_REMOVED] = "Figure taken away",
    [UI_MENU_ACH_POPUP] = "Achievement notices",
    [UI_MENU_ACH_POPUP_DESC] = "A notice at the top of the screen when an achievement unlocks. Achievements are checked "
                               "each time the game saves, so the notice comes with the next save after the feat.",
    [UI_MENU_ACH_SOUND] = "Achievement sound",
    [UI_MENU_ACH_SOUND_DESC] = "A short chime with the achievement notice, at the game volume; the platinum trophy has its "
                               "own.",
    [UI_MENU_TAB_ACHIEVEMENTS] = "Achievements", [UI_MENU_ACH_PROGRESS] = "%d of %d unlocked",
    [UI_MENU_ACH_NONE] = "No achievements are installed. Run the setup again, or save them in the achievements editor.",
    [UI_MENU_ACH_SECRET] = "Secret achievement", [UI_MENU_ACH_SECRET_DESC] = "Keep playing to find out.",
    [UI_MENU_ACH_UNLOCKED_ON] = "Unlocked %s", [UI_MENU_ACH_COMMON] = "Common", [UI_MENU_ACH_RARE] = "Rare",
    [UI_MENU_ACH_EPIC] = "Epic", [UI_MENU_ACH_LEGENDARY] = "Legendary", [UI_MENU_ACH_PLATINUM] = "Platinum",
    [UI_MENU_ACH_NOTICE] = "Achievement unlocked", [UI_MENU_ACH_NOTICE_PLATINUM] = "Platinum trophy!",
    [UI_MENU_ACH_NOTICE_MANY] = "%d achievements unlocked",
    [UI_MENU_ACH_NOTICE_MANY_SUB] = "See them in the menu, on the Achievements tab",
    [UI_MENU_ACH_BLOCKED] = "Done in a save from elsewhere",
    [UI_MENU_ACH_BLOCKED_NOTE] = "%d achievements were already done in a save this game did not write here (copied in, "
                                 "or a backup put back). They count once you earn them by playing, in a new game.",
    [UI_MENU_PERF_OVERLAY] = "Performance overlay",
    [UI_MENU_PERF_OVERLAY_DESC] = "Frame rate in the top right corner. Full adds frame times with a graph, CPU, GPU and "
                                  "memory load, temperatures and the render thread's waits. F10 cycles the modes.",
    [UI_MENU_PERF_OFF] = "Off", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Full",
    // printf format of the overlay line in FPS mode: the frame rate
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_MENU_GRAPHICS] = "Picture",
    [UI_MENU_OUTPUT_RES] = "Game resolution",
    [UI_MENU_OUTPUT_RES_DESC] = "The size the game draws its picture and interface in. Above 1920 x 1080 the game needs "
                                "more video memory and GPU time. A wider size (21:9) widens the camera's view and moves "
                                "the HUD to the screen's corners; a taller one (16:10, Steam Deck) moves it to the top "
                                "and bottom edges.",
    [UI_MENU_RENDER_SCALE] = "3D render scale",
    [UI_MENU_RENDER_SCALE_DESC] = "Size of the 3D scene relative to the game resolution; the game's FSR scales it to "
                                  "that size. Above 100 % smooths jagged edges (supersampling) at a high GPU cost.",
    [UI_MENU_RENDER_SCALE_GAME] = "Game (83 %)",
    [UI_MENU_UPSCALER] = "Scaling to the window",
    [UI_MENU_UPSCALER_DESC] = "How the picture is scaled when the window is larger than the game resolution. FSR keeps "
                              "edges and detail sharper than bilinear.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_ASPECT] = "Picture shape",
    [UI_MENU_ASPECT_DESC] = "When the window's shape differs from the game resolution's (a 16:9 picture on a 21:9 "
                            "screen): keep the picture's shape with black bars, or stretch it over the window.",
    [UI_MENU_ASPECT_FIT] = "Black bars", [UI_MENU_ASPECT_STRETCH] = "Stretch",
    [UI_MENU_FOV] = "Field of view",
    [UI_MENU_FOV_DESC] = "How much of the world the camera shows, in percent of the game's own. Game version 1.4.3 only.",
    [UI_MENU_ANISO] = "Anisotropic filtering",
    [UI_MENU_ANISO_DESC] = "Keeps ground and walls seen at a steep angle sharp. The game's own level is the Switch's; "
                           "16x costs little on a desktop GPU.",
    [UI_MENU_ANISO_GAME] = "The game's",
    [UI_MENU_SHADOW_RES] = "Shadow map size",
    [UI_MENU_SHADOW_RES_DESC] = "Size of the sun's shadow map. The game's own size changes with the view, down to about 400 "
                                "near the ground, so leaf shadows look blocky and jump; a fixed size keeps them still and "
                                "sharper. Above 1024 the shadows cost more GPU time and video memory.",
    [UI_MENU_SHADOW_RES_GAME] = "Game (changes)",
    [UI_MENU_SHARPNESS] = "FSR sharpness",
    [UI_MENU_SHARPNESS_DESC] = "Sharpening after FSR scaling; 0 turns it off.",
    [UI_MENU_COLOR] = "Colour",
    [UI_MENU_COLOR_PRESET] = "Colour profile",
    [UI_MENU_COLOR_PRESET_DESC] = "Sets the four values below. Vivid: more contrast and saturation, shadows opened a "
                                  "little (made for OLED screens). Clarity: a little more contrast, saturation and "
                                  "brightness. Moving a value makes the profile Custom. The HUD changes with the picture; "
                                  "this menu does not.",
    [UI_MENU_COLOR_DEFAULT] = "Game's own", [UI_MENU_COLOR_VIVID] = "Vivid (OLED)", [UI_MENU_COLOR_CLARITY] = "Clarity",
    [UI_MENU_COLOR_CUSTOM] = "Custom",
    [UI_MENU_COLOR_BRIGHTNESS] = "Brightness", [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Added to every colour; 0 leaves it.",
    [UI_MENU_COLOR_CONTRAST] = "Contrast",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Spreads colours away from mid-grey (above 100 %) or towards it; 100 % leaves them.",
    [UI_MENU_COLOR_SATURATION] = "Saturation",
    [UI_MENU_COLOR_SATURATION_DESC] = "Colour strength against grey; 100 % leaves it.",
    [UI_MENU_COLOR_GAMMA] = "Gamma",
    [UI_MENU_COLOR_GAMMA_DESC] = "Below 1 brightens dark and middle tones, above 1 darkens them; 1 leaves them.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "HDR output",
    [UI_MENU_HDR_DESC] = "Bright highlights on an HDR display. Needs HDR turned on in Windows (\"Use HDR\") or in a "
                         "Wayland compositor with colour management; without it the picture stays SDR. Applies after a "
                         "restart. The colour profile above is for SDR and is not used in HDR.",
    [UI_MENU_HDR_PEAK] = "Peak brightness",
    [UI_MENU_HDR_PEAK_DESC] = "Your display's peak in nits; the brightest highlights stop there.",
    [UI_MENU_HDR_GAME] = "Game brightness",
    [UI_MENU_HDR_GAME_DESC] = "Nits of the picture's white (paper white). 203 is the HDR reference; raise it in a "
                              "bright room.",
    [UI_MENU_HDR_UI] = "HUD and menu brightness",
    [UI_MENU_HDR_UI_DESC] = "Nits of the HUD's and this menu's white.",

    [UI_MENU_FPS_LIMIT] = "Frame rate limit",
    [UI_MENU_FPS_LIMIT_DESC] = "The most frames per second the game draws. Above 60 is experimental and needs Game speed "
                               "follows the clock.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    // the monitor's refresh rate; printf format with the rate in Hz when it is known
    [UI_MENU_FPS_DISPLAY] = "Monitor rate", [UI_MENU_FPS_DISPLAY_HZ] = "Monitor rate (%.0f Hz)",
    [UI_MENU_REALTIME] = "Game speed follows the clock",
    [UI_MENU_REALTIME_DESC] = "Steps the game by real time, so a slow frame does not slow the game down.",
    [UI_MENU_CLOTH_REALTIME] = "Cloth on the clock",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Steps cloth and hair by real time as well.",
    [UI_MENU_HIGHFPS_FIX] = "High frame rate fixes",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrects physics that misbehave above 30 frames per second, and objects that stay see-through or invisible above 60.",
    [UI_MENU_GAME_LANG] = "Game language", [UI_MENU_GAME_LANG_DESC] = "Language of the game's text and voices.",
    [UI_MENU_GAME_LANG_AUTO] = "Automatic",
    [UI_MENU_OPERATION_MODE] = "Console mode",
    [UI_MENU_OPERATION_MODE_DESC] = "Whether the game thinks it runs docked or handheld.",
    [UI_MENU_MODE_DOCKED] = "Docked", [UI_MENU_MODE_HANDHELD] = "Handheld",
    [UI_MENU_ASSUME_FOCUS] = "Keep running out of focus",
    [UI_MENU_ASSUME_FOCUS_DESC] = "On (the default), the game keeps running while you are in another window, and the "
                                  "keyboard stays quiet until the window is in front again. Off, it pauses when its "
                                  "window loses focus, as on the Switch when Home is pressed; a load interrupted that "
                                  "way can stop for good.",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Master volume of the game.",

    [UI_MENU_MOTION] = "Motion controls", [UI_MENU_MOTION_DESC] = "Aiming with the controller's gyroscope.",
    [UI_MENU_RUMBLE_STRENGTH] = "Vibration strength",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "How strong the controller vibrates.",
    [UI_MENU_RUMBLE_CURVE] = "Vibration curve",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Lower values make weak vibration stronger.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Trigger vibration (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Vibrates the triggers of an Xbox controller too.",
    [UI_MENU_DS_TRIGGERS] = "Adaptive triggers (DualSense)",
    [UI_MENU_DS_TRIGGERS_DESC] = "The triggers of a PS5 controller resist from where the game takes the press: ZR, the "
                                 "bow, firmly, ZL lightly.",
    [UI_MENU_DS_LIGHT] = "Light bar (DualSense)",
    [UI_MENU_DS_LIGHT_DESC] = "Lights the PS5 controller's light bar in the menu's teal; off, it keeps the usual blue.",
    [UI_MENU_PAD_LAYOUT] = "Button layout",
    [UI_MENU_PAD_LAYOUT_DESC] = "Which button is A. Nintendo: the right one, as on a Switch. Xbox: the bottom one, as "
                                "labelled on an Xbox controller (cross on a PlayStation controller). Automatic picks "
                                "Nintendo for controllers with Nintendo labels and Xbox for the others. Xbox prompts: "
                                "A is the right one as with Nintendo, and the game shows the Xbox labels (B for A, A "
                                "for B, Y for X, X for Y). PlayStation: A is cross as with Xbox, and the game shows the "
                                "PlayStation symbols. Prompts change after a restart. The letters are part of the "
                                 "game's files, so a button you remap in Controls keeps the old letter on screen.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatic", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A right)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A bottom)", [UI_MENU_PAD_LAYOUT_XBOX_PROMPTS] = "Xbox prompts (A right)",
    [UI_MENU_PAD_LAYOUT_PLAYSTATION] = "PlayStation (cross confirms)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "in use: %s",
    [UI_MENU_PAD_LAYOUT_PROMPTS_MISSING] = "prompt files missing: run the setup again",
    [UI_MENU_PAD_LAYOUT_PROMPTS_RESTART] = "prompts change after restart",
    [UI_MENU_STICK_DEADZONE] = "Stick dead zone",
    [UI_MENU_STICK_DEADZONE_DESC] = "How far a gamepad stick moves before the game sees it. Raise it if the character or the camera moves on its own.",
    [UI_MENU_MOUSE_SENS] = "Mouse camera sensitivity",
    [UI_MENU_MOUSE_SENS_DESC] = "How fast the mouse turns the camera.",
    [UI_MENU_MOUSE_SMOOTH] = "Mouse camera smoothing",
    [UI_MENU_MOUSE_SMOOTH_DESC] = "Eases the camera's turn over a few milliseconds so an uneven mouse or frame rate turns it "
                                  "evenly. Higher is smoother but follows the mouse a little later; 0 turns it off.",
    [UI_MENU_STICK_SENS] = "Stick camera speed",
    [UI_MENU_STICK_SENS_DESC] = "Multiplies the game's own right-stick camera speed, top speed included, at each of the "
                                "game's five camera sensitivities. The mouse keeps its own setting.",
    [UI_MENU_WALK_KEY] = "Walk key",
    [UI_MENU_WALK_KEY_DESC] = "Hold it to tilt the keyboard's sticks part of the way, so the keyboard walks instead of "
                              "runs. The walk scale sets how far. A controller and the mouse are not affected.",
    [UI_MENU_WALK_KEY_TAKEN] = "also %s",
    [UI_MENU_WALK_SCALE] = "Walk scale",
    [UI_MENU_WALK_SCALE_DESC] = "The tilt the keyboard's sticks get while the walk key is held. Half is a walk, the "
                                "full tilt a run.",

    [UI_BINDS_HELP] = "Select a cell and press A or Enter, then the key, mouse button or controller button. Delete or "
                      "X clears a cell. A key in use moves here; a controller button swaps with the one it replaces.",
    [UI_BINDS_COL_KEY1] = "Key 1", [UI_BINDS_COL_KEY2] = "Key 2", [UI_BINDS_COL_PAD] = "Controller",
    [UI_BINDS_GROUP_BUTTONS] = "Buttons", [UI_BINDS_GROUP_MOVE] = "Movement", [UI_BINDS_GROUP_CAMERA] = "Camera",
    [UI_BINDS_PRESS_KEY] = "Press a key or mouse button for %s",
    [UI_BINDS_PRESS_PAD] = "Press a controller button for %s",
    [UI_BINDS_CANCEL_HINT] = "Esc cancels, or wait %d s",
    [UI_BINDS_TAKEN] = "%s was taken from %s", [UI_BINDS_SWAPPED] = "%s swapped with %s",
    [UI_BINDS_RESET] = "Reset all buttons",
    [UI_BINDS_RESET_DESC] = "Back to the default keys and controller buttons. The button layout above applies again: remapped controller buttons stay where you put them until this reset.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Left stick click", [UI_BIND_RCLICK] = "Right stick click", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Plus", [UI_BIND_MINUS] = "Minus",
    [UI_BIND_LEFT] = "D-pad left", [UI_BIND_UP] = "D-pad up", [UI_BIND_RIGHT] = "D-pad right",
    [UI_BIND_DOWN] = "D-pad down",
    [UI_BIND_LSTICK_LEFT] = "Move left", [UI_BIND_LSTICK_UP] = "Move forward", [UI_BIND_LSTICK_RIGHT] = "Move right",
    [UI_BIND_LSTICK_DOWN] = "Move back",
    [UI_BIND_RSTICK_LEFT] = "Camera left", [UI_BIND_RSTICK_UP] = "Camera up", [UI_BIND_RSTICK_RIGHT] = "Camera right",
    [UI_BIND_RSTICK_DOWN] = "Camera down",
    [UI_BIND_DESC_A] = "Interact, pick up, talk.", [UI_BIND_DESC_B] = "Run, cancel, go back.",
    [UI_BIND_DESC_X] = "Jump.", [UI_BIND_DESC_Y] = "Attack.", [UI_BIND_DESC_LCLICK] = "Crouch.",
    [UI_BIND_DESC_RCLICK] = "First-person view.", [UI_BIND_DESC_L] = "Abilities.",
    [UI_BIND_DESC_R] = "Throw a weapon.", [UI_BIND_DESC_ZL] = "Shield, lock on.", [UI_BIND_DESC_ZR] = "Bow.",
    [UI_BIND_DESC_PLUS] = "Menu, inventory.", [UI_BIND_DESC_MINUS] = "Map.",
    [UI_BIND_DESC_DPAD] = "Quick-select.",
    [UI_BIND_DESC_MOVE] = "Moves Link like the left stick. The controller's left stick always works too.",
    [UI_BIND_DESC_CAMERA] = "Turns the camera like the right stick. The mouse and the controller's right stick always "
                            "work too.",
    [UI_KEY_SPACE] = "Space", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Left Shift", [UI_KEY_LCTRL] = "Left Ctrl",
    [UI_KEY_UP] = "Up arrow", [UI_KEY_DOWN] = "Down arrow", [UI_KEY_LEFT] = "Left arrow",
    [UI_KEY_RIGHT] = "Right arrow",
    [UI_KEY_MOUSE_L] = "Left mouse", [UI_KEY_MOUSE_R] = "Right mouse", [UI_KEY_MOUSE_M] = "Middle mouse",
    [UI_KEY_MOUSE_X1] = "Mouse 4", [UI_KEY_MOUSE_X2] = "Mouse 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Right Shift",
    [UI_KEY_RCTRL] = "Right Ctrl", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Bottom button", [UI_PAD_EAST] = "Right button", [UI_PAD_WEST] = "Left button",
    [UI_PAD_NORTH] = "Top button", [UI_PAD_LSTICK] = "Left stick", [UI_PAD_RSTICK] = "Right stick",
    [UI_PAD_L] = "Left bumper", [UI_PAD_R] = "Right bumper", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "D-pad left", [UI_PAD_DPAD_UP] = "D-pad up", [UI_PAD_DPAD_RIGHT] = "D-pad right",
    [UI_PAD_DPAD_DOWN] = "D-pad down", [UI_PAD_ZL] = "Left trigger", [UI_PAD_ZR] = "Right trigger",
    [UI_PAD_LPADDLE1] = "Left back button", [UI_PAD_RPADDLE1] = "Right back button",
    [UI_PAD_LPADDLE2] = "Left back button 2", [UI_PAD_RPADDLE2] = "Right back button 2",
    [UI_PAD_CROSS] = "Cross", [UI_PAD_CIRCLE] = "Circle", [UI_PAD_SQUARE] = "Square", [UI_PAD_TRIANGLE] = "Triangle",

    [UI_MENU_MENU_LANG] = "Menu language", [UI_MENU_MENU_LANG_DESC] = "Language of this menu and the overlay.",
    [UI_MENU_MENU_LANG_AUTO] = "Automatic (from Windows)",

    [UI_MENU_ADV_WARNING] = "These settings are for testing and troubleshooting. Change them only when you know why.",
    [UI_MENU_GROUP_SCHED] = "Scheduling", [UI_MENU_GROUP_SHADERS] = "Shaders and pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Textures and memory", [UI_MENU_GROUP_FIXES] = "Render fixes",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Diagnostics",
    [UI_MENU_ASYNC_PREEMPT] = "Virtual core preemption",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Lets a game thread interrupt another one on the same virtual core.",
    [UI_MENU_PREEMPT_OFF] = "Off", [UI_MENU_PREEMPT_ON] = "On", [UI_MENU_PREEMPT_INHERIT] = "Inheritance only",
    [UI_MENU_REPLAY_ASYNC] = "GPU frame prep on its own thread",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prepares frames for the GPU on a separate thread instead of the game thread. Adds up to one frame of latency.",
    [UI_MENU_TIMER_1MS] = "1 ms Windows timer",
    [UI_MENU_TIMER_1MS_DESC] = "Raises the system timer resolution to 1 ms for steadier frame pacing.",
    [UI_MENU_PIPE_WAIT] = "Waiting draw handling",
    [UI_MENU_PIPE_WAIT_DESC] = "Default keeps the current scheduling with a reserved worker for waiting draws; Old (draw) uses the previous scheduling.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Default", [UI_MENU_PIPE_WAIT_DRAW] = "Old (draw)",
    [UI_MENU_PIPE_SKIP] = "Draws skipped while a pipeline builds",
    [UI_MENU_PIPE_SKIP_DESC] = "Which draws are dropped instead of waiting for their pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Strict", [UI_MENU_PIPE_SKIP_ANY] = "Any",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Fullscreen passes",
    [UI_MENU_LIB_PREBUILD] = "Stage library prebuild",
    [UI_MENU_LIB_PREBUILD_DESC] = "Builds shader stage libraries ahead of time in the background.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Stage library prebuild threads",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads used for the prebuild; Automatic picks a number from your CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Pipeline prebuild",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Builds known pipelines ahead of time.",
    [UI_MENU_PREWARM_XLATE] = "Translator prewarm",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads that translate known shaders at start; Automatic takes half the CPU threads, at most 16.",
    [UI_MENU_GPL] = "Graphics pipeline library",
    [UI_MENU_GPL_DESC] = "Uses the Vulkan pipeline library extension; turn off if your driver misbehaves with it.",
    [UI_MENU_ASTC] = "ASTC texture decoding", [UI_MENU_ASTC_DESC] = "Where compressed ASTC textures are decoded.",
    [UI_MENU_ASTC_AUTO] = "Automatic", [UI_MENU_ASTC_GPU] = "GPU shader", [UI_MENU_ASTC_NATIVE] = "Native",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "GPU memory mode", [UI_MENU_MEMORY_DESC] = "How game memory is shared with the GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automatic", [UI_MENU_MEMORY_IMPORT] = "Import", [UI_MENU_MEMORY_MIRROR] = "Mirror",
    [UI_MENU_MEMORY_VERIFY] = "Verify",
    [UI_MENU_TEX_ALIAS] = "Texture aliasing", [UI_MENU_TEX_ALIAS_DESC] = "A new texture created over a live texture's memory starts with that content instead of empty.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Texture memory limit", [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Percent of video memory above which textures the game released are dropped (they come back from storage when used again). 0 keeps them.",
    [UI_MENU_ASTC_BC] = "Compress ASTC to BC3", [UI_MENU_ASTC_BC_DESC] = "With the GPU decoder: re-encodes decoded ASTC textures to BC3, a quarter of the video memory. For GPUs with 4 GB or less; textures look slightly blockier.",
    [UI_MENU_LOD_GUARD] = "LOD guard", [UI_MENU_LOD_GUARD_DESC] = "Prevents objects flashing in the wrong place or a flat far field for a frame, by waiting for the game to finish rewriting level-of-detail data before the GPU reads it.",
    [UI_MENU_FIX_C0] = "Constant buffer fix",
    [UI_MENU_FIX_C0_DESC] = "Fills the driver-reserved constant bank c0 (storage sizes, dispatch size, point size, base instance) instead of reading zeros.",
    [UI_MENU_FIX_ZERO] = "Zero new GPU memory",
    [UI_MENU_FIX_ZERO_DESC] = "New GPU memory pools start zero-filled, as Switch memory does.",
    [UI_MENU_BIND_SKIP] = "Skip repeated binds",
    [UI_MENU_BIND_SKIP_DESC] = "Optimisation: skips bindings that did not change.",
    [UI_MENU_SHADER_MEMO] = "Shader memo", [UI_MENU_SHADER_MEMO_DESC] = "Optimisation: remembers shader lookups.",
    [UI_MENU_PIPE_MEMO] = "Pipeline memo", [UI_MENU_PIPE_MEMO_DESC] = "Optimisation: reuses the previous draw's pipeline when its state repeats.",
    [UI_MENU_TEX_MEMO] = "Texture binding memo", [UI_MENU_TEX_MEMO_DESC] = "Optimisation: remembers texture descriptors between draws.",
    [UI_MENU_DESC_TEMPLATE] = "Descriptor templates",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Optimisation: updates descriptors in bulk.",
    [UI_MENU_PUSH_SKIP] = "Skip repeated push constants",
    [UI_MENU_PUSH_SKIP_DESC] = "Optimisation: skips push constants that did not change.",
    [UI_MENU_SPLIT_DRAWS] = "Early GPU submission",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Submits GPU work in parts every N draws between passes so the GPU starts earlier; 0 turns it off.",
    [UI_MENU_VK_VALIDATION] = "Validation layer",
    [UI_MENU_VK_VALIDATION_DESC] = "Checks every Vulkan call and slows the game down a lot.",
    [UI_MENU_VK_VALIDATION_MISSING] = "The validation layer is not installed.",
    [UI_MENU_VK_DYN_VERTEX] = "Dynamic vertex state",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Forces dynamic vertex input on or off; the default depends on the GPU.",
    [UI_MENU_VK_DYN_A2C] = "Dynamic alpha to coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "Forces dynamic alpha to coverage on or off. Default: off on AMD's driver for RDNA 2 and "
                                "older, which draws characters black with it, on elsewhere.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Pin subgroup size", [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixes the shader subgroup size.",
    [UI_MENU_TRACE] = "Performance log", [UI_MENU_TRACE_DESC] = "Writes frame timings to a file.",
    [UI_MENU_FLIGHT] = "Flight recorder",
    [UI_MENU_FLIGHT_DESC] = "Keeps a short history and saves it when a frame freezes.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Freeze threshold (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Frame time that counts as a freeze.",
    [UI_MENU_INPUT_RECORD] = "Input recording",
    [UI_MENU_INPUT_RECORD_DESC] = "Writes your inputs to a file for bug reports.",
    [UI_MENU_OPEN_LOG] = "Open log folder", [UI_MENU_OPEN_LOG_DESC] = "Shows the folder with the log files.",
    [UI_MENU_COPY_INFO] = "Copy system info",
    [UI_MENU_COPY_INFO_DESC] = "Copies GPU, driver, CPU, RAM and changed settings to the clipboard.",
    [UI_MENU_COPY_INFO_DONE] = "Copied", [UI_MENU_ACTION_FAILED] = "did not work",
    [UI_MENU_RESET] = "Reset all settings to defaults", [UI_MENU_RESET_DESC] = "Applies to every tab.",
    [UI_MENU_RESET_CONFIRM] = "Reset every setting to its default?", [UI_MENU_RESET_YES] = "Reset",
    [UI_MENU_RESET_NO] = "Cancel",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "game %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max %.1f ms",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "RAM", [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Temperature", [UI_MENU_OV_POWER] = "Power", [UI_MENU_OV_CLOCK] = "Clock",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU frame prep", [UI_MENU_OV_GPU_TIME] = "GPU busy per frame",
    [UI_MENU_OV_PIPE_WAIT] = "Pipeline waits",
    [UI_MENU_OV_SHADER_WAIT] = "Shader waits", [UI_MENU_OV_TEX_UPLOADS] = "Texture uploads",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f GB", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "game %.1f GB", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "%.1f per frame",
    [UI_MENU_OV_WINDOW] = "Window", [UI_MENU_OV_OUTPUT] = "Game image", [UI_MENU_OV_SCENE] = "3D scene",
    [UI_MENU_OV_SCALING] = "Scaling", [UI_MENU_OV_LIMIT] = "Frame limit", [UI_MENU_OV_PRESENT] = "Presentation",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, sharpness %d%%", [UI_MENU_OV_SCALE_NONE] = "none (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinear",
};

static const char *const text_ru[UI_TEXT_COUNT] = {
    SWKBD("Ввод текста", "Введите имя", "Лошадь", "ОК", "Отмена", "Контроллер: A — ОК, B — отмена"),

    [UI_MENU_TITLE] = "Настройки",
    [UI_MENU_TAB_SCREEN] = "Экран", [UI_MENU_TAB_GAME] = "Игра", [UI_MENU_TAB_SOUND] = "Звук",
    [UI_MENU_TAB_CONTROLS] = "Управление", [UI_MENU_TAB_AMIIBO] = "Amiibo", [UI_MENU_TAB_MENU] = "Меню", [UI_MENU_TAB_ADVANCED] = "Дополнительно",
    [UI_MENU_CONTINUE] = "Продолжить", [UI_MENU_QUIT] = "Выйти из игры",
    [UI_MENU_ON] = "Вкл.", [UI_MENU_OFF] = "Выкл.", [UI_MENU_AUTO] = "Автоматически",
    [UI_MENU_DEFAULT_MARK] = "по умолчанию",
    [UI_MENU_AFTER_RESTART] = "после перезапуска", [UI_MENU_SET_BY_ENV] = "задано переменной окружения",
    [UI_MENU_SAVE_FAILED] = "не удалось сохранить настройки",
    [UI_MENU_VERSION] = "TotK Recomp %s, игра %s",
    [UI_MENU_HINT] = "B или Esc — закрыть  ·  L / R или Q / E — вкладки  ·  F10 — индикатор  ·  F11 — полный экран",

    [UI_MENU_FULLSCREEN] = "Полный экран", [UI_MENU_FULLSCREEN_DESC] = "Игра на весь экран; F11 переключает режим.",
    [UI_MENU_BORDERLESS] = "Окно без рамки",
    [UI_MENU_BORDERLESS_DESC] = "Окно без заголовка и рамки, по центру экрана, в размере окна. Только вне полноэкранного режима.",
    [UI_MENU_WINDOW_SIZE] = "Размер окна", [UI_MENU_WINDOW_SIZE_DESC] = "Размер окна игры вне полноэкранного режима.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Другой",
    [UI_MENU_PRESENT] = "Вывод кадров", [UI_MENU_PRESENT_DESC] = "Как готовые кадры попадают на экран.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "Вертикальная синхронизация",
    [UI_MENU_PRESENT_IMMEDIATE] = "Без ожидания",
    [UI_MENU_PRESENT_IN_USE] = "сейчас: %s",
    [UI_MENU_GPU] = "Видеокарта", [UI_MENU_GPU_DESC] = "Устройство Vulkan, которое рисует игру.",
    [UI_MENU_GPU_AUTO] = "Автоматически",
    [UI_MENU_SHADER_TOAST] = "Уведомление о шейдерах",
    [UI_MENU_SHADER_TOAST_DESC] = "Небольшое уведомление в правом нижнем углу, пока компилируются шейдеры и конвейеры: "
                                  "в это время игра может ненадолго подтормаживать.",
    [UI_MENU_TOAST_SHADERS] = "Компиляция шейдеров (%u)",
    [UI_MENU_AMIIBO_DIR] = "Папка Amiibo:",
    [UI_MENU_AMIIBO_SET_DIR] = "Выбрать папку",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "По умолчанию (./amiibo)",
    [UI_MENU_AMIIBO_REFRESH] = "Обновить",
    [UI_MENU_AMIIBO_PICK] = "Выберите файл Amiibo (.bin / .nfc):",
    [UI_MENU_AMIIBO_FOLDER] = "[папка]",
    [UI_MENU_AMIIBO_ACTIONS] = "Считыватель NFC",
    [UI_MENU_AMIIBO_READER] = "Считыватель:",
    [UI_MENU_AMIIBO_IDLE] = "Готов",
    [UI_MENU_AMIIBO_SEARCHING] = "Игра ищет фигурку...",
    [UI_MENU_AMIIBO_FOUND] = "Фигурка обнаружена",
    [UI_MENU_AMIIBO_MOUNTED] = "Фигурка читается",
    [UI_MENU_AMIIBO_DISABLED] = "Считыватель NFC выключен",
    [UI_MENU_AMIIBO_SELECTED] = "Выбранный файл:",
    [UI_MENU_AMIIBO_SCAN] = "Сканировать Amiibo",
    [UI_MENU_AMIIBO_NONE_SELECTED] = "(файл не выбран)",
    [UI_MENU_AMIIBO_REMOVE] = "Убрать фигурку",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Убирать фигурку само после сканирования",
    [UI_MENU_AMIIBO_HOLD] = "Время на считывателе",
    [UI_MENU_AMIIBO_INFO] = "Фигурка на считывателе:",
    [UI_MENU_AMIIBO_NO_TAG] = "На считывателе нет фигурки.",
    [UI_MENU_AMIIBO_STATUS] = "Состояние: %s",
    [UI_MENU_AMIIBO_READY] = "Готово",
    [UI_MENU_AMIIBO_CANNOT_OPEN] = "Не удаётся открыть папку %s",
    [UI_MENU_AMIIBO_TAPPED] = "Прочитано: %s",
    [UI_MENU_AMIIBO_LOAD_ERROR] = "Не удалось прочитать %s",
    [UI_MENU_AMIIBO_REMOVED] = "Фигурка убрана",
    [UI_MENU_ACH_POPUP] = "Уведомления о достижениях",
    [UI_MENU_ACH_POPUP_DESC] = "Уведомление вверху экрана, когда открывается достижение. Достижения проверяются при "
                               "каждом сохранении игры, поэтому уведомление приходит со следующим сохранением.",
    [UI_MENU_ACH_SOUND] = "Звук достижения",
    [UI_MENU_ACH_SOUND_DESC] = "Короткий звон вместе с уведомлением о достижении, на громкости игры; у платинового "
                               "трофея свой.",
    [UI_MENU_TAB_ACHIEVEMENTS] = "Достижения", [UI_MENU_ACH_PROGRESS] = "Открыто %d из %d",
    [UI_MENU_ACH_NONE] = "Достижения не установлены. Запустите установку снова или сохраните их в редакторе достижений.",
    [UI_MENU_ACH_SECRET] = "Секретное достижение", [UI_MENU_ACH_SECRET_DESC] = "Продолжайте играть, чтобы узнать.",
    [UI_MENU_ACH_UNLOCKED_ON] = "Получено %s", [UI_MENU_ACH_COMMON] = "Обычное", [UI_MENU_ACH_RARE] = "Редкое",
    [UI_MENU_ACH_EPIC] = "Эпическое", [UI_MENU_ACH_LEGENDARY] = "Легендарное", [UI_MENU_ACH_PLATINUM] = "Платиновое",
    [UI_MENU_ACH_NOTICE] = "Достижение получено", [UI_MENU_ACH_NOTICE_PLATINUM] = "Платиновый трофей!",
    [UI_MENU_ACH_NOTICE_MANY] = "Получено достижений: %d",
    [UI_MENU_ACH_NOTICE_MANY_SUB] = "Смотрите в меню, на вкладке «Достижения»",
    [UI_MENU_ACH_BLOCKED] = "Сделано в чужом сохранении",
    [UI_MENU_ACH_BLOCKED_NOTE] = "Достижений, уже выполненных в сохранении, которое записала не эта игра здесь "
                                 "(скопированном или восстановленном из копии): %d. Они засчитаются, когда вы "
                                 "выполните их в игре, в новой игре.",
    [UI_MENU_PERF_OVERLAY] = "Индикатор производительности",
    [UI_MENU_PERF_OVERLAY_DESC] = "Частота кадров в правом верхнем углу. «Полный» добавляет время кадров с графиком, "
                                  "загрузку ЦП, GPU и памяти, температуры и ожидания потока отрисовки. F10 переключает "
                                  "режимы.",
    [UI_MENU_PERF_OFF] = "Выкл.", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Полный",
    [UI_PERF_LINE_FPS] = "%.0f к/с",
    [UI_MENU_GRAPHICS] = "Изображение",
    [UI_MENU_OUTPUT_RES] = "Разрешение игры",
    [UI_MENU_OUTPUT_RES_DESC] = "Размер, в котором игра рисует картинку и интерфейс. Выше 1920 × 1080 игре нужно больше "
                                "видеопамяти и времени GPU. Более широкий размер (21:9) расширяет обзор камеры и "
                                "переносит интерфейс в углы экрана, более высокий (16:10, Steam Deck) переносит его к "
                                "верхнему и нижнему краям.",
    [UI_MENU_RENDER_SCALE] = "Масштаб 3D-отрисовки",
    [UI_MENU_RENDER_SCALE_DESC] = "Размер 3D-сцены относительно разрешения игры; FSR игры доводит её до этого размера. "
                                  "Выше 100 % сглаживает лесенки на краях (суперсэмплинг) ценой большой нагрузки на GPU.",
    [UI_MENU_RENDER_SCALE_GAME] = "Как в игре (83 %)",
    [UI_MENU_UPSCALER] = "Масштабирование под окно",
    [UI_MENU_UPSCALER_DESC] = "Как картинка растягивается, когда окно больше разрешения игры. FSR сохраняет края и "
                              "детали чётче билинейного.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Билинейное",
    [UI_MENU_ASPECT] = "Форма картинки",
    [UI_MENU_ASPECT_DESC] = "Когда форма окна отличается от разрешения игры (картинка 16:9 на экране 21:9): сохранить "
                            "форму картинки с чёрными полосами или растянуть её на всё окно.",
    [UI_MENU_ASPECT_FIT] = "Чёрные полосы", [UI_MENU_ASPECT_STRETCH] = "Растянуть",
    [UI_MENU_FOV] = "Поле зрения",
    [UI_MENU_FOV_DESC] = "Сколько мира показывает камера, в процентах от игрового. Только для версии игры 1.4.3.",
    [UI_MENU_ANISO] = "Анизотропная фильтрация",
    [UI_MENU_ANISO_DESC] = "Сохраняет чёткость земли и стен под острым углом. Уровень самой игры — как на Switch; "
                           "16x почти ничего не стоит настольной видеокарте.",
    [UI_MENU_ANISO_GAME] = "Как в игре",
    [UI_MENU_SHADOW_RES] = "Размер карты теней",
    [UI_MENU_SHADOW_RES_DESC] = "Размер карты теней от солнца. Собственный размер игры меняется вместе с видом, у земли до "
                                "примерно 400, поэтому тени листвы выглядят квадратными и дёргаются; постоянный размер делает "
                                "их ровными и чётче. Выше 1024 тени требуют больше времени GPU и видеопамяти.",
    [UI_MENU_SHADOW_RES_GAME] = "Как в игре (меняется)",
    [UI_MENU_SHARPNESS] = "Резкость FSR",
    [UI_MENU_SHARPNESS_DESC] = "Повышение резкости после масштабирования FSR; 0 выключает его.",
    [UI_MENU_COLOR] = "Цвет",
    [UI_MENU_COLOR_PRESET] = "Цветовой профиль",
    [UI_MENU_COLOR_PRESET_DESC] = "Задаёт четыре значения ниже. «Яркий»: больше контраста и насыщенности, тени немного "
                                  "светлее (для OLED-экранов). «Чёткость»: немного больше контраста, насыщенности и "
                                  "яркости. Изменение любого значения делает профиль «Своим». Интерфейс игры меняется "
                                  "вместе с картинкой, это меню — нет.",
    [UI_MENU_COLOR_DEFAULT] = "Как в игре", [UI_MENU_COLOR_VIVID] = "Яркий (OLED)", [UI_MENU_COLOR_CLARITY] = "Чёткость",
    [UI_MENU_COLOR_CUSTOM] = "Свой",
    [UI_MENU_COLOR_BRIGHTNESS] = "Яркость", [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Прибавляется к каждому цвету; 0 — без изменений.",
    [UI_MENU_COLOR_CONTRAST] = "Контраст",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Отдаляет цвета от среднего серого (выше 100 %) или приближает к нему; 100 % — без "
                                    "изменений.",
    [UI_MENU_COLOR_SATURATION] = "Насыщенность",
    [UI_MENU_COLOR_SATURATION_DESC] = "Сила цвета относительно серого; 100 % — без изменений.",
    [UI_MENU_COLOR_GAMMA] = "Гамма",
    [UI_MENU_COLOR_GAMMA_DESC] = "Ниже 1 осветляет тёмные и средние тона, выше 1 затемняет их; 1 — без изменений.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "Вывод HDR",
    [UI_MENU_HDR_DESC] = "Яркие блики на HDR-дисплее. Нужен включённый HDR в Windows («Использовать HDR») или "
                         "Wayland-композитор с управлением цветом; без него картинка остаётся SDR. Применяется после "
                         "перезапуска. Цветовой профиль выше — для SDR и в HDR не используется.",
    [UI_MENU_HDR_PEAK] = "Пиковая яркость",
    [UI_MENU_HDR_PEAK_DESC] = "Пик вашего дисплея в нитах; самые яркие блики останавливаются на нём.",
    [UI_MENU_HDR_GAME] = "Яркость игры",
    [UI_MENU_HDR_GAME_DESC] = "Ниты белого цвета картинки (paper white). 203 — эталон HDR; в светлой комнате "
                              "поднимите.",
    [UI_MENU_HDR_UI] = "Яркость интерфейса и меню",
    [UI_MENU_HDR_UI_DESC] = "Ниты белого цвета интерфейса игры и этого меню.",

    [UI_MENU_FPS_LIMIT] = "Ограничение частоты кадров",
    [UI_MENU_FPS_LIMIT_DESC] = "Наибольшее число кадров в секунду. Выше 60 — экспериментальный режим, нужна «Скорость "
                               "игры по часам».",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "Как у монитора", [UI_MENU_FPS_DISPLAY_HZ] = "Как у монитора (%.0f Гц)",
    [UI_MENU_REALTIME] = "Скорость игры по часам",
    [UI_MENU_REALTIME_DESC] = "Игра идёт по реальному времени, медленный кадр её не замедляет.",
    [UI_MENU_CLOTH_REALTIME] = "Ткань по часам",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Ткань и волосы тоже идут по реальному времени.",
    [UI_MENU_HIGHFPS_FIX] = "Исправления для высокого FPS",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Исправляет физику, которая ломается при частоте выше 30 кадров в секунду, и объекты, которые выше 60 остаются полупрозрачными или невидимыми.",
    [UI_MENU_GAME_LANG] = "Язык игры", [UI_MENU_GAME_LANG_DESC] = "Язык текста и озвучки игры.",
    [UI_MENU_GAME_LANG_AUTO] = "Автоматически",
    [UI_MENU_OPERATION_MODE] = "Режим консоли",
    [UI_MENU_OPERATION_MODE_DESC] = "Игра считает, что консоль в док-станции или в портативном режиме.",
    [UI_MENU_MODE_DOCKED] = "В док-станции", [UI_MENU_MODE_HANDHELD] = "Портативный",
    [UI_MENU_ASSUME_FOCUS] = "Работать без фокуса",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Включено (по умолчанию): игра идёт, пока вы в другом окне, а клавиатура не "
                                  "действует, пока окно не вернётся на передний план. Выключено: игра встаёт на паузу, "
                                  "когда окно теряет фокус, как на Switch при нажатии Home; прерванная так загрузка "
                                  "может остановиться насовсем.",

    [UI_MENU_VOLUME] = "Громкость", [UI_MENU_VOLUME_DESC] = "Общая громкость игры.",

    [UI_MENU_MOTION] = "Управление движением", [UI_MENU_MOTION_DESC] = "Прицеливание гироскопом контроллера.",
    [UI_MENU_RUMBLE_STRENGTH] = "Сила вибрации",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Насколько сильно вибрирует контроллер.",
    [UI_MENU_RUMBLE_CURVE] = "Кривая вибрации",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Чем ниже значение, тем сильнее слабая вибрация.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Вибрация триггеров (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Вибрация и в триггерах контроллера Xbox.",
    [UI_MENU_DS_TRIGGERS] = "Адаптивные триггеры (DualSense)",
    [UI_MENU_DS_TRIGGERS_DESC] = "Триггеры контроллера PS5 сопротивляются с того места, где игра засчитывает нажатие: "
                                 "ZR (лук) — сильно, ZL — слабо.",
    [UI_MENU_DS_LIGHT] = "Световая панель (DualSense)",
    [UI_MENU_DS_LIGHT_DESC] = "Световая панель контроллера PS5 горит бирюзовым, как меню; выключено — обычный синий.",
    [UI_MENU_PAD_LAYOUT] = "Раскладка кнопок",
    [UI_MENU_PAD_LAYOUT_DESC] = "Какая кнопка работает как A. Nintendo: правая, как на Switch. Xbox: нижняя, как "
                                "подписано на контроллере Xbox (крест на контроллере PlayStation). Автоматически: "
                                "Nintendo для контроллеров с подписями Nintendo, Xbox для остальных. Подсказки Xbox: "
                                "A справа, как в Nintendo, а игра показывает надписи Xbox (B вместо A, A вместо B, Y "
                                "вместо X, X вместо Y). PlayStation: A — крест, как в Xbox, а игра показывает символы "
                                "PlayStation. Подсказки меняются после перезапуска. Буквы записаны в файлах "
                                 "игры, поэтому у кнопки, переназначенной на вкладке управления, на экране остаётся "
                                 "прежняя буква.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Автоматически", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A справа)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A снизу)", [UI_MENU_PAD_LAYOUT_XBOX_PROMPTS] = "Подсказки Xbox (A справа)",
    [UI_MENU_PAD_LAYOUT_PLAYSTATION] = "PlayStation (крест — подтвердить)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "сейчас: %s",
    [UI_MENU_PAD_LAYOUT_PROMPTS_MISSING] = "нет файлов подсказок: запустите установку снова",
    [UI_MENU_PAD_LAYOUT_PROMPTS_RESTART] = "подсказки сменятся после перезапуска",
    [UI_MENU_STICK_DEADZONE] = "Мёртвая зона стиков",
    [UI_MENU_STICK_DEADZONE_DESC] = "Насколько нужно отклонить стик геймпада, чтобы игра это заметила. Увеличьте, если персонаж или камера двигаются сами.",
    [UI_MENU_MOUSE_SENS] = "Чувствительность мыши",
    [UI_MENU_MOUSE_SENS_DESC] = "Как быстро мышь поворачивает камеру.",
    [UI_MENU_MOUSE_SMOOTH] = "Сглаживание камеры мыши",
    [UI_MENU_MOUSE_SMOOTH_DESC] = "Сглаживает поворот камеры на несколько миллисекунд, чтобы неровное движение мыши или "
                                  "частота кадров поворачивали её ровно. Больше — плавнее, но камера чуть позже следует "
                                  "за мышью; 0 выключает.",
    [UI_MENU_STICK_SENS] = "Скорость камеры (стик)",
    [UI_MENU_STICK_SENS_DESC] = "Умножает скорость поворота камеры правым стиком, включая наибольшую, при каждой из пяти "
                                "чувствительностей камеры в игре. На мышь не влияет.",
    [UI_MENU_WALK_KEY] = "Клавиша шага",
    [UI_MENU_WALK_KEY_DESC] = "Удерживайте: стики клавиатуры отклоняются частично, и персонаж идёт, а не бежит. "
                              "Насколько — задаёт сила шага. Контроллер и мышь не затрагиваются.",
    [UI_MENU_WALK_KEY_TAKEN] = "также %s",
    [UI_MENU_WALK_SCALE] = "Сила шага",
    [UI_MENU_WALK_SCALE_DESC] = "Отклонение стиков клавиатуры при удержании клавиши шага. Половина — шаг, полное "
                                "отклонение — бег.",

    [UI_BINDS_HELP] = "Выберите ячейку и нажмите A или Enter, затем клавишу, кнопку мыши или кнопку контроллера. "
                      "Delete или X очищает ячейку. Занятая клавиша переходит сюда; кнопка контроллера меняется "
                      "местами с той, которую заменяет.",
    [UI_BINDS_COL_KEY1] = "Клавиша 1", [UI_BINDS_COL_KEY2] = "Клавиша 2", [UI_BINDS_COL_PAD] = "Контроллер",
    [UI_BINDS_GROUP_BUTTONS] = "Кнопки", [UI_BINDS_GROUP_MOVE] = "Движение", [UI_BINDS_GROUP_CAMERA] = "Камера",
    [UI_BINDS_PRESS_KEY] = "Нажмите клавишу или кнопку мыши для «%s»",
    [UI_BINDS_PRESS_PAD] = "Нажмите кнопку контроллера для «%s»",
    [UI_BINDS_CANCEL_HINT] = "Esc отменяет, или подождите %d с",
    [UI_BINDS_TAKEN] = "%s: убрано с «%s»", [UI_BINDS_SWAPPED] = "«%s» и «%s» поменялись местами",
    [UI_BINDS_RESET] = "Сбросить все кнопки",
    [UI_BINDS_RESET_DESC] = "Вернуть клавиши и кнопки контроллера по умолчанию. Снова действует раскладка кнопок выше: "
                            "переназначенные кнопки контроллера остаются на своих местах до "
                            "этого сброса.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Нажатие левого стика", [UI_BIND_RCLICK] = "Нажатие правого стика", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Плюс", [UI_BIND_MINUS] = "Минус",
    [UI_BIND_LEFT] = "Крестовина влево", [UI_BIND_UP] = "Крестовина вверх", [UI_BIND_RIGHT] = "Крестовина вправо",
    [UI_BIND_DOWN] = "Крестовина вниз",
    [UI_BIND_LSTICK_LEFT] = "Влево", [UI_BIND_LSTICK_UP] = "Вперёд", [UI_BIND_LSTICK_RIGHT] = "Вправо",
    [UI_BIND_LSTICK_DOWN] = "Назад",
    [UI_BIND_RSTICK_LEFT] = "Камера влево", [UI_BIND_RSTICK_UP] = "Камера вверх",
    [UI_BIND_RSTICK_RIGHT] = "Камера вправо", [UI_BIND_RSTICK_DOWN] = "Камера вниз",
    [UI_BIND_DESC_A] = "Взаимодействие, подобрать, поговорить.", [UI_BIND_DESC_B] = "Бег, отмена, назад.",
    [UI_BIND_DESC_X] = "Прыжок.", [UI_BIND_DESC_Y] = "Атака.", [UI_BIND_DESC_LCLICK] = "Присесть.",
    [UI_BIND_DESC_RCLICK] = "Вид от первого лица.", [UI_BIND_DESC_L] = "Способности.",
    [UI_BIND_DESC_R] = "Бросок оружия.", [UI_BIND_DESC_ZL] = "Щит, захват цели.", [UI_BIND_DESC_ZR] = "Лук.",
    [UI_BIND_DESC_PLUS] = "Меню, инвентарь.", [UI_BIND_DESC_MINUS] = "Карта.",
    [UI_BIND_DESC_DPAD] = "Быстрый выбор.",
    [UI_BIND_DESC_MOVE] = "Двигает Линка, как левый стик. Левый стик контроллера тоже всегда работает.",
    [UI_BIND_DESC_CAMERA] = "Поворачивает камеру, как правый стик. Мышь и правый стик контроллера тоже всегда "
                            "работают.",
    [UI_KEY_SPACE] = "Пробел", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Левый Shift", [UI_KEY_LCTRL] = "Левый Ctrl",
    [UI_KEY_UP] = "Стрелка вверх", [UI_KEY_DOWN] = "Стрелка вниз", [UI_KEY_LEFT] = "Стрелка влево",
    [UI_KEY_RIGHT] = "Стрелка вправо",
    [UI_KEY_MOUSE_L] = "ЛКМ", [UI_KEY_MOUSE_R] = "ПКМ", [UI_KEY_MOUSE_M] = "СКМ", [UI_KEY_MOUSE_X1] = "Мышь 4",
    [UI_KEY_MOUSE_X2] = "Мышь 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Правый Shift",
    [UI_KEY_RCTRL] = "Правый Ctrl", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Нижняя кнопка", [UI_PAD_EAST] = "Правая кнопка", [UI_PAD_WEST] = "Левая кнопка",
    [UI_PAD_NORTH] = "Верхняя кнопка", [UI_PAD_LSTICK] = "Левый стик", [UI_PAD_RSTICK] = "Правый стик",
    [UI_PAD_L] = "Левый бампер", [UI_PAD_R] = "Правый бампер", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "Крестовина влево", [UI_PAD_DPAD_UP] = "Крестовина вверх",
    [UI_PAD_DPAD_RIGHT] = "Крестовина вправо", [UI_PAD_DPAD_DOWN] = "Крестовина вниз", [UI_PAD_ZL] = "Левый курок",
    [UI_PAD_ZR] = "Правый курок",
    [UI_PAD_LPADDLE1] = "Левая задняя кнопка", [UI_PAD_RPADDLE1] = "Правая задняя кнопка",
    [UI_PAD_LPADDLE2] = "Левая задняя кнопка 2", [UI_PAD_RPADDLE2] = "Правая задняя кнопка 2",
    [UI_PAD_CROSS] = "Крест", [UI_PAD_CIRCLE] = "Круг", [UI_PAD_SQUARE] = "Квадрат", [UI_PAD_TRIANGLE] = "Треугольник",

    [UI_MENU_MENU_LANG] = "Язык меню", [UI_MENU_MENU_LANG_DESC] = "Язык этого меню и индикатора.",
    [UI_MENU_MENU_LANG_AUTO] = "Автоматически (из Windows)",

    [UI_MENU_ADV_WARNING] = "Эти настройки нужны для тестов и поиска неполадок. Меняйте их, только если знаете зачем.",
    [UI_MENU_GROUP_SCHED] = "Планирование", [UI_MENU_GROUP_SHADERS] = "Шейдеры и конвейеры",
    [UI_MENU_GROUP_TEXMEM] = "Текстуры и память", [UI_MENU_GROUP_FIXES] = "Исправления графики",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Диагностика",
    [UI_MENU_ASYNC_PREEMPT] = "Вытеснение на виртуальных ядрах",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Позволяет потоку игры прервать другой поток на том же виртуальном ядре.",
    [UI_MENU_PREEMPT_OFF] = "Выкл.", [UI_MENU_PREEMPT_ON] = "Вкл.", [UI_MENU_PREEMPT_INHERIT] = "Только наследование",
    [UI_MENU_REPLAY_ASYNC] = "Подготовка кадра для GPU в отдельном потоке",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Кадр для GPU готовится в отдельном потоке, а не в потоке игры. Добавляет до одного кадра задержки.",
    [UI_MENU_TIMER_1MS] = "Таймер Windows 1 мс",
    [UI_MENU_TIMER_1MS_DESC] = "Повышает точность системного таймера до 1 мс для ровного темпа кадров.",
    [UI_MENU_PIPE_WAIT] = "Обработка ожидающих отрисовок",
    [UI_MENU_PIPE_WAIT_DESC] = "«По умолчанию» — текущее планирование с отдельным потоком для ожидающих отрисовок; «Старый способ» — прежнее планирование.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "По умолчанию", [UI_MENU_PIPE_WAIT_DRAW] = "Старый способ",
    [UI_MENU_PIPE_SKIP] = "Пропуск отрисовок при сборке конвейера",
    [UI_MENU_PIPE_SKIP_DESC] = "Какие отрисовки отбрасываются, а не ждут свой конвейер.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Строго", [UI_MENU_PIPE_SKIP_ANY] = "Любые",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Полноэкранные проходы",
    [UI_MENU_LIB_PREBUILD] = "Предсборка библиотек стадий",
    [UI_MENU_LIB_PREBUILD_DESC] = "Заранее собирает библиотеки стадий шейдеров в фоне.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Потоки предсборки библиотек",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Сколько потоков занято предсборкой; «Автоматически» — по числу ядер процессора.",
    [UI_MENU_PIPE_PREBUILD] = "Предсборка конвейеров",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Заранее собирает известные конвейеры.",
    [UI_MENU_PREWARM_XLATE] = "Прогрев транслятора",
    [UI_MENU_PREWARM_XLATE_DESC] = "Потоки, которые при запуске переводят известные шейдеры; «Автоматически» — половина потоков процессора, не больше 16.",
    [UI_MENU_GPL] = "Библиотека графических конвейеров",
    [UI_MENU_GPL_DESC] = "Использует расширение Vulkan для библиотек конвейеров; выключите, если драйвер с ним сбоит.",
    [UI_MENU_ASTC] = "Распаковка текстур ASTC", [UI_MENU_ASTC_DESC] = "Где распаковываются сжатые текстуры ASTC.",
    [UI_MENU_ASTC_AUTO] = "Автоматически", [UI_MENU_ASTC_GPU] = "Шейдер GPU", [UI_MENU_ASTC_NATIVE] = "Аппаратно",
    [UI_MENU_ASTC_CPU] = "Процессор",
    [UI_MENU_MEMORY] = "Режим памяти GPU", [UI_MENU_MEMORY_DESC] = "Как память игры передаётся видеокарте.",
    [UI_MENU_MEMORY_AUTO] = "Автоматически", [UI_MENU_MEMORY_IMPORT] = "Импорт", [UI_MENU_MEMORY_MIRROR] = "Зеркало",
    [UI_MENU_MEMORY_VERIFY] = "Проверка",
    [UI_MENU_TEX_ALIAS] = "Наследование содержимого текстур", [UI_MENU_TEX_ALIAS_DESC] = "Новая текстура поверх памяти живой текстуры получает её содержимое, а не пустую.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Предел памяти текстур", [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Процент видеопамяти, выше которого освобождённые игрой текстуры удаляются (при повторном использовании загружаются снова). 0 — не удалять.",
    [UI_MENU_ASTC_BC] = "Сжатие ASTC в BC3", [UI_MENU_ASTC_BC_DESC] = "С декодером на GPU: декодированные текстуры ASTC пересжимаются в BC3 и занимают вчетверо меньше видеопамяти. Для видеокарт с 4 ГБ и меньше; текстуры чуть более блочные.",
    [UI_MENU_LOD_GUARD] = "Защита LOD", [UI_MENU_LOD_GUARD_DESC] = "Не даёт объектам мелькать не на своём месте, а дальнему плану — на кадр становиться плоским: ждёт, пока игра допишет данные уровней детализации, и только потом их читает GPU.",
    [UI_MENU_FIX_C0] = "Исправление константного буфера",
    [UI_MENU_FIX_C0_DESC] = "Заполняет зарезервированный драйвером банк констант c0 (размеры хранилищ, размер запуска, размер точки, базовый экземпляр) вместо чтения нулей.",
    [UI_MENU_FIX_ZERO] = "Обнулять новую память GPU",
    [UI_MENU_FIX_ZERO_DESC] = "Новые пулы памяти GPU заполняются нулями, как память Switch.",
    [UI_MENU_BIND_SKIP] = "Пропуск повторных привязок",
    [UI_MENU_BIND_SKIP_DESC] = "Оптимизация: пропускает неизменившиеся привязки.",
    [UI_MENU_SHADER_MEMO] = "Память шейдеров", [UI_MENU_SHADER_MEMO_DESC] = "Оптимизация: запоминает поиск шейдеров.",
    [UI_MENU_PIPE_MEMO] = "Память конвейера", [UI_MENU_PIPE_MEMO_DESC] = "Оптимизация: повторяет конвейер прошлой отрисовки, если её состояние не изменилось.",
    [UI_MENU_TEX_MEMO] = "Память привязок текстур", [UI_MENU_TEX_MEMO_DESC] = "Оптимизация: запоминает дескрипторы текстур между отрисовками.",
    [UI_MENU_DESC_TEMPLATE] = "Шаблоны дескрипторов",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Оптимизация: обновляет дескрипторы пачками.",
    [UI_MENU_PUSH_SKIP] = "Пропуск повторных push-констант",
    [UI_MENU_PUSH_SKIP_DESC] = "Оптимизация: пропускает неизменившиеся push-константы.",
    [UI_MENU_SPLIT_DRAWS] = "Ранняя отправка работы GPU",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Отправляет работу GPU частями через каждые N отрисовок между проходами, чтобы GPU начинал раньше; 0 — выключено.",
    [UI_MENU_VK_VALIDATION] = "Слой проверки",
    [UI_MENU_VK_VALIDATION_DESC] = "Проверяет каждый вызов Vulkan и сильно замедляет игру.",
    [UI_MENU_VK_VALIDATION_MISSING] = "Слой проверки не установлен.",
    [UI_MENU_VK_DYN_VERTEX] = "Динамическое состояние вершин",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Принудительно включает или выключает динамический ввод вершин; по умолчанию зависит от GPU.",
    [UI_MENU_VK_DYN_A2C] = "Динамический alpha to coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "Принудительно включает или выключает динамический alpha to coverage. По умолчанию "
                                "выключен на драйвере AMD для RDNA 2 и старше, где с ним персонажи чёрные, и включён "
                                "на остальных.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Фиксировать размер подгруппы",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Фиксирует размер подгруппы шейдера.",
    [UI_MENU_TRACE] = "Журнал производительности", [UI_MENU_TRACE_DESC] = "Пишет время кадров в файл.",
    [UI_MENU_FLIGHT] = "Запись последних кадров (для отчёта о фризе)",
    [UI_MENU_FLIGHT_DESC] = "Хранит короткую историю и сохраняет её, когда кадр подвисает.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Порог подвисания (мс)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Время кадра, которое считается подвисанием.",
    [UI_MENU_INPUT_RECORD] = "Запись ввода",
    [UI_MENU_INPUT_RECORD_DESC] = "Пишет ваш ввод в файл для отчёта об ошибке.",
    [UI_MENU_OPEN_LOG] = "Открыть папку журналов", [UI_MENU_OPEN_LOG_DESC] = "Показывает папку с файлами журнала.",
    [UI_MENU_COPY_INFO] = "Копировать сведения о системе",
    [UI_MENU_COPY_INFO_DESC] = "Копирует в буфер обмена видеокарту, драйвер, процессор, память и изменённые настройки.",
    [UI_MENU_COPY_INFO_DONE] = "Скопировано", [UI_MENU_ACTION_FAILED] = "не получилось",
    [UI_MENU_RESET] = "Сбросить все настройки", [UI_MENU_RESET_DESC] = "Действует на все вкладки.",
    [UI_MENU_RESET_CONFIRM] = "Сбросить все настройки к значениям по умолчанию?", [UI_MENU_RESET_YES] = "Сбросить",
    [UI_MENU_RESET_NO] = "Отмена",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f мс", [UI_MENU_OV_GAME_PCT] = "игра %.0f%%",
    [UI_MENU_OV_MAX_MS] = "макс. %.1f мс",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "ЦП", [UI_MENU_OV_RAM] = "ОЗУ", [UI_MENU_OV_VRAM] = "Видеопамять",
    [UI_MENU_OV_TEMP] = "Температура", [UI_MENU_OV_POWER] = "Мощность", [UI_MENU_OV_CLOCK] = "Частота",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Подготовка кадра для GPU", [UI_MENU_OV_GPU_TIME] = "Занятость GPU за кадр",
    [UI_MENU_OV_PIPE_WAIT] = "Ожидания конвейеров",
    [UI_MENU_OV_SHADER_WAIT] = "Ожидания шейдеров", [UI_MENU_OV_TEX_UPLOADS] = "Загрузки текстур",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f Вт",
    [UI_MENU_OV_FMT_MHZ] = "%.0f МГц", [UI_MENU_OV_FMT_GB] = "%.1f ГБ", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f ГБ",
    [UI_MENU_OV_GAME_GB] = "игра %.1f ГБ", [UI_MENU_OV_FMT_MS] = "%.2f мс", [UI_MENU_OV_PER_FRAME] = "%.1f за кадр",
    [UI_MENU_OV_WINDOW] = "Окно", [UI_MENU_OV_OUTPUT] = "Изображение игры", [UI_MENU_OV_SCENE] = "3D-сцена",
    [UI_MENU_OV_SCALING] = "Масштабирование", [UI_MENU_OV_LIMIT] = "Лимит кадров", [UI_MENU_OV_PRESENT] = "Вывод кадров",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, резкость %d%%", [UI_MENU_OV_SCALE_NONE] = "нет (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "билинейное",
};

static const char *const text_pt_br[UI_TEXT_COUNT] = {
    SWKBD("Inserir texto", "Digite um nome", "Cavalo", "OK", "Cancelar", "Controle: A = OK, B = Cancelar"),

    [UI_MENU_TITLE] = "Configurações",
    [UI_MENU_TAB_SCREEN] = "Tela", [UI_MENU_TAB_GAME] = "Jogo", [UI_MENU_TAB_SOUND] = "Som",
    [UI_MENU_TAB_CONTROLS] = "Controles", [UI_MENU_TAB_AMIIBO] = "Amiibo", [UI_MENU_TAB_MENU] = "Menu", [UI_MENU_TAB_ADVANCED] = "Avançado",
    [UI_MENU_CONTINUE] = "Continuar", [UI_MENU_QUIT] = "Sair do jogo",
    [UI_MENU_ON] = "Ligado", [UI_MENU_OFF] = "Desligado", [UI_MENU_AUTO] = "Automático",
    [UI_MENU_DEFAULT_MARK] = "padrão",
    [UI_MENU_AFTER_RESTART] = "após reiniciar", [UI_MENU_SET_BY_ENV] = "definido por variável de ambiente",
    [UI_MENU_SAVE_FAILED] = "não foi possível salvar as configurações",
    [UI_MENU_VERSION] = "TotK Recomp %s, jogo %s",
    [UI_MENU_HINT] = "B ou Esc fecha  ·  L / R ou Q / E troca de aba  ·  F10 indicador  ·  F11 tela cheia",

    [UI_MENU_FULLSCREEN] = "Tela cheia", [UI_MENU_FULLSCREEN_DESC] = "O jogo ocupa a tela inteira; F11 alterna.",
    [UI_MENU_BORDERLESS] = "Janela sem borda",
    [UI_MENU_BORDERLESS_DESC] = "A janela sem a barra de título e as bordas, centralizada na tela, no tamanho da janela. Só fora da tela cheia.",
    [UI_MENU_WINDOW_SIZE] = "Tamanho da janela",
    [UI_MENU_WINDOW_SIZE_DESC] = "Tamanho da janela do jogo fora da tela cheia.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Personalizado",
    [UI_MENU_PRESENT] = "Apresentação", [UI_MENU_PRESENT_DESC] = "Como os quadros prontos chegam à tela.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Imediato",
    [UI_MENU_PRESENT_IN_USE] = "em uso: %s",
    [UI_MENU_GPU] = "Placa de vídeo", [UI_MENU_GPU_DESC] = "O dispositivo Vulkan que desenha o jogo.",
    [UI_MENU_GPU_AUTO] = "Automática",
    [UI_MENU_SHADER_TOAST] = "Aviso de compilação de shaders",
    [UI_MENU_SHADER_TOAST_DESC] = "Um aviso pequeno no canto inferior direito enquanto shaders e pipelines são "
                                  "compilados, que é quando o jogo pode engasgar por um instante.",
    [UI_MENU_TOAST_SHADERS] = "Compilando shaders (%u)",
    [UI_MENU_AMIIBO_DIR] = "Pasta dos Amiibo:",
    [UI_MENU_AMIIBO_SET_DIR] = "Usar esta pasta",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Padrão (./amiibo)",
    [UI_MENU_AMIIBO_REFRESH] = "Atualizar",
    [UI_MENU_AMIIBO_PICK] = "Escolha um arquivo de Amiibo (.bin / .nfc):",
    [UI_MENU_AMIIBO_FOLDER] = "[pasta]",
    [UI_MENU_AMIIBO_ACTIONS] = "Leitor NFC",
    [UI_MENU_AMIIBO_READER] = "Leitor:",
    [UI_MENU_AMIIBO_IDLE] = "Pronto",
    [UI_MENU_AMIIBO_SEARCHING] = "O jogo está procurando um Amiibo...",
    [UI_MENU_AMIIBO_FOUND] = "Amiibo detectado",
    [UI_MENU_AMIIBO_MOUNTED] = "Lendo o Amiibo",
    [UI_MENU_AMIIBO_DISABLED] = "Leitor NFC desligado",
    [UI_MENU_AMIIBO_SELECTED] = "Arquivo escolhido:",
    [UI_MENU_AMIIBO_SCAN] = "Ler Amiibo",
    [UI_MENU_AMIIBO_NONE_SELECTED] = "(nenhum arquivo escolhido)",
    [UI_MENU_AMIIBO_REMOVE] = "Tirar o Amiibo do leitor",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Tirar o Amiibo sozinho depois de ler",
    [UI_MENU_AMIIBO_HOLD] = "Tempo no leitor",
    [UI_MENU_AMIIBO_INFO] = "Amiibo no leitor:",
    [UI_MENU_AMIIBO_NO_TAG] = "Nenhum Amiibo no leitor.",
    [UI_MENU_AMIIBO_STATUS] = "Situação: %s",
    [UI_MENU_AMIIBO_READY] = "Pronto",
    [UI_MENU_AMIIBO_CANNOT_OPEN] = "Não foi possível abrir a pasta %s",
    [UI_MENU_AMIIBO_TAPPED] = "Lido: %s",
    [UI_MENU_AMIIBO_LOAD_ERROR] = "Não foi possível ler %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo retirado",
    [UI_MENU_ACH_POPUP] = "Avisos de conquista",
    [UI_MENU_ACH_POPUP_DESC] = "Um aviso no alto da tela quando uma conquista é desbloqueada. As conquistas são "
                               "verificadas a cada salvamento do jogo, então o aviso chega com o próximo salvamento.",
    [UI_MENU_ACH_SOUND] = "Som de conquista",
    [UI_MENU_ACH_SOUND_DESC] = "Um toque curto junto com o aviso de conquista, no volume do jogo; o troféu de platina tem "
                               "um próprio.",
    [UI_MENU_TAB_ACHIEVEMENTS] = "Conquistas", [UI_MENU_ACH_PROGRESS] = "%d de %d desbloqueadas",
    [UI_MENU_ACH_NONE] = "Nenhuma conquista instalada. Rode a instalação de novo ou salve-as no Editor de Conquistas.",
    [UI_MENU_ACH_SECRET] = "Conquista secreta", [UI_MENU_ACH_SECRET_DESC] = "Continue jogando para descobrir.",
    [UI_MENU_ACH_UNLOCKED_ON] = "Desbloqueada em %s", [UI_MENU_ACH_COMMON] = "Comum", [UI_MENU_ACH_RARE] = "Rara",
    [UI_MENU_ACH_EPIC] = "Épica", [UI_MENU_ACH_LEGENDARY] = "Lendária", [UI_MENU_ACH_PLATINUM] = "Platina",
    [UI_MENU_ACH_NOTICE] = "Conquista desbloqueada", [UI_MENU_ACH_NOTICE_PLATINUM] = "Troféu de platina!",
    [UI_MENU_ACH_NOTICE_MANY] = "%d conquistas desbloqueadas",
    [UI_MENU_ACH_NOTICE_MANY_SUB] = "Veja no menu, na aba Conquistas",
    [UI_MENU_ACH_BLOCKED] = "Feita num save de fora",
    [UI_MENU_ACH_BLOCKED_NOTE] = "%d conquistas já estavam feitas num save que não foi gravado por este jogo aqui "
                                 "(copiado ou restaurado de um backup). Elas contam quando você as fizer jogando, num "
                                 "jogo novo.",
    [UI_MENU_PERF_OVERLAY] = "Indicador de desempenho",
    [UI_MENU_PERF_OVERLAY_DESC] = "Taxa de quadros no canto superior direito. Completo acrescenta os tempos de quadro com "
                                  "um gráfico, o uso de CPU, GPU e memória, as temperaturas e as esperas da thread de "
                                  "renderização. F10 alterna os modos.",
    [UI_MENU_PERF_OFF] = "Desligado", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Completo",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_MENU_GRAPHICS] = "Imagem",
    [UI_MENU_OUTPUT_RES] = "Resolução do jogo",
    [UI_MENU_OUTPUT_RES_DESC] = "O tamanho em que o jogo desenha a imagem e a interface. Acima de 1920 x 1080 o jogo "
                                "precisa de mais memória de vídeo e de mais tempo de GPU. Um tamanho mais largo (21:9) amplia a visão da "
                                "câmera e leva o HUD para os cantos da tela; um mais alto (16:10, Steam Deck) o leva "
                                "para as bordas de cima e de baixo.",
    [UI_MENU_ASPECT] = "Formato da imagem",
    [UI_MENU_ASPECT_DESC] = "Quando o formato da janela é diferente do da resolução do jogo (uma imagem 16:9 numa tela "
                            "21:9): manter o formato da imagem com barras pretas, ou esticá-la pela janela.",
    [UI_MENU_ASPECT_FIT] = "Barras pretas", [UI_MENU_ASPECT_STRETCH] = "Esticar",
    [UI_MENU_FOV] = "Campo de visão",
    [UI_MENU_FOV_DESC] = "Quanto do mundo a câmera mostra, em porcentagem do campo do próprio jogo. Só na versão 1.4.3 do jogo.",
    [UI_MENU_RENDER_SCALE] = "Escala da renderização 3D",
    [UI_MENU_RENDER_SCALE_DESC] = "Tamanho da cena 3D em relação à resolução do jogo; o FSR do jogo a leva até esse "
                                  "tamanho. Acima de 100 % suaviza as bordas serrilhadas (supersampling), com alto custo "
                                  "de GPU.",
    [UI_MENU_RENDER_SCALE_GAME] = "Jogo (83 %)",
    [UI_MENU_UPSCALER] = "Ajuste à janela",
    [UI_MENU_UPSCALER_DESC] = "Como a imagem é ampliada quando a janela é maior que a resolução do jogo. O FSR deixa "
                              "bordas e detalhes mais nítidos que o bilinear.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_SHARPNESS] = "Nitidez do FSR",
    [UI_MENU_SHARPNESS_DESC] = "Realce de nitidez depois da ampliação FSR; 0 desliga.",
    [UI_MENU_COLOR] = "Cor",
    [UI_MENU_COLOR_PRESET] = "Perfil de cor",
    [UI_MENU_COLOR_PRESET_DESC] = "Define os quatro valores abaixo. Vívido: mais contraste e saturação, sombras um pouco "
                                  "mais abertas (feito para telas OLED). Clareza: um pouco mais de contraste, saturação e "
                                  "brilho. Mexer em um valor deixa o perfil como Personalizado. O HUD muda junto com a "
                                  "imagem; este menu não.",
    [UI_MENU_COLOR_DEFAULT] = "Original do jogo", [UI_MENU_COLOR_VIVID] = "Vívido (OLED)", [UI_MENU_COLOR_CLARITY] = "Clareza",
    [UI_MENU_COLOR_CUSTOM] = "Personalizado",
    [UI_MENU_COLOR_BRIGHTNESS] = "Brilho", [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Somado a cada cor; 0 não muda nada.",
    [UI_MENU_COLOR_CONTRAST] = "Contraste",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Afasta as cores do cinza médio (acima de 100 %) ou as aproxima dele; 100 % não muda nada.",
    [UI_MENU_COLOR_SATURATION] = "Saturação",
    [UI_MENU_COLOR_SATURATION_DESC] = "Força da cor em relação ao cinza; 100 % não muda nada.",
    [UI_MENU_COLOR_GAMMA] = "Gama",
    [UI_MENU_COLOR_GAMMA_DESC] = "Abaixo de 1 clareia os tons escuros e médios, acima de 1 os escurece; 1 não muda nada.",

    [UI_MENU_FPS_LIMIT] = "Limite de quadros",
    [UI_MENU_FPS_LIMIT_DESC] = "O máximo de quadros por segundo que o jogo desenha. Acima de 60 é experimental e exige "
                               "Velocidade do jogo pelo relógio.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "Taxa do monitor", [UI_MENU_FPS_DISPLAY_HZ] = "Taxa do monitor (%.0f Hz)",
    [UI_MENU_REALTIME] = "Velocidade do jogo pelo relógio",
    [UI_MENU_REALTIME_DESC] = "Avança o jogo pelo tempo real, para que um quadro lento não deixe o jogo mais lento.",
    [UI_MENU_CLOTH_REALTIME] = "Tecidos pelo relógio",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Avança tecidos e cabelos pelo tempo real também.",
    [UI_MENU_HIGHFPS_FIX] = "Correções para taxa de quadros alta",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrige a física que se comporta mal acima de 30 quadros por segundo e os objetos que ficam transparentes ou invisíveis acima de 60.",
    [UI_MENU_ASSUME_FOCUS] = "Continuar rodando fora de foco",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Ligado (o padrão), o jogo continua rodando enquanto você está em outra janela, e o "
                                  "teclado fica sem efeito até a janela voltar para a frente. Desligado, ele pausa "
                                  "quando a janela perde o foco, como no Switch ao apertar Home; um carregamento "
                                  "interrompido assim pode travar de vez.",
    [UI_MENU_GAME_LANG] = "Idioma do jogo", [UI_MENU_GAME_LANG_DESC] = "Idioma dos textos e das vozes do jogo.",
    [UI_MENU_GAME_LANG_AUTO] = "Automático",
    [UI_MENU_OPERATION_MODE] = "Modo do console",
    [UI_MENU_OPERATION_MODE_DESC] = "Se o jogo acha que está na base (TV) ou no modo portátil.",
    [UI_MENU_MODE_DOCKED] = "Na base (TV)", [UI_MENU_MODE_HANDHELD] = "Portátil",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Volume geral do jogo.",

    [UI_MENU_MOTION] = "Controles de movimento", [UI_MENU_MOTION_DESC] = "Mirar com o giroscópio do controle.",
    [UI_MENU_RUMBLE_STRENGTH] = "Intensidade da vibração",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Quão forte o controle vibra.",
    [UI_MENU_RUMBLE_CURVE] = "Curva da vibração",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Valores menores deixam as vibrações fracas mais fortes.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Vibração dos gatilhos (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Vibra também os gatilhos de um controle Xbox.",
    [UI_MENU_DS_TRIGGERS] = "Gatilhos adaptáveis (DualSense)",
    [UI_MENU_DS_TRIGGERS_DESC] = "Os gatilhos do controle de PS5 fazem resistência a partir do ponto em que o jogo "
                                 "conta o aperto: o ZR, do arco, com força, e o ZL de leve.",
    [UI_MENU_DS_LIGHT] = "Barra de luz (DualSense)",
    [UI_MENU_DS_LIGHT_DESC] = "Acende a barra de luz do controle de PS5 no turquesa do menu; desligado, fica no azul de sempre.",
    [UI_MENU_PAD_LAYOUT] = "Layout dos botões",
    [UI_MENU_PAD_LAYOUT_DESC] = "Qual botão é o A. Nintendo: o da direita, como no Switch. Xbox: o de baixo, como está "
                                "escrito no controle Xbox (a cruz no controle PlayStation). Automático escolhe Nintendo "
                                "para controles com os rótulos da Nintendo e Xbox para os demais. Ícones Xbox: o A é o da "
                                "direita, como no Nintendo, e o jogo mostra as letras do Xbox (B no lugar de A, A no "
                                "de B, Y no de X, X no de Y). PlayStation: o A é o X (cruz), como no Xbox, e o jogo "
                                "mostra os símbolos do PlayStation. Os ícones mudam depois de reiniciar. As letras fazem "
                                "parte dos arquivos do jogo, então um botão remapeado em Controles continua com a "
                                "letra antiga na tela.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automático", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A à direita)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A embaixo)", [UI_MENU_PAD_LAYOUT_PLAYSTATION] = "PlayStation (X confirma)",
    [UI_MENU_PAD_LAYOUT_XBOX_PROMPTS] = "Ícones Xbox (A à direita)", [UI_MENU_PAD_LAYOUT_IN_USE] = "em uso: %s",
    [UI_MENU_PAD_LAYOUT_PROMPTS_MISSING] = "faltam os arquivos dos ícones: rode a instalação de novo",
    [UI_MENU_PAD_LAYOUT_PROMPTS_RESTART] = "os ícones mudam depois de reiniciar",
    [UI_MENU_STICK_DEADZONE] = "Zona morta dos analógicos",
    [UI_MENU_STICK_DEADZONE_DESC] = "Quanto o analógico do controle precisa se mover antes de o jogo perceber. Aumente se o personagem ou a câmera se mexerem sozinhos.",
    [UI_MENU_MOUSE_SENS] = "Sensibilidade da câmera no mouse",
    [UI_MENU_MOUSE_SENS_DESC] = "Quão rápido o mouse gira a câmera.",
    [UI_MENU_MOUSE_SMOOTH] = "Suavização da câmera no mouse",
    [UI_MENU_MOUSE_SMOOTH_DESC] = "Suaviza o giro da câmera ao longo de alguns milissegundos, para que um mouse ou uma taxa "
                                  "de quadros irregular gire a câmera de forma uniforme. Mais alto é mais suave, mas segue "
                                  "o mouse um pouco depois; 0 desliga.",

    [UI_BINDS_HELP] = "Selecione uma célula e aperte A ou Enter, depois a tecla, o botão do mouse ou o botão do controle. "
                      "Delete ou X limpa a célula. Uma tecla já usada passa para cá; um botão do controle troca de lugar "
                      "com o que ele substitui.",
    [UI_BINDS_COL_KEY1] = "Tecla 1", [UI_BINDS_COL_KEY2] = "Tecla 2", [UI_BINDS_COL_PAD] = "Controle",
    [UI_BINDS_GROUP_BUTTONS] = "Botões", [UI_BINDS_GROUP_MOVE] = "Movimento", [UI_BINDS_GROUP_CAMERA] = "Câmera",
    [UI_BINDS_PRESS_KEY] = "Aperte uma tecla ou um botão do mouse para “%s”",
    [UI_BINDS_PRESS_PAD] = "Aperte um botão do controle para “%s”",
    [UI_BINDS_CANCEL_HINT] = "Esc cancela, ou espere %d s",
    [UI_BINDS_TAKEN] = "%s saiu de “%s”", [UI_BINDS_SWAPPED] = "“%s” trocou de lugar com “%s”",
    [UI_BINDS_RESET] = "Restaurar todos os botões",
    [UI_BINDS_RESET_DESC] = "Volta às teclas e aos botões do controle padrão. O layout dos botões acima volta a valer: os "
                            "botões do controle remapeados ficam onde você os colocou até esta restauração.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Clique do analógico esquerdo", [UI_BIND_RCLICK] = "Clique do analógico direito",
    [UI_BIND_L] = "L", [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Mais",
    [UI_BIND_MINUS] = "Menos",
    [UI_BIND_LEFT] = "Direcional: esquerda", [UI_BIND_UP] = "Direcional: cima", [UI_BIND_RIGHT] = "Direcional: direita",
    [UI_BIND_DOWN] = "Direcional: baixo",
    [UI_BIND_LSTICK_LEFT] = "Mover para a esquerda", [UI_BIND_LSTICK_UP] = "Mover para a frente",
    [UI_BIND_LSTICK_RIGHT] = "Mover para a direita", [UI_BIND_LSTICK_DOWN] = "Mover para trás",
    [UI_BIND_RSTICK_LEFT] = "Câmera para a esquerda", [UI_BIND_RSTICK_UP] = "Câmera para cima",
    [UI_BIND_RSTICK_RIGHT] = "Câmera para a direita", [UI_BIND_RSTICK_DOWN] = "Câmera para baixo",
    [UI_BIND_DESC_A] = "Interagir, pegar, conversar.", [UI_BIND_DESC_B] = "Correr, cancelar, voltar.",
    [UI_BIND_DESC_X] = "Pular.", [UI_BIND_DESC_Y] = "Atacar.", [UI_BIND_DESC_LCLICK] = "Agachar.",
    [UI_BIND_DESC_RCLICK] = "Visão em primeira pessoa.", [UI_BIND_DESC_L] = "Habilidades.",
    [UI_BIND_DESC_R] = "Arremessar a arma.", [UI_BIND_DESC_ZL] = "Escudo, travar a mira.", [UI_BIND_DESC_ZR] = "Arco.",
    [UI_BIND_DESC_PLUS] = "Menu, inventário.", [UI_BIND_DESC_MINUS] = "Mapa.",
    [UI_BIND_DESC_DPAD] = "Seleção rápida.",
    [UI_BIND_DESC_MOVE] = "Move o Link como o analógico esquerdo. O analógico esquerdo do controle sempre funciona também.",
    [UI_BIND_DESC_CAMERA] = "Gira a câmera como o analógico direito. O mouse e o analógico direito do controle sempre "
                            "funcionam também.",
    [UI_KEY_SPACE] = "Espaço", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Shift esquerdo",
    [UI_KEY_LCTRL] = "Ctrl esquerdo",
    [UI_KEY_UP] = "Seta para cima", [UI_KEY_DOWN] = "Seta para baixo", [UI_KEY_LEFT] = "Seta para a esquerda",
    [UI_KEY_RIGHT] = "Seta para a direita",
    [UI_KEY_MOUSE_L] = "Mouse esquerdo", [UI_KEY_MOUSE_R] = "Mouse direito", [UI_KEY_MOUSE_M] = "Mouse do meio",
    [UI_KEY_MOUSE_X1] = "Mouse 4", [UI_KEY_MOUSE_X2] = "Mouse 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Shift direito",
    [UI_KEY_RCTRL] = "Ctrl direito", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Botão de baixo", [UI_PAD_EAST] = "Botão da direita", [UI_PAD_WEST] = "Botão da esquerda",
    [UI_PAD_NORTH] = "Botão de cima", [UI_PAD_LSTICK] = "Analógico esquerdo", [UI_PAD_RSTICK] = "Analógico direito",
    [UI_PAD_L] = "Bumper esquerdo", [UI_PAD_R] = "Bumper direito", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "Direcional: esquerda", [UI_PAD_DPAD_UP] = "Direcional: cima",
    [UI_PAD_DPAD_RIGHT] = "Direcional: direita", [UI_PAD_DPAD_DOWN] = "Direcional: baixo",
    [UI_PAD_ZL] = "Gatilho esquerdo", [UI_PAD_ZR] = "Gatilho direito",
    [UI_PAD_LPADDLE1] = "Botão traseiro esquerdo", [UI_PAD_RPADDLE1] = "Botão traseiro direito",
    [UI_PAD_LPADDLE2] = "Botão traseiro esquerdo 2", [UI_PAD_RPADDLE2] = "Botão traseiro direito 2",
    [UI_PAD_CROSS] = "Cruz", [UI_PAD_CIRCLE] = "Círculo", [UI_PAD_SQUARE] = "Quadrado", [UI_PAD_TRIANGLE] = "Triângulo",

    [UI_MENU_MENU_LANG] = "Idioma do menu",
    [UI_MENU_MENU_LANG_DESC] = "Idioma deste menu e do indicador de desempenho.",
    [UI_MENU_MENU_LANG_AUTO] = "Automático (do Windows)",

    [UI_MENU_ADV_WARNING] = "Estas configurações são para testes e solução de problemas. Altere-as só quando souber por "
                            "quê.",
    [UI_MENU_GROUP_SCHED] = "Escalonamento", [UI_MENU_GROUP_SHADERS] = "Shaders e pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Texturas e memória", [UI_MENU_GROUP_FIXES] = "Correções de renderização",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Diagnóstico",
    [UI_MENU_ASYNC_PREEMPT] = "Preempção nos núcleos virtuais",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Permite que uma thread do jogo interrompa outra no mesmo núcleo virtual.",
    [UI_MENU_PREEMPT_OFF] = "Desligado", [UI_MENU_PREEMPT_ON] = "Ligado", [UI_MENU_PREEMPT_INHERIT] = "Só herança",
    [UI_MENU_REPLAY_ASYNC] = "Preparo dos quadros da GPU em thread própria",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prepara os quadros para a GPU numa thread separada em vez da thread do jogo. Acrescenta até um quadro de latência.",
    [UI_MENU_TIMER_1MS] = "Timer do Windows de 1 ms",
    [UI_MENU_TIMER_1MS_DESC] = "Aumenta a resolução do timer do sistema para 1 ms, para um ritmo de quadros mais estável.",
    [UI_MENU_PIPE_WAIT] = "Tratamento dos desenhos em espera",
    [UI_MENU_PIPE_WAIT_DESC] = "Padrão mantém o escalonamento atual, com um worker reservado para os desenhos em espera; Antigo (desenho) usa o escalonamento anterior.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Padrão", [UI_MENU_PIPE_WAIT_DRAW] = "Antigo (desenho)",
    [UI_MENU_PIPE_SKIP] = "Desenhos pulados ao compilar pipelines",
    [UI_MENU_PIPE_SKIP_DESC] = "Quais desenhos são descartados em vez de esperar pelo seu pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Estrito", [UI_MENU_PIPE_SKIP_ANY] = "Qualquer",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Passes de tela cheia",
    [UI_MENU_LIB_PREBUILD] = "Pré-compilação das bibliotecas de estágio",
    [UI_MENU_LIB_PREBUILD_DESC] = "Compila as bibliotecas de estágios de shader antecipadamente, em segundo plano.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Threads da pré-compilação de bibliotecas",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads usadas na pré-compilação; Automático escolhe um número conforme a sua CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Pré-compilação de pipelines",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Compila antecipadamente os pipelines já conhecidos.",
    [UI_MENU_PREWARM_XLATE] = "Pré-aquecimento do tradutor",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads que traduzem os shaders conhecidos na inicialização; Automático usa metade das threads da CPU, no máximo 16.",
    [UI_MENU_GPL] = "Biblioteca de pipelines gráficos",
    [UI_MENU_GPL_DESC] = "Usa a extensão de biblioteca de pipelines do Vulkan; desligue se o seu driver se comportar mal com ela.",
    [UI_MENU_ASTC] = "Decodificação de texturas ASTC",
    [UI_MENU_ASTC_DESC] = "Onde as texturas comprimidas ASTC são decodificadas.",
    [UI_MENU_ASTC_AUTO] = "Automático", [UI_MENU_ASTC_GPU] = "Shader na GPU", [UI_MENU_ASTC_NATIVE] = "Nativa",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_ASTC_BC] = "Comprimir ASTC em BC3", [UI_MENU_ASTC_BC_DESC] = "Com o decodificador na GPU: recodifica as texturas ASTC decodificadas em BC3, que ocupa um quarto da memória de vídeo. Para placas com 4 GB ou menos; as texturas ficam um pouco mais quadriculadas.",
    [UI_MENU_MEMORY] = "Modo de memória da GPU", [UI_MENU_MEMORY_DESC] = "Como a memória do jogo é compartilhada com a GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automático", [UI_MENU_MEMORY_IMPORT] = "Importar", [UI_MENU_MEMORY_MIRROR] = "Espelhar",
    [UI_MENU_MEMORY_VERIFY] = "Verificar",
    [UI_MENU_TEX_ALIAS] = "Herança de conteúdo das texturas", [UI_MENU_TEX_ALIAS_DESC] = "Uma textura nova criada sobre a memória de uma textura viva começa com esse conteúdo em vez de vazia.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Limite de memória de texturas", [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Porcentagem da memória de vídeo acima da qual as texturas que o jogo liberou são descartadas (voltam do armazenamento quando usadas de novo). 0 as mantém.",
    [UI_MENU_LOD_GUARD] = "Proteção de LOD", [UI_MENU_LOD_GUARD_DESC] = "Evita que objetos pisquem no lugar errado ou que o cenário distante fique plano por um quadro: espera o jogo terminar de reescrever os dados de nível de detalhe antes que a GPU os leia.",
    [UI_MENU_FIX_C0] = "Correção do buffer de constantes",
    [UI_MENU_FIX_C0_DESC] = "Preenche o banco de constantes c0 reservado ao driver (tamanhos de armazenamento, tamanho do dispatch, tamanho do ponto, instância base) em vez de ler zeros.",
    [UI_MENU_FIX_ZERO] = "Zerar a memória nova da GPU",
    [UI_MENU_FIX_ZERO_DESC] = "Os novos pools de memória da GPU começam zerados, como a memória do Switch.",
    [UI_MENU_BIND_SKIP] = "Pular vinculações repetidas",
    [UI_MENU_BIND_SKIP_DESC] = "Otimização: pula as vinculações que não mudaram.",
    [UI_MENU_SHADER_MEMO] = "Memória de shaders", [UI_MENU_SHADER_MEMO_DESC] = "Otimização: memoriza as buscas de shaders.",
    [UI_MENU_PIPE_MEMO] = "Memória de pipeline", [UI_MENU_PIPE_MEMO_DESC] = "Otimização: reaproveita o pipeline do desenho anterior quando o estado se repete.",
    [UI_MENU_TEX_MEMO] = "Memória de vinculação de texturas", [UI_MENU_TEX_MEMO_DESC] = "Otimização: memoriza os descritores de textura entre os desenhos.",
    [UI_MENU_DESC_TEMPLATE] = "Modelos de descritores",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Otimização: atualiza os descritores em lote.",
    [UI_MENU_PUSH_SKIP] = "Pular push constants repetidas",
    [UI_MENU_PUSH_SKIP_DESC] = "Otimização: pula as push constants que não mudaram.",
    [UI_MENU_SPLIT_DRAWS] = "Envio antecipado à GPU",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Envia o trabalho da GPU em partes a cada N desenhos entre os passes, para que a GPU comece antes; 0 desliga.",
    [UI_MENU_VK_VALIDATION] = "Camada de validação",
    [UI_MENU_VK_VALIDATION_DESC] = "Verifica cada chamada Vulkan e deixa o jogo bem mais lento.",
    [UI_MENU_VK_VALIDATION_MISSING] = "A camada de validação não está instalada.",
    [UI_MENU_VK_DYN_VERTEX] = "Estado dinâmico de vértices",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Força a entrada dinâmica de vértices ligada ou desligada; o padrão depende da GPU.",
    [UI_MENU_VK_DYN_A2C] = "Alpha to coverage dinâmico",
    [UI_MENU_VK_DYN_A2C_DESC] = "Força o alpha to coverage dinâmico ligado ou desligado. Padrão: desligado no driver da "
                                "AMD para RDNA 2 e anteriores, que com ele desenha os personagens pretos, e ligado nos "
                                "demais.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Fixar o tamanho do subgrupo",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixa o tamanho do subgrupo dos shaders.",
    [UI_MENU_TRACE] = "Log de desempenho", [UI_MENU_TRACE_DESC] = "Grava os tempos dos quadros num arquivo.",
    [UI_MENU_FLIGHT] = "Gravador de travamentos",
    [UI_MENU_FLIGHT_DESC] = "Guarda um histórico curto e o salva quando um quadro trava.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Limite de travamento (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Tempo de quadro que conta como travamento.",
    [UI_MENU_INPUT_RECORD] = "Gravação de comandos",
    [UI_MENU_INPUT_RECORD_DESC] = "Grava os seus comandos num arquivo para relatórios de bug.",
    [UI_MENU_OPEN_LOG] = "Abrir a pasta de logs", [UI_MENU_OPEN_LOG_DESC] = "Mostra a pasta com os arquivos de log.",
    [UI_MENU_COPY_INFO] = "Copiar informações do sistema",
    [UI_MENU_COPY_INFO_DESC] = "Copia GPU, driver, CPU, RAM e as configurações alteradas para a área de transferência.",
    [UI_MENU_COPY_INFO_DONE] = "Copiado", [UI_MENU_ACTION_FAILED] = "não funcionou",
    [UI_MENU_RESET] = "Restaurar todas as configurações padrão", [UI_MENU_RESET_DESC] = "Vale para todas as abas.",
    [UI_MENU_RESET_CONFIRM] = "Restaurar todas as configurações para o padrão?", [UI_MENU_RESET_YES] = "Restaurar",
    [UI_MENU_RESET_NO] = "Cancelar",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "jogo %.0f%%",
    [UI_MENU_OV_MAX_MS] = "máx. %.1f ms",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "RAM", [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Temperatura", [UI_MENU_OV_POWER] = "Consumo", [UI_MENU_OV_CLOCK] = "Frequência",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Preparo dos quadros da GPU", [UI_MENU_OV_GPU_TIME] = "GPU ocupada por quadro",
    [UI_MENU_OV_PIPE_WAIT] = "Esperas de pipeline",
    [UI_MENU_OV_SHADER_WAIT] = "Esperas de shader", [UI_MENU_OV_TEX_UPLOADS] = "Envios de texturas",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f GB", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "jogo %.1f GB", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "%.1f por quadro",
    [UI_MENU_OV_WINDOW] = "Janela", [UI_MENU_OV_OUTPUT] = "Imagem do jogo", [UI_MENU_OV_SCENE] = "Cena 3D",
    [UI_MENU_OV_SCALING] = "Ajuste", [UI_MENU_OV_LIMIT] = "Limite de quadros", [UI_MENU_OV_PRESENT] = "Apresentação",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, nitidez %d%%", [UI_MENU_OV_SCALE_NONE] = "nenhum (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinear",
    [UI_MENU_ANISO] = "Filtro anisotrópico",
    [UI_MENU_ANISO_DESC] = "Mantém nítidos o chão e as paredes vistos em ângulo. O nível do próprio jogo é o do Switch; "
                           "16x custa pouco numa placa de vídeo de PC.",
    [UI_MENU_ANISO_GAME] = "Do jogo",
    [UI_MENU_SHADOW_RES] = "Tamanho do mapa de sombras",
    [UI_MENU_SHADOW_RES_DESC] = "Tamanho do mapa de sombras do sol. O tamanho do próprio jogo muda com a vista, até cerca de "
                                "400 perto do chão, então as sombras das folhas ficam quadradas e pulam; um tamanho fixo as "
                                "deixa paradas e mais nítidas. Acima de 1024 as sombras custam mais tempo de GPU e memória "
                                "de vídeo.",
    [UI_MENU_SHADOW_RES_GAME] = "Do jogo (muda)",
    [UI_MENU_STICK_SENS] = "Velocidade da câmera (analógico)",
    [UI_MENU_STICK_SENS_DESC] = "Multiplica a velocidade da câmera do próprio jogo no analógico direito, a máxima também, "
                                "em cada uma das cinco sensibilidades de câmera do jogo. O mouse tem a sua própria opção.",
    [UI_MENU_WALK_KEY] = "Tecla de andar",
    [UI_MENU_WALK_KEY_DESC] = "Segure para inclinar os analógicos do teclado só até parte do caminho, assim o teclado anda em "
                              "vez de correr. A força do andar define quanto. Controle e mouse não mudam.",
    [UI_MENU_WALK_KEY_TAKEN] = "também %s",
    [UI_MENU_WALK_SCALE] = "Força do andar",
    [UI_MENU_WALK_SCALE_DESC] = "A inclinação dos analógicos do teclado enquanto a tecla de andar está apertada. Metade é "
                                "andar, a inclinação toda é correr.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "Saída HDR",
    [UI_MENU_HDR_DESC] = "Brilhos intensos numa tela HDR. Precisa do HDR ligado no Windows (\"Usar HDR\") ou num "
                         "compositor Wayland com gerenciamento de cor; sem isso a imagem continua SDR. Vale depois de "
                         "reiniciar. O perfil de cor acima é para SDR e não é usado em HDR.",
    [UI_MENU_HDR_PEAK] = "Brilho máximo",
    [UI_MENU_HDR_PEAK_DESC] = "O pico da sua tela em nits; os brilhos mais fortes param aí.",
    [UI_MENU_HDR_GAME] = "Brilho do jogo",
    [UI_MENU_HDR_GAME_DESC] = "Nits do branco da imagem (branco de papel). 203 é a referência do HDR; aumente num "
                              "ambiente claro.",
    [UI_MENU_HDR_UI] = "Brilho do HUD e do menu",
    [UI_MENU_HDR_UI_DESC] = "Nits do branco do HUD e deste menu.",
};

#define SWKBD_ONLY(name, ...) static const char *const name[UI_TEXT_COUNT] = {SWKBD(__VA_ARGS__)}
SWKBD_ONLY(text_ja, "文字入力", "名前を入力してください", "ウマ", "OK", "キャンセル", "コントローラー: A = OK、B = キャンセル");
SWKBD_ONLY(text_fr, "Saisie de texte", "Entrez un nom", "Cheval", "OK", "Annuler", "Manette : A = OK, B = Annuler");
SWKBD_ONLY(text_de, "Texteingabe", "Gib einen Namen ein", "Pferd", "OK", "Abbrechen", "Controller: A = OK, B = Abbrechen");
SWKBD_ONLY(text_it, "Inserimento testo", "Inserisci un nome", "Cavallo", "OK", "Annulla", "Controller: A = OK, B = Annulla");
SWKBD_ONLY(text_es, "Introducir texto", "Introduce un nombre", "Caballo", "Aceptar", "Cancelar",
           "Mando: A = Aceptar, B = Cancelar");
SWKBD_ONLY(text_nl, "Tekstinvoer", "Voer een naam in", "Paard", "OK", "Annuleren", "Controller: A = OK, B = Annuleren");
SWKBD_ONLY(text_pt, "Inserir texto", "Introduz um nome", "Cavalo", "OK", "Cancelar", "Comando: A = OK, B = Cancelar");
SWKBD_ONLY(text_zh_hans, "文字输入", "请输入名字", "马儿", "确定", "取消", "手柄：A 确定，B 取消");
SWKBD_ONLY(text_zh_hant, "文字輸入", "請輸入名字", "馬兒", "確定", "取消", "手把：A 確定，B 取消");
SWKBD_ONLY(text_ko, "문자 입력", "이름을 입력하세요", "말", "확인", "취소", "컨트롤러: A 확인, B 취소");

static const Row rows[] = {
    {"en", text_en}, {"ja", text_ja}, {"fr", text_fr}, {"de", text_de}, {"it", text_it}, {"es", text_es},
    {"nl", text_nl}, {"pt", text_pt}, {"pt-BR", text_pt_br}, {"ru", text_ru}, {"zh-Hans", text_zh_hans}, {"zh-Hant", text_zh_hant},
    {"ko", text_ko},
};

const char *ui_text_lang(const char *code) {
    static const struct { const char *code, *lang; } map[] = {
        {"en-US", "en"}, {"en-GB", "en"}, {"fr-CA", "fr"}, {"es-419", "es"},
        {"zh-CN", "zh-Hans"}, {"zh-TW", "zh-Hant"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(code, map[i].code)) return map[i].lang;
    return code;
}

static const Row *find_row(const char *lang) {
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
        if (!strcmp(rows[i].lang, lang)) return &rows[i];
    return NULL;
}

int ui_text_defined(const char *lang, UiTextId id) {
    const Row *r = find_row(lang);
    return (unsigned)id < UI_TEXT_COUNT && r && r->text[id] && *r->text[id];
}

const char *ui_text_in(const char *lang, UiTextId id) {
    if ((unsigned)id >= UI_TEXT_COUNT) return "";
    return ui_text_defined(lang, id) ? find_row(lang)->text[id] : rows[0].text[id];
}

const char *ui_text(UiTextId id) { return ui_text_in(ui_text_lang(ov_language()), id); }

// 1 when the language tag is lang or a regional form of it ("pt", "pt-BR", "pt-PT" for "pt").
static int is_lang(const char *tag, const char *lang) {
    size_t n = strlen(lang);
    return tag && !strncmp(tag, lang, n) && (!tag[n] || tag[n] == '-');
}

// The menu language, re-read when cfg_version changes. TOTK_MENU_LANG "en", "ru" or "pt-BR"; anything else (unset
// included) follows the system UI language: Russian for "ru", "ru-RU", ..., Brazilian Portuguese for any Portuguese
// (the menu has no European Portuguese row), else English.
static const char *resolve_menu_lang(void) {
    const char *v = cfg_value("TOTK_MENU_LANG");
    if (v && !strcmp(v, "ru")) return "ru";
    if (v && !strcmp(v, "pt-BR")) return "pt-BR";
    if (v && !strcmp(v, "en")) return "en";
    const char *sys = plat_ui_language();
    return is_lang(sys, "ru") ? "ru" : is_lang(sys, "pt") ? "pt-BR" : "en";
}

const char *menu_text(UiTextId id) {
    static _Atomic(const char *) lang = "en";
    static _Atomic unsigned seen = ~0u;
    unsigned v = cfg_version();
    if (v != atomic_load_explicit(&seen, memory_order_relaxed)) {
        atomic_store_explicit(&lang, resolve_menu_lang(), memory_order_relaxed);
        atomic_store_explicit(&seen, v, memory_order_relaxed);
    }
    return ui_text_in(atomic_load_explicit(&lang, memory_order_relaxed), id);
}
