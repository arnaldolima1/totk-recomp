// The in-game menu core: Dear ImGui state, the event queue the window adapter fills, gamepad navigation, the Vulkan back
// end on our device and the menu's pages. Free of any window system (menu.h). ImGui runs on the present path only,
// under gfx_present.c's present lock; the event functions only queue.
//
// The Vulkan set-up follows LAS Recomp's runtime/nvn/vulkan/imgui_glue.cpp (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai): dynamic rendering into the swapchain
// image, an internal descriptor pool, vertex buffers rotated over more frames than are in flight.
//
// The look (colours, rounding, the tab bar with L / R and Q / E, the window over a dimmed game) follows
// SavageWindRecomp's runtime/ui/menu.mm (https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai); the pages are rows of option_rows.cpp.
#include <algorithm>
#include <atomic>
#include <vector>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "imgui.h"
#include "imgui_internal.h"   // SetFocusID, NavCursorVisible
#include "backends/imgui_impl_vulkan.h"
// A private copy of stb_truetype, the parser Dear ImGui builds its font atlas with, to check a system font before the
// atlas gets it (font_usable). Its asserts only mark the font bad here.
static int g_font_bad;
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((x) ? (void)0 : (void)(g_font_bad = 1))
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "imstb_truetype.h"
#pragma clang diagnostic pop
#include "ui/amiibo_page.h"
#include "ui/binds_page.h"
#include "ui/option_rows.h"

extern "C" {
#include "achievements.h"
#include "link_figure.h"
#include "net_mp.h"
#include "voice.h"
#include "amiibo.h"
#include "cheats.h"
#include "cfg.h"
#include "fs.h"
#include "input.h"
#include "input_map.h"
#include "mods.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "sysinfo.h"
#include "ui/menu.h"
#include "ui_text.h"
// host.h and vk.h pull in cpu.h, which is C only: the functions the menu needs from them, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vk_queue_lock(void);
void vk_queue_unlock(void);
uint32_t vk_pipeline_building(void);
void res_sizes(unsigned out[2], unsigned scene[2]);
double frame_step_limit_hz(void);
extern const int rt_game_version;
// host.h (frame_rate.c): game time since the loaded save's first frame, -1 before; achievement notices wait for it.
int64_t frame_step_world_us(void);
// host.h (gfx_present.c): a world position on the window from the game's camera; the friends drawn in the world.
int gfx_world_to_screen(const float world[3], float out[2], float *dist);
int gfx_camera_ground_axes(float right[2], float fwd[2]);
int gfx_hud_to_screen(float from_right, float from_bottom, float out[2], float *height);
int gfx_view(float view_proj[16], float eye[3], float map[4]);
void gfx_depth_want(const float ndc_rect[4]);
int gfx_depth_begin(void);
float gfx_depth_at(float ndc_x, float ndc_y);
void gfx_depth_end(void);
int gfx_occlusion(const float (*pts)[3], int n, float *visible);
// host.h (vk_figure.c): friends' figures drawn into the game's G-buffer.
typedef struct {
    int anim, prev_anim;             // link_figure.h LF_*: playing, and blending out of
    float frame, prev_frame, fps;    // their frames, and the frames a second both advance by
    float blend;                     // 0..1: how far into anim (it rises by 4 a second)
    float feet[3], vel[3], yaw;      // world metres, metres a second, radians (ui/menu.cpp draw_link_mesh)
    float hair[3];                   // sRGB 0..1
} GfxFigure;
int gfx_figures_wanted(void);
void gfx_figures_set(const GfxFigure *f, int n);
// audren.h (audio_mixer.c): the achievement chime over the game's audio.
void audio_chime(int platinum, float volume);
extern const char rt_port_version[];
void cheats_frame(uint64_t frame);   // cheats.c
bool tab_cheats(float scale);        // ui/cheats_tab.cpp; true: restart into the rebuild
}

// Roboto Medium (Apache-2.0, ui/fonts/LICENSE-Roboto.txt) with Latin and Cyrillic glyphs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed in C++
static const unsigned char roboto_ttf[] = {
#embed "fonts/Roboto-Medium.ttf"
};
#pragma clang diagnostic pop

// ---- open state, any thread
static std::atomic<int> g_open;     // the menu is open
static std::atomic<int> g_reset;    // opened or closed since the last laid-out frame: drop stale input
static std::atomic<int> g_chat;     // opened as the multiplayer chat box (menu_chat_open), not the whole menu

// g_open, the input block and the log line change together under open_lock, so they cannot disagree when the window thread
// (F1, Esc), the polling thread (combo) and the present path (Continue, B) race.
static PlatMutex open_lock = PLAT_MUTEX_INIT;

// want 0 or 1 sets the state; -1 flips it. Returns the new state.
static int change_open(int want) {
    plat_mutex_lock(&open_lock);
    int open = want < 0 ? !g_open.load() : want;
    if (g_open.load() != open) {
        g_open.store(open);
        if (!open) g_chat.store(0);
        g_reset.store(1);
        input_menu(open);
        rt_log("menu: %s", open ? "open" : "closed");
    }
    plat_mutex_unlock(&open_lock);
    return open;
}

static void set_open(int open) { change_open(open); }

// The whole menu shows (not the chat box alone): what the notices and the overlays make room for.
static bool full_menu(void) { return g_open.load(std::memory_order_relaxed) && !g_chat.load(std::memory_order_relaxed); }

static int g_failed_flag();
extern "C" int menu_is_open(void) { return g_open.load(std::memory_order_relaxed); }
extern "C" void menu_toggle(void) {
    if (!g_failed_flag()) change_open(-1);
}

// ---- events from the window adapter, queued while the menu is open; the present path hands them to ImGui
enum { EV_KEY, EV_POS, EV_BUTTON, EV_WHEEL, EV_FOCUS, EV_CHAR, EV_CHAR16, EV_MAX = 256 };
struct Ev { int type, a, b; float x, y; };
static PlatMutex ev_lock = PLAT_MUTEX_INIT;
static Ev ev_q[EV_MAX];
static int ev_n;

static void push(Ev e) {
    if (!menu_is_open()) return;
    plat_mutex_lock(&ev_lock);
    if (e.type == EV_POS && ev_n && ev_q[ev_n - 1].type == EV_POS) ev_q[ev_n - 1] = e;   // only the newest position
    else if (ev_n < EV_MAX) ev_q[ev_n++] = e;
    plat_mutex_unlock(&ev_lock);
}

extern "C" void menu_event_key(int key, int down) { push({EV_KEY, key, down, 0, 0}); }
extern "C" void menu_event_mouse_pos(float x, float y) { push({EV_POS, 0, 0, x, y}); }
extern "C" void menu_event_mouse_button(int button, int down) { push({EV_BUTTON, button, down, 0, 0}); }
extern "C" void menu_event_wheel(float dx, float dy) { push({EV_WHEEL, 0, 0, dx, dy}); }
extern "C" void menu_event_focus(int focused) { push({EV_FOCUS, focused, 0, 0, 0}); }
extern "C" void menu_event_char16(unsigned unit) {
    if (unit >= 0x20 && unit != 0x7f) push({EV_CHAR16, (int)unit, 0, 0, 0});
}
// UTF-8 text (SDL's text input) to code points; a malformed byte is dropped.
extern "C" void menu_event_text(const char *utf8) {
    const unsigned char *s = (const unsigned char *)utf8;
    while (*s) {
        unsigned c = *s, n = c < 0x80 ? 0 : c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 4;
        if (n == 4) { s++; continue; }
        if (n) c &= 0x3f >> n;
        s++;
        for (unsigned i = 0; i < n && (*s & 0xc0) == 0x80; i++, s++) c = c << 6 | (*s & 0x3f);
        if (c >= 0x20 && c != 0x7f) push({EV_CHAR, (int)c, 0, 0, 0});
    }
}

// Set after each laid-out frame: a text field has the keyboard (the SDL adapter turns text input on for it).
static std::atomic<int> g_wants_text;
extern "C" int menu_wants_text(void) { return g_wants_text.load(std::memory_order_relaxed); }

static void drain_events(ImGuiIO &io) {
    Ev q[EV_MAX];
    plat_mutex_lock(&ev_lock);
    int n = ev_n;
    for (int i = 0; i < n; i++) q[i] = ev_q[i];
    ev_n = 0;
    plat_mutex_unlock(&ev_lock);
    for (int i = 0; i < n; i++) {
        const Ev &e = q[i];
        switch (e.type) {
        case EV_KEY:
            io.AddKeyEvent((ImGuiKey)e.a, e.b != 0);
            // ImGui reads Ctrl, Shift and Alt from its modifier keys, not from the left/right ones: text fields need
            // them for Ctrl+V and the like, and AltGr (Ctrl+Alt) for the characters it types on ABNT2 and other layouts
            if (e.a == ImGuiKey_LeftCtrl) io.AddKeyEvent(ImGuiMod_Ctrl, e.b != 0);
            if (e.a == ImGuiKey_LeftShift) io.AddKeyEvent(ImGuiMod_Shift, e.b != 0);
            if (e.a == ImGuiKey_LeftAlt) io.AddKeyEvent(ImGuiMod_Alt, e.b != 0);
            break;
        case EV_CHAR: io.AddInputCharacter((unsigned)e.a); break;
        case EV_CHAR16: io.AddInputCharacterUTF16((ImWchar16)e.a); break;
        case EV_POS: io.AddMousePosEvent(e.x, e.y); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        case EV_FOCUS: io.AddFocusEvent(e.a != 0); break;
        }
    }
}

// ---- gamepad navigation: the Switch buttons of our SDL3 gamepad to ImGui's gamepad keys. Nintendo layout: A (east)
// activates, B (south) cancels.
static const struct { int bit; ImGuiKey key; } pad_keys[] = {
    {B_A, ImGuiKey_GamepadFaceDown}, {B_B, ImGuiKey_GamepadFaceRight}, {B_X, ImGuiKey_GamepadFaceUp},
    {B_Y, ImGuiKey_GamepadFaceLeft}, {B_L, ImGuiKey_GamepadL1}, {B_R, ImGuiKey_GamepadR1},
    {B_ZL, ImGuiKey_GamepadL2}, {B_ZR, ImGuiKey_GamepadR2}, {B_LEFT, ImGuiKey_GamepadDpadLeft},
    {B_UP, ImGuiKey_GamepadDpadUp}, {B_RIGHT, ImGuiKey_GamepadDpadRight}, {B_DOWN, ImGuiKey_GamepadDpadDown},
};
static uint64_t pad_prev;   // buttons at the last frame; a button held when the menu opened counts once released
static int stick_dir = -1;  // the D-pad button the left stick holds, -1 none

// The left stick moves the cursor as the D-pad does (ImGui's own stick handling only scrolls the page): past half way
// it holds the D-pad direction of its larger axis, kept until that axis falls under 0.35; ImGui repeats a held one.
static int stick_to_dpad(int32_t lx, int32_t ly) {
    float x = lx / 32767.0f, y = ly / 32767.0f;   // y up
    float along = stick_dir == B_LEFT ? -x : stick_dir == B_RIGHT ? x : stick_dir == B_UP ? y : stick_dir == B_DOWN ? -y : 0;
    if (stick_dir >= 0 && along > 0.35f) return stick_dir;
    if (fmaxf(fabsf(x), fabsf(y)) < 0.5f) return -1;
    return fabsf(x) > fabsf(y) ? (x < 0 ? B_LEFT : B_RIGHT) : (y > 0 ? B_UP : B_DOWN);
}

static void pad_events(ImGuiIO &io, int reset) {
    uint64_t b;
    int32_t lx, ly;
    int have = input_pad_read(&b, &lx, &ly);
    if (have) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    stick_dir = stick_to_dpad(lx, ly);
    if (stick_dir >= 0) b |= 1ull << stick_dir;
    if (reset) {
        pad_prev = b;
        return;
    }
    for (const auto &k : pad_keys) {
        uint64_t m = 1ull << k.bit;
        if ((b ^ pad_prev) & m) io.AddKeyEvent(k.key, (b & m) != 0);
    }
    pad_prev = b;
}

// ---- ImGui and its Vulkan back end (present path)
static int g_ctx_ok;              // context made
static std::atomic<int> g_failed; // set-up failed for good (the menu stays closed)
static int g_failed_flag() { return g_failed.load(std::memory_order_relaxed); }
static int g_vk_ok;               // back end initialised for g_vk_fmt
static VkFormat g_vk_fmt;
static float g_font_scale;        // the scale of the uploaded font atlas; 0 when none is uploaded
static float g_style_scale;       // the scale the style was last sized for
static ImGuiStyle g_base_style;   // the menu's style at 1080 lines
static uint64_t g_last_ns;
static uint64_t g_cost_ns, g_cost_frames;   // CPU time of layout and recording while open, logged per 600 frames

// Sizes are tuned for 1080 lines and follow the window's height. Under 720 lines at 100% display scale they stay at the
// 720 size; that floor grows with the window's DPI, so a small window on a 150% or 200% display (sizes are physical
// pixels) still gets text as large as the display scale asks for.
static float menu_scale(uint32_t h) {
    float least = 720.0f * (float)gfx_window_dpi() / 96.0f;
    return ((float)h >= least ? (float)h : least) / 1080.0f;
}

// The colours of option_rows.h: near-black green, gold buttons and lines, the Zonai teal for the cursor.
static void style_init(ImGuiStyle &s) {
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 12; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.PopupRounding = 12;
    s.WindowPadding = ImVec2(28, 22); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.PopupBorderSize = 1; s.ScrollbarSize = 14; s.ScrollbarRounding = 7;
    auto a = [](ImVec4 c, float alpha) { c.w = alpha; return c; };
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = kUiPanel;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.065f, 0.058f, 0.98f);
    c[ImGuiCol_Border] = a(kUiGold, 0.30f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
    c[ImGuiCol_Text] = kUiCream;
    c[ImGuiCol_TextDisabled] = kUiMuted;
    c[ImGuiCol_Button] = a(kUiGold, 0.14f);
    c[ImGuiCol_ButtonHovered] = a(kUiGold, 0.30f);
    c[ImGuiCol_ButtonActive] = a(kUiGold, 0.45f);
    c[ImGuiCol_NavCursor] = kUiZonai;
    c[ImGuiCol_Separator] = a(kUiGold, 0.18f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = a(kUiGold, 0.22f);
    c[ImGuiCol_ScrollbarGrabHovered] = a(kUiGold, 0.38f);
    c[ImGuiCol_ScrollbarGrabActive] = a(kUiZonai, 0.6f);
}

static void make_context(void) {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "totk_menu";
    style_init(g_base_style);
    ImGui::GetStyle() = g_base_style;
    g_ctx_ok = 1;
}

static void vk_down(void) {
    if (!g_vk_ok) return;
    ImGui_ImplVulkan_Shutdown();
    g_vk_ok = 0;
    g_font_scale = 0;
}

static int vk_up(const MenuTarget *t) {
    if (g_vk_ok && g_vk_fmt == t->format) return 1;
    if (g_vk_ok) {   // the swapchain changed format: pipelines for the new one
        vk_queue_lock();
        vkQueueWaitIdle(t->queue);
        vk_queue_unlock();
        vk_down();
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = t->instance;
    ii.PhysicalDevice = t->phys;
    ii.Device = t->device;
    ii.QueueFamily = t->queue_family;
    ii.Queue = t->queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 4;   // vertex buffers rotate per recorded frame: more than gfx_present.c's 2 frames in flight
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    g_vk_fmt = t->format;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return 0;
    g_vk_ok = 1;
    return 1;
}

// Roboto Medium (Latin, Cyrillic, dashes and quotes) at 26 px per 1080 lines, with the glyphs of the Japanese, Korean
// and Chinese language names merged in from a system font when there is one (each language is named in its own script).
// A catalog row also holds menu text (Simplified Chinese), so every string of the row goes into the builder: the merged
// font must cover the whole menu of that language, not only its name. Made again when the window height changes; the old
// texture goes after the queue is idle (CreateFontsTexture).
static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2010, 0x2027, 0};
static const struct { const char *lang; UiTextId name; } kCjk[] = {
    {"ja", UI_MENU_LANGNAME_JA}, {"ko", UI_MENU_LANGNAME_KO}, {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS},
    {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT},
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// 1 when Dear ImGui can take this font file: more than the 100 bytes AddFontFromMemoryTTF asserts on, every table of its
// directory inside the file (stb_truetype reads tables without bounds checks, so a cut-off file faults when the glyphs
// are made), and stb_truetype parses it. ImGui asserts on a font it cannot parse, and an assert ends the process with
// 0xc0000409 and no line in the log.
static int font_usable(const uint8_t *d, uint64_t n) {
    if (!d || n <= 100 || n > INT_MAX) return 0;
    int off = stbtt_GetFontOffsetForIndex(d, 0);
    if (off < 0 || (uint64_t)off + 12 > n) return 0;
    uint32_t tables = (uint32_t)d[off + 4] << 8 | d[off + 5];
    if ((uint64_t)off + 12 + 16ull * tables > n) return 0;
    for (uint32_t i = 0; i < tables; i++) {
        const uint8_t *r = d + off + 12 + 16 * i;
        if ((uint64_t)be32(r + 8) + be32(r + 12) > n) return 0;
    }
    stbtt_fontinfo fi;
    g_font_bad = 0;
    return stbtt_InitFont(&fi, d, off) && !g_font_bad;
}

extern "C" int menu_font_check(void) {
    uint64_t n = sizeof roboto_ttf;
    uint8_t *bad = (uint8_t *)malloc(n);
    if (!bad) return 0;
    memcpy(bad, roboto_ttf, n);
    uint32_t tables = (uint32_t)bad[4] << 8 | bad[5];
    for (uint32_t i = 0; i < tables; i++)
        if (!memcmp(bad + 12 + 16 * i, "cmap", 4)) memset(bad + 12 + 16 * i, 0, 4);   // no character map
    int ok = font_usable(roboto_ttf, n) && !font_usable(roboto_ttf, 4096) && !font_usable(roboto_ttf, 100) &&
             !font_usable(bad, n);
    memset(bad, 0, 4);             // an unknown font tag
    ok = ok && !font_usable(bad, n);
    free(bad);
    return ok;
}

// The achievement icons (achievements.c, 64x64 RGBA each) ride in the font atlas as custom rectangles, so they reach the
// GPU with the glyphs and draw with the atlas texture: no texture of their own. g_ach_uv: each icon's place, made with
// the atlas.
enum { kAchMax = 512 };
static ImVec2 g_ach_uv0[kAchMax], g_ach_uv1[kAchMax];
static bool g_ach_icon[kAchMax];

static void ach_icons_add(ImFontAtlas *fa, int *rect) {
    int n = ach_count();
    for (int i = 0; i < kAchMax; i++) {
        AchView v;
        rect[i] = i < n && ach_get(i, &v) && v.icon ? fa->AddCustomRectRegular(ACH_ICON, ACH_ICON) : -1;
    }
}

static void ach_icons_fill(ImFontAtlas *fa, const int *rect) {
    unsigned char *px;
    int tw, th;
    fa->GetTexDataAsRGBA32(&px, &tw, &th);
    for (int i = 0; i < kAchMax; i++) {
        AchView v;
        g_ach_icon[i] = false;
        if (rect[i] < 0 || !ach_get(i, &v) || !v.icon) continue;
        const ImFontAtlasCustomRect *r = fa->GetCustomRectByIndex(rect[i]);
        if (!r || !r->IsPacked()) continue;
        for (int y = 0; y < ACH_ICON; y++)
            memcpy(px + ((size_t)(r->Y + y) * tw + r->X) * 4, v.icon + (size_t)y * ACH_ICON * 4, ACH_ICON * 4);
        fa->CalcCustomRectUV(r, &g_ach_uv0[i], &g_ach_uv1[i]);
        g_ach_icon[i] = true;
    }
}

// Link's texture atlas for the friends' figures (link_figure.c) rides in the font atlas as one more custom rectangle,
// like the icons: g_fig_uv0/1 is its place there, g_fig_tex 0 until it is packed.
static ImVec2 g_fig_uv0, g_fig_uv1;
static bool g_fig_tex;

// The glyphs of every string of a catalog row (ui_text_visit), for the font that draws that language.
static void add_row_glyphs(void *ctx, const char *text) {
    static_cast<ImFontGlyphRangesBuilder *>(ctx)->AddText(text);
}

static int fonts_up(float scale) {
    enum { N = sizeof kCjk / sizeof *kCjk };
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    fa->Clear();
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, 26.0f * scale, &fc, kRanges);
    uint8_t *data[N] = {};
    ImVector<ImWchar> ranges[N];
    int merged = 0;
    for (int i = 0; i < N; i++) {
        char path[512];
        uint64_t size = 0;
        if (!plat_font_path(kCjk[i].lang, path, sizeof path) || !(data[i] = plat_file_read(path, &size))) continue;
        if (!font_usable(data[i], size)) {
            rt_log("menu: system font %s left out: the font parser cannot read it (%llu bytes)", path,
                   (unsigned long long)size);
            free(data[i]);
            data[i] = NULL;
            continue;
        }
        merged++;
        ImFontGlyphRangesBuilder b;
        b.AddText(ui_text_in("en", kCjk[i].name));
        ui_text_visit(kCjk[i].lang, add_row_glyphs, &b);
        b.BuildRanges(&ranges[i]);
        ImFontConfig mc;
        mc.MergeMode = true;
        mc.FontDataOwnedByAtlas = false;
        fa->AddFontFromMemoryTTF(data[i], (int)size, 26.0f * scale, &mc, ranges[i].Data);
    }
    static int ach_rect[kAchMax];
    ach_icons_add(fa, ach_rect);
    const LinkFigure *lf = link_figure();
    int fig_rect = lf ? fa->AddCustomRectRegular((int)lf->atlas_side, (int)lf->atlas_side) : -1;
    // the atlas width follows the glyphs alone (1024 or less here): wide enough for Link's atlas next to them
    fa->TexDesiredWidth = lf ? (int)lf->atlas_side * 2 : 0;
    fa->Build();
    ach_icons_fill(fa, ach_rect);
    g_fig_tex = false;
    if (fig_rect >= 0) {
        unsigned char *px;
        int tw, th;
        fa->GetTexDataAsRGBA32(&px, &tw, &th);
        const ImFontAtlasCustomRect *r = fa->GetCustomRectByIndex(fig_rect);
        if (r && r->IsPacked()) {
            for (uint32_t y = 0; y < lf->atlas_side; y++)
                memcpy(px + ((size_t)(r->Y + y) * tw + r->X) * 4, lf->atlas + (size_t)y * lf->atlas_side * 4, lf->atlas_side * 4);
            fa->CalcCustomRectUV(r, &g_fig_uv0, &g_fig_uv1);
            g_fig_tex = true;
        }
        rt_log("menu: Link's texture atlas %s the font atlas (%dx%d)", g_fig_tex ? "packed into" : "did not fit", tw, th);
    }
    vk_queue_lock();   // the upload submits on the queue and waits for it
    int ok = ImGui_ImplVulkan_CreateFontsTexture();
    vk_queue_unlock();
    fa->ClearInputData();   // the glyphs are in the texture: the font files and the pixels are not needed again
    fa->ClearTexData();
    for (uint8_t *d : data) free(d);
    rt_log("menu: font atlas at %.1f px, language names from %d of %d system fonts", 26.0f * scale, merged, (int)N);
    g_font_scale = ok ? scale : 0;
    return ok;
}

extern "C" void menu_vk_release(void) {
    if (g_ctx_ok) vk_down();
}

// ---- test hooks
// TOTK_MENU_TEST=<frame>[:<frames per page>]: at that presented frame the menu opens on its first tab; each tab stays
// <frames per page> frames (default 30) and its last frame's swapchain image goes to TOTK_MENU_TEST_DIR (default: the
// working directory) as menu-test-<frame>-<page>.png; after the last tab the menu closes.
// TOTK_MENU_TEST_SET=<frame>:<key>=<value>[,<key>=<value>...][;<frame>:...]: cfg_set of each key (TOTK_ prefix
// optional) at that presented frame; changes live settings with no input, menu open or not. totk.ini is written only by
// the pseudo-key SAVE=1 (cfg_save, as a row change does; a failed write shows the menu's save note).
enum { TAB_SCREEN, TAB_GAME, TAB_SOUND, TAB_CONTROLS, TAB_AMIIBO, TAB_MENU, TAB_ADVANCED, TAB_MODS, TAB_CHEATS,
       TAB_ACHIEVEMENTS, TAB_MULTIPLAYER, TAB_COUNT };
enum { TEST_PAGE_FRAMES = 30, TEST_SETS = 16 };
struct TestSet { uint64_t frame; char key[64], value[64]; };
static struct {
    int parsed;
    uint64_t at;          // TOTK_MENU_TEST frame; 0 none
    uint64_t per_page;
    uint64_t opened;      // the frame the hook opened the menu; 0 not yet
    int page;             // the tab being shot; -1 none
    TestSet sets[TEST_SETS];
    int n_sets, next_set; // TOTK_MENU_TEST_SET, in frame order
} g_test = {0, 0, 0, 0, -1, {}, 0, 0};

static void test_parse(void) {
    g_test.parsed = 1;
    if (const char *e = cfg_get("TOTK_MENU_TEST")) {
        long long at = atoll(e), per = 0;
        if (const char *c = strchr(e, ':')) per = atoll(c + 1);
        g_test.at = at > 0 ? (uint64_t)at : 0;
        g_test.per_page = per > 0 ? (uint64_t)per : TEST_PAGE_FRAMES;
    }
    const char *s = cfg_get("TOTK_MENU_TEST_SET");
    while (s && *s) {   // groups "<frame>:<key>=<value>,..." separated by ';'
        char *end;
        unsigned long long f = strtoull(s, &end, 10);
        if (*end != ':') break;
        s = end + 1;
        while (*s && *s != ';' && g_test.n_sets < TEST_SETS) {
            size_t n = strcspn(s, ",;");
            const char *eq = (const char *)memchr(s, '=', n);
            if (eq) {
                TestSet &t = g_test.sets[g_test.n_sets++];
                t.frame = f;
                int kn = (int)(eq - s), vn = (int)(s + n - eq - 1);
                snprintf(t.key, sizeof t.key, "%s%.*s", strncmp(s, "TOTK_", 5) ? "TOTK_" : "", kn, s);
                snprintf(t.value, sizeof t.value, "%.*s", vn, eq + 1);
            }
            s += n;
            if (*s == ',') s++;
        }
        while (*s && *s != ';') s++;
        if (*s == ';') s++;
    }
    if (g_test.at || g_test.n_sets)
        rt_log("menu: test hook: open at frame %llu, %llu frames per page, %d settings changes",
               (unsigned long long)g_test.at, (unsigned long long)g_test.per_page, g_test.n_sets);
}

static void test_apply_sets(uint64_t frame) {
    for (; g_test.next_set < g_test.n_sets && g_test.sets[g_test.next_set].frame <= frame; g_test.next_set++) {
        const TestSet &t = g_test.sets[g_test.next_set];
        rt_log("menu: test hook: frame %llu: %s=%s", (unsigned long long)frame, t.key, t.value);
        if (!strcmp(t.key, "TOTK_SAVE")) rt_log("menu: test hook: cfg_save %s", cfg_save() ? "wrote" : "failed");
        else cfg_set(t.key, t.value);
    }
}

// ---- the lists the Screen and Game tabs build
enum { MAX_GPUS = 8 };
static char g_gpu_names[MAX_GPUS][VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
static int g_gpu_n = -1;   // -1: not listed yet

// The Vulkan devices in vkEnumeratePhysicalDevices order, which TOTK_GPU_INDEX counts in (vk_device.c pick_device).
static void list_gpus(VkInstance inst) {
    if (g_gpu_n >= 0) return;
    VkPhysicalDevice pd[MAX_GPUS];
    uint32_t n = MAX_GPUS;
    if (vkEnumeratePhysicalDevices(inst, &n, pd) < 0) n = 0;   // VK_INCOMPLETE keeps the first MAX_GPUS
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        snprintf(g_gpu_names[i], sizeof g_gpu_names[i], "%s", p.deviceName);
    }
    g_gpu_n = (int)n;
}

// Window sizes: the common 16:9 sizes that fit the monitor, the 1920x1080 default and the current size.
static const int kSizes[][2] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800},
                                {3840, 2160}};
