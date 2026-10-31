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
static std::string g_status_msg = "Ready";

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

    // Fall back to "." if directory does not exist or is not a directory
    if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
        if (g_current_dir != ".") {
            set_current_dir(".");
            dir_path = fs::path(g_current_dir);
        }
    }

    if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
        g_status_msg = "Cannot open directory: " + g_current_dir;
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
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "Amiibo NFC Directory:");

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputText("##amiibo_dir_input", g_dir_input, sizeof(g_dir_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
        set_current_dir(g_dir_input);
        refresh_dir();
    }

    ImGui::Spacing();
    if (ImGui::Button("Set Directory")) {
        set_current_dir(g_dir_input);
        refresh_dir();
    }
    ImGui::SameLine();
    if (ImGui::Button("Default (./amiibo)")) {
        set_current_dir(kDefaultAmiiboDir);
        refresh_dir();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        refresh_dir();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Select an Amiibo file (.bin / .nfc):");

    ImGui::BeginChild("##file_list", ImVec2(0, 0), true);
    for (size_t i = 0; i < g_entries.size(); i++) {
        const auto &e = g_entries[i];
        std::string label = (e.is_dir ? "[DIR] " : "  ") + e.name;
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
                        g_status_msg = "Scanned & Tapped: " + e.name;
                    } else {
                        g_status_msg = "Error loading " + e.name;
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
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "NFC Reader & Tag Actions");
    ImGui::Separator();

    AmiiboDeviceState state = amiibo_get_device_state();
    const char *state_str = "Initialized";
    ImVec4 state_col = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    switch (state) {
    case AMIIBO_DEV_STATE_INITIALIZED: state_str = "Idle / Ready"; break;
    case AMIIBO_DEV_STATE_SEARCHING: state_str = "Game Searching for Tag..."; state_col = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); break;
    case AMIIBO_DEV_STATE_TAG_FOUND: state_str = "Tag Present / Detected"; state_col = ImVec4(0.3f, 0.9f, 0.3f, 1.0f); break;
    case AMIIBO_DEV_STATE_TAG_MOUNTED: state_str = "Tag Mounted (Reading Area)"; state_col = ImVec4(0.2f, 0.9f, 1.0f, 1.0f); break;
    case AMIIBO_DEV_STATE_UNAVAILABLE: state_str = "NFC Reader Disabled"; state_col = ImVec4(0.9f, 0.3f, 0.3f, 1.0f); break;
    default: break;
    }
    ImGui::Text("Reader Status: ");
    ImGui::SameLine();
    ImGui::TextColored(state_col, "%s", state_str);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Selected File:");
    if (g_selected_entry >= 0 && g_selected_entry < (int)g_entries.size() && !g_entries[g_selected_entry].is_dir) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", g_entries[g_selected_entry].name.c_str());

        if (ImGui::Button("Scan / Tap Amiibo", ImVec2(ImGui::GetContentRegionAvail().x, 32 * scale))) {
            std::string full_path = (fs::path(g_current_dir) / g_entries[g_selected_entry].name).string();
            if (amiibo_scan_and_tap(full_path.c_str())) {
                g_status_msg = "Tapped " + g_entries[g_selected_entry].name;
            } else {
                g_status_msg = "Failed to load dump!";
            }
        }
    } else {
        ImGui::TextDisabled("(No file selected)");
        ImGui::BeginDisabled();
        ImGui::Button("Scan / Tap Amiibo", ImVec2(ImGui::GetContentRegionAvail().x, 32 * scale));
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    if (ImGui::Button("Remove Tag (Pull away)", ImVec2(ImGui::GetContentRegionAvail().x, 28 * scale))) {
        amiibo_remove_tag();
        g_status_msg = "Tag removed";
    }

    ImGui::Spacing();
    ImGui::Separator();
    bool auto_remove = amiibo_get_auto_remove();
    if (ImGui::Checkbox("Auto-remove tag after scan (CEMU style)", &auto_remove)) {
        amiibo_set_auto_remove(auto_remove);
    }
    if (auto_remove) {
        float delay = amiibo_get_auto_remove_delay_s();
        if (ImGui::SliderFloat("Hold duration", &delay, 1.0f, 6.0f, "%.1f s")) {
            amiibo_set_auto_remove_delay_s(delay);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Loaded Tag Information:");
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
        ImGui::TextDisabled("No active tag on reader.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Status: %s", g_status_msg.c_str());

    ImGui::EndChild();
}
