#pragma once

#include <filesystem>
#include <string>
#include <vector>

/// @brief Represents a sysex patch
struct sysex_patch {
    std::string name;
    std::vector<unsigned char> data;
};

/// @brief Represents a sysex bank of patches
struct sysex_bank {
    std::string name;
    std::vector<unsigned char> data;
    std::vector<sysex_patch> patches;
};

/// @brief Loads recursively all sysex banks
[[nodiscard]] std::vector<sysex_bank> load_sysex_banks_recursive(const std::filesystem::path& root_path);
