#pragma once

#include <functional>
#include <string>
#include <vector>

/// @brief Gets a list of the available hardware port names
[[nodiscard]] std::vector<std::string> get_hardware_ports();

/// @brief Opens the selected hardware port
void open_hardware_output(const std::size_t& index);

/// @brief Closes the hardware port if open
void close_hardware_output();

/// @brief Gets if the hardware port is open
[[nodiscard]] bool is_hardware_output_open();

/// @brief Sends bytes to the hardware port
void send_to_hardware_output(const std::vector<unsigned char>& message);

/// @brief Opens the virtual port with the selected name and executes a callback when bytes are received
void open_virtual_input(const std::string& port, const std::function<void(const std::vector<unsigned char>&)>& callback);

/// @brief Closes the virtual port if open
void close_virtual_input();

/// @brief Gets if the virtual port is open
[[nodiscard]] bool is_virtual_input_open();
