#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace swb {

[[nodiscard]] inline std::optional<std::string> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] inline std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    return read_binary_file(path);
}

[[nodiscard]] inline bool write_text_file(const std::filesystem::path& path, std::string_view content) {
    std::error_code error_code;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error_code);
        if (error_code) {
            return false;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(output);
}

}