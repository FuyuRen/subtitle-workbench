#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace swb {

[[nodiscard]] std::filesystem::path executable_directory();

[[nodiscard]] std::filesystem::path output_root();

[[nodiscard]] std::filesystem::path resolve_output_root(std::string_view custom);

[[nodiscard]] std::string format_bytes(std::int64_t bytes);

[[nodiscard]] bool is_url(std::string_view source);

[[nodiscard]] std::string sanitize_path_component(std::string_view input);

[[nodiscard]] std::string short_hash(std::string_view input);

[[nodiscard]] std::string make_workdir_name(std::string_view title, std::string_view source);

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

[[nodiscard]] std::filesystem::path resolve_tool(std::wstring_view filename);

}
