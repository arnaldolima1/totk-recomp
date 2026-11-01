// Amiibo / NFC menu page (Dear ImGui): browser and scan controls.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "ui/amiibo_page.h"

extern "C" {
#include "amiibo.h"
#include "cfg.h"
#include "platform/platform.h"
#include "ui_text.h"
}

namespace fs = std::filesystem;

struct FileEntry {
    std::string name;
    bool is_dir;
};

static const char kDefaultAmiiboDir[] = "./amiibo";
static std::string g_current_dir = kDefaultAmiiboDir;
static char g_dir_input[512] = "./amiibo";
static std::vector<FileEntry> g_entries;
static int g_selected_entry = -1;
static std::string g_status_msg;   // empty: ready

// A catalog text with one %s filled in.
static std::string fmt(UiTextId id, const std::string &arg) {
    char buf[512];
    snprintf(buf, sizeof buf, menu_text(id), arg.c_str());
    return buf;
}

static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static void set_current_dir(const std::string &path) {
    g_current_dir = path;
    snprintf(g_dir_input, sizeof(g_dir_input), "%s", g_current_dir.c_str());
    cfg_set("TOTK_AMIIBO_DIR", g_current_dir.c_str());
}

static void refresh_dir(void) {
    g_entries.clear();
    g_selected_entry = -1;

    std::error_code ec;
    fs::path dir_path(g_current_dir);

    // The default folder is made when it is missing, so the page opens on an empty amiibo folder rather than on the
    // install folder's own files. Fall back to "." if a chosen directory does not exist or is not a directory.
    if (g_current_dir == kDefaultAmiiboDir && !fs::exists(dir_path, ec)) fs::create_directories(dir_path, ec);
    if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
        if (g_current_dir != ".") {
            set_current_dir(".");
            dir_path = fs::path(g_current_dir);
        }
    }

    if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
        g_status_msg = fmt(UI_MENU_AMIIBO_CANNOT_OPEN, g_current_dir);
        return;
    }

    // Add parent navigation if not at root
    if (dir_path.has_parent_path() && dir_path != dir_path.parent_path()) {
        g_entries.push_back({"..", true});
    }

    for (const auto &entry : fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        std::error_code status_ec;
        bool is_dir = entry.is_directory(status_ec);
        if (status_ec) continue;

        std::string filename = entry.path().filename().string();
        if (filename.empty() || filename == "." || filename == "..") continue;

        if (!is_dir) {
            std::string ext = entry.path().extension().string();
            if (!iequals(ext, ".bin") && !iequals(ext, ".nfc")) {
                continue;
            }
        }

        g_entries.push_back({std::move(filename), is_dir});
    }

    std::sort(g_entries.begin(), g_entries.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.name == "..") return true;
        if (b.name == "..") return false;
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
    });
}

