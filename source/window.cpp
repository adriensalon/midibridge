#include "window.hpp"
#include "dialog.hpp"
#include "router.hpp"
#include "sysex.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <chrono>
#include <thread>
#include <iostream>

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
static std::string setup_virtual_port_name = "MIDI Bridge";
static std::string setup_library_directory = "Path to the directory...";

void draw_setup_text(const float modal_width)
{
    const float _wrap_width = modal_width - ImGui::GetStyle().WindowPadding.x * 2.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + _wrap_width);
    ImGui::TextUnformatted(
        "User projects will be stored as .dxcc in the collection directory. "
        "This can be modified later from [Settings > Collection] or from the "
        "settings.json next to the dawxchange executable.");
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
        if (ImGui::Begin(IMGUID("Library"))) {

            static int selectedBank, selectedPatch = -1;
            static std::vector<sysex_bank> _banks = load_sysex_banks_recursive(setup_library_directory);

            ImGuiTableFlags tblFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV;

            const float tableH = ImGui::GetContentRegionAvail().y; // fill remaining space
            if (ImGui::BeginTable("SysexBanksTable", 2, tblFlags, ImVec2(-FLT_MIN, tableH))) {
                ImGui::TableSetupColumn("Bank / Patches", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("# / Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < (int)_banks.size(); ++i) {
                    const sysex_bank& bank = _banks[i];

                    // --- bank row ---
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    // Use a TreeNode in column 0 for the expand arrow
                    ImGuiTreeNodeFlags tflags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding;
                    if (selectedBank == i && selectedPatch == -1)
                        tflags |= ImGuiTreeNodeFlags_Selected;

                    // Use a stable ID separate from the label (pointer-as-ID pattern)
                    bool open = ImGui::TreeNodeEx((void*)(intptr_t)(i + 1), tflags, "%s", bank.name.c_str());

                    // Column 1: patch count
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", (int)bank.patches.size());

                    // Row click selection behavior for the bank itself
                    // (clicking the label toggles the node, clicking empty row selects)
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                        selectedBank = i;
                        selectedPatch = -1;
                        
                    }

                    // --- patch rows when expanded ---
                    if (open) {
                        for (int j = 0; j < (int)bank.patches.size(); ++j) {
                            const auto& patch = bank.patches[j];

                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);

                            // Leaf-style row (aligned with tree indent), selectable
                            ImGuiTreeNodeFlags leaf = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth;
                            if (selectedBank == i && selectedPatch == j)
                                leaf |= ImGuiTreeNodeFlags_Selected;

                            // unique ID for the leaf (combine bank+patch indices)
                            const intptr_t leafId = ((intptr_t)(i + 1) << 16) | (intptr_t)j;
                            ImGui::TreeNodeEx((void*)leafId, leaf, "%s", patch.name.c_str());
                            if (ImGui::IsItemClicked()) {
                                selectedBank = i;
                                selectedPatch = j;
                                send_to_hardware_output(_banks[selectedBank].patches[selectedPatch].data);
                            }

                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("%zu B", patch.data.size());
                        }

                        ImGui::TreePop(); // close bank node
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
    draw_setup_modal();
    draw_library_window();
    draw_edit_window();
}