enum { MAX_SIZES = sizeof kSizes / sizeof *kSizes + 1 };

static void row_window_size(void) {
    int sz[MAX_SIZES][2], n = 0, mw = 0, mh = 0, cw = 0, ch = 0;
    int have = gfx_window_monitor_size(&mw, &mh);
    int cur_ok = sscanf(cfg_value("TOTK_WINDOW_SIZE"), "%dx%d", &cw, &ch) == 2;
    bool listed = false;
    for (size_t i = 0; i < sizeof kSizes / sizeof *kSizes; i++) {
        int w = kSizes[i][0], h = kSizes[i][1];
        bool cur = cur_ok && w == cw && h == ch;
        if (have && (w > mw || h > mh) && !cur && !(w == 1920 && h == 1080)) continue;
        listed |= cur;
        sz[n][0] = w, sz[n][1] = h, n++;
    }
    if (cur_ok && !listed) {   // the current size where its area puts it
        int at = n;
        while (at > 0 && (long)sz[at - 1][0] * sz[at - 1][1] > (long)cw * ch) at--;
        memmove(sz[at + 1], sz[at], sizeof sz[0] * (n - at));
        sz[at][0] = cw, sz[at][1] = ch, n++;
    }
    char vals[MAX_SIZES][24], shown[MAX_SIZES][32];
    const char *pv[MAX_SIZES], *pn[MAX_SIZES];
    for (int i = 0; i < n; i++) {
        snprintf(vals[i], sizeof vals[i], "%dx%d", sz[i][0], sz[i][1]);
        snprintf(shown[i], sizeof shown[i], "%d × %d", sz[i][0], sz[i][1]);   // U+00D7, the multiplication sign
        pv[i] = vals[i];
        pn[i] = shown[i];
    }
    opt_choice_list("TOTK_WINDOW_SIZE", UI_MENU_WINDOW_SIZE, pv, pn, n, UI_MENU_WINDOW_SIZE_DESC);
}

// The game resolution (resolution.c): 16:9 sizes from its smallest, 1280x720 (for weak GPUs), up, the common 21:9 and
// 32:9 monitor sizes and the 16:10 screens of the Steam Deck, handhelds and laptops (the camera and the HUD follow
// another shape, resolution.c res_init; the installer lays the HUD out for each, installer/steps.py ASPECT_SIZES), and
// the current value when it is another size.
static void row_output_res(void) {
    static const char *const vals[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3200x1800", "3840x2160",
                                       "2560x1080", "3440x1440", "3840x1600", "5120x2160", "3840x1080", "5120x1440",
                                       "1280x800", "1680x1050", "1920x1200", "2560x1600"};
    enum { N = sizeof vals / sizeof *vals };
    char shown[N][32];
    const char *names[N];
    for (int i = 0; i < N; i++) {
        int w = 0, h = 0;
        sscanf(vals[i], "%dx%d", &w, &h);
        snprintf(shown[i], sizeof shown[i], "%d × %d", w, h);   // U+00D7, the multiplication sign
        names[i] = shown[i];
    }
    opt_choice_list("TOTK_OUTPUT_RES", UI_MENU_OUTPUT_RES, vals, names, N, UI_MENU_OUTPUT_RES_DESC);
}

static void row_render_scale(void) {
    static const char *const vals[] = {"game", "50", "67", "75", "100", "125", "150", "200"};
    enum { N = sizeof vals / sizeof *vals };
    char shown[N][32];
    const char *names[N];
    names[0] = menu_text(UI_MENU_RENDER_SCALE_GAME);
    for (int i = 1; i < N; i++) {
        snprintf(shown[i], sizeof shown[i], "%s %%", vals[i]);
        names[i] = shown[i];
    }
    opt_choice_list("TOTK_RENDER_SCALE", UI_MENU_RENDER_SCALE, vals, names, N, UI_MENU_RENDER_SCALE_DESC);
}

static void row_shadow_res(void) {
    static const char *const vals[] = {"game", "512", "768", "1024", "1536", "2048"};
    enum { N = sizeof vals / sizeof *vals };
    const char *names[N];
    names[0] = menu_text(UI_MENU_SHADOW_RES_GAME);
    for (int i = 1; i < N; i++) names[i] = vals[i];
    opt_choice_list("TOTK_SHADOW_RES", UI_MENU_SHADOW_RES, vals, names, N, UI_MENU_SHADOW_RES_DESC);
}

static void row_shadow_scale(void) {
    static const char *const vals[] = {"1", "2", "3", "4"};
    static const char *const names[] = {"960 (1x)", "1920 (2x)", "2880 (3x)", "3840 (4x)"};
    opt_choice_list("TOTK_SHADOW_SCALE", UI_MENU_SHADOW_SCALE, vals, names, 4, UI_MENU_SHADOW_SCALE_DESC);
}

static void row_shadow_range(void) {
    static const char *const vals[] = {"100", "150", "200", "300"};
    static const char *const names[] = {"160 m (1x)", "240 m (1.5x)", "320 m (2x)", "480 m (3x)"};
    opt_choice_list("TOTK_SHADOW_RANGE", UI_MENU_SHADOW_RANGE, vals, names, 4, UI_MENU_SHADOW_RANGE_DESC);
}

static void row_gpu(void) {
    const char *vals[MAX_GPUS + 1], *names[MAX_GPUS + 1];
    char idx[MAX_GPUS][4];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GPU_AUTO);
    for (int i = 0; i < g_gpu_n; i++) {
        snprintf(idx[i], sizeof idx[i], "%d", i);
        vals[i + 1] = idx[i];
        names[i + 1] = g_gpu_names[i];
    }
    opt_choice_list("TOTK_GPU_INDEX", UI_MENU_GPU, vals, names, g_gpu_n + 1, UI_MENU_GPU_DESC);
}

// The game language: one entry per language TotK ships (a Switch code that names it), each under its own name. The
// Switch's Portuguese is not among them. A translation mod the launcher turned on (TOTK_LANG_MOD "<code>:<name>",
// launcher/src/main.js syncLangMod) puts its text under the language it replaces: that entry shows the translation's
// name and sets the mod's code (en-GB for a mod of the British English text).
static void row_game_lang(void) {
    static const struct { const char *code; UiTextId name; } langs[] = {
        {"en-US", UI_MENU_LANGNAME_EN}, {"ja", UI_MENU_LANGNAME_JA}, {"fr", UI_MENU_LANGNAME_FR},
        {"de", UI_MENU_LANGNAME_DE}, {"it", UI_MENU_LANGNAME_IT}, {"es", UI_MENU_LANGNAME_ES},
        {"nl", UI_MENU_LANGNAME_NL}, {"ru", UI_MENU_LANGNAME_RU},
        {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS}, {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT}, {"ko", UI_MENU_LANGNAME_KO},
    };
    enum { N = sizeof langs / sizeof *langs + 1 };
    const char *vals[N], *names[N];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GAME_LANG_AUTO);
    const char *now = cfg_value("TOTK_LANG");
    static char mod_code[16];
    const char *mod = cfg_get("TOTK_LANG_MOD"), *mod_name = NULL;
    const char *colon = mod ? strchr(mod, ':') : NULL;
    if (colon && colon[1] && colon > mod && (size_t)(colon - mod) < sizeof mod_code) {
        memcpy(mod_code, mod, colon - mod);
        mod_code[colon - mod] = 0;
        mod_name = colon + 1;
    }
    int cur = *now ? -1 : 0;
    for (int i = 1; i < N; i++) {
        vals[i] = langs[i - 1].code;
        names[i] = menu_text(langs[i - 1].name);
        if (mod_name && !strcmp(ui_text_lang(mod_code), ui_text_lang(vals[i]))) {
            vals[i] = mod_code;
            names[i] = mod_name;
        }
        if (*now && !strcmp(ui_text_lang(now), ui_text_lang(vals[i]))) cur = i;   // en-GB is English too
    }
    opt_choice_list("TOTK_LANG", UI_MENU_GAME_LANG, vals, names, N, UI_MENU_GAME_LANG_DESC, cur);
}

// The frame limit (frame_rate.c): unset, the older fps60 = 0 still means 30. The monitor choice shows the rate it gives.
static void row_fps_limit() {
    // 40 and 45: the Steam Deck's screen runs at 40 to 60 Hz (the OLED to 90); 40 divides 40 and 120 Hz evenly
    static const char *const vals[] = {"30", "40", "45", "60", "90", "120", "144", "display"};
    static const UiTextId ids[] = {UI_MENU_FPS_30, UI_MENU_FPS_40,  UI_MENU_FPS_45,  UI_MENU_FPS_60,
                                   UI_MENU_FPS_90, UI_MENU_FPS_120, UI_MENU_FPS_144, UI_MENU_FPS_DISPLAY};
    enum { N = sizeof vals / sizeof *vals };
    const char *names[N];
    for (int i = 0; i < N; i++) names[i] = menu_text(ids[i]);
    static char display[64];
    double hz = gfx_window_refresh_hz();
    if (hz > 0) {
        snprintf(display, sizeof display, menu_text(UI_MENU_FPS_DISPLAY_HZ), hz);
        names[N - 1] = display;
    }
    const char *set = cfg_get("TOTK_FPS_LIMIT"), *old = cfg_get("TOTK_FPS60");
    int cur = (!set || !*set) && old && !strcmp(old, "0") ? 0 : -1;
    opt_choice_list("TOTK_FPS_LIMIT", UI_MENU_FPS_LIMIT, vals, names, N, UI_MENU_FPS_LIMIT_DESC, cur);
}

// The menu's name of a TOTK_VK_PRESENT mode (gfx_window_present_mode); NULL for another.
static const char *present_name(const char *mode) {
    static const struct { const char *mode; UiTextId name; } names[] = {
        {"mailbox", UI_MENU_PRESENT_MAILBOX}, {"fifo", UI_MENU_PRESENT_VSYNC}, {"immediate", UI_MENU_PRESENT_IMMEDIATE},
    };
    for (const auto &n : names)
        if (!strcmp(n.mode, mode)) return menu_text(n.name);
    return nullptr;
}

// "in use: VSync" under the Presentation value when the swapchain got another mode than the setting (a driver without
// mailbox gets fifo); NULL when they agree or before the first swapchain.
static const char *present_note(void) {
    const char *got = gfx_window_present_mode(), *name = present_name(got);
    if (!*got || !strcmp(got, cfg_value("TOTK_VK_PRESENT")) || !name) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PRESENT_IN_USE), name);
    return note;
}

// ---- the tabs
// Colour adjustment (vk_color.c): a profile writes the four values (STS78721's presets, issue #136); moving one of them
// makes the profile Custom.
static void rows_color(void) {
    static const struct { const char *name, *b, *c, *s, *g; } presets[] = {
        {"default", "0", "100", "100", "1"}, {"vivid", "0", "115", "135", "0.95"}, {"clarity", "2", "110", "115", "1"}};
    static const UiTextId names[] = {UI_MENU_COLOR_DEFAULT, UI_MENU_COLOR_VIVID, UI_MENU_COLOR_CLARITY,
                                     UI_MENU_COLOR_CUSTOM};
    opt_group(UI_MENU_COLOR);
    if (opt_choice("TOTK_COLOR_PRESET", UI_MENU_COLOR_PRESET, names, 4, UI_MENU_COLOR_PRESET_DESC)) {
        const char *p = cfg_value("TOTK_COLOR_PRESET");
        for (const auto &e : presets)
            if (!strcmp(p, e.name)) {
                cfg_set("TOTK_COLOR_BRIGHTNESS", e.b), cfg_set("TOTK_COLOR_CONTRAST", e.c);
                cfg_set("TOTK_COLOR_SATURATION", e.s), cfg_set("TOTK_COLOR_GAMMA", e.g);
                cfg_save();
            }
    }
    bool moved = opt_slider("TOTK_COLOR_BRIGHTNESS", UI_MENU_COLOR_BRIGHTNESS, -30, 30, UI_MENU_COLOR_BRIGHTNESS_DESC, 1,
                            "%+.0f %%");
    moved |= opt_slider("TOTK_COLOR_CONTRAST", UI_MENU_COLOR_CONTRAST, 70, 150, UI_MENU_COLOR_CONTRAST_DESC, 5, "%.0f %%");
    moved |= opt_slider("TOTK_COLOR_SATURATION", UI_MENU_COLOR_SATURATION, 50, 200, UI_MENU_COLOR_SATURATION_DESC, 5,
                        "%.0f %%");
    moved |= opt_slider("TOTK_COLOR_GAMMA", UI_MENU_COLOR_GAMMA, 0.8f, 1.2f, UI_MENU_COLOR_GAMMA_DESC, 0.02f, "%.2f");
    if (moved && strcmp(cfg_value("TOTK_COLOR_PRESET"), "custom")) {
        cfg_set("TOTK_COLOR_PRESET", "custom");
        cfg_save();
    }
}

static void tab_screen(float scale) {
    opt_begin("##screen", scale);
    opt_toggle("TOTK_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
    opt_toggle("TOTK_BORDERLESS", UI_MENU_BORDERLESS, UI_MENU_BORDERLESS_DESC);
    row_window_size();
    static const UiTextId present[] = {UI_MENU_PRESENT_MAILBOX, UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_IMMEDIATE};
    opt_choice("TOTK_VK_PRESENT", UI_MENU_PRESENT, present, 3, UI_MENU_PRESENT_DESC, present_note());
    row_gpu();
    static const UiTextId perf[] = {UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL};
    opt_toggle("TOTK_SHADER_TOAST", UI_MENU_SHADER_TOAST, UI_MENU_SHADER_TOAST_DESC);
    opt_toggle("TOTK_ACH_POPUP", UI_MENU_ACH_POPUP, UI_MENU_ACH_POPUP_DESC);
    opt_toggle("TOTK_ACH_SOUND", UI_MENU_ACH_SOUND, UI_MENU_ACH_SOUND_DESC);
    opt_choice("TOTK_PERF_OVERLAY", UI_MENU_PERF_OVERLAY, perf, 3, UI_MENU_PERF_OVERLAY_DESC);
    opt_group(UI_MENU_GRAPHICS);
    row_output_res();
    row_render_scale();
    row_shadow_res();
    static const UiTextId up[] = {UI_MENU_UPSCALER_FSR, UI_MENU_UPSCALER_BILINEAR};
    opt_choice("TOTK_UPSCALER", UI_MENU_UPSCALER, up, 2, UI_MENU_UPSCALER_DESC);
    opt_slider("TOTK_SHARPNESS", UI_MENU_SHARPNESS, 0, 100, UI_MENU_SHARPNESS_DESC, 5, "%.0f %%");
    static const UiTextId aspect[] = {UI_MENU_ASPECT_FIT, UI_MENU_ASPECT_STRETCH};
    opt_choice("TOTK_ASPECT", UI_MENU_ASPECT, aspect, 2, UI_MENU_ASPECT_DESC);
    opt_slider("TOTK_FOV", UI_MENU_FOV, 70, 140, UI_MENU_FOV_DESC, 5, "%.0f %%");
    static const char *const aniso[] = {"game", "2", "4", "8", "16"};
    const char *aniso_names[] = {menu_text(UI_MENU_ANISO_GAME), "2x", "4x", "8x", "16x"};
    opt_choice_list("TOTK_ANISO", UI_MENU_ANISO, aniso, aniso_names, 5, UI_MENU_ANISO_DESC);
    row_shadow_scale();
    row_shadow_range();
    opt_slider("TOTK_PCSS", UI_MENU_PCSS, 0, 200, UI_MENU_PCSS_DESC, 10, "%.0f %%");
    opt_slider("TOTK_CONTACT_SHADOWS", UI_MENU_CONTACT_SHADOWS, 0, 100, UI_MENU_CONTACT_SHADOWS_DESC, 10, "%.0f %%");
    opt_toggle("TOTK_DLAA", UI_MENU_DLAA, UI_MENU_DLAA_DESC);
    static const UiTextId dlss[] = {UI_MENU_DLSS_DLAA, UI_MENU_DLSS_QUALITY, UI_MENU_DLSS_BALANCED,
                                    UI_MENU_DLSS_PERFORMANCE};
    opt_choice("TOTK_DLSS", UI_MENU_DLSS, dlss, 4, UI_MENU_DLSS_DESC);
    opt_toggle("TOTK_FXAA", UI_MENU_FXAA, UI_MENU_FXAA_DESC);
    rows_color();
    opt_group(UI_MENU_HDR_GROUP);
    opt_toggle("TOTK_HDR", UI_MENU_HDR, UI_MENU_HDR_DESC);
    opt_slider("TOTK_HDR_PEAK_NITS", UI_MENU_HDR_PEAK, 400, 10000, UI_MENU_HDR_PEAK_DESC, 50, "%.0f");
    opt_slider("TOTK_HDR_GAME_NITS", UI_MENU_HDR_GAME, 80, 500, UI_MENU_HDR_GAME_DESC, 5, "%.0f");
    opt_slider("TOTK_HDR_UI_NITS", UI_MENU_HDR_UI, 80, 500, UI_MENU_HDR_UI_DESC, 5, "%.0f");
    opt_end();
}

static void tab_game(float scale) {
    opt_begin("##game", scale);
    row_fps_limit();
    row_game_lang();
    static const UiTextId mode[] = {UI_MENU_MODE_DOCKED, UI_MENU_MODE_HANDHELD};
    opt_choice("TOTK_OPERATION_MODE", UI_MENU_OPERATION_MODE, mode, 2, UI_MENU_OPERATION_MODE_DESC);
    opt_toggle("TOTK_ASSUME_FOCUS", UI_MENU_ASSUME_FOCUS, UI_MENU_ASSUME_FOCUS_DESC);
    opt_end();
}

static void tab_sound(float scale) {
    opt_begin("##sound", scale);
    opt_slider("TOTK_VOLUME", UI_MENU_VOLUME, 0, 100, UI_MENU_VOLUME_DESC, 5, "%.0f %%");
    opt_group(UI_MENU_VOICE_GROUP);
    static const char *const chat_keys[] = {"t", "y", "enter", "off"};
    const char *chat_names[] = {"T", "Y", "Enter", menu_text(UI_MENU_OFF)};
    opt_choice_list("TOTK_MP_CHAT_KEY", UI_MENU_MP_CHAT_KEY, chat_keys, chat_names, 4, UI_MENU_MP_CHAT_KEY_DESC);
    opt_toggle("TOTK_MP_VOICE", UI_MENU_MP_VOICE, UI_MENU_MP_VOICE_DESC);
    static const UiTextId modes[] = {UI_MENU_MP_VOICE_PUSH, UI_MENU_MP_VOICE_OPEN};
    opt_choice("TOTK_MP_VOICE_MODE", UI_MENU_MP_VOICE_MODE, modes, 2, UI_MENU_MP_VOICE_MODE_DESC);
    static const char *const voice_keys[] = {"v", "b", "g", "h", "capslock"};
    static const char *const voice_names[] = {"V", "B", "G", "H", "Caps Lock"};
    opt_choice_list("TOTK_MP_VOICE_KEY", UI_MENU_MP_VOICE_KEY, voice_keys, voice_names, 5, UI_MENU_MP_VOICE_KEY_DESC);
    opt_slider("TOTK_MP_VOICE_VOL", UI_MENU_MP_VOICE_VOL, 0, 200, UI_MENU_MP_VOICE_VOL_DESC, 10, "%.0f %%");
    opt_toggle("TOTK_MP_RADAR", UI_MENU_MP_RADAR, UI_MENU_MP_RADAR_DESC);
    static const char *const mark_keys[] = {"g", "b", "h", "n", "off"};
    const char *mark_names[] = {"G", "B", "H", "N", menu_text(UI_MENU_OFF)};
    opt_choice_list("TOTK_MP_MARK_KEY", UI_MENU_MP_MARK_KEY, mark_keys, mark_names, 5, UI_MENU_MP_MARK_KEY_DESC);
    opt_end();
}

// A confirmation is open (Controls: restart, Advanced: reset): B and Esc close it, not the menu; L / R stay on the page.
static bool g_modal;

// The note under the layout: Automatic with a gamepad connected, the layout it picked ("in use: Xbox"). NULL otherwise.
static const char *pad_layout_note(void) {
    int got = input_pad_layout();
    if (got < 0 || strcmp(cfg_value("TOTK_PAD_LAYOUT"), "auto")) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PAD_LAYOUT_IN_USE),
             menu_text(got == PAD_LAYOUT_XBOX ? UI_MENU_PAD_LAYOUT_XBOX : UI_MENU_PAD_LAYOUT_NINTENDO));
    return note;
}

// The action a key presses, or -1: the walk key is a key of its own, but Shift is B in the default bindings, and a
// walk key that also presses something walks and does that something at the same time. The bindings come from a
// snapshot, and every key of an action is looked at: binds_refresh rebuilds the live table on the polling thread while
// the menu draws, and an action may hold more than the two keys its cells show.
static int key_action(const InputMap *m, int key) {
    for (int a = 0; a < ACTION_COUNT; a++) {
        uint8_t keys[INPUT_BINDING_MAX];
        int n = input_map_keys_of(m, a, keys, INPUT_BINDING_MAX);
        for (int i = 0; i < n; i++)
            if (keys[i] == key) return a;
    }
    return -1;
}

// The walk key as the handful of keys a player is likely to pick. "none" rather than an empty value closes it: an
// unset key falls back to the default in the settings table, the same as a fresh installation. totk.ini takes any name
// input_map.c knows (walks of the keyboard sticks, not of the gamepad). A key the current bindings also use says which
// action it presses.
static void row_walk_key(void) {
    static const char *const keys[] = {"none", "lshift", "lalt", "lctrl", "tab", "capslock"};
    const char *names[sizeof keys / sizeof *keys] = {menu_text(UI_MENU_OFF), "Shift", "Alt", "Ctrl", "Tab", "Caps Lock"};
    const char *cur = cfg_value("TOTK_WALK_KEY");
    static char note[64];
    const char *n = nullptr;
    InputMap m;
    input_map_snapshot(&m);
    for (int k = KEY_A; k < KEY_COUNT && !n; k++) {
        if (!input_key_name(k) || strcasecmp(input_key_name(k), cur)) continue;
        int a = key_action(&m, k);
        if (a >= 0 && input_action_name(a)) snprintf(note, sizeof note, menu_text(UI_MENU_WALK_KEY_TAKEN), input_action_name(a)), n = note;
    }
    opt_choice_list("TOTK_WALK_KEY", UI_MENU_WALK_KEY, keys, names, sizeof keys / sizeof *keys, UI_MENU_WALK_KEY_DESC,
                    -1, n);
}

// The note under the prompts. The settings name a folder of prompts (input_prompts_folder: the prompts and the pad
// layout, as a layout that puts A on the right button needs the face buttons named the other way round), but the game
// reads its fonts and UI archive once at start (romfs.c overlay_init). While the folder in use differs: "files
// missing" when the setup did not write the one asked for, why it was left out at start (damaged, a game resolution
// that is not 16:9, a RomFS mod with the same files, TOTK_ROMFS_PATCHES), else "prompts change after restart", and only then
// *restart is set: a restart would bring the prompts asked for. NULL when they match.
static const char *prompts_note(bool *restart) {
    *restart = false;
    const char *layout = cfg_value("TOTK_PAD_LAYOUT"), *used = romfs_prompts();
    const char *want = input_prompts_folder(input_prompts_pick(cfg_get("TOTK_PROMPTS"), layout), layout);
    if (want == used || (want && used && !strcmp(want, used))) return nullptr;
    if (want && !romfs_prompts_ready(want)) return menu_text(UI_MENU_PROMPTS_MISSING);
    switch (romfs_prompts_off(want)) {
    case ROMFS_PROMPTS_FAILED: return menu_text(UI_MENU_PROMPTS_FAILED);
    case ROMFS_PROMPTS_SHAPE: return menu_text(UI_MENU_PROMPTS_OFF_SHAPE);
    case ROMFS_PROMPTS_MODS: return menu_text(UI_MENU_PROMPTS_OFF_MODS);
    case ROMFS_PROMPTS_PATCHES: return menu_text(UI_MENU_PROMPTS_OFF_PATCHES);
    default: *restart = true; return menu_text(UI_MENU_PROMPTS_RESTART);
    }
}

