#pragma once

#include <cstdint>
#include <string>

namespace swb {

[[nodiscard]] inline std::uint16_t read_little_endian_16(const char* bytes) {
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes[0]) |
        (static_cast<unsigned char>(bytes[1]) << 8u));
}

[[nodiscard]] inline std::uint32_t read_little_endian_32(const char* bytes) {
    return static_cast<std::uint32_t>(
        static_cast<unsigned char>(bytes[0]) |
        (static_cast<unsigned char>(bytes[1]) << 8u) |
        (static_cast<unsigned char>(bytes[2]) << 16u) |
        (static_cast<unsigned char>(bytes[3]) << 24u));
}

inline void append_little_endian_16(std::string& output, std::uint16_t value) {
    output.push_back(static_cast<char>(value & 0xFFu));
    output.push_back(static_cast<char>((value >> 8u) & 0xFFu));
}

inline void append_little_endian_32(std::string& output, std::uint32_t value) {
    output.push_back(static_cast<char>(value & 0xFFu));
    output.push_back(static_cast<char>((value >> 8u) & 0xFFu));
    output.push_back(static_cast<char>((value >> 16u) & 0xFFu));
    output.push_back(static_cast<char>((value >> 24u) & 0xFFu));
}

}