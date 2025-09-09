#pragma once

#include "sysex.hpp"

#include <chrono>
#include <string>
#include <vector>

/// @brief Represents the state shared with the gui
struct window_state {
    std::vector<std::string> hardware_port_names;
    std::size_t hardware_port_selected = 0;
    std::string hardware_port_name = "USB Uno MIDI Interface 1"; // REMOVE
    std::string virtual_port_name = "MIDI Bridge";
    std::vector<sysex_patch> sysex_patches;
    std::size_t sysex_patch_selected = 0;
    std::chrono::milliseconds send_delay { 50 };
    bool is_sending = false;
};

/// @brief Draws the window gui
void draw_main_window(window_state& state);