// The confirmation of "Restart now", over the menu; Cancel has the cursor first. Restart writes totk.ini, then closes
// the game as Quit does and starts it again (gfx_window_request_restart). It waits while the game writes a save, and
// the game stays when totk.ini cannot be written, as the new prompts would not come back. From the Cheats tab
// (restart_rebuild) Restart also lets the next start rebuild for cheats whose last rebuild failed (cheats_rebuild_allow).
static bool restart_rebuild;
static void restart_confirm(void) {
    g_modal = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##restart_confirm", nullptr,
                                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize))
        return;
    g_modal = true;
    ImGui::TextUnformatted(menu_text(UI_MENU_RESTART_CONFIRM));
    bool saving = fs_save_busy() != 0;
    if (saving || !cfg_last_save_ok())
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s",
                           menu_text(saving ? UI_MENU_RESTART_SAVING : UI_MENU_SAVE_FAILED));
    ImGui::Spacing();
    const char *yes = menu_text(UI_MENU_RESTART_YES), *no = menu_text(UI_MENU_RESET_NO);
    const ImGuiStyle &st = ImGui::GetStyle();
    float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
    bool close = ImGui::Button(no, ImVec2(bw, 0));
    if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    ImGui::BeginDisabled(saving);
    if (ImGui::Button(yes, ImVec2(bw, 0)) && cfg_save()) {
        if (restart_rebuild) cheats_rebuild_allow();
        gfx_window_request_restart();
    }
    ImGui::EndDisabled();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        close = true;
    if (close) {
        ImGui::CloseCurrentPopup();
        restart_rebuild = false;
        opt_focus_key("##restart");   // back on the row it came from
    }
    ImGui::EndPopup();
}

static void tab_controls(float scale) {
    opt_begin("##controls", scale);
    static const UiTextId layout[] = {UI_MENU_PAD_LAYOUT_AUTO, UI_MENU_PAD_LAYOUT_NINTENDO, UI_MENU_PAD_LAYOUT_XBOX};
    opt_choice("TOTK_PAD_LAYOUT", UI_MENU_PAD_LAYOUT, layout, 3, UI_MENU_PAD_LAYOUT_DESC, pad_layout_note());
    static const UiTextId prompts[] = {UI_MENU_PROMPTS_NINTENDO, UI_MENU_PROMPTS_XBOX, UI_MENU_PROMPTS_PLAYSTATION};
    bool restart;
    const char *note = prompts_note(&restart);
    opt_choice("TOTK_PROMPTS", UI_MENU_PROMPTS, prompts, 3, UI_MENU_PROMPTS_DESC, note);
    bool ask = restart && opt_button("##restart", UI_MENU_RESTART, UI_MENU_RESTART_DESC);
    opt_toggle("TOTK_MOTION", UI_MENU_MOTION, UI_MENU_MOTION_DESC);
    opt_slider("TOTK_RUMBLE_STRENGTH", UI_MENU_RUMBLE_STRENGTH, 0, 300, UI_MENU_RUMBLE_STRENGTH_DESC, 10, "%.0f %%");
    opt_slider("TOTK_RUMBLE_CURVE", UI_MENU_RUMBLE_CURVE, 0.2f, 1.5f, UI_MENU_RUMBLE_CURVE_DESC, 0.05f, "%.2f");
    opt_toggle("TOTK_RUMBLE_TRIGGERS", UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC);
    opt_toggle("TOTK_DS_TRIGGERS", UI_MENU_DS_TRIGGERS, UI_MENU_DS_TRIGGERS_DESC);
    opt_toggle("TOTK_DS_LIGHT", UI_MENU_DS_LIGHT, UI_MENU_DS_LIGHT_DESC);
    // One row per stick, falling back to TOTK_STICK_DEADZONE (both sticks) while unset. With a gamepad connected each
    // row shows its stick's deflection now, before the dead zone: a drifting stick's resting value, to set it above.
    double now[2];
    bool pad = input_pad_stick_percent(now) != 0;
    char stick_note[2][64];
    for (int s = 0; s < 2; s++)
        snprintf(stick_note[s], sizeof stick_note[s], menu_text(UI_MENU_STICK_DEADZONE_NOW), now[s]);
    opt_slider("TOTK_LSTICK_DEADZONE", UI_MENU_LSTICK_DEADZONE, 0, 50, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%",
               pad ? stick_note[0] : nullptr, "TOTK_STICK_DEADZONE");
    opt_slider("TOTK_RSTICK_DEADZONE", UI_MENU_RSTICK_DEADZONE, 0, 50, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%",
               pad ? stick_note[1] : nullptr, "TOTK_STICK_DEADZONE");
    opt_slider("TOTK_STICK_SENS", UI_MENU_STICK_SENS, 0.1f, 3, UI_MENU_STICK_SENS_DESC, 0.05f, "%.2f");
    opt_slider("TOTK_MOUSE_SENS", UI_MENU_MOUSE_SENS, 0, 10, UI_MENU_MOUSE_SENS_DESC, 0.1f, "%.1f");
    opt_slider("TOTK_MOUSE_SMOOTH", UI_MENU_MOUSE_SMOOTH, 0, 100, UI_MENU_MOUSE_SMOOTH_DESC, 5, "%.0f %%");
    row_walk_key();
    opt_slider("TOTK_WALK_SCALE", UI_MENU_WALK_SCALE, 0.2f, 0.9f, UI_MENU_WALK_SCALE_DESC, 0.01f, "%.2f");
    binds_rows(scale);
    opt_end();
    binds_after_page();
    if (ask) ImGui::OpenPopup("##restart_confirm");   // at the level BeginPopupModal looks it up
    restart_confirm();
}

static void tab_menu(float scale) {
    opt_begin("##menu", scale);
    static const UiTextId langs[] = {UI_MENU_MENU_LANG_AUTO, UI_MENU_LANGNAME_EN, UI_MENU_LANGNAME_RU,
                                     UI_MENU_LANGNAME_PT_BR, UI_MENU_LANGNAME_ZH_HANS};
    opt_choice("TOTK_MENU_LANG", UI_MENU_MENU_LANG, langs, sizeof langs / sizeof *langs, UI_MENU_MENU_LANG_DESC);
    opt_end();
}

// ---- Advanced: the testing and troubleshooting keys, the bug report helpers and the reset of every setting

// A thread count with no fixed default: Automatic (unset), then the common counts.
static void row_threads(const char *key, UiTextId label, UiTextId desc) {
    static const char *const vals[] = {"", "1", "2", "4", "6", "8", "12", "16", "24", "32", "48", "64"};
    enum { N = sizeof vals / sizeof *vals };
    const char *names[N];
    names[0] = menu_text(UI_MENU_AUTO);
    for (int i = 1; i < N; i++) names[i] = vals[i];
    opt_choice_list(key, label, vals, names, N, desc);
}

// A free-text key shown as Off (unset) / On (value set): On writes on_value, any set value reads as On.
static void row_set_or_not(const char *key, UiTextId label, UiTextId desc, const char *on_value) {
    const char *vals[] = {"", on_value}, *names[] = {menu_text(UI_MENU_OFF), menu_text(UI_MENU_ON)};
    opt_choice_list(key, label, vals, names, 2, desc, *cfg_value(key) ? 1 : 0);
}

static bool validation_layer_present(void) {
    static int have = -1;
    if (have < 0) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        VkLayerProperties *l = (VkLayerProperties *)calloc(n ? n : 1, sizeof *l);
        if (l && vkEnumerateInstanceLayerProperties(&n, l) >= 0)
            for (uint32_t i = 0; i < n && have <= 0; i++) have = !strcmp(l[i].layerName, "VK_LAYER_KHRONOS_validation");
        free(l);
        if (have < 0) have = 0;
    }
    return have != 0;
}

// Every setting the menu shows back to its default (the environment's keys stay), written to totk.ini.
static void reset_all(void) {
    const char *keys[128];
    int n = 0;
    for (const CfgMeta *m; n < 128 && (m = cfg_meta_at(n)); n++) keys[n] = m->key;
    int done = cfg_reset(keys, n);
    opt_save_soon();
    rt_log("menu: reset %d settings to their defaults", done);
}

// A button's result shown in its value for a few seconds: 1 done (done_text), -1 failed.
struct ActionNote { uint64_t until; int result; };

static const char *action_value(const ActionNote &a, UiTextId done_text) {
    if (!a.result || plat_now_ns() > a.until) return "";
    return menu_text(a.result > 0 ? done_text : UI_MENU_ACTION_FAILED);
}

static void action_done(ActionNote &a, bool ok) { a = {plat_now_ns() + 3000000000ull, ok ? 1 : -1}; }

static void tab_advanced(float scale) {
    static ActionNote copy_note, log_note;
    opt_begin("##advanced", scale);
    opt_text(UI_MENU_ADV_WARNING, true);

    opt_group(UI_MENU_GROUP_SCHED);
    static const UiTextId preempt[] = {UI_MENU_PREEMPT_OFF, UI_MENU_PREEMPT_ON, UI_MENU_PREEMPT_INHERIT};   // 0|1|2
    opt_choice("TOTK_ASYNC_PREEMPT", UI_MENU_ASYNC_PREEMPT, preempt, 3, UI_MENU_ASYNC_PREEMPT_DESC);
    opt_toggle("TOTK_REPLAY_ASYNC", UI_MENU_REPLAY_ASYNC, UI_MENU_REPLAY_ASYNC_DESC);
    opt_toggle("TOTK_TIMER_1MS", UI_MENU_TIMER_1MS, UI_MENU_TIMER_1MS_DESC);

    opt_group(UI_MENU_GROUP_SHADERS);
    static const UiTextId wait[] = {UI_MENU_PIPE_WAIT_DEFAULT, UI_MENU_PIPE_WAIT_DRAW};
    opt_choice("TOTK_PIPE_WAIT", UI_MENU_PIPE_WAIT, wait, 2, UI_MENU_PIPE_WAIT_DESC);
    static const UiTextId skip[] = {UI_MENU_PIPE_SKIP_STRICT, UI_MENU_PIPE_SKIP_ANY, UI_MENU_PIPE_SKIP_FULLSCREEN};
    opt_choice("TOTK_PIPE_SKIP", UI_MENU_PIPE_SKIP, skip, 3, UI_MENU_PIPE_SKIP_DESC);
    opt_toggle("TOTK_LIB_PREBUILD", UI_MENU_LIB_PREBUILD, UI_MENU_LIB_PREBUILD_DESC);
    row_threads("TOTK_LIB_PREBUILD_THREADS", UI_MENU_LIB_PREBUILD_THREADS, UI_MENU_LIB_PREBUILD_THREADS_DESC);
    opt_toggle("TOTK_PIPE_PREBUILD", UI_MENU_PIPE_PREBUILD, UI_MENU_PIPE_PREBUILD_DESC);
    row_threads("TOTK_PREWARM_XLATE", UI_MENU_PREWARM_XLATE, UI_MENU_PREWARM_XLATE_DESC);
    opt_toggle("TOTK_GPL", UI_MENU_GPL, UI_MENU_GPL_DESC);

    opt_group(UI_MENU_GROUP_TEXMEM);
    static const UiTextId astc[] = {UI_MENU_ASTC_AUTO, UI_MENU_ASTC_GPU, UI_MENU_ASTC_NATIVE, UI_MENU_ASTC_CPU};
    opt_choice("TOTK_ASTC", UI_MENU_ASTC, astc, 4, UI_MENU_ASTC_DESC);
    static const UiTextId mem[] = {UI_MENU_MEMORY_AUTO, UI_MENU_MEMORY_IMPORT, UI_MENU_MEMORY_MIRROR,
                                   UI_MENU_MEMORY_VERIFY};
    opt_choice("TOTK_MEMORY", UI_MENU_MEMORY, mem, 4, UI_MENU_MEMORY_DESC);
    opt_toggle("TOTK_TEX_ALIAS", UI_MENU_TEX_ALIAS, UI_MENU_TEX_ALIAS_DESC);
    opt_slider("TOTK_TEX_VRAM_PRESSURE", UI_MENU_TEX_VRAM_PRESSURE, 0, 100, UI_MENU_TEX_VRAM_PRESSURE_DESC, 1, "%.0f%%");
    opt_toggle("TOTK_ASTC_BC", UI_MENU_ASTC_BC, UI_MENU_ASTC_BC_DESC);

    opt_group(UI_MENU_GROUP_FIXES);
    opt_toggle("TOTK_LOD_GUARD", UI_MENU_LOD_GUARD, UI_MENU_LOD_GUARD_DESC);
    opt_toggle("TOTK_FIX_C0", UI_MENU_FIX_C0, UI_MENU_FIX_C0_DESC);
    opt_toggle("TOTK_FIX_ZERO", UI_MENU_FIX_ZERO, UI_MENU_FIX_ZERO_DESC);
    opt_toggle("TOTK_BIND_SKIP", UI_MENU_BIND_SKIP, UI_MENU_BIND_SKIP_DESC);
    opt_toggle("TOTK_SHADER_MEMO", UI_MENU_SHADER_MEMO, UI_MENU_SHADER_MEMO_DESC);
    opt_toggle("TOTK_PIPE_MEMO", UI_MENU_PIPE_MEMO, UI_MENU_PIPE_MEMO_DESC);
    opt_toggle("TOTK_TEX_MEMO", UI_MENU_TEX_MEMO, UI_MENU_TEX_MEMO_DESC);
    opt_toggle("TOTK_DESC_TEMPLATE", UI_MENU_DESC_TEMPLATE, UI_MENU_DESC_TEMPLATE_DESC);
    opt_toggle("TOTK_PUSH_SKIP", UI_MENU_PUSH_SKIP, UI_MENU_PUSH_SKIP_DESC);
    opt_slider("TOTK_SPLIT_DRAWS", UI_MENU_SPLIT_DRAWS, 0, 1000, UI_MENU_SPLIT_DRAWS_DESC, 8, "%.0f");

    opt_group(UI_MENU_GROUP_VULKAN);
    if (!validation_layer_present() && !cfg_bool("TOTK_VK_VALIDATION")) opt_disable_next(UI_MENU_VK_VALIDATION_MISSING);
    opt_toggle("TOTK_VK_VALIDATION", UI_MENU_VK_VALIDATION, UI_MENU_VK_VALIDATION_DESC);
    static const UiTextId dyn[] = {UI_MENU_AUTO, UI_MENU_OFF, UI_MENU_ON};   // unset, 0, 1
    opt_choice("TOTK_VK_DYN_VERTEX", UI_MENU_VK_DYN_VERTEX, dyn, 3, UI_MENU_VK_DYN_VERTEX_DESC);
    opt_choice("TOTK_VK_DYN_A2C", UI_MENU_VK_DYN_A2C, dyn, 3, UI_MENU_VK_DYN_A2C_DESC);
    opt_toggle("TOTK_VK_SUBGROUP_PIN", UI_MENU_VK_SUBGROUP_PIN, UI_MENU_VK_SUBGROUP_PIN_DESC);

    opt_group(UI_MENU_GROUP_DIAG);
    row_set_or_not("TOTK_TRACE", UI_MENU_TRACE, UI_MENU_TRACE_DESC, "perf");
    opt_toggle("TOTK_FLIGHT", UI_MENU_FLIGHT, UI_MENU_FLIGHT_DESC);
    opt_slider("TOTK_FLIGHT_SPIKE_MS", UI_MENU_FLIGHT_SPIKE_MS, 20, 1000, UI_MENU_FLIGHT_SPIKE_MS_DESC, 10, "%.0f");
    static char rec_path[600];   // the recording goes beside the log
    if (!rec_path[0]) {
        char dir[512];
        snprintf(rec_path, sizeof rec_path, "%s/input-record.txt", sysinfo_log_folder(dir, sizeof dir) ? dir : ".");
    }
    row_set_or_not("TOTK_INPUT_RECORD", UI_MENU_INPUT_RECORD, UI_MENU_INPUT_RECORD_DESC, rec_path);
    if (opt_button("##open_log", UI_MENU_OPEN_LOG, UI_MENU_OPEN_LOG_DESC, action_value(log_note, UI_MENU_OPEN_LOG))) {
        char dir[512];
        bool ok = sysinfo_log_folder(dir, sizeof dir) && plat_open_folder(dir);
        if (!ok) action_done(log_note, false);   // success shows as the opened folder
    }
    if (opt_button("##copy_info", UI_MENU_COPY_INFO, UI_MENU_COPY_INFO_DESC,
                   action_value(copy_note, UI_MENU_COPY_INFO_DONE))) {
        static char text[16384];
        action_done(copy_note, sysinfo_text(text, sizeof text) > 0 && plat_clipboard_set(text));
    }
    bool ask = opt_button("##reset", UI_MENU_RESET, UI_MENU_RESET_DESC);
    opt_end();
    if (ask) ImGui::OpenPopup("##reset_confirm");   // at the level BeginPopupModal looks it up

    // the confirmation, over the menu; Cancel has the cursor first
    g_modal = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##reset_confirm", nullptr,
                               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        g_modal = true;
        ImGui::TextUnformatted(menu_text(UI_MENU_RESET_CONFIRM));
        ImGui::Spacing();
        const char *yes = menu_text(UI_MENU_RESET_YES), *no = menu_text(UI_MENU_RESET_NO);
        const ImGuiStyle &st = ImGui::GetStyle();
        float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
        bool close = false;
        if (ImGui::Button(no, ImVec2(bw, 0))) close = true;
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(yes, ImVec2(bw, 0))) {
            reset_all();
            close = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            close = true;
        if (close) {
            ImGui::CloseCurrentPopup();
            opt_focus_key("##reset");   // back on the row it came from
        }
        ImGui::EndPopup();
    }
}

// ---- the Achievements tab: the unlocked count over a bar, then a card per achievement (its icon, greyed while locked;
// its name and description; its rarity; the progress or the unlock date). The cards are nav items, so the D-pad walks
// the list and scrolls it.
static float g_page_bottom;     // the height the bottom row takes under the page (layout_menu)
static bool g_ach_focus;        // the first card takes the cursor at the next layout (the tab was just chosen)

static ImVec4 ach_rarity_col(int r) {
    static const ImVec4 c[ACH_RARITIES] = {{0.78f, 0.62f, 0.40f, 1}, {0.47f, 0.80f, 0.84f, 1}, {0.72f, 0.54f, 0.95f, 1},
                                           {0.95f, 0.79f, 0.38f, 1}, {0.93f, 0.95f, 0.98f, 1}};
    return c[r >= 0 && r < ACH_RARITIES ? r : 0];
}

static UiTextId ach_rarity_name(int r) {
    static const UiTextId n[ACH_RARITIES] = {UI_MENU_ACH_COMMON, UI_MENU_ACH_RARE, UI_MENU_ACH_EPIC,
                                             UI_MENU_ACH_LEGENDARY, UI_MENU_ACH_PLATINUM};
    return n[r >= 0 && r < ACH_RARITIES ? r : 0];
}

// An achievement's icon at p, size s across; lit: in colour, else dimmed. Without an icon: a ring with a question mark.
static void ach_icon(ImDrawList *dl, int i, const AchView &v, ImVec2 p, float s, bool lit, float alpha) {
    if (i >= 0 && i < kAchMax && g_ach_icon[i] && !(v.hidden && !v.unlocked)) {
        ImU32 tint = lit ? IM_COL32(255, 255, 255, (int)(255 * alpha)) : IM_COL32(95, 100, 98, (int)(170 * alpha));
        dl->AddImage(ImGui::GetIO().Fonts->TexID, p, ImVec2(p.x + s, p.y + s), g_ach_uv0[i], g_ach_uv1[i], tint);
        return;
    }
    ImVec2 c(p.x + s / 2, p.y + s / 2);
    dl->AddCircleFilled(c, s * 0.46f, ui_col(kUiPanel, 0.9f * alpha));
    dl->AddCircle(c, s * 0.46f, ui_col(kUiMuted, 0.6f * alpha), 32, s * 0.05f);
    ImFont *font = ImGui::GetFont();
    float fs = s * 0.5f;
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, "?");
    dl->AddText(font, fs, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), ui_col(kUiMuted, alpha), "?");
}

static void ach_date(int64_t when, char *out, size_t cap) {
    time_t t = (time_t)when;
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(out, cap, "%d/%m/%Y", &tmv);
}

static void ach_cards(const char *id, float h, bool only_mp, float scale);

static void tab_achievements(float scale) {
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), small = fs * 0.78f;
    int n = ach_count(), done = ach_unlocked_count();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (!n) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail.x * 0.7f);
        ImGui::PushStyleColor(ImGuiCol_Text, kUiMuted);
        ImGui::TextUnformatted(menu_text(UI_MENU_ACH_NONE));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(1, fmaxf(0, avail.y - g_page_bottom - ImGui::GetTextLineHeightWithSpacing() * 3)));
        return;
    }
    // the header: the platinum's progress (the multiplayer ones left out) and 100 % (all of them), each with a bar
    int plat_n = 0, plat_done = 0;
    for (int i = 0; i < n; i++) {
        AchView v;
        if (ach_get(i, &v) && !v.multiplayer && v.rarity != ACH_PLATINUM) plat_n++, plat_done += v.unlocked;
    }
    const struct { UiTextId text; int done, total; ImVec4 col; } bars[] = {
        {UI_MENU_ACH_PLATINUM_PROGRESS, plat_done, plat_n, ach_rarity_col(ACH_PLATINUM)},
        {UI_MENU_ACH_FULL_PROGRESS, done, n, kUiZonai}};
    for (const auto &b : bars) {
        char head[128];
        snprintf(head, sizeof head, menu_text(b.text), b.done, b.total);
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(font, fs, p, ui_col(kUiCream), head);
        float bw = avail.x * 0.42f, bh = 6 * scale, bx = p.x + avail.x - bw, by = p.y + fs * 0.5f - bh / 2;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), ui_col(kUiMuted, 0.25f), bh);
        if (b.total)
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * b.done / (float)b.total, by + bh), ui_col(b.col, 0.9f), bh);
        ImGui::Dummy(ImVec2(avail.x, fs * 1.35f));
    }
    ImGui::Dummy(ImVec2(1, 2 * scale));
    if (int blocked = ach_blocked_count()) {   // a save from elsewhere: why some done ones stay locked
        char note[512];
        snprintf(note, sizeof note, menu_text(UI_MENU_ACH_BLOCKED_NOTE), blocked);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail.x);
        ImGui::PushStyleColor(ImGuiCol_Text, kUiWarn);
        ImGui::TextUnformatted(note);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(1, 4 * scale));
    }

    float list_h = ImGui::GetContentRegionAvail().y - g_page_bottom;
    ach_cards("##ach", fmaxf(list_h, fs * 4), false, scale);
}

// The achievement cards in a scrolling child of height h: every one, or the multiplayer ones only.
static void ach_cards(const char *id, float h, bool only_mp, float scale) {
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), small = fs * 0.78f;
    int n = ach_count();
    ImGui::BeginChild(id, ImVec2(ImGui::GetContentRegionAvail().x, h), ImGuiChildFlags_NavFlattened);
    float card_h = 84 * scale, icon = 64 * scale, pad = 10 * scale, w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool first = true;
    for (int i = 0; i < n; i++) {
        AchView v;
        if (!ach_get(i, &v) || (only_mp && !v.multiplayer)) continue;
        ImGui::PushID(i);
        ImVec2 q = ImGui::GetCursorScreenPos();
        bool take = first;
        first = false;
        if (g_ach_focus && take) {
            ImGui::SetKeyboardFocusHere();
            g_ach_focus = false;
        }
        ImGui::Selectable("##card", false, 0, ImVec2(w, card_h));
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImGui::PopID();
        ImDrawList *cl = ImGui::GetWindowDrawList();
        ImVec4 rc = ach_rarity_col(v.rarity);
        ImVec2 q1(q.x + w, q.y + card_h);
        cl->AddRectFilled(q, q1, ui_col(v.unlocked ? rc : kUiMuted, hot ? 0.13f : v.unlocked ? 0.07f : 0.04f), 10 * scale);
        cl->AddRect(q, q1, ui_col(v.unlocked ? rc : kUiMuted, hot ? 0.75f : v.unlocked ? 0.35f : 0.18f), 10 * scale, 0,
                    (hot ? 1.6f : 1.0f) * scale);
        ach_icon(cl, i, v, ImVec2(q.x + pad, q.y + (card_h - icon) / 2), icon, v.unlocked != 0, 1.0f);
        bool secret = v.hidden && !v.unlocked;
        float tx = q.x + pad * 2 + icon, right = q1.x - pad * 1.5f;
        const char *name = secret ? menu_text(UI_MENU_ACH_SECRET) : v.name;
        const char *desc = secret ? menu_text(UI_MENU_ACH_SECRET_DESC) : v.desc;
        // the rarity and the progress or date, right-aligned on two lines
        char right1[64], right2[64];
        if (v.multiplayer)
            snprintf(right1, sizeof right1, "%s  ·  %s", menu_text(UI_MENU_ACH_MP_TAG), menu_text(ach_rarity_name(v.rarity)));
        else
            snprintf(right1, sizeof right1, "%s", menu_text(ach_rarity_name(v.rarity)));
        if (v.unlocked) {
            char d[32];
            ach_date(v.when, d, sizeof d);
            snprintf(right2, sizeof right2, menu_text(UI_MENU_ACH_UNLOCKED_ON), d);
        } else if (v.blocked) {
            snprintf(right2, sizeof right2, "%s", menu_text(UI_MENU_ACH_BLOCKED));
        } else if (v.target > 1) {
            snprintf(right2, sizeof right2, "%.0f / %.0f", floor(fmin(v.cur, v.target)), v.target);
        } else {
            right2[0] = 0;
        }
        float r1 = font->CalcTextSizeA(small, FLT_MAX, 0, right1).x, r2 = font->CalcTextSizeA(small, FLT_MAX, 0, right2).x;
        float text_w = right - fmaxf(r1, r2) - pad * 2 - tx;
        cl->AddText(font, small, ImVec2(right - r1, q.y + pad), ui_col(rc, v.unlocked ? 1.0f : 0.55f), right1);
        cl->AddText(font, small, ImVec2(right - r2, q.y + pad + small * 1.3f), ui_col(kUiMuted), right2);
        cl->PushClipRect(ImVec2(tx, q.y), ImVec2(tx + text_w, q1.y), true);
        cl->AddText(font, fs, ImVec2(tx, q.y + pad), ui_col(v.unlocked ? kUiCream : kUiMuted), name);
        cl->AddText(font, small, ImVec2(tx, q.y + pad + fs * 1.2f), ui_col(kUiMuted, 0.9f), desc, nullptr, text_w);
        cl->PopClipRect();
        if (!v.unlocked && v.target > 1) {   // the progress bar along the card's bottom
            float bx0 = tx, bx1 = right, byy = q1.y - pad - 4 * scale;
            cl->AddRectFilled(ImVec2(bx0, byy), ImVec2(bx1, byy + 4 * scale), ui_col(kUiMuted, 0.2f), 2 * scale);
            float k = (float)fmin(1.0, v.cur / v.target);
            cl->AddRectFilled(ImVec2(bx0, byy), ImVec2(bx0 + (bx1 - bx0) * k, byy + 4 * scale), ui_col(rc, 0.85f), 2 * scale);
        }
        ImGui::Dummy(ImVec2(1, 4 * scale));
    }
    ImGui::PopStyleColor(4);
    ImGui::EndChild();
}

