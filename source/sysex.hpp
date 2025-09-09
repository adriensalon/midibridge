#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct sysex_patch {
    std::string name;
    std::vector<unsigned char> data; // single F0..F7 packet
};

[[nodiscard]] std::vector<unsigned char> read_all(const std::string& path);

[[nodiscard]] std::vector<std::vector<unsigned char>> split(const std::vector<unsigned char>& buf); // F0..F7 packets

[[nodiscard]] std::string guess_dx7_name(const std::vector<unsigned char>& msg, int idx); // 10-char ASCII heuristic

[[nodiscard]] std::vector<sysex_patch> load_sysex_file(const std::filesystem::path& sysex_path);
