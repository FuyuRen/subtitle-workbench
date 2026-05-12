#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace swb::json {

class Cursor {
public:
    explicit Cursor(std::string_view text) : text_(text) {}

    void skip_whitespace() {
        while (position_ < text_.size()) {
            const unsigned char character = static_cast<unsigned char>(text_[position_]);
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool consume(char expected) {
        skip_whitespace();
        if (position_ >= text_.size() || text_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    [[nodiscard]] char peek() {
        skip_whitespace();
        if (position_ >= text_.size()) {
            return '\0';
        }
        return text_[position_];
    }

    [[nodiscard]] std::optional<std::string> parse_string() {
        skip_whitespace();
        if (position_ >= text_.size() || text_[position_] != '"') {
            return std::nullopt;
        }
        ++position_;

        std::string value;
        while (position_ < text_.size()) {
            const char character = text_[position_++];
            if (character == '"') {
                return value;
            }
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            if (position_ >= text_.size()) {
                return std::nullopt;
            }
            const char escaped = text_[position_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u': {
                const std::optional<std::uint32_t> code_point = parse_code_point();
                if (!code_point.has_value()) {
                    return std::nullopt;
                }
                append_utf8_code_point(value, *code_point);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<double> parse_number() {
        skip_whitespace();
        const std::size_t begin_index = position_;
        if (position_ < text_.size() && text_[position_] == '-') {
            ++position_;
        }
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if ((character >= '0' && character <= '9') || character == '.' || character == 'e' || character == 'E' || character == '-') {
                ++position_;
                continue;
            }
            break;
        }
        if (begin_index == position_) {
            return std::nullopt;
        }

        double value = 0.0;
        const char* begin = text_.data() + begin_index;
        const char* end = text_.data() + position_;
        const auto [parsed_end, error_code] = std::from_chars(begin, end, value);
        if (error_code != std::errc{} || parsed_end != end) {
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] bool skip_value() {
        skip_whitespace();
        if (position_ >= text_.size()) {
            return false;
        }
        const char character = text_[position_];
        if (character == '"') {
            return parse_string().has_value();
        }
        if (character == '{') {
            return skip_object();
        }
        if (character == '[') {
            return skip_array();
        }
        if ((character >= '0' && character <= '9') || character == '-') {
            return parse_number().has_value();
        }
        return match_literal("true") || match_literal("false") || match_literal("null");
    }

private:
    static void append_utf8_code_point(std::string& text, std::uint32_t code_point) {
        if (code_point <= 0x7Fu) {
            text.push_back(static_cast<char>(code_point));
            return;
        }
        if (code_point <= 0x7FFu) {
            text.push_back(static_cast<char>(0xC0u | (code_point >> 6u)));
            text.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
            return;
        }
        if (code_point <= 0xFFFFu) {
            text.push_back(static_cast<char>(0xE0u | (code_point >> 12u)));
            text.push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu)));
            text.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
            return;
        }
        text.push_back(static_cast<char>(0xF0u | (code_point >> 18u)));
        text.push_back(static_cast<char>(0x80u | ((code_point >> 12u) & 0x3Fu)));
        text.push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu)));
        text.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }

    [[nodiscard]] bool skip_array() {
        if (!consume('[')) {
            return false;
        }
        skip_whitespace();
        if (consume(']')) {
            return true;
        }
        for (;;) {
            if (!skip_value()) {
                return false;
            }
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    [[nodiscard]] bool skip_object() {
        if (!consume('{')) {
            return false;
        }
        skip_whitespace();
        if (consume('}')) {
            return true;
        }
        for (;;) {
            if (!parse_string().has_value()) {
                return false;
            }
            if (!consume(':')) {
                return false;
            }
            if (!skip_value()) {
                return false;
            }
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    [[nodiscard]] bool match_literal(std::string_view literal) {
        skip_whitespace();
        if (!text_.substr(position_).starts_with(literal)) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    [[nodiscard]] std::optional<std::uint32_t> parse_code_point() {
        const std::optional<std::uint32_t> first_code_unit = parse_hex_code_unit();
        if (!first_code_unit.has_value()) {
            return std::nullopt;
        }
        if (*first_code_unit < 0xD800u || *first_code_unit > 0xDBFFu) {
            return first_code_unit;
        }
        if (position_ + 6 > text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u') {
            return std::nullopt;
        }
        position_ += 2;
        const std::optional<std::uint32_t> second_code_unit = parse_hex_code_unit();
        if (!second_code_unit.has_value() || *second_code_unit < 0xDC00u || *second_code_unit > 0xDFFFu) {
            return std::nullopt;
        }
        return 0x10000u + (((*first_code_unit - 0xD800u) << 10u) | (*second_code_unit - 0xDC00u));
    }

    [[nodiscard]] std::optional<std::uint32_t> parse_hex_code_unit() {
        if (position_ + 4 > text_.size()) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = text_[position_++];
            value <<= 4u;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        return value;
    }

    std::string_view text_;
    std::size_t position_{0};
};

namespace detail {

[[nodiscard]] inline bool find_string_field_in_value(
    Cursor& cursor,
    std::string_view field_name,
    std::optional<std::string>& extracted_value);

[[nodiscard]] inline bool find_string_field_in_array(
    Cursor& cursor,
    std::string_view field_name,
    std::optional<std::string>& extracted_value) {
    if (!cursor.consume('[')) {
        return false;
    }
    cursor.skip_whitespace();
    if (cursor.consume(']')) {
        return true;
    }
    for (;;) {
        if (!find_string_field_in_value(cursor, field_name, extracted_value)) {
            return false;
        }
        if (extracted_value.has_value()) {
            return true;
        }
        if (cursor.consume(']')) {
            return true;
        }
        if (!cursor.consume(',')) {
            return false;
        }
    }
}

[[nodiscard]] inline bool find_string_field_in_object(
    Cursor& cursor,
    std::string_view field_name,
    std::optional<std::string>& extracted_value) {
    if (!cursor.consume('{')) {
        return false;
    }
    cursor.skip_whitespace();
    if (cursor.consume('}')) {
        return true;
    }
    for (;;) {
        const std::optional<std::string> key = cursor.parse_string();
        if (!key.has_value() || !cursor.consume(':')) {
            return false;
        }
        if (*key == field_name && cursor.peek() == '"') {
            extracted_value = cursor.parse_string();
            return extracted_value.has_value();
        }
        if (!find_string_field_in_value(cursor, field_name, extracted_value)) {
            return false;
        }
        if (extracted_value.has_value()) {
            return true;
        }
        if (cursor.consume('}')) {
            return true;
        }
        if (!cursor.consume(',')) {
            return false;
        }
    }
}

[[nodiscard]] inline bool find_string_field_in_value(
    Cursor& cursor,
    std::string_view field_name,
    std::optional<std::string>& extracted_value) {
    cursor.skip_whitespace();
    const char token = cursor.peek();
    if (token == '{') {
        return find_string_field_in_object(cursor, field_name, extracted_value);
    }
    if (token == '[') {
        return find_string_field_in_array(cursor, field_name, extracted_value);
    }
    return cursor.skip_value();
}

}

[[nodiscard]] inline std::optional<std::string> find_string_field(std::string_view json_text, std::string_view field_name) {
    Cursor cursor{json_text};
    std::optional<std::string> extracted_value;
    if (!detail::find_string_field_in_value(cursor, field_name, extracted_value)) {
        return std::nullopt;
    }
    return extracted_value;
}

}