// ---- multiplayer shortcuts: the chat box the chat key opens over the game, and the voice indicators

static const char *key_label(const char *key) {
    const char *v = cfg_value(key);
    if (!strcmp(v, "enter")) return "Enter";
    if (!strcmp(v, "capslock")) return "Caps Lock";
    if (!strcmp(v, "off")) return menu_text(UI_MENU_OFF);
    static char up[2][8];
    static int k;
    k ^= 1;
    snprintf(up[k], sizeof up[k], "%c", v[0] >= 'a' && v[0] <= 'z' ? v[0] - 32 : v[0]);
    return up[k];
}

// Called by the window adapters on the chat key: opens the chat box while this game is in a room (else the key goes
// on to the game). 1 when it opened.
extern "C" int menu_chat_open(void) {
    if (g_failed_flag() || menu_is_open()) return 0;
    MpView v;
    mp_view(&v);
    if (v.state != MP_CONNECTED) return 0;
    g_chat.store(1);
    change_open(1);
    return 1;
}

// The chat box: the last lines of the room's chat over a text field, at the bottom left, above the game.
static void layout_chat(uint32_t w, uint32_t h, float scale) {
    static char text[300];
    static bool focus;
    if (g_reset.load() == 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        set_open(0);
        return;
    }
    if (ImGui::GetFrameCount() <= 1 || !ImGui::IsAnyItemActive()) focus = true;
    float bw = fminf((float)w * 0.42f, 760 * scale);
    ImGui::SetNextWindowPos(ImVec2(24 * scale, (float)h - 24 * scale), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSize(ImVec2(bw, 0));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("##chat_box", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    static MpLine lines[8];
    int n = mp_lines(lines, 8);
    ImGui::PushTextWrapPos(bw - 16 * scale);
    for (int i = 0; i < n; i++) {
        const MpLine &l = lines[i];
        if (l.kind == MP_LINE_CHAT) {
            ImGui::TextColored(kUiGoldHi, "%s:", l.from);
            ImGui::SameLine();
            ImGui::TextUnformatted(l.text);
        } else {
            ImGui::TextColored(l.kind == MP_LINE_ACH ? kUiZonai : kUiMuted, "%s", l.text);
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(-1);
    if (focus) {
        ImGui::SetKeyboardFocusHere();
        focus = false;
    }
    if (ImGui::InputText("##chat_say", text, sizeof text, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (text[0]) mp_send_chat(text);
        text[0] = 0;
        set_open(0);   // Enter sends and goes back to the game
    }
    ImGui::TextColored(kUiMuted, "%s", menu_text(UI_MENU_MP_CHAT_BOX_HINT));
    ImGui::End();
}

static bool voice_active(void) {
    char names[VOICE_SPEAKERS][VOICE_NAME_MAX];
    return voice_transmitting() || voice_speakers(names, VOICE_SPEAKERS) > 0;
}

// (unused since the radar panel shows who talks; kept for a build without positions)
[[maybe_unused]] static void voice_draw(const MenuTarget *t, float scale) {
    (void)t;
    char names[VOICE_SPEAKERS][VOICE_NAME_MAX];
    int n = voice_speakers(names, VOICE_SPEAKERS), me = voice_transmitting();
    if (!n && !me) return;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * 0.8f, pad = 8 * scale, r = 6 * scale;
    float x = (full_menu() ? 124 : 24) * scale, y = 24 * scale;
    for (int i = -1; i < n; i++) {
        if (i < 0 && !me) continue;
        const char *label = i < 0 ? menu_text(UI_MENU_MP_TALKING) : names[i];
        ImVec4 col = i < 0 ? kUiDanger : kUiZonai;
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label);
        float hgt = ts.y + pad * 2, wid = pad * 3 + r * 2 + ts.x;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + wid, y + hgt), ui_col(kUiPanel, 0.85f), hgt / 2);
        dl->AddCircleFilled(ImVec2(x + pad + r, y + hgt / 2), r, ui_col(col, 1.0f));
        dl->AddText(font, fs, ImVec2(x + pad * 2 + r * 2, y + pad), ui_col(kUiCream, 1.0f), label);
        y += hgt + 6 * scale;
    }
}

// The Multiplayer tab's line about the shortcuts, with the microphone's level while it is open.
static void mp_voice_line(float scale) {
    char hint[200];
    const char *chat = key_label("TOTK_MP_CHAT_KEY");
    if (!cfg_bool("TOTK_MP_VOICE")) snprintf(hint, sizeof hint, menu_text(UI_MENU_MP_HINT_NOVOICE), chat);
    else if (!strcmp(cfg_value("TOTK_MP_VOICE_MODE"), "open")) snprintf(hint, sizeof hint, menu_text(UI_MENU_MP_HINT_OPEN), chat);
    else snprintf(hint, sizeof hint, menu_text(UI_MENU_MP_HINT_PUSH), chat, key_label("TOTK_MP_VOICE_KEY"));
    ImGui::TextColored(kUiMuted, "%s", hint);
    if (strcmp(cfg_value("TOTK_MP_MARK_KEY"), "off")) {
        snprintf(hint, sizeof hint, menu_text(UI_MENU_MP_HINT_MARK), key_label("TOTK_MP_MARK_KEY"));
        ImGui::TextColored(kUiMuted, "%s", hint);
    }
    if (!cfg_bool("TOTK_MP_VOICE")) return;
    if (voice_mic_error()) {
        ImGui::TextColored(kUiWarn, "%s", menu_text(UI_MENU_MP_NO_MIC));
        return;
    }
    float lv = voice_level();
    ImGui::TextColored(kUiMuted, "%s", menu_text(UI_MENU_MP_MIC));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, voice_transmitting() ? kUiDanger : kUiZonai);
    ImGui::ProgressBar(lv, ImVec2(160 * scale, 8 * scale), "");
    ImGui::PopStyleColor();
}

// ---- the Multiplayer tab (net_mp.c): a banner with the room and its state over sub-pages (players, group
// challenges, multiplayer achievements, chat, connection); before joining, the connection page alone. The text fields
// write totk.ini when they lose the keyboard.
static void mp_field(const char *key, UiTextId label, char *buf, size_t cap, ImGuiInputTextFlags flags, float w) {
    ImGui::PushID(key);
    ImGui::TextColored(kUiMuted, "%s", menu_text(label));
    ImGui::SetNextItemWidth(w);
    ImGui::InputText("##v", buf, cap, flags);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        cfg_set(key, buf);
        cfg_save();
        mp_reconnect();
    }
    ImGui::PopID();
}

enum { MP_SUB_PLAYERS, MP_SUB_CHALLENGES, MP_SUB_ACH, MP_SUB_CHAT, MP_SUB_ROOM, MP_SUBS };
static int g_mp_sub;
static int64_t g_mp_chat_seen;   // the newest chat line's time when the chat page was last shown (the unread badge)
static int g_mp_chat_frame = -10;   // the ImGui frame the chat page was last laid out in (the notices hide under it)

static const ImVec4 kTierCol[MP_TIERS] = {ImVec4(0.80f, 0.52f, 0.30f, 1), ImVec4(0.78f, 0.80f, 0.84f, 1),
                                          ImVec4(0.93f, 0.78f, 0.36f, 1), ImVec4(0.72f, 0.55f, 1.0f, 1)};
static const UiTextId kStatName[MP_STATS] = {UI_MENU_MP_COL_SHRINES, UI_MENU_MP_COL_LIGHTROOTS, UI_MENU_MP_COL_TOWERS,
                                             UI_MENU_MP_COL_KOROKS, UI_MENU_MP_COL_HEARTS, UI_MENU_MP_COL_ACH};
static const ImVec4 kStatCol[MP_STATS] = {ImVec4(0.37f, 0.95f, 0.78f, 1), ImVec4(0.99f, 0.86f, 0.45f, 1),
                                          ImVec4(0.55f, 0.75f, 1.0f, 1), ImVec4(0.55f, 0.85f, 0.40f, 1),
                                          ImVec4(1.0f, 0.42f, 0.42f, 1), ImVec4(0.93f, 0.78f, 0.36f, 1)};

// A small drawn symbol for a progress figure, centred on c, radius r.
static void mp_stat_icon(ImDrawList *dl, int stat, ImVec2 c, float r, ImU32 col) {
    float t = fmaxf(1.0f, r * 0.18f);
    switch (stat) {
    case MP_STAT_SHRINES:   // a diamond with an eye
        dl->AddQuad(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col, t);
        dl->AddCircleFilled(c, r * 0.3f, col);
        break;
    case MP_STAT_LIGHTROOTS:   // a light with rays
        dl->AddCircleFilled(c, r * 0.42f, col);
        for (int k = 0; k < 8; k++) {
            float a = k * 3.14159f / 4, s = sinf(a), co = cosf(a);
            dl->AddLine(ImVec2(c.x + co * r * 0.62f, c.y + s * r * 0.62f), ImVec2(c.x + co * r, c.y + s * r), col, t);
        }
        break;
    case MP_STAT_TOWERS:   // a tower with its top
        dl->AddRectFilled(ImVec2(c.x - r * 0.25f, c.y - r * 0.45f), ImVec2(c.x + r * 0.25f, c.y + r), col);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r * 0.4f), ImVec2(c.x + r * 0.6f, c.y - r * 0.4f),
                              ImVec2(c.x, c.y - r), col);
        break;
    case MP_STAT_KOROKS:   // a leaf on its stem
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.2f), r * 0.6f, col, 5);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.2f), ImVec2(c.x, c.y + r), col, t * 1.4f);
        break;
    case MP_STAT_HEARTS:   // a heart
        dl->AddCircleFilled(ImVec2(c.x - r * 0.42f, c.y - r * 0.25f), r * 0.48f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.42f, c.y - r * 0.25f), r * 0.48f, col);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.88f, c.y - r * 0.05f), ImVec2(c.x + r * 0.88f, c.y - r * 0.05f),
                              ImVec2(c.x, c.y + r * 0.9f), col);
        break;
    default:   // a trophy
        dl->AddRectFilled(ImVec2(c.x - r * 0.6f, c.y - r * 0.85f), ImVec2(c.x + r * 0.6f, c.y - r * 0.1f), col,
                          r * 0.5f, ImDrawFlags_RoundCornersBottom);
        dl->AddRectFilled(ImVec2(c.x - r * 0.12f, c.y - r * 0.15f), ImVec2(c.x + r * 0.12f, c.y + r * 0.55f), col);
        dl->AddRectFilled(ImVec2(c.x - r * 0.5f, c.y + r * 0.55f), ImVec2(c.x + r * 0.5f, c.y + r * 0.85f), col);
        break;
    }
}

// A colour of its own for each name (the avatars).
static ImVec4 mp_name_col(const char *name) {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    float hue = (h % 360) / 360.0f, r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.45f, 0.85f, r, g, b);
    return ImVec4(r, g, b, 1);
}

// The first character of a name (a whole UTF-8 sequence), upper case for ASCII letters.
static void mp_initial(const char *name, char out[8]) {
    int n = 1;
    unsigned char c = (unsigned char)name[0];
    if (c >= 0xf0) n = 4; else if (c >= 0xe0) n = 3; else if (c >= 0xc0) n = 2;
    int i = 0;
    for (; i < n && name[i]; i++) out[i] = name[i];
    out[i] = 0;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
    if (!out[0]) out[0] = '?', out[1] = 0;
}

static bool mp_speaking(const char *name) {
    char names[VOICE_SPEAKERS][VOICE_NAME_MAX];
    int n = voice_speakers(names, VOICE_SPEAKERS);
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], name)) return true;
    return false;
}

// The connection page: the fields, the button, the state and the shortcuts.
static void mp_connection(const MpView &v, float scale) {
    static char name[32], server[128], password[64];
    static unsigned seen = ~0u;
    if (cfg_version() != seen && !ImGui::GetIO().WantTextInput) {   // the fields follow totk.ini unless being typed in
        seen = cfg_version();
        snprintf(name, sizeof name, "%s", cfg_value("TOTK_MP_NAME"));
        snprintf(server, sizeof server, "%s", cfg_value("TOTK_MP_SERVER"));
        snprintf(password, sizeof password, "%s", cfg_value("TOTK_MP_PASSWORD"));
    }
    float w = ImGui::GetContentRegionAvail().x, gap = 18 * scale;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w * 0.8f);
    ImGui::TextColored(kUiMuted, "%s", menu_text(UI_MENU_MP_INTRO));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(1, 6 * scale));
    float fw = fminf((w - gap * 2) / 3, 360 * scale);
    ImGui::BeginGroup();
    mp_field("TOTK_MP_NAME", UI_MENU_MP_NAME, name, sizeof name, 0, fw);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    mp_field("TOTK_MP_SERVER", UI_MENU_MP_SERVER, server, sizeof server, 0, fw);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    mp_field("TOTK_MP_PASSWORD", UI_MENU_MP_PASSWORD, password, sizeof password, ImGuiInputTextFlags_Password, fw);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(1, 8 * scale));
    bool on = cfg_bool("TOTK_MP");
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.35f, 0.12f, 0.12f, 1) : ImVec4(0.08f, 0.32f, 0.27f, 1));
    if (ImGui::Button(menu_text(on ? UI_MENU_MP_DISCONNECT : UI_MENU_MP_CONNECT), ImVec2(fw * 0.6f, 0))) {
        cfg_set("TOTK_MP", on ? "0" : "1");
        cfg_save();
        mp_reconnect();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, gap);
    char state[256];
    ImVec4 sc = kUiMuted;
    switch (v.state) {
    case MP_CONNECTING: snprintf(state, sizeof state, menu_text(UI_MENU_MP_STATE_CONNECTING), v.server); break;
    case MP_CONNECTED: snprintf(state, sizeof state, menu_text(UI_MENU_MP_STATE_CONNECTED), v.room), sc = kUiZonai; break;
    case MP_RETRYING: snprintf(state, sizeof state, menu_text(UI_MENU_MP_STATE_RETRYING), v.server), sc = kUiWarn; break;
    case MP_REFUSED: snprintf(state, sizeof state, menu_text(UI_MENU_MP_STATE_REFUSED), v.error), sc = kUiDanger; break;
    default: snprintf(state, sizeof state, "%s", menu_text(UI_MENU_MP_STATE_OFF));
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(sc, "%s", state);
    ImGui::Dummy(ImVec2(1, 10 * scale));
    mp_voice_line(scale);
}

// The banner over the pages: the room (or "Multiplayer"), a state pill, how many play and the shortcuts.
static void mp_banner(const MpView &v, float scale) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = 64 * scale, pad = 16 * scale;
    ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + w, p.y + h);
    bool joined = v.state == MP_CONNECTED;
    dl->AddRectFilledMultiColor(p, q, ui_col(kUiZonai, joined ? 0.16f : 0.06f), ui_col(kUiPanel, 0.2f),
                                ui_col(kUiPanel, 0.2f), ui_col(kUiZonai, joined ? 0.10f : 0.04f));
    dl->AddRect(p, q, ui_col(joined ? kUiZonai : kUiMuted, 0.35f), 10 * scale, 0, 1.2f * scale);
    const char *title = joined ? v.room : menu_text(UI_MENU_TAB_MULTIPLAYER);
    float big = fs * 1.3f;
    dl->AddText(font, big, ImVec2(p.x + pad, p.y + (h - big) / 2), ui_col(kUiGoldHi), title);
    float tx = p.x + pad + font->CalcTextSizeA(big, FLT_MAX, 0, title).x + 14 * scale;
    // the state pill
    const char *st = menu_text(v.state == MP_CONNECTED ? UI_MENU_MP_PILL_ON : v.state == MP_CONNECTING || v.state == MP_RETRYING
                                                                               ? UI_MENU_MP_PILL_TRYING : UI_MENU_MP_PILL_OFF);
    ImVec4 sc = v.state == MP_CONNECTED ? kUiZonai : v.state == MP_REFUSED ? kUiDanger
              : v.state == MP_OFF ? kUiMuted : kUiWarn;
    float small = fs * 0.78f, pw = font->CalcTextSizeA(small, FLT_MAX, 0, st).x + 30 * scale, ph = small + 10 * scale;
    ImVec2 pp(tx, p.y + (h - ph) / 2);
    dl->AddRectFilled(pp, ImVec2(pp.x + pw, pp.y + ph), ui_col(sc, 0.16f), ph / 2);
    dl->AddCircleFilled(ImVec2(pp.x + 11 * scale, pp.y + ph / 2), 4 * scale, ui_col(sc));
    dl->AddText(font, small, ImVec2(pp.x + 20 * scale, pp.y + 5 * scale), ui_col(sc), st);
    // on the right: how many play
    if (joined) {
        char count[64];
        snprintf(count, sizeof count, menu_text(UI_MENU_MP_PLAYERS_COUNT), v.nplayers);
        float cw = font->CalcTextSizeA(fs, FLT_MAX, 0, count).x;
        dl->AddText(font, fs, ImVec2(q.x - pad - cw, p.y + (h - fs) / 2), ui_col(kUiCream), count);
    }
    ImGui::Dummy(ImVec2(w, h + 8 * scale));
}

// The sub-page buttons; the chat's shows how many lines came since it was last open.
static void mp_subtabs(float scale, int unread) {
    static const UiTextId names[MP_SUBS] = {UI_MENU_MP_SUB_PLAYERS, UI_MENU_MP_SUB_CHALLENGES, UI_MENU_MP_SUB_ACH,
                                            UI_MENU_MP_SUB_CHAT, UI_MENU_MP_SUB_ROOM};
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14 * scale);
    for (int i = 0; i < MP_SUBS; i++) {
        if (i) ImGui::SameLine(0, 8 * scale);
        bool on = g_mp_sub == i;
        char label[64];
        if (i == MP_SUB_CHAT && unread > 0 && !on) snprintf(label, sizeof label, "%s (%d)###sub%d", menu_text(names[i]), unread, i);
        else snprintf(label, sizeof label, "%s###sub%d", menu_text(names[i]), i);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.37f, 0.95f, 0.78f, 0.22f) : ImVec4(1, 1, 1, 0.04f));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? kUiGoldHi : kUiCream);
        if (ImGui::Button(label, ImVec2(0, 0))) g_mp_sub = i;
        ImGui::PopStyleColor(2);
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(1, 6 * scale));
}

// ---- friends on the map and in the world (net_mp.c positions): figures in the world (world_tags), dots on the game's
// minimap (minimap_dots), and at the top right under the frame rate a line per friend: name, distance and layer, green
// while they talk; then "Talking" while this game sends its voice.
static const UiTextId kLayerName[3] = {UI_MENU_MP_LAYER_SURFACE, UI_MENU_MP_LAYER_SKY, UI_MENU_MP_LAYER_DEPTHS};

static void mp_distance(float d, char *out, size_t cap) {
    if (d >= 1000) snprintf(out, cap, "%.1f km", d / 1000);
    else snprintf(out, cap, "%.0f m", d);
}

static bool radar_on(const MpView &v) {
    return v.state == MP_CONNECTED && cfg_bool("TOTK_MP_RADAR") && v.me_has_pos && frame_step_world_us() >= 0;
}

static bool voice_active(void);

static bool radar_active(void) {
    MpView v;
    mp_view(&v);
    if (!radar_on(v)) return voice_active();
    for (int i = 0; i < v.nplayers; i++)
        if (v.players[i].has_pos && strcmp(v.players[i].name, v.me)) return true;
    return v.nmarkers > 0 || voice_active();
}

// A friend's figure: a Link-like silhouette of our own drawing (hair with pointed ears, tunic in the friend's colour
// with a belt, arms, legs and boots, a sword and shield on the back), seen from the front, its feet at feet and tall
// pixels high; alpha is its opacity. Sizes are fractions of the height; y runs up from the feet.
static void draw_figure(ImDrawList *dl, ImVec2 feet, float tall, ImVec4 col, float alpha) {
    auto P = [&](float x, float y) { return ImVec2(feet.x + x * tall, feet.y - y * tall); };
    auto C = [&](ImVec4 c, float k) {   // the colour scaled toward black (k < 1) or white (k > 1)
        ImVec4 o = k <= 1 ? ImVec4(c.x * k, c.y * k, c.z * k, 1)
                          : ImVec4(c.x + (1 - c.x) * (k - 1), c.y + (1 - c.y) * (k - 1), c.z + (1 - c.z) * (k - 1), 1);
        return ui_col(o, alpha);
    };
    const ImVec4 skin(0.93f, 0.80f, 0.66f, 1), hair(0.86f, 0.70f, 0.36f, 1), steel(0.80f, 0.84f, 0.88f, 1);
    float px = tall;   // pixels per unit
    // on the back: the shield, then the sword across it
    dl->AddEllipseFilled(P(-0.02f, 0.60f), ImVec2(0.115f * px, 0.13f * px), C(col, 0.45f));
    dl->AddLine(P(0.15f, 0.80f), P(-0.13f, 0.38f), C(steel, 1.0f), fmaxf(1.0f, 0.022f * px));
    dl->AddLine(P(0.09f, 0.66f), P(0.16f, 0.71f), C(hair, 0.7f), fmaxf(1.0f, 0.03f * px));   // the guard
    // legs and boots
    for (int s = -1; s <= 1; s += 2) {
        dl->AddRectFilled(P(s < 0 ? -0.075f : 0.015f, 0.42f), P(s < 0 ? -0.015f : 0.075f, 0.10f), C(col, 0.55f), 0.02f * px);
        dl->AddRectFilled(P(s < 0 ? -0.082f : 0.010f, 0.13f), P(s < 0 ? -0.010f : 0.082f, 0.0f), C(col, 0.30f), 0.02f * px);
    }
    // tunic: the chest, then the skirt flaring under the belt
    dl->AddQuadFilled(P(-0.11f, 0.71f), P(0.11f, 0.71f), P(0.12f, 0.44f), P(-0.12f, 0.44f), C(col, 1.0f));
    dl->AddQuadFilled(P(-0.12f, 0.45f), P(0.12f, 0.45f), P(0.15f, 0.30f), P(-0.15f, 0.30f), C(col, 0.85f));
    dl->AddRectFilled(P(-0.122f, 0.475f), P(0.122f, 0.44f), C(col, 0.35f));
    // arms with hands
    for (int s = -1; s <= 1; s += 2) {
        dl->AddLine(P(s * 0.115f, 0.69f), P(s * 0.165f, 0.47f), C(col, 0.8f), fmaxf(1.5f, 0.05f * px));
        dl->AddCircleFilled(P(s * 0.168f, 0.455f), 0.028f * px, C(skin, 1.0f));
    }
    // neck, ears, face, hair
    dl->AddRectFilled(P(-0.025f, 0.76f), P(0.025f, 0.70f), C(skin, 0.9f));
    for (int s = -1; s <= 1; s += 2)
        dl->AddTriangleFilled(P(s * 0.055f, 0.855f), P(s * 0.06f, 0.81f), P(s * 0.165f, 0.885f), C(skin, 0.95f));
    dl->AddCircleFilled(P(0, 0.83f), 0.072f * px, C(skin, 1.0f));
    dl->PathArcTo(P(0, 0.835f), 0.08f * px, IM_PI, 2 * IM_PI);   // the hair over the top of the head
    dl->PathFillConvex(C(hair, 1.0f));
    for (int s = -1; s <= 1; s += 2)   // locks down the sides of the face
        dl->AddQuadFilled(P(s * 0.08f, 0.84f), P(s * 0.045f, 0.84f), P(s * 0.055f, 0.77f), P(s * 0.08f, 0.78f), C(hair, 0.9f));
}

// A friend as Link's mesh (link_figure.c), standing at feet and turned yaw radians (the figure's forward, +z, towards
// x = sin yaw, z = cos yaw): each vertex through the game's camera, the triangles from far to near, each shaded by its
// facing to a light from above and in front. The hair takes the friend's colour, so each Link is told apart at a
// glance; alpha is the opacity. Triangles of less than a quarter pixel are left out, which thins a far figure.
// The NDC rectangle around every figure drawn this frame (x0, y0, x1, y1): the depth to copy for the next one.
static float g_fig_rect[4];
static bool g_fig_any;

