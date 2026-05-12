#include "swb/workspace.h"

#include "swb/win32_headers.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace swb {

namespace {

constexpr std::array<char, 16> hex_digits{
    '0','1','2','3','4','5','6','7',
    '8','9','a','b','c','d','e','f',
};

}

std::filesystem::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return std::filesystem::current_path();
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path output_root() {
    return executable_directory() / "output";
}

std::filesystem::path resolve_output_root(std::string_view custom) {
    if (custom.empty()) {
        return output_root();
    }
    std::filesystem::path resolved_path{std::filesystem::u8path(std::string{custom})};
    if (resolved_path.is_relative()) {
        resolved_path = executable_directory() / resolved_path;
    }
    return resolved_path;
}

std::string format_bytes(std::int64_t bytes) {
    if (bytes < 0) {
        return "?";
    }
    constexpr std::int64_t kib = 1024;
    constexpr std::int64_t mib = kib * 1024;
    constexpr std::int64_t gib = mib * 1024;

    const auto format_with_unit = [](double value, std::string_view unit) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(1) << value << ' ' << unit;
        return stream.str();
    };

    if (bytes >= gib) {
        return format_with_unit(static_cast<double>(bytes) / static_cast<double>(gib), "GB");
    }
    if (bytes >= mib) {
        return format_with_unit(static_cast<double>(bytes) / static_cast<double>(mib), "MB");
    }
    if (bytes >= kib) {
        return format_with_unit(static_cast<double>(bytes) / static_cast<double>(kib), "KB");
    }
    return std::to_string(bytes) + " B";
}

bool is_url(std::string_view source) {
    return source.starts_with("http://") || source.starts_with("https://");
}

std::string sanitize_path_component(std::string_view input) {
    std::string sanitized_component;
    sanitized_component.reserve(input.size());
    for (const char character : input) {
        const unsigned char code_point = static_cast<unsigned char>(character);
        if (code_point < 0x20) {
            continue;
        }
        switch (character) {
        case '<': case '>': case ':': case '"':
        case '/': case '\\': case '|': case '?': case '*':
            sanitized_component.push_back('_');
            break;
        default:
            sanitized_component.push_back(character);
        }
    }
    while (!sanitized_component.empty() && (sanitized_component.back() == ' ' || sanitized_component.back() == '.')) {
        sanitized_component.pop_back();
    }
    if (sanitized_component.empty()) {
        sanitized_component = "video";
    }
    if (sanitized_component.size() > 80) {
        sanitized_component.resize(80);
    }
    return sanitized_component;
}

std::string short_hash(std::string_view input) {
    std::uint32_t hash = 0x811C9DC5u;
    for (const char character : input) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 0x01000193u;
    }
    std::string hash_text(8, '0');
    for (int digit_index = 7; digit_index >= 0; --digit_index) {
        hash_text[static_cast<std::size_t>(digit_index)] = hex_digits[hash & 0xFu];
        hash >>= 4;
    }
    return hash_text;
}

std::string make_workdir_name(std::string_view title, std::string_view source) {
    std::string name = sanitize_path_component(title);
    name.push_back('_');
    name.append(short_hash(source));
    return name;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    std::string utf8_text;
    utf8_text.reserve(utf8.size());
    for (const char8_t code_unit : utf8) {
        utf8_text.push_back(static_cast<char>(code_unit));
    }
    return utf8_text;
}

std::filesystem::path resolve_tool(std::wstring_view filename) {
    const std::filesystem::path exe_dir = executable_directory();
    const std::array candidate_paths{
        exe_dir / L"vendor" / L"bin" / filename,
        exe_dir / filename,
        exe_dir.parent_path().parent_path() / L"vendor" / L"bin" / filename,
        std::filesystem::current_path() / L"vendor" / L"bin" / filename,
    };
    std::error_code error_code;
    for (const std::filesystem::path& candidate_path : candidate_paths) {
        if (std::filesystem::exists(candidate_path, error_code)) {
            return candidate_path;
        }
    }

    const std::wstring tool_name{filename};
    const DWORD required = SearchPathW(nullptr, tool_name.c_str(), nullptr, 0, nullptr, nullptr);
    if (required != 0) {
        std::wstring resolved(static_cast<std::size_t>(required), L'\0');
        const DWORD written = SearchPathW(
            nullptr,
            tool_name.c_str(),
            nullptr,
            static_cast<DWORD>(resolved.size()),
            resolved.data(),
            nullptr);
        if (written != 0) {
            resolved.resize(written);
            return std::filesystem::path{resolved};
        }
    }

    return {};
}

}
