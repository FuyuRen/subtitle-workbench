#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swb::test {

[[nodiscard]] inline std::filesystem::path make_temp_directory(std::string_view stem) {
    const auto unique = std::to_string(std::filesystem::file_time_type::clock::now().time_since_epoch().count());
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / std::filesystem::u8path(std::string{stem} + "-" + unique);
    std::filesystem::create_directories(directory);
    return directory;
}

[[nodiscard]] inline std::filesystem::path make_temp_directory(std::string_view group, std::string_view stem) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "subtitle-workbench-tests" / std::filesystem::u8path(std::string{group}) / std::filesystem::u8path(std::string{stem});
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

inline void write_text_file(const std::filesystem::path& path, std::string_view content = "test") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

inline void create_file(const std::filesystem::path& path, std::string_view content = "test") {
    write_text_file(path, content);
}

[[nodiscard]] inline std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] inline bool contains_text(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] inline bool contains_argument(std::span<const std::string> arguments, std::string_view value) {
    for (const std::string& argument : arguments) {
        if (argument == value) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool has_header_prefix(const std::vector<std::string>& headers, std::string_view prefix) {
    for (const std::string& header : headers) {
        if (std::string_view{header}.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

}