static bool draw_link_mesh(ImDrawList *dl, const LinkFigure *lf, const float *pv, const float feet[3], float yaw, ImVec4 col,
                           float alpha, float W, float H) {
    float vp[16], eye[3], map[4];
    if (!gfx_view(vp, eye, map)) return false;
    static std::vector<float> sx, sy, sw, nx, ny;
    static std::vector<std::pair<float, uint32_t>> order;
    sx.resize(lf->nverts), sy.resize(lf->nverts), sw.resize(lf->nverts), nx.resize(lf->nverts), ny.resize(lf->nverts);
    float cy = cosf(yaw), syaw = sinf(yaw);
    for (uint32_t i = 0; i < lf->nverts; i++) {
        const float *m = pv + 3 * i;   // the posed vertex
        float w[3] = {feet[0] + cy * m[0] + syaw * m[2], feet[1] + m[1], feet[2] - syaw * m[0] + cy * m[2]};
        float c[4];
        for (int r = 0; r < 4; r++) c[r] = vp[4 * r] * w[0] + vp[4 * r + 1] * w[1] + vp[4 * r + 2] * w[2] + vp[4 * r + 3];
        sw[i] = c[3];
        if (c[3] <= 0.05f) continue;
        nx[i] = c[0] / c[3], ny[i] = c[1] / c[3];
        sx[i] = (map[0] + map[1] * nx[i]) * W, sy[i] = (map[2] + map[3] * ny[i]) * H;
        if (!g_fig_any) g_fig_rect[0] = g_fig_rect[2] = nx[i], g_fig_rect[1] = g_fig_rect[3] = ny[i], g_fig_any = true;
        g_fig_rect[0] = fminf(g_fig_rect[0], nx[i]), g_fig_rect[1] = fminf(g_fig_rect[1], ny[i]);
        g_fig_rect[2] = fmaxf(g_fig_rect[2], nx[i]), g_fig_rect[3] = fmaxf(g_fig_rect[3], ny[i]);
    }
    // A soft shadow on the ground under the feet, which sets the figure on the world: two rings, darker inside.
    for (int ring = 0; ring < 2; ring++) {
        float rad = ring ? 0.28f : 0.45f, a = ring ? 0.22f : 0.16f;
        ImVec2 poly[24];
        int np_ = 0;
        for (int k = 0; k < 24; k++) {
            float ang = (float)k / 24 * 6.2831853f;
            float w[3] = {feet[0] + rad * cosf(ang), feet[1] + 0.02f, feet[2] + rad * sinf(ang)}, c[4];
            for (int r = 0; r < 4; r++) c[r] = vp[4 * r] * w[0] + vp[4 * r + 1] * w[1] + vp[4 * r + 2] * w[2] + vp[4 * r + 3];
            if (c[3] <= 0.05f) break;
            poly[np_++] = ImVec2((map[0] + map[1] * c[0] / c[3]) * W, (map[2] + map[3] * c[1] / c[3]) * H);
        }
        if (np_ == 24) dl->AddConvexPolyFilled(poly, 24, ui_col(ImVec4(0, 0, 0, 1), a * alpha));
    }
    order.clear();
    // Each triangle against the scene's depth at its centre (last frame's copy, gfx_depth_want): left out when the
    // game's scene (the player's Link, a tree, a rock) is nearer there by more than a few centimetres.
    int depth = gfx_depth_begin();
    for (uint32_t tri = 0; tri < lf->ntris; tri++) {
        const uint32_t *ix = lf->tris + 3 * tri;
        if (sw[ix[0]] <= 0.05f || sw[ix[1]] <= 0.05f || sw[ix[2]] <= 0.05f) continue;
        float area = (sx[ix[1]] - sx[ix[0]]) * (sy[ix[2]] - sy[ix[0]]) - (sx[ix[2]] - sx[ix[0]]) * (sy[ix[1]] - sy[ix[0]]);
        if (fabsf(area) < 0.5f) continue;
        float dist = (sw[ix[0]] + sw[ix[1]] + sw[ix[2]]) / 3;
        if (depth) {
            float scene = gfx_depth_at((nx[ix[0]] + nx[ix[1]] + nx[ix[2]]) / 3, (ny[ix[0]] + ny[ix[1]] + ny[ix[2]]) / 3);
            if (scene > 0 && scene < dist - 0.06f - 0.01f * dist) continue;
        }
        order.push_back({-dist, tri});
    }
    gfx_depth_end();
    if (order.empty()) return true;
    std::sort(order.begin(), order.end());
    ImVec4 base[LF_CLASSES] = {
        ImVec4(0.91f, 0.74f, 0.60f, 1),   // skin
        ImVec4(col.x * 0.9f + 0.08f, col.y * 0.9f + 0.08f, col.z * 0.9f + 0.08f, 1),   // hair: the friend's colour
        ImVec4(0.24f, 0.52f, 0.27f, 1),   // tunic: the Hylian green
        ImVec4(0.78f, 0.70f, 0.55f, 1),   // trousers
        ImVec4(0.45f, 0.30f, 0.19f, 1),   // leather
        ImVec4(0.92f, 0.92f, 0.92f, 1),   // eye
        ImVec4(0.15f, 0.12f, 0.12f, 1),   // dark
        ImVec4(0.60f, 0.66f, 0.72f, 1),   // metal
        ImVec4(0.35f, 0.78f, 0.63f, 1),   // rauru
    };
    // The light: from above, from the camera's side and a little from its left, so the side the player sees is lit
    // the way the game lights a character in daylight. Half-Lambert (the dark side keeps some light) on smooth vertex
    // normals, which are the posed faces' normals summed at each vertex.
    float rgt[2] = {1, 0}, fwd[2] = {0, 1};
    gfx_camera_ground_axes(rgt, fwd);
    float light[3] = {-fwd[0] * 0.55f - rgt[0] * 0.3f, 0.78f, -fwd[1] * 0.55f - rgt[1] * 0.3f};
    float ll = sqrtf(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
    for (float &x : light) x /= ll;
    static std::vector<float> vn, lit;
    vn.assign(3ull * lf->nverts, 0.0f);
    lit.resize(lf->nverts);
    for (uint32_t tri = 0; tri < lf->ntris; tri++) {
        const uint32_t *ix = lf->tris + 3 * tri;
        const float *a = pv + 3 * ix[0], *b = pv + 3 * ix[1], *c3 = pv + 3 * ix[2];
        float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w3[3] = {c3[0] - a[0], c3[1] - a[1], c3[2] - a[2]};
        float n[3] = {u[1] * w3[2] - u[2] * w3[1], u[2] * w3[0] - u[0] * w3[2], u[0] * w3[1] - u[1] * w3[0]};
        for (int k = 0; k < 3; k++)
            for (int q = 0; q < 3; q++) vn[3 * ix[k] + q] += n[q] * (float)lf->winding;   // area-weighted, outward
    }
    for (uint32_t i = 0; i < lf->nverts; i++) {
        float *n = &vn[3 * i];
        float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-12f) { lit[i] = 0.8f; continue; }
        float wn[3] = {(cy * n[0] + syaw * n[2]) / nl, n[1] / nl, (-syaw * n[0] + cy * n[2]) / nl};   // to the world
        float hl = (wn[0] * light[0] + wn[1] * light[1] + wn[2] * light[2]) * 0.5f + 0.5f;
        lit[i] = fminf(1.0f, 0.50f + 0.62f * hl * hl);
    }
    ImDrawListFlags flags = dl->Flags;   // no anti-aliased fringe: it shows as light seams between the triangles
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    // Textured through the font atlas (g_fig_uv0/1, the background list's own texture); untextured triangles (the
    // hair, and any part whose texture was not found) take the atlas's white corner and their class colour.
    ImVec2 uv0 = g_fig_uv0, uvd(g_fig_uv1.x - g_fig_uv0.x, g_fig_uv1.y - g_fig_uv0.y);
    ImVec2 white = g_fig_tex ? ImVec2(uv0.x + uvd.x * (4.0f / lf->atlas_side), uv0.y + uvd.y * (4.0f / lf->atlas_side))
                             : ImGui::GetIO().Fonts->TexUvWhitePixel;
    dl->PrimReserve((int)order.size() * 3, (int)order.size() * 3);
    for (const auto &o : order) {
        uint32_t tri = o.second;
        const uint32_t *ix = lf->tris + 3 * tri;
        uint8_t cls = lf->cls[tri] & 0x7f;
        bool textured = g_fig_tex && !(lf->cls[tri] & LF_UNTEXTURED);
        ImVec4 c = textured ? ImVec4(1, 1, 1, 1) : base[cls < LF_CLASSES ? cls : LF_LEATHER];
        if (textured && cls == LF_HAIR)   // the hair texture's shading in the friend's colour (its blond brightened back)
            c = ImVec4(fminf(1.0f, base[LF_HAIR].x * 1.6f), fminf(1.0f, base[LF_HAIR].y * 1.6f), fminf(1.0f, base[LF_HAIR].z * 1.6f), 1);
        const float *uv = lf->uvs + 6 * tri;
        for (int k = 0; k < 3; k++) {
            float sh = lit[ix[k]];
            ImVec2 tuv = textured ? ImVec2(uv0.x + uvd.x * uv[2 * k], uv0.y + uvd.y * uv[2 * k + 1]) : white;
            dl->PrimVtx(ImVec2(sx[ix[k]], sy[ix[k]]), tuv, ui_col(ImVec4(c.x * sh, c.y * sh, c.z * sh, 1), alpha));
        }
    }
    dl->Flags = flags;
    return true;
}

// Each friend's figure state, by name: the way they face (where they last walked, kept while they stand), their ground
// speed (smoothed), and the animation playing (link_figure.h LF_*: standing, walking, running, dashing, picked by the
// speed) with its frame; a change of animation blends from the old one over a quarter second.
struct FigureState {
    char name[MP_NAME_MAX];
    float pos[3], yaw, speed;
    uint64_t at_ms, anim_ms;   // last position update; last animation step
    int anim, prev_anim, want;
    float frame, prev_frame, blend;   // blend 1: only anim
    float want_s;                     // how long the speed has asked for want
    float fps;                        // frames a second the animations advance by
    float vel[3];                     // the drawn place's velocity (m/s, smoothed)
};

static FigureState *figure_state(const char *name, const float pos[3]) {
    static FigureState seen[MP_MAX_PLAYERS];
    int k = -1, free_k = 0;
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        if (!strcmp(seen[i].name, name)) k = i;
        if (!seen[i].name[0]) free_k = i;
    }
    uint64_t now = plat_now_ms();
    if (k < 0) {
        k = free_k;
        memset(&seen[k], 0, sizeof seen[k]);
        snprintf(seen[k].name, sizeof seen[k].name, "%s", name);
        memcpy(seen[k].pos, pos, sizeof seen[k].pos);
        seen[k].at_ms = now, seen[k].blend = 1;
    }
    FigureState *s = &seen[k];
    float dt = (float)(now - s->at_ms) / 1000.0f;
    if (dt <= 0) return s;
    if (dt > 0.5f) dt = 0.5f;
    float dx = pos[0] - s->pos[0], dz = pos[2] - s->pos[2], d = sqrtf(dx * dx + dz * dz);
    float v = d / dt;
    if (v > 40) v = 0;   // a jump (fast travel)
    s->speed += (v - s->speed) * fminf(1.0f, dt * 2.0f);   // positions come in steps (200 ms): a slow average
    if (d > 0.02f) s->yaw = atan2f(dx, dz);
    float kv = fminf(1.0f, dt * 8.0f);
    for (int c = 0; c < 3; c++) s->vel[c] += ((pos[c] - s->pos[c]) / dt - s->vel[c]) * kv;
    memcpy(s->pos, pos, sizeof s->pos);
    s->at_ms = now;
    return s;
}

// Advances a friend's animation for dt seconds: the animation by speed (between the made-for speeds of the next two),
// played at the rate that matches the friend's speed, so the feet keep pace.
static void figure_animate(FigureState *s, const LinkFigure *lf, float dt) {
    // the animation for the speed, with a margin around each threshold so a speed near one does not flip between two
    static const float up[3] = {0.45f, 2.9f, 4.5f}, down[3] = {0.25f, 2.5f, 4.1f};
    int want = s->anim;
    while (want < LF_DASH && s->speed > up[want]) want++;
    while (want > LF_WAIT && s->speed < down[want - 1]) want--;
    if (want >= lf->nanims) want = lf->nanims - 1;
    // and only once it has held for 0.3 s
    if (want != s->want) s->want = want, s->want_s = 0;
    else s->want_s += dt;
    if (want != s->anim && s->want_s >= 0.3f) {
        float phase = s->frame / (float)lf->anims[s->anim].frames;   // the stride's place, kept between moving gaits
        s->prev_anim = s->anim, s->prev_frame = s->frame, s->blend = 0;
        s->anim = want;
        s->frame = (s->anim != LF_WAIT && s->prev_anim != LF_WAIT) ? (phase - floorf(phase)) * (float)lf->anims[want].frames : 0;
    }
    float rate = 1;
    float made = lf->anims[s->anim].speed;
    if (made > 0.1f) rate = fminf(1.6f, fmaxf(0.6f, s->speed / made));
    s->fps = 30.0f * rate;
    s->frame += dt * 30.0f * rate;
    s->prev_frame += dt * 30.0f * rate;
    s->blend = fminf(1.0f, s->blend + dt * 4.0f);
}

// ---- friends in the world: a figure where each friend stands (draw_figure), with name and distance above, and a
// marker as a diamond over its spot; drawn with the game's camera of the latest frame (gfx_world_to_screen). Behind the
// scene (vk_occl.c tests three points of each figure against the game's depth) a figure fades to a faint outline of
// itself while its name stays, so a friend behind a hill can still be found. The figure is 1.8 m tall at the friend's
// feet; the distances are the radar's, along the ground from this player.
static void world_tags(const MenuTarget *t, const MpView &v, float scale, bool (*talking)(const char *, void *), void *ctx) {
    ImDrawList *dl = ImGui::GetBackgroundDrawList();   // under the menu's own windows and notices
    ImFont *font = ImGui::GetFont();
    float W = (float)t->width, H = (float)t->height, fs = ImGui::GetFontSize() * 0.8f;
    float pts[MP_MAX_PLAYERS * 3][3], vis[MP_MAX_PLAYERS * 3];
    int np = 0;
    for (int i = 0; i < v.nplayers && np + 3 <= MP_MAX_PLAYERS * 3; i++) {   // feet, chest and head of each friend
        const MpPlayer &p = v.players[i];
        if (!p.has_pos || !strcmp(p.name, v.me)) continue;
        static const float heights[3] = {0.3f, 1.0f, 1.6f};
        for (float h : heights) pts[np][0] = p.pos[0], pts[np][1] = p.pos[1] + h, pts[np][2] = p.pos[2], np++;
    }
    gfx_occlusion(pts, np, vis);
    g_fig_any = false;
    int k = 0;
    // Drawn by the game's renderer (vk_figure.c) when it can: the poses go there, the overlay keeps the names.
    const bool gbuf = gfx_figures_wanted() != 0;
    GfxFigure gfig[MP_MAX_PLAYERS];
    int ngfig = 0;
    for (int i = 0; i < v.nplayers; i++) {
        const MpPlayer &p = v.players[i];
        if (!p.has_pos || !strcmp(p.name, v.me)) continue;
        float seen = 0, known = 0;
        for (int j = 0; j < 3 && k + j < np; j++)
            if (vis[k + j] >= 0) seen += vis[k + j], known++;
        k += 3;
        float shown = known ? seen / known : 1;   // unknown (no test yet, off the depth target): drawn as seen
        float head[3] = {p.pos[0], p.pos[1] + 1.8f, p.pos[2]}, f[2], h[2], dist;
        if (!gfx_world_to_screen(p.pos, f, &dist) || !gfx_world_to_screen(head, h, NULL)) continue;
        ImVec2 feet(f[0] * W, f[1] * H), top(h[0] * W, h[1] * H);
        float tall = feet.y - top.y;
        if (tall < 10 * scale) tall = 10 * scale, top.y = feet.y - tall;   // far away: a small figure stays visible
        if (feet.x < -50 || feet.x > W + 50 || top.y > H + 50 || feet.y < -50) continue;   // off screen: the minimap shows them
        ImVec4 col = mp_name_col(p.name);
        float a = dist < 15 ? 0.3f + 0.025f * dist : 0.68f;   // fainter up close, not to hide the view
        a = 0.12f + (a - 0.12f) * shown;                     // behind the scene: a faint figure
        const LinkFigure *lf = link_figure();
        FigureState *fs_ = figure_state(p.name, p.pos);
        bool drawn = false;
        if (lf) {
            static std::vector<float> pose, pose2;
            uint64_t now = plat_now_ms();
            float dt = fs_->anim_ms ? fminf(0.1f, (float)(now - fs_->anim_ms) / 1000.0f) : 0;
            fs_->anim_ms = now;
            figure_animate(fs_, lf, dt);
            if (gbuf && ngfig < MP_MAX_PLAYERS) {   // posed by the renderer at the moment it draws
                GfxFigure &g = gfig[ngfig];
                g.anim = fs_->anim, g.prev_anim = fs_->prev_anim;
                g.frame = fs_->frame, g.prev_frame = fs_->prev_frame, g.fps = fs_->fps, g.blend = fs_->blend;
                memcpy(g.feet, p.pos, sizeof g.feet);
                memcpy(g.vel, fs_->vel, sizeof g.vel);
                g.yaw = fs_->yaw;
                g.hair[0] = col.x * 0.9f + 0.08f, g.hair[1] = col.y * 0.9f + 0.08f, g.hair[2] = col.z * 0.9f + 0.08f;
                ngfig++;
                drawn = true;
            } else {
                pose.resize(3ull * lf->nverts), pose2.resize(3ull * lf->nverts);
                link_figure_pose(lf, fs_->anim, fs_->frame, pose.data());
                if (fs_->blend < 1) {   // blend out of the previous animation
                    link_figure_pose(lf, fs_->prev_anim, fs_->prev_frame, pose2.data());
                    for (size_t q = 0; q < pose.size(); q++) pose[q] = pose2[q] + (pose[q] - pose2[q]) * fs_->blend;
                }
                drawn = draw_link_mesh(dl, lf, pose.data(), p.pos, fs_->yaw, col, 1.0f, W, H);
            }
        }
        if (!drawn) draw_figure(dl, feet, tall, col, a);
        if (talking(p.name, ctx)) dl->AddCircle(ImVec2(top.x, top.y + tall * 0.17f), tall * 0.12f + 3 * scale, ui_col(kUiZonai, 0.9f), 20, 2 * scale);
        char label[96], d[32];
        mp_distance(hypotf(p.pos[0] - v.me_pos[0], p.pos[2] - v.me_pos[2]), d, sizeof d);   // as the list measures it
        snprintf(label, sizeof label, "%s  %s", p.name, d);
        ImVec2 ls = font->CalcTextSizeA(fs, FLT_MAX, 0, label);
        ImVec2 lp(top.x - ls.x / 2, top.y - ls.y - 6 * scale);
        dl->AddRectFilled(ImVec2(lp.x - 6 * scale, lp.y - 2 * scale), ImVec2(lp.x + ls.x + 6 * scale, lp.y + ls.y + 2 * scale),
                          ui_col(kUiPanel, 0.7f), 4 * scale);
        dl->AddText(font, fs, lp, ui_col(col), label);
    }
    if (gbuf) gfx_figures_set(gfig, ngfig);
    if (g_fig_any) {   // the depth around the figures, for the next frame (a margin for their movement)
        float r[4] = {g_fig_rect[0] - 0.05f, g_fig_rect[1] - 0.05f, g_fig_rect[2] + 0.05f, g_fig_rect[3] + 0.05f};
        gfx_depth_want(r);
    }
    for (int i = 0; i < v.nmarkers; i++) {
        float above[3] = {v.markers[i].pos[0], v.markers[i].pos[1] + 2.5f, v.markers[i].pos[2]}, m[2], dist;
        if (!gfx_world_to_screen(above, m, &dist)) continue;
        ImVec2 q(m[0] * W, m[1] * H);
        if (q.x < 0 || q.x > W || q.y < 0 || q.y > H) continue;
        float k = 9 * scale;
        ImU32 col = ui_col(mp_name_col(v.markers[i].name));
        dl->AddQuadFilled(ImVec2(q.x, q.y - k), ImVec2(q.x + k, q.y), ImVec2(q.x, q.y + k), ImVec2(q.x - k, q.y), col);
        dl->AddQuad(ImVec2(q.x, q.y - k), ImVec2(q.x + k, q.y), ImVec2(q.x, q.y + k), ImVec2(q.x - k, q.y),
                    ui_col(ImVec4(0, 0, 0, 1), 0.7f), 1.5f * scale);
        char d[32];
        mp_distance(hypotf(v.markers[i].pos[0] - v.me_pos[0], v.markers[i].pos[2] - v.me_pos[2]), d, sizeof d);
        ImVec2 ds = font->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, d);
        dl->AddText(font, fs * 0.85f, ImVec2(q.x - ds.x / 2, q.y + k + 2 * scale), ui_col(kUiCream), d);
    }
}

// ---- friends on the game's own minimap (bottom right): a dot with their initial, in their colour, hollow with an arrow
// in another layer; markers as diamonds; a friend beyond the map's edge sits on its rim. The minimap turns with the
// camera (forward is up) and shows about 1 m per pixel of a 1440-row picture; its centre is 0.1819 and 0.1667 picture
// heights from the picture's right and bottom edges and its radius 0.1097 (measured on 1.4.3 at 2560x1440: walking
// 10.5 m and 11.2 m moved the map 11 pixels each time).
enum { MINI_M_PER_HEIGHT = 1440 };
static const float kMiniRight = 0.1819f, kMiniBottom = 0.1667f, kMiniRadius = 0.1097f;

static void minimap_dots(const MenuTarget *t, const MpView &v, float scale, bool (*talking)(const char *, void *), void *ctx) {
    float c01[2], hpx, rx[2], fw[2];
    if (!gfx_hud_to_screen(kMiniRight, kMiniBottom, c01, &hpx) || !gfx_camera_ground_axes(rx, fw)) return;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * 0.66f, R = kMiniRadius * hpx, ppm = hpx / MINI_M_PER_HEIGHT;
    ImVec2 c(c01[0] * (float)t->width, c01[1] * (float)t->height);
    auto place = [&](const float *pos, bool *edge) {
        float dx = pos[0] - v.me_pos[0], dz = pos[2] - v.me_pos[2];
        float mx = (dx * rx[0] + dz * rx[1]) * ppm, my = -(dx * fw[0] + dz * fw[1]) * ppm, d = hypotf(mx, my);
        float lim = R - 7 * scale;
        *edge = d > lim;
        if (*edge) mx *= lim / d, my *= lim / d;
        return ImVec2(c.x + mx, c.y + my);
    };
    for (int i = 0; i < v.nmarkers; i++) {   // markers under the players
        bool edge;
        ImVec2 m = place(v.markers[i].pos, &edge);
        float k = 6 * scale;
        dl->AddQuadFilled(ImVec2(m.x, m.y - k), ImVec2(m.x + k, m.y), ImVec2(m.x, m.y + k), ImVec2(m.x - k, m.y),
                          ui_col(mp_name_col(v.markers[i].name)));
        dl->AddQuad(ImVec2(m.x, m.y - k), ImVec2(m.x + k, m.y), ImVec2(m.x, m.y + k), ImVec2(m.x - k, m.y),
                    ui_col(ImVec4(0, 0, 0, 1), 0.7f), 1.2f * scale);
    }
    int my_layer = mp_layer(v.me_pos[1]);
    for (int i = 0; i < v.nplayers; i++) {
        const MpPlayer &p = v.players[i];
        if (!p.has_pos || !strcmp(p.name, v.me)) continue;
        bool edge;
        ImVec2 q = place(p.pos, &edge);
        float r = 7 * scale;
        ImVec4 col = mp_name_col(p.name);
        int layer = mp_layer(p.pos[1]);
        if (talking(p.name, ctx)) dl->AddCircle(q, r + 3 * scale, ui_col(kUiZonai), 0, 2 * scale);
        dl->AddCircleFilled(q, r + 1.5f * scale, ui_col(ImVec4(0, 0, 0, 1), 0.6f));   // readable over a light map
        if (layer == my_layer) dl->AddCircleFilled(q, r, ui_col(col));
        else dl->AddCircle(q, r, ui_col(col), 0, 2.2f * scale);
        char ini[8];
        mp_initial(p.name, ini);
        ImVec2 is = font->CalcTextSizeA(fs, FLT_MAX, 0, ini);
        dl->AddText(font, fs, ImVec2(q.x - is.x / 2, q.y - is.y / 2),
                    ui_col(layer == my_layer ? ImVec4(0.05f, 0.07f, 0.07f, 1) : col), ini);
        if (layer != my_layer) {   // above or below this player: a small arrow beside the dot
            bool up = (layer == MP_LAYER_SKY) || (layer == MP_LAYER_SURFACE && my_layer == MP_LAYER_DEPTHS);
            float ax = q.x + r + 4 * scale, k = 4 * scale;
            if (up) dl->AddTriangleFilled(ImVec2(ax, q.y - k), ImVec2(ax - k, q.y + k * 0.6f), ImVec2(ax + k, q.y + k * 0.6f), ui_col(col));
            else dl->AddTriangleFilled(ImVec2(ax, q.y + k), ImVec2(ax - k, q.y - k * 0.6f), ImVec2(ax + k, q.y - k * 0.6f), ui_col(col));
        }
    }
}

