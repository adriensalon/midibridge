#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <optional>

struct dialog_file_filter {
    std::string text;
    std::vector<std::string> extensions;
};

[[nodiscard]] std::optional<std::filesystem::path> open_file_dialog(
    const std::vector<dialog_file_filter> filters,
    const std::filesystem::path& default_path);

[[nodiscard]] std::optional<std::filesystem::path> save_file_dialog(
    const std::vector<dialog_file_filter> filters,
    const std::filesystem::path& default_path);

[[nodiscard]] std::optional<std::filesystem::path> pick_directory_dialog(
    const std::filesystem::path& default_path);
    