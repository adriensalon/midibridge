#include "window.hpp"
#include "dialog.hpp"
#include "router.hpp"
#include "sysex.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <chrono>
#include <iostream>
#include <thread>

// clang-format off
#define IMGUID_CONCAT(lhs, rhs) lhs # rhs
#define IMGUID_CONCAT_WRAPPER(lhs, rhs) IMGUID_CONCAT(lhs, rhs)
#define IMGUID_UNIQUE IMGUID_CONCAT_WRAPPER(__FILE__, __LINE__)
#define IMGUID(NAME) NAME "###" IMGUID_UNIQUE
#define IMGUIDU "###" IMGUID_UNIQUE
// clang-format on

namespace {

static bool is_setup_finished = false;
static bool is_setup_modal_shown = false;
static const char* setup_modal_id = IMGUID("Setup");
static std::size_t setup_selected_hardware_port = 0;
static std::vector<std::string> setup_detected_hardware_ports;
static std::string setup_virtual_port_name = "DX7 MIDI Bridge";
static std::string setup_library_directory = "Path to the directory...";
static std::vector<std::filesystem::path> library_banks;
static std::vector<sysex_patch> library_patches;
static int library_selected_bank_index = -1;
static int library_selected_patch_index = -1;
static int library_patches_cached_bank = -1;

void draw_setup_text(const float modal_width)
{
    const float _wrap_width = modal_width - ImGui::GetStyle().WindowPadding.x * 2.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + _wrap_width);
    ImGui::TextUnformatted(
        "Define hardware and virtual ports to use for bridge. Every .syx file will be loaded "
        "recursively from the selected library path.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Spacing();
}

void draw_setup_hardware_port_control()
{
    if (setup_detected_hardware_ports.empty()) {
        setup_detected_hardware_ports = get_hardware_ports();
    }
    ImGui::Text("Hardware port");
    const float _full_width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(_full_width);
    const std::string _combo_preview = setup_detected_hardware_ports.empty() ? "No hardware port detected" : setup_detected_hardware_ports[setup_selected_hardware_port];
    if (ImGui::BeginCombo(IMGUIDU, _combo_preview.c_str())) {
        std::size_t _index = 0;
        for (const std::string& _hardware_port : setup_detected_hardware_ports) {
            if (ImGui::Selectable(setup_detected_hardware_ports[_index].c_str())) {
                setup_selected_hardware_port = _index;
            }
            _index++;
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
}

void draw_setup_virtual_port_control()
{
    ImGui::Text("Virtual port");
    const float _full_width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(_full_width);
    ImGui::InputText(IMGUIDU, &setup_virtual_port_name);
    ImGui::Spacing();
}

void draw_setup_library_path_control()
{
    ImGui::Text("Library path");
    const float _full_width = ImGui::GetContentRegionAvail().x;
    const float _button_width = 95.0f;
    const float _spacing_width = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(_full_width - _button_width - _spacing_width);
    ImGui::InputText(IMGUIDU, &setup_library_directory);
    ImGui::SameLine();
    if (ImGui::Button("Select...", ImVec2(_button_width, 0))) {
        if (const std::optional<std::filesystem::path> _path = pick_directory_dialog(std::filesystem::current_path()))
            setup_library_directory = _path->string();
    }
    ImGui::Spacing();
}

void draw_setup_start_control()
{
    if (!std::filesystem::is_directory(setup_library_directory)) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(IMGUID("Start"), ImVec2(-FLT_MIN, 0.f))) {
        open_hardware_output(setup_selected_hardware_port);
        open_virtual_input(setup_virtual_port_name, [](const std::vector<unsigned char>& data) {
            send_to_hardware_output(data);
        });
        library_banks = load_sysex_banks_recursive(setup_library_directory);
        is_setup_finished = true;
        ImGui::CloseCurrentPopup();
    }
    if (!std::filesystem::is_directory(setup_library_directory)) {
        ImGui::EndDisabled();
    }
}

void draw_setup_modal()
{
    if (!is_setup_modal_shown) {
        ImGui::OpenPopup(setup_modal_id);
        is_setup_modal_shown = true;
    }
    const float _modal_width = 400.f;
    const ImVec2 _viewport_center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(_viewport_center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(_modal_width, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(_modal_width, 0.f), ImVec2(_modal_width, FLT_MAX));
    if (ImGui::BeginPopupModal(setup_modal_id, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        draw_setup_text(_modal_width);
        draw_setup_hardware_port_control();
        draw_setup_virtual_port_control();
        draw_setup_library_path_control();
        draw_setup_start_control();
        ImGui::EndPopup();
    }
}

void draw_library_window()
{
    if (is_setup_finished) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
        const ImGuiWindowFlags _window_flags = ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoBringToFrontOnFocus;

        if (ImGui::Begin(IMGUID("Library"), 0, _window_flags)) {

            const ImGuiTableFlags _table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
            const float _table_height = ImGui::GetContentRegionAvail().y;
            if (ImGui::BeginTable(IMGUIDU, 1, _table_flags, ImVec2(-FLT_MIN, _table_height))) {
                ImGui::TableSetupColumn(IMGUIDU, ImGuiTableColumnFlags_WidthStretch);

                for (int _bank_index = 0; _bank_index < static_cast<int>(library_banks.size()); ++_bank_index) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    const bool _is_bank_selected = (library_selected_bank_index == _bank_index);
                    ImGuiTreeNodeFlags _tree_node_flags = ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
                    if (_is_bank_selected && library_selected_patch_index == -1) {
                        _tree_node_flags |= ImGuiTreeNodeFlags_Selected;
                    }

                    ImGui::SetNextItemOpen(_is_bank_selected, ImGuiCond_Always);
                    const std::string _bank_name = std::filesystem::relative(library_banks[_bank_index], setup_library_directory).string();
                    const bool _is_bank_open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(_bank_index + 1)), _tree_node_flags, "%s", _bank_name.c_str());

                    if (ImGui::IsItemToggledOpen()) {
                        if (_is_bank_open) {
                            library_selected_bank_index = _bank_index;
                            library_selected_patch_index = -1;
                        } else if (_is_bank_selected) {
                            library_selected_bank_index = -1;
                            library_selected_patch_index = -1;
                        }
                    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        if (!_is_bank_selected) {
                            library_selected_bank_index = _bank_index;
                            library_selected_patch_index = -1;
                        } else {
                            library_selected_bank_index = -1;
                            library_selected_patch_index = -1;
                        }
                    }

                    if (library_selected_bank_index != library_patches_cached_bank) {
                        if (library_selected_bank_index >= 0) {
                            library_patches = load_sysex_patches(library_banks[library_selected_bank_index]);
                        } else {
                            library_patches.clear();
                        }
                        library_patches_cached_bank = library_selected_bank_index;
                    }

                    if (_is_bank_open) {
                        for (int _patch_index = 0; _patch_index < static_cast<int>(library_patches.size()); ++_patch_index) {
                            const sysex_patch& _patch = library_patches[_patch_index];

                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGuiTreeNodeFlags _leaf_flags = ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth;
                            if (library_selected_bank_index == _bank_index && library_selected_patch_index == _patch_index) {
                                _leaf_flags |= ImGuiTreeNodeFlags_Selected;
                            }

                            const intptr_t leafId = (static_cast<intptr_t>(_bank_index + 1) << 16) | static_cast<intptr_t>(_patch_index);
                            ImGui::TreeNodeEx(reinterpret_cast<void*>(leafId), _leaf_flags, "%s", _patch.name.c_str());
                            if (ImGui::IsItemClicked()) {
                                library_selected_bank_index = _bank_index;
                                library_selected_patch_index = _patch_index;
                                send_to_hardware_output(library_patches[library_selected_patch_index].data);
                            }
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::End();
        }
    }
}

void draw_edit_window()
{
    if (is_setup_finished) {
        if (ImGui::Begin(IMGUID("Edit"))) {
            ImGui::End();
        }
    }
}
}

void draw_main_window()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
    draw_setup_modal();
    draw_library_window();
    // draw_edit_window();
    ImGui::PopStyleVar(3);
}