// Friends' positions arrive every 200 ms while they move. Between two, each friend keeps going at the velocity of the
// last two (for at most a quarter second, so a stop does not overshoot), and the drawn place follows that guess with a
// short lag (a tenth of a second), so a running friend moves evenly instead of in steps. A jump of over 50 m (fast
// travel) or a second without news snaps.
static void smooth_positions(MpView &v) {
    static struct {
        char name[MP_NAME_MAX];
        float got[3], vel[3], shown[3];   // the last received place, the velocity, the drawn place
        uint64_t got_ms, at;
    } held[MP_MAX_PLAYERS];
    uint64_t now = plat_now_ms();
    for (int i = 0; i < v.nplayers; i++) {
        MpPlayer &p = v.players[i];
        if (!p.has_pos || !strcmp(p.name, v.me)) continue;
        int k = -1, old = 0;
        for (int j = 0; j < MP_MAX_PLAYERS; j++) {
            if (!strcmp(held[j].name, p.name)) k = j;
            if (held[j].at < held[old].at) old = j;
        }
        auto *h = k >= 0 ? &held[k] : NULL;
        if (!h || now - h->at > 1000 ||
            hypotf(p.pos[0] - h->got[0], p.pos[2] - h->got[2]) + fabsf(p.pos[1] - h->got[1]) > 50) {
            h = &held[k < 0 ? old : k];
            snprintf(h->name, sizeof h->name, "%s", p.name);
            memcpy(h->got, p.pos, sizeof p.pos), memcpy(h->shown, p.pos, sizeof p.pos);
            memset(h->vel, 0, sizeof h->vel);
            h->got_ms = h->at = now;
            continue;
        }
        if (memcmp(p.pos, h->got, sizeof p.pos)) {   // a new place: the velocity since the last one
            float span = (float)(now - h->got_ms) / 1000.0f;
            for (int c = 0; c < 3; c++) h->vel[c] = span > 0.05f ? (p.pos[c] - h->got[c]) / fminf(span, 0.5f) : 0;
            memcpy(h->got, p.pos, sizeof p.pos);
            h->got_ms = now;
        }
        float ahead = fminf(0.25f, (float)(now - h->got_ms) / 1000.0f);
        float a = 1 - expf(-(float)(now - h->at) / 100.0f);
        for (int c = 0; c < 3; c++) {
            float guess = h->got[c] + h->vel[c] * ahead;
            h->shown[c] += (guess - h->shown[c]) * a;
        }
        memcpy(p.pos, h->shown, sizeof p.pos);
        h->at = now;
    }
}

static void radar_draw(const MenuTarget *t, float scale) {
    if (full_menu()) return;
    MpView v;
    mp_view(&v);
    smooth_positions(v);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * 0.78f, margin = 18 * scale;
    float right = (float)t->width - margin, y = 64 * scale;
    char speak[VOICE_SPEAKERS][VOICE_NAME_MAX];
    int nspeak = voice_speakers(speak, VOICE_SPEAKERS);
    auto talking = [&](const char *name) {
        for (int i = 0; i < nspeak; i++)
            if (!strcmp(speak[i], name)) return true;
        return false;
    };
    if (radar_on(v))
        world_tags(t, v, scale, [](const char *name, void *ctx) { return (*(decltype(talking) *)ctx)(name); }, &talking);
    if (radar_on(v)) minimap_dots(t, v, scale, [](const char *name, void *ctx) { return (*(decltype(talking) *)ctx)(name); }, &talking);
    // the list: friends with their distance and layer (or just who talks, without positions)
    float lh = fs + 10 * scale;
    for (int i = 0; i < v.nplayers; i++) {
        const MpPlayer &p = v.players[i];
        if (!strcmp(p.name, v.me)) continue;
        bool talk = talking(p.name);
        bool where = radar_on(v) && p.has_pos;
        if (!where && !talk) continue;
        char text[128], dist[32] = "";
        if (where) {
            mp_distance(hypotf(p.pos[0] - v.me_pos[0], p.pos[2] - v.me_pos[2]), dist, sizeof dist);
            snprintf(text, sizeof text, "%s  ·  %s  ·  %s", p.name, dist, menu_text(kLayerName[mp_layer(p.pos[1])]));
        } else {
            snprintf(text, sizeof text, "%s", p.name);
        }
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, text);
        float w = ts.x + 30 * scale, x = right - w;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(right, y + lh), ui_col(kUiPanel, 0.78f), lh / 2);
        dl->AddCircleFilled(ImVec2(x + 12 * scale, y + lh / 2), 5 * scale, ui_col(talk ? kUiZonai : mp_name_col(p.name)));
        dl->AddText(font, fs, ImVec2(x + 22 * scale, y + 5 * scale), ui_col(talk ? kUiZonai : kUiCream), text);
        y += lh + 4 * scale;
    }
    if (voice_transmitting()) {
        const char *me = menu_text(UI_MENU_MP_TALKING);
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, me);
        float w = ts.x + 30 * scale, x = right - w;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(right, y + lh), ui_col(kUiPanel, 0.85f), lh / 2);
        dl->AddCircleFilled(ImVec2(x + 12 * scale, y + lh / 2), 5 * scale, ui_col(kUiDanger));
        dl->AddText(font, fs, ImVec2(x + 22 * scale, y + 5 * scale), ui_col(kUiCream), me);
    }
}

// Players: one card each, the most achievements first.
static void mp_page_players(const MpView &v, float h, float scale) {
    ImGui::BeginChild("##mp_players", ImVec2(0, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), small = fs * 0.8f, w = ImGui::GetContentRegionAvail().x, gap = 14 * scale;
    int cols = w > 1100 * scale ? 3 : 2;
    float cw = (w - gap * (cols - 1)) / cols, ch = 150 * scale, pad = 14 * scale;
    int order[MP_MAX_PLAYERS];
    for (int i = 0; i < v.nplayers; i++) order[i] = i;
    for (int i = 1; i < v.nplayers; i++)
        for (int j = i; j > 0 && v.players[order[j]].stats[MP_STAT_ACHIEVEMENTS] >
                                   v.players[order[j - 1]].stats[MP_STAT_ACHIEVEMENTS]; j--) {
            int t = order[j];
            order[j] = order[j - 1], order[j - 1] = t;
        }
    ImVec2 o = ImGui::GetCursorScreenPos();
    for (int i = 0; i < v.nplayers; i++) {
        const MpPlayer &pl = v.players[order[i]];
        bool me = !strcmp(pl.name, v.me), talk = mp_speaking(pl.name) || (me && voice_transmitting());
        ImVec2 p(o.x + (i % cols) * (cw + gap), o.y + (i / cols) * (ch + gap)), q(p.x + cw, p.y + ch);
        ImVec4 nc = mp_name_col(pl.name);
        dl->AddRectFilled(p, q, ui_col(me ? kUiZonai : kUiCream, me ? 0.07f : 0.035f), 12 * scale);
        dl->AddRect(p, q, ui_col(i == 0 ? kUiGold : kUiMuted, i == 0 ? 0.6f : 0.22f), 12 * scale, 0, 1.2f * scale);
        // the avatar, ringed green while talking
        float r = 24 * scale;
        ImVec2 ac(p.x + pad + r, p.y + pad + r);
        if (talk) dl->AddCircle(ac, r + 4 * scale, ui_col(kUiZonai), 0, 3 * scale);
        dl->AddCircleFilled(ac, r, ui_col(nc, 0.9f));
        char ini[8];
        mp_initial(pl.name, ini);
        ImVec2 is = font->CalcTextSizeA(fs * 1.2f, FLT_MAX, 0, ini);
        dl->AddText(font, fs * 1.2f, ImVec2(ac.x - is.x / 2, ac.y - is.y / 2), ui_col(ImVec4(0.05f, 0.07f, 0.07f, 1)), ini);
        float tx = ac.x + r + 12 * scale;
        dl->AddText(font, fs, ImVec2(tx, p.y + pad + 2 * scale), ui_col(me ? kUiGoldHi : kUiCream), pl.name);
        char sub[96];
        if (me) {
            snprintf(sub, sizeof sub, "%s%s", menu_text(UI_MENU_MP_YOU), talk ? "  ·  " : "");
        } else if (pl.has_pos && v.me_has_pos) {
            char dist[32];
            mp_distance(hypotf(pl.pos[0] - v.me_pos[0], pl.pos[2] - v.me_pos[2]), dist, sizeof dist);
            snprintf(sub, sizeof sub, "%s  ·  %s%s", menu_text(kLayerName[mp_layer(pl.pos[1])]), dist, talk ? "  ·  " : "");
        } else {
            sub[0] = 0;
        }
        if (talk) snprintf(sub + strlen(sub), sizeof sub - strlen(sub), "%s", menu_text(UI_MENU_MP_TALKING));
        dl->AddText(font, small, ImVec2(tx, p.y + pad + fs * 1.25f), ui_col(talk ? kUiZonai : kUiMuted), sub);
        char rank[8];
        snprintf(rank, sizeof rank, "#%d", i + 1);
        float rw = font->CalcTextSizeA(fs, FLT_MAX, 0, rank).x;
        dl->AddText(font, fs, ImVec2(q.x - pad - rw, p.y + pad), ui_col(i == 0 ? kUiGold : kUiMuted), rank);
        // the six figures in two rows of three
        float gy = p.y + pad * 2 + r * 2, gw = (cw - pad * 2) / 3, ir = 8 * scale;
        for (int k = 0; k < MP_STATS; k++) {
            float x = p.x + pad + (k % 3) * gw, y = gy + (k / 3) * (fs * 1.6f);
            mp_stat_icon(dl, k, ImVec2(x + ir, y + fs * 0.5f), ir, ui_col(kStatCol[k]));
            char num[16];
            snprintf(num, sizeof num, "%d", pl.stats[k]);
            dl->AddText(font, fs, ImVec2(x + ir * 2 + 8 * scale, y), ui_col(kUiCream), num);
        }
    }
    int rows = (v.nplayers + cols - 1) / cols;
    ImGui::Dummy(ImVec2(w, rows * (ch + gap)));
    ImGui::EndChild();
}

// Group challenges: one card per track with its four medals and the next tier's progress.
static void mp_page_challenges(const MpView &v, float h, float scale) {
    ImGui::BeginChild("##mp_challenges", ImVec2(0, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), small = fs * 0.8f, w = ImGui::GetContentRegionAvail().x, gap = 14 * scale;
    if (!v.ntracks) {   // a room from before the tracks: its one challenge
        ImGui::TextColored(kUiGold, "%s", menu_text(UI_MENU_MP_CHALLENGE));
        if (v.challenges_done) ImGui::TextColored(kUiZonai, "%s", menu_text(UI_MENU_MP_CHALLENGES_DONE));
        else if (v.challenge_target > 0) {
            ImGui::TextUnformatted(v.challenge);
            char over[32];
            snprintf(over, sizeof over, "%d / %d", v.challenge_total, v.challenge_target);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kUiZonai);
            ImGui::ProgressBar(fminf(1.0f, (float)v.challenge_total / (float)v.challenge_target), ImVec2(-1, 0), over);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        return;
    }
    int tiers = 0, all = v.ntracks * MP_TIERS;
    for (int i = 0; i < v.ntracks; i++) tiers += v.tracks[i].done;
    char head[128];
    snprintf(head, sizeof head, menu_text(UI_MENU_MP_TIERS_TOTAL), tiers, all);
    ImGui::TextColored(kUiGoldHi, "%s", head);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kTierCol[2]);
    ImGui::ProgressBar((float)tiers / (float)all, ImVec2(-1, 6 * scale), "");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(1, 8 * scale));
    int cols = 2;
    float cw = (w - gap) / cols, ch = 118 * scale, pad = 14 * scale;
    ImVec2 o = ImGui::GetCursorScreenPos();
    static const UiTextId tier_names[MP_TIERS] = {UI_MENU_MP_TIER_1, UI_MENU_MP_TIER_2, UI_MENU_MP_TIER_3,
                                                  UI_MENU_MP_TIER_4};
    for (int i = 0; i < v.ntracks; i++) {
        const MpTrack &t = v.tracks[i];
        ImVec2 p(o.x + (i % cols) * (cw + gap), o.y + (i / cols) * (ch + gap)), q(p.x + cw, p.y + ch);
        bool full = t.done >= MP_TIERS;
        ImVec4 cur = kTierCol[full ? MP_TIERS - 1 : t.done];
        dl->AddRectFilled(p, q, ui_col(cur, full ? 0.10f : 0.045f), 12 * scale);
        dl->AddRect(p, q, ui_col(cur, full ? 0.7f : 0.28f), 12 * scale, 0, 1.2f * scale);
        float ir = 14 * scale;
        mp_stat_icon(dl, t.stat, ImVec2(p.x + pad + ir, p.y + pad + ir), ir, ui_col(kStatCol[t.stat]));
        dl->AddText(font, fs * 1.05f, ImVec2(p.x + pad * 2 + ir * 2, p.y + pad + ir - fs * 0.55f), ui_col(kUiCream),
                    menu_text(kStatName[t.stat]));
        // the four medals, filled once reached
        float mr = 10 * scale;
        for (int k = 0; k < MP_TIERS; k++) {
            ImVec2 mc(q.x - pad - mr - (MP_TIERS - 1 - k) * (mr * 2 + 8 * scale), p.y + pad + ir);
            if (k < t.done) {
                dl->AddCircleFilled(mc, mr, ui_col(kTierCol[k]));
                dl->AddCircle(mc, mr * 0.55f, ui_col(ImVec4(1, 1, 1, 1), 0.5f), 0, 1.2f * scale);
            } else {
                dl->AddCircle(mc, mr, ui_col(kTierCol[k], 0.55f), 0, 1.6f * scale);
            }
        }
        float by = p.y + pad + ir * 2 + 12 * scale;
        char line[128], over[48];
        if (full) {
            snprintf(line, sizeof line, "%s", menu_text(UI_MENU_MP_TRACK_DONE));
            snprintf(over, sizeof over, "%d", t.total);
        } else {
            snprintf(line, sizeof line, menu_text(UI_MENU_MP_TRACK_NEXT), menu_text(tier_names[t.done]), t.target[t.done]);
            snprintf(over, sizeof over, "%d / %d", t.total, t.target[t.done]);
        }
        dl->AddText(font, small, ImVec2(p.x + pad, by), ui_col(full ? kTierCol[MP_TIERS - 1] : kUiMuted), line);
        float ow = font->CalcTextSizeA(small, FLT_MAX, 0, over).x;
        dl->AddText(font, small, ImVec2(q.x - pad - ow, by), ui_col(kUiCream), over);
        float bar_y = by + small * 1.5f, bx0 = p.x + pad, bx1 = q.x - pad, bh = 8 * scale;
        float k = full ? 1.0f : fminf(1.0f, (float)t.total / (float)fmaxf(1, (float)t.target[t.done]));
        dl->AddRectFilled(ImVec2(bx0, bar_y), ImVec2(bx1, bar_y + bh), ui_col(kUiMuted, 0.2f), bh / 2);
        dl->AddRectFilled(ImVec2(bx0, bar_y), ImVec2(bx0 + (bx1 - bx0) * k, bar_y + bh), ui_col(cur, 0.9f), bh / 2);
    }
    int rows = (v.ntracks + cols - 1) / cols;
    ImGui::Dummy(ImVec2(w, rows * (ch + gap)));
    ImGui::EndChild();
}

// The chat: bubbles (this player's on the right), the room's events between them, the field at the bottom.
static void mp_page_chat(const MpView &v, float h, float scale) {
    static char chat[300];
    float input_h = ImGui::GetFrameHeightWithSpacing() + 6 * scale;
    ImGui::BeginChild("##mp_log", ImVec2(0, h - input_h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), small = fs * 0.78f, w = ImGui::GetContentRegionAvail().x, pad = 10 * scale;
    static MpLine lines[80];
    int n = mp_lines(lines, 80);
    static int last_n = -1;
    static int64_t last_when;
    for (int i = 0; i < n; i++) {
        const MpLine &l = lines[i];
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (l.kind == MP_LINE_CHAT) {
            bool mine = !strcmp(l.from, v.me);
            float maxw = w * 0.68f;
            ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, maxw - pad * 2, l.text);
            float nh = mine ? 0 : small * 1.25f, bw = fmaxf(ts.x, mine ? 0 : font->CalcTextSizeA(small, FLT_MAX, 0, l.from).x) + pad * 2;
            float bh = ts.y + nh + pad * 1.4f, x = mine ? p.x + w - bw : p.x;
            ImVec4 bc = mine ? kUiZonai : mp_name_col(l.from);
            dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + bw, p.y + bh), ui_col(bc, mine ? 0.16f : 0.10f), 10 * scale);
            if (!mine) dl->AddText(font, small, ImVec2(x + pad, p.y + pad * 0.6f), ui_col(bc), l.from);
            dl->AddText(font, fs, ImVec2(x + pad, p.y + pad * 0.7f + nh), ui_col(kUiCream), l.text, nullptr, maxw - pad * 2);
            ImGui::Dummy(ImVec2(w, bh + 6 * scale));
        } else {   // an event or a friend's unlock: a centred line
            char t[400];
            if (l.kind == MP_LINE_ACH) {
                char who[160];
                snprintf(who, sizeof who, menu_text(UI_MENU_MP_FRIEND_ACH), l.from);
                snprintf(t, sizeof t, "%s: %s", who, l.text);
            } else {
                snprintf(t, sizeof t, "%s", l.text);
            }
            ImVec2 ts = font->CalcTextSizeA(small, FLT_MAX, w * 0.9f, t);
            dl->AddText(font, small, ImVec2(p.x + (w - ts.x) / 2, p.y), ui_col(l.kind == MP_LINE_ACH ? kUiZonai : kUiMuted),
                        t, nullptr, w * 0.9f);
            ImGui::Dummy(ImVec2(w, ts.y + 8 * scale));
        }
    }
    if (n != last_n || (n && lines[n - 1].when != last_when)) ImGui::SetScrollHereY(1.0f);   // a new line: the bottom
    last_n = n, last_when = n ? lines[n - 1].when : 0;
    ImGui::EndChild();
    float bw = ImGui::CalcTextSize(menu_text(UI_MENU_MP_SEND)).x + ImGui::GetStyle().FramePadding.x * 4;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x);
    bool send = ImGui::InputText("##mp_say", chat, sizeof chat, ImGuiInputTextFlags_EnterReturnsTrue);
    if (send) ImGui::SetKeyboardFocusHere(-1);   // keep typing after Enter
    ImGui::SameLine();
    if ((ImGui::Button(menu_text(UI_MENU_MP_SEND)) || send) && chat[0] && mp_send_chat(chat)) chat[0] = 0;
    g_mp_chat_seen = n ? lines[n - 1].when : g_mp_chat_seen;
    g_mp_chat_frame = ImGui::GetFrameCount();
}

static void tab_multiplayer(float scale) {
    MpView v;
    mp_view(&v);
    if (g_test.page >= 0)   // the menu test hook's screenshots: TOTK_MENU_TEST_MP_SUB picks the sub-page shown
        if (const char *sub = cfg_get("TOTK_MENU_TEST_MP_SUB")) g_mp_sub = atoi(sub) % MP_SUBS;
    mp_banner(v, scale);
    float h = fmaxf(ImGui::GetContentRegionAvail().y - g_page_bottom, 160 * scale);
    if (v.state != MP_CONNECTED) {   // not in a room: the connection page alone
        ImGui::BeginChild("##mp_room", ImVec2(0, h), ImGuiChildFlags_NavFlattened);
        mp_connection(v, scale);
        ImGui::EndChild();
        return;
    }
    // the unread badge: friends' chat lines newer than the chat page last showed
    static MpLine lines[80];
    int n = mp_lines(lines, 80), unread = 0;
    for (int i = 0; i < n; i++)
        unread += lines[i].kind == MP_LINE_CHAT && lines[i].when > g_mp_chat_seen && strcmp(lines[i].from, v.me);
    mp_subtabs(scale, unread);
    h = fmaxf(ImGui::GetContentRegionAvail().y - g_page_bottom, 120 * scale);
    switch (g_mp_sub) {
    case MP_SUB_PLAYERS: mp_page_players(v, h, scale); break;
    case MP_SUB_CHALLENGES: mp_page_challenges(v, h, scale); break;
    case MP_SUB_ACH: {
        ImGui::TextColored(kUiMuted, "%s", menu_text(UI_MENU_MP_ACH_NOTE));
        ach_cards("##mp_ach", fmaxf(ImGui::GetContentRegionAvail().y - g_page_bottom, 80 * scale), true, scale);
        break;
    }
    case MP_SUB_CHAT: mp_page_chat(v, h, scale); break;
    default:
        ImGui::BeginChild("##mp_room", ImVec2(0, h), ImGuiChildFlags_NavFlattened);
        mp_connection(v, scale);
        ImGui::EndChild();
    }
}

// ---- the room's notices (net_mp.c mp_take_toast): chat lines, friends' unlocks and the room's events, stacked at
// the bottom left for a few seconds each; a friend's unlock shows its icon when this install has that achievement.
enum { MP_NOTES = 4 };
static struct { MpLine l; double at; } g_mp_notes[MP_NOTES];
static int g_mp_nnotes;
static const double kMpNoteSecs = 6.0;

static bool mp_notes_tick(void) {
    double now = plat_now_ns() / 1e9;
    int w = 0;
    for (int i = 0; i < g_mp_nnotes; i++)
        if (now - g_mp_notes[i].at < kMpNoteSecs) g_mp_notes[w++] = g_mp_notes[i];
    g_mp_nnotes = w;
    MpLine l;
    while (mp_take_toast(&l)) {
        if (g_mp_nnotes == MP_NOTES) {   // the oldest goes
            memmove(g_mp_notes, g_mp_notes + 1, sizeof g_mp_notes[0] * (MP_NOTES - 1));
            g_mp_nnotes--;
        }
        g_mp_notes[g_mp_nnotes].l = l, g_mp_notes[g_mp_nnotes].at = now;
        g_mp_nnotes++;
    }
    return g_mp_nnotes > 0;
}

static void mp_notes_draw(const MenuTarget *t, float scale) {
    // the chat box and the chat page show the lines themselves
    if (!g_mp_nnotes || g_chat.load() || (full_menu() && ImGui::GetFrameCount() - g_mp_chat_frame <= 1)) return;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    // above the menu's bottom row while the menu is open
    float fs = ImGui::GetFontSize() * 0.8f, pad = 10 * scale, icon = 40 * scale,
          y = (float)t->height - (full_menu() ? 96 : 24) * scale;
    double now = plat_now_ns() / 1e9;
    for (int i = g_mp_nnotes - 1; i >= 0; i--) {
        const MpLine &l = g_mp_notes[i].l;
        double age = now - g_mp_notes[i].at;
        float a = (float)fmin(1.0, fmin(age / 0.25, (kMpNoteSecs - age) / 0.5));
        if (a <= 0) continue;
        char head[160], body[340];
        ImVec4 col = kUiCream;
        int idx = -1;
        if (l.kind == MP_LINE_ACH) {
            snprintf(head, sizeof head, menu_text(UI_MENU_MP_FRIEND_ACH), l.from);
            snprintf(body, sizeof body, "%s", l.text);
            static const char *const rar[] = {"common", "rare", "epic", "legendary", "platinum"};
            for (int r = 0; r < 5; r++)
                if (!strcmp(l.rarity, rar[r])) col = ach_rarity_col(r);
            idx = ach_find(l.ach_id);
        } else if (l.kind == MP_LINE_CHAT) {
            snprintf(head, sizeof head, "%s", l.from);
            snprintf(body, sizeof body, "%s", l.text);
            col = kUiGoldHi;
        } else {
            head[0] = 0;
            snprintf(body, sizeof body, "%s", l.text);
            col = kUiZonai;
        }
        float maxw = (float)t->width * 0.32f;
        ImVec2 hs = font->CalcTextSizeA(fs, FLT_MAX, 0, head);
        ImVec2 bs = font->CalcTextSizeA(fs, FLT_MAX, maxw, body);
        float tw = fmaxf(hs.x, bs.x), th = (head[0] ? hs.y + 2 * scale : 0) + bs.y;
        float ic = idx >= 0 ? icon + pad : 0, w = pad * 2 + ic + tw, h = fmaxf(pad * 2 + th, idx >= 0 ? icon + pad * 2 : 0);
        float x0 = (full_menu() ? 124 : 24) * scale;   // past the rail while it shows
        ImVec2 p(x0, y - h), q(p.x + w, y);
        dl->AddRectFilled(p, q, ui_col(kUiPanel, 0.88f * a), 10 * scale);
        dl->AddRect(p, q, ui_col(col, 0.6f * a), 10 * scale, 0, 1.2f * scale);
        if (idx >= 0) {
            AchView v;
            if (ach_get(idx, &v)) ach_icon(dl, idx, v, ImVec2(p.x + pad, p.y + (h - icon) / 2), icon, true, a);
        }
        float tx = p.x + pad + ic, ty = p.y + (h - th) / 2;
        if (head[0]) {
            dl->AddText(font, fs, ImVec2(tx, ty), ui_col(col, a), head);
            ty += hs.y + 2 * scale;
        }
        dl->AddText(font, fs, ImVec2(tx, ty), ui_col(kUiCream, a), body, nullptr, maxw);
        y -= h + 8 * scale;
    }
}

static const UiTextId kTabNames[TAB_COUNT] = {UI_MENU_TAB_SCREEN, UI_MENU_TAB_GAME, UI_MENU_TAB_SOUND,
                                              UI_MENU_TAB_CONTROLS, UI_MENU_TAB_AMIIBO, UI_MENU_TAB_MENU,
                                              UI_MENU_TAB_ADVANCED, UI_MENU_TAB_MODS, UI_MENU_TAB_CHEATS,
                                              UI_MENU_TAB_ACHIEVEMENTS, UI_MENU_TAB_MULTIPLAYER};
static int g_tab;               // the tab shown
static int g_tab_request = -1;  // select this tab at the next layout; -1 none
static int g_focus_page = -1;   // this tab's first row takes the cursor once it is shown; -1 none

// A row of another tab: that tab opens with the row under the cursor.
static void go_to(int tab, const char *key) {
    g_tab_request = tab;
    opt_focus_key(key);
}

// ---- Mods: the mods folder (mods.c) with switches and the load order, then the port's own patches. The folder was
// read at start; the rows change the order and switches for the next start.
static void mods_builtin_rows(void) {
    opt_group(UI_MENU_MODS_BUILTIN);
    char fps[64], res[64];
    const char *lim = cfg_value("TOTK_FPS_LIMIT");
    if (!strcmp(lim, "display")) snprintf(fps, sizeof fps, "%s", menu_text(UI_MENU_FPS_DISPLAY));
    else snprintf(fps, sizeof fps, "%s FPS", lim);
    if (opt_button("##mods_fps60", UI_MENU_MODS_FPS60, UI_MENU_MODS_FPS60_DESC, fps)) go_to(TAB_GAME, "TOTK_FPS_LIMIT");
    int w = 0, h = 0;
    sscanf(cfg_value("TOTK_OUTPUT_RES"), "%dx%d", &w, &h);
    snprintf(res, sizeof res, "%d × %d", w, h);   // U+00D7, as row_output_res shows it
    if (opt_button("##mods_res", UI_MENU_MODS_RES, UI_MENU_MODS_RES_DESC, res)) go_to(TAB_SCREEN, "TOTK_OUTPUT_RES");
    opt_toggle("TOTK_REALTIME", UI_MENU_REALTIME, UI_MENU_REALTIME_DESC);
    opt_toggle("TOTK_CLOTH_REALTIME", UI_MENU_CLOTH_REALTIME, UI_MENU_CLOTH_REALTIME_DESC);
    opt_toggle("TOTK_HIGHFPS_FIX", UI_MENU_HIGHFPS_FIX, UI_MENU_HIGHFPS_FIX_DESC);
    opt_button("##mods_quickmenu", UI_MENU_MODS_QUICKMENU, UI_MENU_MODS_QUICKMENU_DESC,
               menu_text(romfs_patches_loaded() ? UI_MENU_ON : UI_MENU_OFF));
}

