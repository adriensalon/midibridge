#pragma once

#include <filesystem>
#include <string>
#include <vector>

/// @brief Represents a sysex patch
struct sysex_patch {
    std::string name;
    std::vector<unsigned char> data;
};

/// @brief Loads recursively all sysex banks but does not load patches
[[nodiscard]] std::vector<std::filesystem::path> load_sysex_banks_recursive(const std::filesystem::path& root_path);

/// @brief Loads recursively all patches from the bank
[[nodiscard]] std::vector<sysex_patch> load_sysex_patches(const std::filesystem::path& bank);
