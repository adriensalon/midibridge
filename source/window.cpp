#include "window.hpp"
#include "router.hpp"

#include <imgui.h>

#include <chrono>
#include <thread>


void draw_main_window(window_state& state)
{
    ImGui::Begin("DX7Bridge");

    // HW out open
    static char hwbuf[128] = {};
    static bool init = true;
    if (init) {
        strncpy(hwbuf, state.hardware_port_name.c_str(), sizeof(hwbuf));
        init = false;
    }
    ImGui::InputText("HW Out match", hwbuf, sizeof(hwbuf));
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        state.hardware_port_name = hwbuf;
        open_hardware_output(1);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(is_hardware_output_open() ? "Opened" : "Closed");

    // Virtual port status
#ifdef _WIN32
    ImGui::Text("Virtual In: %s", is_virtual_input_open() ? "Running" : "Stopped");
#endif

    // ImGui::InputInt("Packet delay (ms)", &state.send_delay);

    // Load .syx (plug your file dialog; here a stub button)
    if (ImGui::Button("Load .syx")) {
        // TODO: replace with actual picker result:
        // auto loaded = sysex::loadSyxFile(path);
        // state.sysex_patches = std::move(loaded); state.selected = -1;
    }

    // ImGui::Separator();
    // if (ImGui::BeginListBox("Patches", ImVec2(-FLT_MIN, 300))) {
    //     for (int i = 0; i < (int)state.sysex_patches.size(); ++i) {
    //         bool sel = (state.selected == i);
    //         if (ImGui::Selectable(state.sysex_patches[i].name.c_str(), sel))
    //             state.selected = i;
    //     }
    //     ImGui::EndListBox();
    // }

    // ImGui::BeginDisabled(state.selected < 0 || state.is_sending || !is_hardware_output_open());
    // if (ImGui::Button("Send to DX7")) {
    //     state.is_sending = true;
    //     auto msg = state.sysex_patches[state.selected].data;
    //     int delay = state.send_delay;
    //     std::thread([msg, delay, &state] {
    //         send(msg);
    //         if (delay > 0)
    //             std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    //         state.is_sending = false;
    //     }).detach();
    // }
    // ImGui::EndDisabled();

    ImGui::End();
}