static void tab_mods(float scale) {
    opt_begin("##mods", scale);
    opt_group(UI_MENU_MODS_INSTALLED);
    int n = mods_count();
    if (!n) opt_text(UI_MENU_MODS_NONE);
    const char *after = mods_changed() ? menu_text(UI_MENU_AFTER_RESTART) : nullptr;
    for (int i = 0; i < n; i++) {
        const ModInfo *m = mods_at(i);
        char id[32], desc[1536];
        snprintf(id, sizeof id, "##mod%d", i);
        char over[512] = "", other[512] = "";
        if (m->overridden) snprintf(over, sizeof over, menu_text(UI_MENU_MODS_OVERRIDDEN), m->overridden, m->overridden_by);
        if (m->other_version) snprintf(other, sizeof other, menu_text(UI_MENU_MODS_OTHER_VERSION), m->other_version);
        char cheat[256] = "";
        if (m->kind & MOD_CHEATS) {   // its cheats as read at start, or the file they come from
            int count = 0;
            for (int j = 0; j < cheats_count(); j++)
                count += !strcmp(cheats_info(j)->mod, m->name) && cheats_info(j)->kind != CHEAT_HEADING;
            if (count) snprintf(cheat, sizeof cheat, menu_text(UI_MENU_MODS_CHEATS), count);
            else snprintf(cheat, sizeof cheat, menu_text(UI_MENU_MODS_CHEATS_FILE), cheats_file());
        }
        char code[512] = "", problem[512] = "";
        if (m->nsets && m->overrides[0])
            snprintf(code, sizeof code, menu_text(UI_MENU_MODS_REPLACES_BUILTIN), m->nsets, m->overrides);
        else if (m->nsets)
            snprintf(code, sizeof code, menu_text(UI_MENU_MODS_EXEFS_ON), m->nsets);
        switch (m->problem) {
        case MOD_EMPTY: snprintf(problem, sizeof problem, "%s", menu_text(UI_MENU_MODS_EMPTY)); break;
        case MOD_PLUGIN: snprintf(problem, sizeof problem, "%s", menu_text(UI_MENU_MODS_SUBSDK)); break;
        case MOD_EXEFS_LATER: snprintf(problem, sizeof problem, "%s", menu_text(UI_MENU_MODS_EXEFS_LATER)); break;
        case MOD_EXEFS_UNSUPPORTED:
            snprintf(problem, sizeof problem, menu_text(UI_MENU_MODS_EXEFS_UNSUPPORTED), m->note);
            break;
        case MOD_EXEFS_OTHER: snprintf(problem, sizeof problem, "%s", menu_text(UI_MENU_MODS_EXEFS_OTHER)); break;
        case MOD_BAD_NAME: snprintf(problem, sizeof problem, "%s", menu_text(UI_MENU_MODS_BAD_NAME)); break;
        default: break;
        }
        int k = snprintf(desc, sizeof desc, menu_text(UI_MENU_MODS_ROW_DESC), m->replaced, m->added);
        const char *parts[] = {over, other, cheat, code, problem, menu_text(UI_MENU_MODS_MOVE_HINT)};
        for (const char *part : parts)
            if (*part && k >= 0 && k < (int)sizeof desc) k += snprintf(desc + k, sizeof desc - k, "\n\n%s", part);
        const char *note = m->enabled && m->problem != MOD_OK ? menu_text(UI_MENU_MODS_UNUSED) : after;
        int s = opt_text_row(id, m->name, menu_text(m->enabled ? UI_MENU_ON : UI_MENU_OFF), note, desc, true);
        if (s == 2 || s == -1 || s == 1) {
            mods_set_enabled(i, !m->enabled);
            opt_save_soon();
        } else if ((s == 3 && i > 0) || (s == 4 && i + 1 < n)) {
            int to = s == 3 ? i - 1 : i + 1;
            mods_move(i, to - i);
            snprintf(id, sizeof id, "##mod%d", to);
            opt_focus_key(id);   // the cursor follows the mod
            opt_save_soon();
        }
    }
    bool rebuild =
        cheats_rebuild_pending() && opt_button("##restart", UI_MENU_CHEATS_REBUILD, UI_MENU_CHEATS_REBUILD_DESC);
    static uint64_t open_failed;   // plat_now_ns until which the button says it failed
    if (opt_button("##mods_open", UI_MENU_MODS_OPEN, UI_MENU_MODS_OPEN_DESC,
                   plat_now_ns() < open_failed ? menu_text(UI_MENU_ACTION_FAILED) : "")) {
        bool ok = mods_dir()[0] != 0;
        if (ok) plat_file_mkdir_p(mods_dir());
        if (!ok || !plat_open_folder(mods_dir())) open_failed = plat_now_ns() + 3000000000ull;
    }
    mods_builtin_rows();
    opt_end();
    if (rebuild) {
        restart_rebuild = true;
        ImGui::OpenPopup("##restart_confirm");
    }
    restart_confirm();
}

// ---- the look: a panel from the left edge over the dimmed game, a rail of tab icons down its left side and the page
// beside it, in the look of the UltraTears overlay (colours in option_rows.h). The panel slides in when the menu opens
// and a page fades in when its tab is chosen.
static double g_opened_at, g_tab_at;   // ImGui::GetTime() the menu opened, the tab showed
static int g_last_layout = -2;         // ImGui frame count of the last layout: a gap means the menu opened again

static float ease_out(float x) {
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return 1 - (1 - x) * (1 - x) * (1 - x);
}

// A tab's icon (TAB_COUNT: close), line drawings of radius r around c.
static void tab_icon(ImDrawList *dl, int tab, ImVec2 c, float r, ImU32 col, float th) {
    switch (tab) {
    case TAB_SCREEN:   // a monitor on its stand
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.78f), ImVec2(c.x + r, c.y + r * 0.42f), col, r * 0.16f, 0, th);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.42f), ImVec2(c.x, c.y + r * 0.82f), col, th);
        dl->AddLine(ImVec2(c.x - r * 0.48f, c.y + r * 0.86f), ImVec2(c.x + r * 0.48f, c.y + r * 0.86f), col, th);
        break;
    case TAB_GAME: {   // three triangles in one: the outline and the upturned middle
        float hs = r * 1.0f, hh = hs * 0.866f;
        ImVec2 t(c.x, c.y - hh), l(c.x - hs, c.y + hh * 0.9f), rr(c.x + hs, c.y + hh * 0.9f);
        ImVec2 ml((t.x + l.x) / 2, (t.y + l.y) / 2), mr((t.x + rr.x) / 2, (t.y + rr.y) / 2), mb(c.x, l.y);
        dl->AddTriangle(t, rr, l, col, th);
        dl->AddTriangle(ml, mr, mb, col, th);
        break;
    }
    case TAB_SOUND: {   // a speaker and two waves
        ImVec2 body[] = {{c.x - r, c.y - r * 0.32f}, {c.x - r * 0.5f, c.y - r * 0.32f}, {c.x - r * 0.05f, c.y - r * 0.78f},
                         {c.x - r * 0.05f, c.y + r * 0.78f}, {c.x - r * 0.5f, c.y + r * 0.32f}, {c.x - r, c.y + r * 0.32f}};
        dl->AddPolyline(body, 6, col, ImDrawFlags_Closed, th);
        static const float waves[] = {0.42f, 0.82f};
        for (float k : waves) {
            dl->PathArcTo(ImVec2(c.x + r * 0.05f, c.y), r * k, -0.8f, 0.8f, 12);
            dl->PathStroke(col, 0, th);
        }
        break;
    }
    case TAB_CONTROLS:   // a controller: the body, a cross, two buttons
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.55f), ImVec2(c.x + r, c.y + r * 0.6f), col, r * 0.5f, 0, th);
        dl->AddLine(ImVec2(c.x - r * 0.72f, c.y), ImVec2(c.x - r * 0.28f, c.y), col, th);
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y - r * 0.22f), ImVec2(c.x - r * 0.5f, c.y + r * 0.22f), col, th);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.36f, c.y + r * 0.12f), r * 0.12f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.62f, c.y - r * 0.12f), r * 0.12f, col);
        break;
    case TAB_MENU:   // a globe: the menu's language
        dl->AddCircle(c, r * 0.9f, col, 24, th);
        dl->AddEllipse(c, ImVec2(r * 0.4f, r * 0.9f), col, 0, 20, th);
        dl->AddLine(ImVec2(c.x - r * 0.9f, c.y), ImVec2(c.x + r * 0.9f, c.y), col, th);
        break;
    case TAB_AMIIBO: {   // a figure on its round stand
        dl->AddCircle(ImVec2(c.x, c.y - r * 0.55f), r * 0.26f, col, 16, th);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.29f), ImVec2(c.x, c.y + r * 0.35f), col, th);
        dl->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.05f), ImVec2(c.x + r * 0.4f, c.y - r * 0.05f), col, th);
        dl->AddEllipse(ImVec2(c.x, c.y + r * 0.62f), ImVec2(r * 0.72f, r * 0.24f), col, 0, 20, th);
        break;
    }
    case TAB_MULTIPLAYER:   // two figures side by side
        for (int k = -1; k <= 1; k += 2) {
            float x = c.x + k * r * 0.42f;
            dl->AddCircle(ImVec2(x, c.y - r * 0.38f), r * 0.24f, col, 14, th);
            dl->PathArcTo(ImVec2(x, c.y + r * 0.62f), r * 0.42f, 3.14159f, 6.28318f, 12);
            dl->PathStroke(col, 0, th);
        }
        break;
    case TAB_ACHIEVEMENTS: {   // a trophy: the cup, its handles, the stem and the base
        ImVec2 cup[] = {{c.x - r * 0.62f, c.y - r * 0.82f}, {c.x + r * 0.62f, c.y - r * 0.82f}, {c.x + r * 0.5f, c.y - r * 0.1f},
                        {c.x, c.y + r * 0.22f}, {c.x - r * 0.5f, c.y - r * 0.1f}};
        dl->AddPolyline(cup, 5, col, ImDrawFlags_Closed, th);
        dl->PathArcTo(ImVec2(c.x - r * 0.62f, c.y - r * 0.5f), r * 0.3f, 1.5707963f, 4.712389f, 10);
        dl->PathStroke(col, 0, th);
        dl->PathArcTo(ImVec2(c.x + r * 0.62f, c.y - r * 0.5f), r * 0.3f, -1.5707963f, 1.5707963f, 10);
        dl->PathStroke(col, 0, th);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.22f), ImVec2(c.x, c.y + r * 0.62f), col, th);
        dl->AddLine(ImVec2(c.x - r * 0.45f, c.y + r * 0.82f), ImVec2(c.x + r * 0.45f, c.y + r * 0.82f), col, th);
        dl->AddLine(ImVec2(c.x - r * 0.25f, c.y + r * 0.62f), ImVec2(c.x + r * 0.25f, c.y + r * 0.62f), col, th);
        break;
    }
    case TAB_ADVANCED:   // a gear
        dl->AddCircle(c, r * 0.42f, col, 16, th);
        for (int i = 0; i < 8; i++) {
            float an = i * 0.7853982f, cs = cosf(an), sn = sinf(an);
            dl->AddLine(ImVec2(c.x + cs * r * 0.62f, c.y + sn * r * 0.62f), ImVec2(c.x + cs * r * 0.95f, c.y + sn * r * 0.95f),
                        col, th * 1.6f);
        }
        break;
    case TAB_MODS: {   // a box, its lid open
        dl->AddRect(ImVec2(c.x - r * 0.8f, c.y - r * 0.15f), ImVec2(c.x + r * 0.8f, c.y + r * 0.85f), col, r * 0.1f, 0, th);
        ImVec2 lid[] = {{c.x - r * 0.8f, c.y - r * 0.15f}, {c.x - r * 0.45f, c.y - r * 0.85f},
                        {c.x + r * 0.95f, c.y - r * 0.85f}, {c.x + r * 0.8f, c.y - r * 0.15f}};
        dl->AddPolyline(lid, 4, col, 0, th);
        dl->AddLine(ImVec2(c.x - r * 0.25f, c.y + r * 0.3f), ImVec2(c.x + r * 0.25f, c.y + r * 0.3f), col, th);
        break;
    }
    case TAB_CHEATS: {   // a five-pointed star
        ImVec2 pts[10];
        for (int i = 0; i < 10; i++) {
            float an = -1.5707963f + i * 0.6283185f, rr = i % 2 ? r * 0.42f : r * 0.95f;
            pts[i] = ImVec2(c.x + cosf(an) * rr, c.y + sinf(an) * rr + r * 0.05f);
        }
        dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, th);
        break;
    }
    default:   // close
        dl->AddLine(ImVec2(c.x - r * 0.6f, c.y - r * 0.6f), ImVec2(c.x + r * 0.6f, c.y + r * 0.6f), col, th);
        dl->AddLine(ImVec2(c.x + r * 0.6f, c.y - r * 0.6f), ImVec2(c.x - r * 0.6f, c.y + r * 0.6f), col, th);
    }
}

// A square button of the rail; the chosen tab is lit in teal with a slow glow. Not a nav item: the D-pad stays on the
// page, and L / R change the tab.
static bool rail_button(int tab, bool active, float size, float scale, const char *tip) {
    ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + size, p.y + size), c(p.x + size / 2, p.y + size / 2);
    ImGui::PushID(tab);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    bool clicked = ImGui::InvisibleButton("##rail", ImVec2(size, size));
    ImGui::PopItemFlag();
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = size * 0.24f;
    if (active) {
        float glow = 0.7f + 0.3f * sinf((float)ImGui::GetTime() * 2.2f), g = 3 * scale;
        dl->AddRect(ImVec2(p.x - g, p.y - g), ImVec2(q.x + g, q.y + g), ui_col(kUiZonai, 0.10f * glow), r + g, 0, 2 * g);
        dl->AddRectFilled(p, q, ui_col(kUiZonai, 0.13f), r);
        dl->AddRect(p, q, ui_col(kUiZonai, 0.60f), r, 0, 1.3f * scale);
    } else if (hov) {
        dl->AddRectFilled(p, q, ui_col(ImVec4(1, 1, 1, 1), 0.06f), r);
    }
    tab_icon(dl, tab, c, size * 0.25f, active ? ui_col(kUiZonai) : hov ? ui_col(kUiCream) : ui_col(kUiMuted), 2 * scale);
    if (hov) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// The rail: the mark at the top, a button per tab, close at the bottom. A click on a tab asks for it (g_tab_request),
// except while a confirmation or a key capture is open.
static void layout_rail(float x, float rail, float H, float scale) {
    ImGui::SetNextWindowPos(ImVec2(x, 0));
    ImGui::SetNextWindowSize(ImVec2(rail, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##rail", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 c(x + rail / 2, 46 * scale);   // the mark: a gold diamond in a teal haze
    float r = 14 * scale, glow = 0.6f + 0.4f * sinf((float)ImGui::GetTime() * 1.5f);
    ImVec2 d[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
    dl->AddPolyline(d, 4, ui_col(kUiZonai, 0.16f * glow), ImDrawFlags_Closed, 8 * scale);
    dl->AddPolyline(d, 4, ui_col(kUiGold), ImDrawFlags_Closed, 1.6f * scale);
    dl->AddCircleFilled(c, 2.5f * scale, ui_col(kUiGoldHi));
    float bs = 62 * scale;
    ImGui::SetCursorPosY(96 * scale);
    for (int t = 0; t < TAB_COUNT; t++) {
        ImGui::SetCursorPosX((rail - bs) / 2);
        if (rail_button(t, g_tab == t, bs, scale, menu_text(kTabNames[t])) && t != g_tab && !g_modal && !binds_modal())
            g_tab_request = t;
        ImGui::Dummy(ImVec2(1, 6 * scale));
    }
    float cs = 52 * scale;
    ImGui::SetCursorPos(ImVec2((rail - cs) / 2, H - 26 * scale - cs));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (ImGui::InvisibleButton("##close", ImVec2(cs, cs))) set_open(0);
    ImGui::PopItemFlag();
    bool hov = ImGui::IsItemHovered();
    dl->AddRect(p, ImVec2(p.x + cs, p.y + cs), ui_col(kUiGold, hov ? 0.55f : 0.25f), 12 * scale, 0, 1.2f * scale);
    tab_icon(dl, TAB_COUNT, ImVec2(p.x + cs / 2, p.y + cs / 2), cs * 0.22f, hov ? ui_col(kUiCream) : ui_col(kUiMuted),
             2 * scale);
    if (hov) ImGui::SetTooltip("%s", menu_text(UI_MENU_CONTINUE));
    ImGui::End();
}

// ---- the menu: the rail, then the title, the page and the bottom line with the keys, "Continue" and "Quit game". L / R
// or Q / E switch tabs; B on the gamepad and Esc close it.
static void layout_menu(uint32_t w, uint32_t h, float scale) {
    double now = ImGui::GetTime();
    if (ImGui::GetFrameCount() != g_last_layout + 1) g_opened_at = g_tab_at = now;
    g_last_layout = ImGui::GetFrameCount();
    float W = (float)w, H = (float)h, k = ease_out((float)((now - g_opened_at) / 0.24));
    float pw = fminf(W * 0.94f, 1560 * scale), x = -pw * (1 - k), rail = 104 * scale;

    // the game dimmed, darker on the panel's side; the panel, its rail and its edges; its shadow on the game
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    ImVec4 shade(0.016f, 0.031f, 0.027f, 1);
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), ui_col(shade, 0.78f * k), ui_col(shade, 0.35f * k),
                                ui_col(shade, 0.35f * k), ui_col(shade, 0.78f * k));
    bg->AddRectFilled(ImVec2(x, 0), ImVec2(x + pw, H), ui_col(kUiPanel, kUiPanel.w));
    bg->AddRectFilled(ImVec2(x, 0), ImVec2(x + rail, H), ui_col(ImVec4(1, 1, 1, 1), 0.025f));
    bg->AddLine(ImVec2(x + rail, 0), ImVec2(x + rail, H), ui_col(kUiGold, 0.12f), 1);
    bg->AddLine(ImVec2(x + pw - 0.5f, 0), ImVec2(x + pw - 0.5f, H), ui_col(kUiGold, 0.30f), 1);
    bg->AddRectFilledMultiColor(ImVec2(x + pw, 0), ImVec2(x + pw + 56 * scale, H), ui_col(ImVec4(0, 0, 0, 1), 0.5f * k),
                                0, 0, ui_col(ImVec4(0, 0, 0, 1), 0.5f * k));

    int step = g_modal || binds_modal() || ImGui::GetIO().WantTextInput                                  ? 0
             : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
             : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1
                                                                                                         : 0;
    if (step) g_tab_request = (g_tab + step + TAB_COUNT) % TAB_COUNT;
    layout_rail(x, rail, H, scale);
    if (g_tab_request >= 0) {   // the new page's first row takes the cursor, and the page fades in
        if (g_tab_request != g_tab) g_tab_at = now;
        g_tab = g_tab_request;
        g_focus_page = g_tab;
        g_tab_request = -1;
    }

    float px = 40 * scale;
    ImGui::SetNextWindowPos(ImVec2(x + rail, 0));
    ImGui::SetNextWindowSize(ImVec2(pw - rail, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px, 30 * scale));
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    const ImGuiStyle &st = ImGui::GetStyle();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), small = fs * 0.78f;

    // the title, large in pale gold; the tab's name under it; the port version and the game update on the right (what a
    // bug report needs first)
    ImVec2 tp = ImGui::GetCursorScreenPos();
    float content_w = ImGui::GetContentRegionAvail().x, title = fs * 1.45f;
    dl->AddText(font, title, tp, ui_col(kUiGoldHi), menu_text(UI_MENU_TITLE));
    dl->AddText(font, small, ImVec2(tp.x, tp.y + title * 1.08f), ui_col(kUiMuted), menu_text(kTabNames[g_tab]));
    char game[16], version[96];
    snprintf(game, sizeof game, "%d.%d.%d", rt_game_version / 100, rt_game_version / 10 % 10, rt_game_version % 10);
    snprintf(version, sizeof version, menu_text(UI_MENU_VERSION), rt_port_version, game);
    float vw = font->CalcTextSizeA(small, FLT_MAX, 0, version).x;
    dl->AddText(font, small, ImVec2(tp.x + content_w - vw, tp.y + (title - small) * 0.5f), ui_col(kUiMuted), version);
    ImGui::Dummy(ImVec2(content_w, title * 1.08f + small * 1.6f));

    // the page, fading and rising in after a tab change. Its rows are a NavFlattened child of this window itself: ImGui
    // clips the items of a direct child only, so behind a second child level the rows scrolled out above the page won
    // D-pad moves from the buttons.
    float fade = ease_out((float)((now - g_tab_at - 0.03) / 0.28));
    float bottom = ImGui::GetFrameHeight() + st.ItemSpacing.y * 2 + 8 * scale;
    if (g_focus_page == g_tab) {
        if (g_tab == TAB_ACHIEVEMENTS) g_ach_focus = true;   // its cards are not option rows
        else opt_focus_first();
        g_focus_page = -1;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st.Alpha * fade);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1 - fade) * 12 * scale);
    opt_reserve_bottom(bottom + (1 - fade) * 12 * scale);
    g_page_bottom = bottom + (1 - fade) * 12 * scale;
    switch (g_tab) {
    case TAB_SCREEN: tab_screen(scale); break;
    case TAB_GAME: tab_game(scale); break;
    case TAB_SOUND: tab_sound(scale); break;
    case TAB_CONTROLS: tab_controls(scale); break;
    case TAB_MENU: tab_menu(scale); break;
    case TAB_AMIIBO:   // its own panes in a child that leaves the bottom row free, as opt_begin does for the others
        ImGui::BeginChild("##amiibo", ImVec2(0, fmaxf(ImGui::GetContentRegionAvail().y - g_page_bottom, 100 * scale)),
                          ImGuiChildFlags_NavFlattened);
        tab_amiibo(scale);
        ImGui::EndChild();
        break;
    case TAB_ADVANCED: tab_advanced(scale); break;
    case TAB_MODS: tab_mods(scale); break;
    case TAB_CHEATS:
        if (tab_cheats(scale)) {
            restart_rebuild = true;
            ImGui::OpenPopup("##restart_confirm");
        }
        restart_confirm();
        break;
    case TAB_ACHIEVEMENTS: tab_achievements(scale); break;
    case TAB_MULTIPLAYER: tab_multiplayer(scale); break;
    }
    ImGui::PopStyleVar();

    // the bottom line: a gold rule, the keys (or the save failure) on the left, the two buttons on the right
    ImGui::Dummy(ImVec2(1, 8 * scale));
    float y = ImGui::GetCursorPosY();
    ImVec2 rule = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(rule.x, rule.y - 4 * scale), ImVec2(rule.x + content_w, rule.y - 4 * scale), ui_col(kUiGold, 0.14f), 1);
    float ty = rule.y + st.ItemSpacing.y * 0.5f + (ImGui::GetFrameHeight() - small) * 0.5f;
    if (!cfg_last_save_ok())   // stays until a save succeeds; the settings hold for the session
        dl->AddText(font, small, ImVec2(rule.x, ty), ui_col(kUiDanger), menu_text(UI_MENU_SAVE_FAILED));
    else
        dl->AddText(font, small, ImVec2(rule.x, ty), ui_col(kUiMuted), menu_text(UI_MENU_HINT));
    const char *cont = menu_text(UI_MENU_CONTINUE), *quit = menu_text(UI_MENU_QUIT);
    float bw = fmaxf(ImGui::CalcTextSize(cont).x, ImGui::CalcTextSize(quit).x) + st.FramePadding.x * 4;
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - px - bw * 2 - st.ItemSpacing.x, y + st.ItemSpacing.y * 0.5f));
    if (ImGui::Button(cont, ImVec2(bw, 0))) set_open(0);
    ImGui::SameLine();
    if (ImGui::Button(quit, ImVec2(bw, 0))) gfx_window_request_close();
    bool was_modal = g_modal || binds_modal();
    if (g_tab != TAB_ADVANCED && g_tab != TAB_CONTROLS && g_tab != TAB_MODS && g_tab != TAB_CHEATS) g_modal = false;
    if (!was_modal && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        set_open(0);
    ImGui::End();
}

static int g_save_pending;   // a changed setting still waits for its write to totk.ini (opt_save_tick)

// ---- the performance overlay (TOTK_PERF_OVERLAY), in the top right corner. "fps": the frame rate over the presents of
// the last half second. "full": a panel of perfmon.c's numbers, in the style of SavageWindRecomp's perf_overlay_draw
// (menu.mm): the machine's load and temperatures, the frame rate and frame times with a graph, the render thread's waits.
// Off, it costs a cfg_version load per present.
static unsigned g_perf_ver = ~0u;
static int g_perf_mode;                  // 0 off, 1 fps, 2 full
static std::atomic<int> g_perf_save;     // F10 changed the mode: totk.ini is written on the present thread
static uint64_t g_perf_last_ns, g_perf_win_ns, g_perf_max_ns;
static int g_perf_frames;
static char g_perf_text[96];

static int perf_mode_of(const char *s) { return !strcmp(s, "full") ? 2 : !strcmp(s, "fps") ? 1 : 0; }

