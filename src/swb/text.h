#pragma once

#include "swb/win32_headers.h"

#include <string>
#include <string_view>

namespace swb {

inline std::string_view trim_ascii_whitespace(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

inline std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int wide_length = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (wide_length <= 0) {
        return {};
    }

    std::wstring wide_text(static_cast<std::size_t>(wide_length), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        wide_text.data(),
        wide_length);
    return wide_text;
}

inline std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int utf8_length = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (utf8_length <= 0) {
        return {};
    }

    std::string utf8_text(static_cast<std::size_t>(utf8_length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        utf8_text.data(),
        utf8_length,
        nullptr,
        nullptr);
    return utf8_text;
}

}