void tab_amiibo(float scale) {
    static bool s_inited = false;
    if (!s_inited) {
        const char *cfg_dir = cfg_get("TOTK_AMIIBO_DIR");
        if (cfg_dir && *cfg_dir) {
            g_current_dir = cfg_dir;
        } else {
            g_current_dir = kDefaultAmiiboDir;
        }
        snprintf(g_dir_input, sizeof(g_dir_input), "%s", g_current_dir.c_str());
        refresh_dir();
        s_inited = true;
    }

    // Two panes: Left (Directory Browser & Tag List), Right (Amiibo Tag Details & Controls)
    const float content_h = ImGui::GetContentRegionAvail().y - 20.0f;
    const float left_w = ImGui::GetContentRegionAvail().x * 0.58f;
    const float right_w = ImGui::GetContentRegionAvail().x - left_w - ImGui::GetStyle().ItemSpacing.x;

    // ---- Left Pane: Explorer & List ----
    ImGui::BeginChild("##amiibo_explorer", ImVec2(left_w, content_h), true);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "%s", menu_text(UI_MENU_AMIIBO_DIR));

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputText("##amiibo_dir_input", g_dir_input, sizeof(g_dir_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
        set_current_dir(g_dir_input);
        refresh_dir();
    }

    ImGui::Spacing();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_SET_DIR))) {
        set_current_dir(g_dir_input);
        refresh_dir();
    }
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_DEFAULT_DIR))) {
        set_current_dir(kDefaultAmiiboDir);
        refresh_dir();
    }
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_REFRESH))) {
        refresh_dir();
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_PICK));

    ImGui::BeginChild("##file_list", ImVec2(0, 0), true);
    for (size_t i = 0; i < g_entries.size(); i++) {
        const auto &e = g_entries[i];
        std::string label = (e.is_dir ? std::string(menu_text(UI_MENU_AMIIBO_FOLDER)) + " " : "  ") + e.name;
        bool selected = ((int)i == g_selected_entry);

        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            g_selected_entry = (int)i;
            if (e.is_dir) {
                if (ImGui::IsMouseDoubleClicked(0) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
                    if (e.name == "..") {
                        fs::path cur(g_current_dir);
                        if (cur.has_parent_path() && cur != cur.parent_path()) {
                            set_current_dir(cur.parent_path().string());
                        } else {
                            set_current_dir(".");
                        }
                    } else {
                        set_current_dir((fs::path(g_current_dir) / e.name).string());
                    }
                    refresh_dir();
                    break;
                }
            } else {
                std::string full_path = (fs::path(g_current_dir) / e.name).string();
                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (amiibo_scan_and_tap(full_path.c_str())) {
                        g_status_msg = fmt(UI_MENU_AMIIBO_TAPPED, e.name);
                    } else {
                        g_status_msg = fmt(UI_MENU_AMIIBO_LOAD_ERROR, e.name);
                    }
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Right Pane: Controls & Status ----
    ImGui::BeginChild("##amiibo_controls", ImVec2(right_w, content_h), true);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "%s", menu_text(UI_MENU_AMIIBO_ACTIONS));
    ImGui::Separator();

    AmiiboDeviceState state = amiibo_get_device_state();
    const char *state_str = menu_text(UI_MENU_AMIIBO_IDLE);
    ImVec4 state_col = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    switch (state) {
    case AMIIBO_DEV_STATE_INITIALIZED: state_str = menu_text(UI_MENU_AMIIBO_IDLE); break;
    case AMIIBO_DEV_STATE_SEARCHING: state_str = menu_text(UI_MENU_AMIIBO_SEARCHING); state_col = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); break;
    case AMIIBO_DEV_STATE_TAG_FOUND: state_str = menu_text(UI_MENU_AMIIBO_FOUND); state_col = ImVec4(0.3f, 0.9f, 0.3f, 1.0f); break;
    case AMIIBO_DEV_STATE_TAG_MOUNTED: state_str = menu_text(UI_MENU_AMIIBO_MOUNTED); state_col = ImVec4(0.2f, 0.9f, 1.0f, 1.0f); break;
    case AMIIBO_DEV_STATE_UNAVAILABLE: state_str = menu_text(UI_MENU_AMIIBO_DISABLED); state_col = ImVec4(0.9f, 0.3f, 0.3f, 1.0f); break;
    default: break;
    }
    ImGui::Text("%s ", menu_text(UI_MENU_AMIIBO_READER));
    ImGui::SameLine();
    ImGui::TextColored(state_col, "%s", state_str);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("%s", menu_text(UI_MENU_AMIIBO_SELECTED));
    if (g_selected_entry >= 0 && g_selected_entry < (int)g_entries.size() && !g_entries[g_selected_entry].is_dir) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", g_entries[g_selected_entry].name.c_str());

        if (ImGui::Button(menu_text(UI_MENU_AMIIBO_SCAN), ImVec2(ImGui::GetContentRegionAvail().x, 32 * scale))) {
            std::string full_path = (fs::path(g_current_dir) / g_entries[g_selected_entry].name).string();
            if (amiibo_scan_and_tap(full_path.c_str())) {
                g_status_msg = fmt(UI_MENU_AMIIBO_TAPPED, g_entries[g_selected_entry].name);
            } else {
                g_status_msg = fmt(UI_MENU_AMIIBO_LOAD_ERROR, g_entries[g_selected_entry].name);
            }
        }
    } else {
        ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_NONE_SELECTED));
        ImGui::BeginDisabled();
        ImGui::Button(menu_text(UI_MENU_AMIIBO_SCAN), ImVec2(ImGui::GetContentRegionAvail().x, 32 * scale));
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_REMOVE), ImVec2(ImGui::GetContentRegionAvail().x, 28 * scale))) {
        amiibo_remove_tag();
        g_status_msg = menu_text(UI_MENU_AMIIBO_REMOVED);
    }

    ImGui::Spacing();
    ImGui::Separator();
    bool auto_remove = amiibo_get_auto_remove();
    if (ImGui::Checkbox(menu_text(UI_MENU_AMIIBO_AUTO_REMOVE), &auto_remove)) {
        amiibo_set_auto_remove(auto_remove);
    }
    if (auto_remove) {
        float delay = amiibo_get_auto_remove_delay_s();
        if (ImGui::SliderFloat(menu_text(UI_MENU_AMIIBO_HOLD), &delay, 1.0f, 6.0f, "%.1f s")) {
            amiibo_set_auto_remove_delay_s(delay);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("%s", menu_text(UI_MENU_AMIIBO_INFO));
    if (amiibo_has_tag()) {
        const AmiiboInfo *info = amiibo_get_current_info();
        ImGui::Text("File: %s", info->filename);
        ImGui::Text("Char ID: %02x%02x%02x | Series: %02x",
                    info->character_id[0], info->character_id[1], info->character_id[2], info->series_id);
        ImGui::Text("Amiibo Number: %04x | Type: %02x", info->numbering_id, info->nfp_type);
        ImGui::Text("UID: %02x:%02x:%02x:%02x:%02x:%02x:%02x",
                    info->uuid[0], info->uuid[1], info->uuid[2], info->uuid[3],
                    info->uuid[4], info->uuid[5], info->uuid[6]);
    } else {
        ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_NO_TAG));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled(menu_text(UI_MENU_AMIIBO_STATUS),
                        g_status_msg.empty() ? menu_text(UI_MENU_AMIIBO_READY) : g_status_msg.c_str());

    ImGui::EndChild();
}