extern "C" void menu_perf_cycle(void) {
    static const char *const modes[] = {"off", "fps", "full"};
    cfg_set("TOTK_PERF_OVERLAY", modes[(perf_mode_of(cfg_value("TOTK_PERF_OVERLAY")) + 1) % 3]);
    g_perf_save.store(1);
}

// The device the game renders on, for perfmon's GPU counters (by LUID), NVML (by name) and ADL (by PCI bus, else
// name); once, before full mode.
static void perf_gpu_once(const MenuTarget *t) {
    static bool done;
    if (done) return;
    done = true;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *ep);
    bool has_bus = false;
    if (ep && vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, ep) >= 0)
        for (uint32_t i = 0; i < ne && !has_bus; i++) has_bus = !strcmp(ep[i].extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    free(ep);
    VkPhysicalDevicePCIBusInfoPropertiesEXT bus = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, has_bus ? &bus : nullptr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(t->phys, &p2);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    uint64_t vram = 0;
    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[h].size;
    perfmon_set_gpu(p2.properties.deviceName, id.deviceLUIDValid ? id.deviceLUID : NULL, has_bus ? (int)bus.pciBus : -1,
                    (float)(vram >> 20));
}

static void perf_tick(uint64_t now, const MenuTarget *t) {
    if (cfg_version() != g_perf_ver) {
        g_perf_ver = cfg_version();
        int m = perf_mode_of(cfg_value("TOTK_PERF_OVERLAY"));
        if (m != g_perf_mode) {
            g_perf_last_ns = 0, g_perf_text[0] = 0;
            if (m == 2) perf_gpu_once(t);
            perfmon_mode(m);
        }
        g_perf_mode = m;
    }
    if (g_perf_mode != 1) return;
    uint64_t d = now - g_perf_last_ns;
    if (!g_perf_last_ns || d > 2000000000ull) {   // first frame, or a stall the window should not average in
        g_perf_win_ns = g_perf_max_ns = 0;
        g_perf_frames = 0;
    } else {
        g_perf_win_ns += d;
        g_perf_frames++;
        if (d > g_perf_max_ns) g_perf_max_ns = d;
        if (g_perf_win_ns >= 500000000ull) {
            snprintf(g_perf_text, sizeof g_perf_text, menu_text(UI_PERF_LINE_FPS), g_perf_frames * 1e9 / g_perf_win_ns);
            g_perf_win_ns = g_perf_max_ns = 0;
            g_perf_frames = 0;
        }
    }
    g_perf_last_ns = now;
}

// The panel keeps its width while the numbers change: every column is as wide as the widest text it can show, measured
// on samples of 8s (Roboto's digits share one width), and the numbers are right-aligned in it.
struct PerfCol { UiTextId fmt; float sample; };

static float perf_text_w(UiTextId fmt, float v) {
    char b[64];
    snprintf(b, sizeof b, menu_text(fmt), v);
    return ImGui::CalcTextSize(b).x;
}

// A fixed column as wide as the widest of the formats (with their samples) and labels given.
static void perf_column(const char *id, const PerfCol *cols, int n, const UiTextId *labels = nullptr, int nl = 0) {
    float w = ImGui::CalcTextSize(menu_text(UI_MENU_OV_NONE)).x;
    for (int i = 0; i < n; i++) w = fmaxf(w, perf_text_w(cols[i].fmt, cols[i].sample));
    for (int i = 0; i < nl; i++) w = fmaxf(w, ImGui::CalcTextSize(menu_text(labels[i])).x);
    ImGui::TableSetupColumn(id, ImGuiTableColumnFlags_WidthFixed, w);
}

static void perf_right(const char *s, bool dim) {
    float pad = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(s).x;
    if (pad > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    if (dim) ImGui::TextDisabled("%s", s);
    else ImGui::TextUnformatted(s);
}

// One cell of the full panel's table: fmt (a catalog format, one number) with v, or "-" when the machine gives none.
static void perf_cell(UiTextId fmt, float v) {
    ImGui::TableNextColumn();
    if (v < 0) return perf_right(menu_text(UI_MENU_OV_NONE), true);
    char b[64];
    snprintf(b, sizeof b, menu_text(fmt), v);
    perf_right(b, false);
}

static void perf_label(UiTextId label, const ImVec4 &c) {
    ImGui::TableNextColumn();
    ImGui::TextColored(c, "%s", menu_text(label));
}

static float perf_gb(float mb) { return mb < 0 ? -1 : mb / 1024; }

// The picture lines: the window, the game image the present shows and the 3D scene it holds (resolution.c), how the
// present scales the image to the window (gfx_present.c, the last present), the frame limit and the presentation mode
// in use.
static void perf_picture(const MenuTarget *t, const ImVec4 &c) {
    unsigned out[2], scene[2];
    res_sizes(out, scene);
    char v[6][64];
    snprintf(v[0], sizeof v[0], menu_text(UI_MENU_OV_FMT_SIZE), t->width, t->height);
    snprintf(v[1], sizeof v[1], menu_text(UI_MENU_OV_FMT_SIZE), (unsigned)t->image_w, (unsigned)t->image_h);
    snprintf(v[2], sizeof v[2], menu_text(UI_MENU_OV_FMT_SCENE), scene[0], scene[1], scene[0] * 100.0 / out[0]);
    if (t->scaler == 2) {
        int s = cfg_int("TOTK_SHARPNESS");
        snprintf(v[3], sizeof v[3], menu_text(UI_MENU_OV_FMT_FSR), s < 0 ? 0 : s > 100 ? 100 : s);
    } else {
        // FSR enlarges only (vk_fsr_record): an image larger than the window is shrunk by the blit even with FSR selected
        int down = t->scaler == 1 && !strcmp(cfg_value("TOTK_UPSCALER"), "fsr") &&
                   (t->image_w > (int)t->width || t->image_h > (int)t->height);
        snprintf(v[3], sizeof v[3], "%s",
                 menu_text(down ? UI_MENU_OV_SCALE_DOWN : t->scaler ? UI_MENU_OV_SCALE_BILINEAR : UI_MENU_OV_SCALE_NONE));
    }
    double hz = frame_step_limit_hz();
    snprintf(v[4], sizeof v[4], "%s", menu_text(UI_MENU_OV_NONE));
    if (hz > 0) snprintf(v[4], sizeof v[4], menu_text(UI_MENU_OV_FPS), hz);
    const char *pm = present_name(gfx_window_present_mode());
    snprintf(v[5], sizeof v[5], "%s", pm ? pm : menu_text(UI_MENU_OV_NONE));
    static const UiTextId labels[6] = {UI_MENU_OV_WINDOW, UI_MENU_OV_OUTPUT, UI_MENU_OV_SCENE,
                                       UI_MENU_OV_SCALING, UI_MENU_OV_LIMIT, UI_MENU_OV_PRESENT};
    if (!ImGui::BeginTable("##perfp", 2, ImGuiTableFlags_SizingFixedFit)) return;
    float vw = 0;
    for (auto &s : v) vw = fmaxf(vw, ImGui::CalcTextSize(s).x);
    ImGui::TableSetupColumn("##p0", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##p1", ImGuiTableColumnFlags_WidthFixed, vw);
    for (int i = 0; i < 6; i++) {
        perf_label(labels[i], c);
        ImGui::TableNextColumn();
        perf_right(v[i], false);
    }
    ImGui::EndTable();
}

static void perf_full_draw(const MenuTarget *t, float scale) {
    float w = (float)t->width;
    PerfStats s;
    perfmon_snapshot(&s);
    ImGui::SetNextWindowPos(ImVec2(w - 12 * scale, 12 * scale), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##perf", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    // under the menu while it is open (the settings stay readable), else over everything
    if (full_menu()) ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    else ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::SetWindowFontScale(0.72f);
    const ImVec4 gpu_c(0.55f, 0.83f, 0.31f, 1), cpu_c(0.18f, 0.59f, 0.80f, 1), ram_c(0.80f, 0.45f, 0.70f, 1),
        fps_c(0.95f, 0.95f, 0.95f, 1), wait_c(0.93f, 0.76f, 0.33f, 1), pic_c(0.62f, 0.70f, 0.95f, 1);
    if (ImGui::BeginTable("##perft", 4, ImGuiTableFlags_SizingFixedFit)) {
        static const UiTextId labels[] = {UI_MENU_OV_GPU, UI_MENU_OV_VRAM, UI_MENU_OV_CPU, UI_MENU_OV_RAM};
        static const PerfCol c0[] = {{UI_MENU_OV_FPS, 888}};
        static const PerfCol c1[] = {{UI_MENU_OV_FMT_PCT, 100}, {UI_MENU_OV_FMT_GB, 888.8f}, {UI_MENU_OV_FRAME_MS, 888.8f}};
        static const PerfCol c2[] = {{UI_MENU_OV_FMT_TEMP, 188}, {UI_MENU_OV_FMT_GB_OF, 888.8f}, {UI_MENU_OV_MAX_MS, 8888.8f}};
        static const PerfCol c3[] = {{UI_MENU_OV_FMT_POWER, 888}, {UI_MENU_OV_FMT_MHZ, 8888}, {UI_MENU_OV_GAME_PCT, 100},
                                     {UI_MENU_OV_GAME_GB, 888.8f}};
        perf_column("##c0", c0, 1, labels, 4);
        perf_column("##c1", c1, 3);
        perf_column("##c2", c2, 3);
        perf_column("##c3", c3, 4);
        perf_label(UI_MENU_OV_GPU, gpu_c);
        perf_cell(UI_MENU_OV_FMT_PCT, s.gpu_pct);
        perf_cell(UI_MENU_OV_FMT_TEMP, s.gpu_temp_c);
        perf_cell(UI_MENU_OV_FMT_POWER, s.gpu_power_w);
        perf_label(UI_MENU_OV_VRAM, gpu_c);
        perf_cell(UI_MENU_OV_FMT_GB, perf_gb(s.vram_used_mb));
        perf_cell(UI_MENU_OV_FMT_GB_OF, perf_gb(s.vram_total_mb));
        perf_cell(UI_MENU_OV_FMT_MHZ, s.gpu_clock_mhz);
        perf_label(UI_MENU_OV_CPU, cpu_c);
        perf_cell(UI_MENU_OV_FMT_PCT, s.cpu_total_pct);
        perf_cell(UI_MENU_OV_FMT_TEMP, s.cpu_temp_c);
        perf_cell(UI_MENU_OV_GAME_PCT, s.cpu_game_pct);
        perf_label(UI_MENU_OV_RAM, ram_c);
        perf_cell(UI_MENU_OV_FMT_GB, perf_gb(s.ram_used_mb));
        perf_cell(UI_MENU_OV_FMT_GB_OF, perf_gb(s.ram_total_mb));
        perf_cell(UI_MENU_OV_GAME_GB, perf_gb(s.ram_game_mb));
        ImGui::TableNextColumn();   // the frame row: no label, the rate in its place
        char b[64];
        snprintf(b, sizeof b, menu_text(UI_MENU_OV_FPS), s.fps);
        ImGui::TextColored(fps_c, "%s", b);
        perf_cell(UI_MENU_OV_FRAME_MS, s.frame_ms_avg);
        perf_cell(UI_MENU_OV_MAX_MS, s.frame_ms_max);
        ImGui::EndTable();
    }
    float top = s.frame_ms_max > 50 ? s.frame_ms_max : 50;
    // as wide as the panel the tables size; the plot does not count toward that width, so the panel can still shrink
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    float max_x = win->DC.CursorMaxPos.x;
    ImGui::PlotLines("##ft", s.frame_ms, PERF_HISTORY, 0, nullptr, 0, top, ImVec2(-FLT_MIN, 48 * scale));
    win->DC.CursorMaxPos.x = max_x;
    // per frame, averaged over the last half second; the labels' column stretches, so the numbers end at the panel's
    // right edge, and the table still asks for no more than its widest label and number
    if (ImGui::BeginTable("##perfw", 2, ImGuiTableFlags_SizingFixedFit)) {
        static const PerfCol c1[] = {{UI_MENU_OV_FMT_MS, 888.88f}, {UI_MENU_OV_PER_FRAME, 888.8f}};
        ImGui::TableSetupColumn("##w0", ImGuiTableColumnFlags_WidthStretch);
        perf_column("##w1", c1, 2);
        perf_label(UI_MENU_OV_REPLAY, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.replay_ms);
        perf_label(UI_MENU_OV_GPU_TIME, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.gpu_ms);
        perf_label(UI_MENU_OV_PIPE_WAIT, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.pipe_wait_ms);
        perf_label(UI_MENU_OV_SHADER_WAIT, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.xlate_ms);
        perf_label(UI_MENU_OV_TEX_UPLOADS, wait_c);
        perf_cell(UI_MENU_OV_PER_FRAME, s.tex_uploads);
        ImGui::EndTable();
    }
    perf_picture(t, pic_c);
    if (s.gpu_name[0]) ImGui::TextDisabled("%s", s.gpu_name);
    if (s.cpu_name[0]) ImGui::TextDisabled("%s", s.cpu_name);
    ImGui::End();
}

static void perf_draw(const MenuTarget *t, float scale) {
    if (g_perf_mode == 2) return perf_full_draw(t, scale);
    float w = (float)t->width;
    if (!g_perf_mode || !g_perf_text[0]) return;
    // with the menu open: under its window, over the dimming layout_menu drew before
    ImDrawList *dl = full_menu() ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    ImVec2 sz = ImGui::CalcTextSize(g_perf_text);
    float pad = 6 * scale;
    ImVec2 p(w - 12 * scale - sz.x - pad * 2, 12 * scale);   // the top right corner: the game's HUD leaves it empty
    dl->AddRectFilled(p, ImVec2(p.x + sz.x + pad * 2, p.y + sz.y + pad * 2), IM_COL32(0, 0, 0, 150), 4 * scale);
    dl->AddText(ImVec2(p.x + pad, p.y + pad), IM_COL32(255, 255, 255, 235), g_perf_text);
}

// ---- the shader notice (TOTK_SHADER_TOAST): while the background workers build pipelines for more than a moment, a
// small panel in the bottom right corner says so, so a short stutter has a visible reason. It fades out once they are
// done. Off, it costs a cfg_bool per present.
static float g_toast_a;          // 0 hidden .. 1 shown
static uint64_t g_toast_since;   // plat_now_ns the workers became busy, 0 idle
static uint32_t g_toast_n;       // pipelines in flight, for the text

// Advances the notice; true while it is shown or fading.
static bool toast_tick(uint64_t now, float dt) {
    uint32_t n = cfg_bool("TOTK_SHADER_TOAST") ? vk_pipeline_building() : 0;
    if (!n) g_toast_since = 0;
    else if (!g_toast_since) g_toast_since = now;
    if (n) g_toast_n = n;
    bool want = n && now - g_toast_since > 300000000ull;   // a build shorter than 0.3 s shows nothing
    float to = want ? 1.0f : 0.0f, rate = want ? 6.0f : 2.5f;
    g_toast_a += (to - g_toast_a) * fminf(1.0f, dt * rate);
    if (!want && g_toast_a < 0.01f) g_toast_a = 0;
    return g_toast_a > 0 || want;
}

static void toast_draw(const MenuTarget *t, float scale) {
    if (g_toast_a <= 0) return;
    char text[96];
    snprintf(text, sizeof text, menu_text(UI_MENU_TOAST_SHADERS), g_toast_n);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * 0.78f, a = g_toast_a, pad = 14 * scale, icon = fs * 1.2f;
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, text);
    float w = pad * 2 + icon + 10 * scale + ts.x, h = pad * 1.5f + fs, rise = (1 - a) * 10 * scale;
    ImVec2 p((float)t->width - 24 * scale - w, (float)t->height - 24 * scale - h + rise), q(p.x + w, p.y + h);
    dl->AddRectFilled(p, q, ui_col(kUiPanel, 0.90f * a), 12 * scale);
    dl->AddRect(p, q, ui_col(kUiZonai, 0.40f * a), 12 * scale, 0, 1.2f * scale);
    // a diamond turning a quarter at a time, then resting
    float tm = (float)ImGui::GetTime(), ph = fmodf(tm, 1.1f), turn = ph < 0.55f ? ph / 0.55f : 1.0f;
    float ang = (floorf(tm / 1.1f) + turn * turn * (3 - 2 * turn)) * 1.5707963f, r = icon * 0.42f;
    ImVec2 c(p.x + pad + icon * 0.5f, p.y + h * 0.5f), d[4];
    for (int i = 0; i < 4; i++) {
        float an = ang + i * 1.5707963f;
        d[i] = ImVec2(c.x + cosf(an) * r, c.y + sinf(an) * r);
    }
    dl->AddPolyline(d, 4, ui_col(kUiZonai, a), ImDrawFlags_Closed, 1.8f * scale);
    dl->AddText(font, fs, ImVec2(p.x + pad + icon + 10 * scale, p.y + (h - fs) * 0.5f), ui_col(kUiCream, a), text);
}

// ---- the achievement notice (TOTK_ACH_POPUP): a panel sliding down at the top of the screen for each unlock
// (achievements.c queues them after a save), with the icon, "Achievement unlocked" in the rarity's colour and the name.
// The platinum one stays longer and glows gold; a batch of more than three (an old save on the first run) is one panel.
static int g_note = -1, g_note_count;   // the achievement shown (index), how many its batch held
static double g_note_at;                // ImGui-independent clock (seconds) it appeared
static const double kNoteSecs = 5.0, kNotePlatSecs = 8.0;

static double note_clock(void) { return plat_now_ns() / 1e9; }

static bool note_tick(void) {
    double now = note_clock();
    if (g_note >= 0) {
        AchView v;
        double len = ach_get(g_note, &v) && v.rarity == ACH_PLATINUM && g_note_count == 1 ? kNotePlatSecs : kNoteSecs;
        if (now - g_note_at < len) return true;
        g_note = -1;
    }
    // Not on the title screen or a loading screen: the unlocks a save already held at start (recorded while the save
    // storage came up) wait until the player has been in the world for two seconds.
    if (frame_step_world_us() < 2000000) return false;
    int count = 0, next = ach_take_notice(&count);
    if (next < 0) return false;
    if (!cfg_bool("TOTK_ACH_POPUP")) return false;   // turned off: the unlock is recorded, the notice dropped
    g_note = next, g_note_count = count, g_note_at = now;
    AchView v;
    if (cfg_bool("TOTK_ACH_SOUND") && ach_get(next, &v)) {   // at the game volume the player set
        double vol = cfg_double("TOTK_VOLUME");
        audio_chime(v.rarity == ACH_PLATINUM && count == 1, (float)((vol < 0 ? 0 : vol > 100 ? 100 : vol) / 100));
    }
    return true;
}

static void note_draw(const MenuTarget *t, float scale) {
    AchView v;
    if (g_note < 0 || !ach_get(g_note, &v)) return;
    bool plat = v.rarity == ACH_PLATINUM && g_note_count == 1;
    double age = note_clock() - g_note_at, len = plat ? kNotePlatSecs : kNoteSecs;
    float in = ease_out((float)(age / 0.35)), out = (float)fmin(1.0, fmax(0.0, (len - age) / 0.4)), a = fminf(in, out);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), small = fs * 0.78f, icon = 64 * scale, pad = 14 * scale;
    ImVec4 rc = ach_rarity_col(v.rarity);
    char title[96];
    if (g_note_count > 1) snprintf(title, sizeof title, menu_text(UI_MENU_ACH_NOTICE_MANY), g_note_count);
    else snprintf(title, sizeof title, "%s", menu_text(plat ? UI_MENU_ACH_NOTICE_PLATINUM : UI_MENU_ACH_NOTICE));
    const char *name = g_note_count > 1 ? menu_text(UI_MENU_ACH_NOTICE_MANY_SUB) : v.name;
    float tw = fmaxf(font->CalcTextSizeA(small, FLT_MAX, 0, title).x, font->CalcTextSizeA(fs, FLT_MAX, 0, name).x);
    float w = fminf((float)t->width * 0.9f, pad * 3 + icon + tw), h = pad * 2 + icon;
    ImVec2 p(((float)t->width - w) / 2, 28 * scale - (1 - in) * (h + 40 * scale)), q(p.x + w, p.y + h);
    if (plat) {   // a slow gold glow around the panel
        float g = (0.55f + 0.45f * sinf((float)age * 3.2f)) * a;
        for (int k = 3; k >= 1; k--)
            dl->AddRect(ImVec2(p.x - k * 4 * scale, p.y - k * 4 * scale), ImVec2(q.x + k * 4 * scale, q.y + k * 4 * scale),
                        ui_col(kUiGold, 0.10f * g * (4 - k)), 18 * scale, 0, 4 * scale);
    }
    dl->AddRectFilled(p, q, ui_col(kUiPanel, 0.94f * a), 16 * scale);
    dl->AddRect(p, q, ui_col(plat ? kUiGold : rc, 0.85f * a), 16 * scale, 0, 1.6f * scale);
    // the icon: the achievement's, or for a batch its rarest one
    ach_icon(dl, g_note, v, ImVec2(p.x + pad, p.y + pad), icon, true, a);
    float tx = p.x + pad * 2 + icon;
    dl->PushClipRect(ImVec2(tx, p.y), ImVec2(q.x - pad, q.y), true);
    dl->AddText(font, small, ImVec2(tx, p.y + pad + icon * 0.12f), ui_col(plat ? kUiGoldHi : rc, a), title);
    dl->AddText(font, fs, ImVec2(tx, p.y + pad + icon * 0.12f + small * 1.35f), ui_col(kUiCream, a), name);
    dl->PopClipRect();
}

extern "C" int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot) {
    *shot = NULL;
    cheats_frame(frame);   // the data cheats and the buttons of the cheats that need them held
    if (!g_test.parsed) test_parse();
    if (g_test.next_set < g_test.n_sets && frame >= g_test.sets[g_test.next_set].frame) test_apply_sets(frame);
    if (g_perf_save.exchange(0)) cfg_save();
    perf_tick(plat_now_ns(), t);
    static uint64_t toast_last;
    uint64_t toast_now = plat_now_ns();
    bool toast = toast_tick(toast_now, toast_last ? (float)((toast_now - toast_last) / 1e9) : 0);
    toast_last = toast_now;
    toast = note_tick() || toast;   // an achievement notice needs the overlay's frame as the shader notice does
    mp_tick();
    toast = mp_notes_tick() || toast;
    toast = radar_active() || toast;   // the radar and the voice indicators need the overlay
    amiibo_tick();
    bool open = g_open.load(std::memory_order_relaxed);
    if (!open) {
        if (g_save_pending) g_save_pending = opt_save_tick(1);   // closed: the last change goes to totk.ini now
        if (g_test.at && !g_test.opened && frame >= g_test.at) {
            g_test.opened = frame;
            g_test.page = 0;
            g_tab_request = 0;
            set_open(1);
            open = true;
        } else if (!g_perf_mode && !toast) {
            return 0;
        }
    }
    if (g_failed_flag()) {
        set_open(0);
        return 0;
    }
    uint64_t t0 = plat_now_ns();
    if (!g_ctx_ok) make_context();
    float scale = menu_scale(t->height);
    if (!vk_up(t) || (g_font_scale != scale && !fonts_up(scale))) {
        rt_log("menu: Dear ImGui Vulkan back end set-up failed; the menu stays closed");
        g_failed.store(1);
        set_open(0);
        return 0;
    }
    if (g_style_scale != scale) {
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(scale);
        g_style_scale = scale;
    }
    list_gpus(t->instance);
    ImGuiIO &io = ImGui::GetIO();
    int reset = g_reset.exchange(0);
    if (reset) {
        io.ClearInputKeys();
        io.ClearInputMouse();
        g_last_ns = 0;
        g_focus_page = g_tab_request >= 0 ? g_tab_request : g_tab;   // opening: the first row has the cursor
        if (GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);   // a confirmation left open on closing
        g_modal = false;
    }
    io.DisplaySize = ImVec2((float)t->width, (float)t->height);
    double dt = g_last_ns ? (t0 - g_last_ns) / 1e9 : 1 / 60.0;
    io.DeltaTime = (float)(dt < 1e-4 ? 1e-4 : dt > 0.1 ? 0.1 : dt);
    g_last_ns = t0;
    if (!open) {   // the overlay alone: no input reaches ImGui
    } else if (g_test.page < 0) {
        drain_events(io);
        pad_events(io, reset);
    } else {   // the test hook's pages: the user's input in a background test window must not change settings
        plat_mutex_lock(&ev_lock);
        ev_n = 0;
        plat_mutex_unlock(&ev_lock);
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    g_wants_text.store(open && io.WantTextInput, std::memory_order_relaxed);
    if (open && g_chat.load()) layout_chat(t->width, t->height, scale);
    else if (open) layout_menu(t->width, t->height, scale);
    radar_draw(t, scale);
    perf_draw(t, scale);
    toast_draw(t, scale);
    note_draw(t, scale);
    mp_notes_draw(t, scale);
    ImGui::Render();
    if (open) g_save_pending = opt_save_tick(0);
    g_cost_ns += plat_now_ns() - t0;
    if (g_test.page >= 0 && frame >= g_test.opened + g_test.per_page * (uint64_t)(g_test.page + 1)) {
        static char path[512];   // this frame's image goes to a file; then the next tab, or the menu closes
        const char *dir = cfg_get("TOTK_MENU_TEST_DIR");
        snprintf(path, sizeof path, "%s/menu-test-%llu-%d.png", dir ? dir : ".", (unsigned long long)frame, g_test.page);
        *shot = path;
        if (++g_test.page == TAB_COUNT) {
            g_test.page = -1;
            set_open(0);   // the next frame lays out nothing
        } else {
            g_tab_request = g_test.page;
        }
    }
    return ImGui::GetDrawData()->TotalVtxCount > 0;
}

extern "C" void menu_overlay_draw(VkCommandBuffer cb) {
    uint64_t t0 = plat_now_ns();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    g_cost_ns += plat_now_ns() - t0;
    if (++g_cost_frames == 600) {
        rt_log("menu: %.3f ms CPU per open frame (layout and recording, mean of 600)", g_cost_ns / 600 / 1e6);
        g_cost_ns = g_cost_frames = 0;
    }
}
