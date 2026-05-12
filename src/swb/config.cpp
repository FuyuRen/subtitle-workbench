#include "swb/config.h"

#include "swb/text.h"
#include "swb/workspace.h"

#include <charconv>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace swb {

namespace {

[[nodiscard]] std::string lower_ascii(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char character : value) {
        if (character >= 'A' && character <= 'Z') {
            lowered.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            lowered.push_back(character);
        }
    }
    return lowered;
}

[[nodiscard]] bool parse_bool(std::string_view value) {
    const std::string lowered = lower_ascii(trim_ascii_whitespace(value));
    return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
}

[[nodiscard]] std::optional<int> parse_int(std::string_view value) {
    const std::string_view trimmed = trim_ascii_whitespace(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    int parsed_value = 0;
    const char* begin = trimmed.data();
    const char* end = trimmed.data() + trimmed.size();
    const auto [parsed_end, error_code] = std::from_chars(begin, end, parsed_value);
    if (error_code != std::errc{} || parsed_end != end) {
        return std::nullopt;
    }
    return parsed_value;
}

[[nodiscard]] std::optional<float> parse_non_negative_float(std::string_view value) {
    const std::string trimmed{trim_ascii_whitespace(value)};
    if (trimmed.empty()) {
        return std::nullopt;
    }
    try {
        const float parsed_value = std::stof(trimmed);
        if (parsed_value < 0.0f) {
            return std::nullopt;
        }
        return parsed_value;
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::uint8_t> parse_color_component(std::string_view value) {
    const std::optional<int> parsed_value = parse_int(value);
    if (!parsed_value.has_value() || *parsed_value < 0 || *parsed_value > 255) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(*parsed_value);
}

[[nodiscard]] std::optional<SubtitleColor> parse_color(std::string_view value) {
    SubtitleColor color;
    std::string_view remaining = trim_ascii_whitespace(value);
    std::uint8_t* components[] = {
        &color.red,
        &color.green,
        &color.blue,
        &color.alpha,
    };

    for (std::size_t index = 0; index < std::size(components); ++index) {
        const std::size_t separator_position = remaining.find(',');
        const std::string_view component = separator_position == std::string_view::npos
            ? remaining
            : remaining.substr(0, separator_position);
        const std::optional<std::uint8_t> parsed_component = parse_color_component(component);
        if (!parsed_component.has_value()) {
            return std::nullopt;
        }
        *components[index] = *parsed_component;
        if (separator_position == std::string_view::npos) {
            remaining = {};
        } else {
            remaining = remaining.substr(separator_position + 1);
        }
    }

    if (!trim_ascii_whitespace(remaining).empty()) {
        return std::nullopt;
    }
    return color;
}

[[nodiscard]] std::string serialize_color(const SubtitleColor& color) {
    return std::to_string(static_cast<int>(color.red))
        + ","
        + std::to_string(static_cast<int>(color.green))
        + ","
        + std::to_string(static_cast<int>(color.blue))
        + ","
        + std::to_string(static_cast<int>(color.alpha));
}

}

std::filesystem::path default_config_path() {
    return executable_directory() / L"config.ini";
}

Config load_config(const std::filesystem::path& path) {
    Config configuration;
    std::ifstream input(path);
    if (!input) {
        return configuration;
    }

    std::unordered_map<std::string, std::string> key_values;
    std::string line;
    while (std::getline(input, line)) {
        const std::string_view view = trim_ascii_whitespace(line);
        if (view.empty() || view.front() == '#') {
            continue;
        }
        const std::string_view::size_type separator_position = view.find('=');
        if (separator_position == std::string_view::npos) {
            continue;
        }
        const std::string_view key = trim_ascii_whitespace(view.substr(0, separator_position));
        const std::string_view value = trim_ascii_whitespace(view.substr(separator_position + 1));
        key_values[std::string{key}] = std::string{value};
    }

    const auto assign_if_present = [&](std::string_view key, std::string& value) {
        if (const auto key_value_it = key_values.find(std::string(key)); key_value_it != key_values.end()) {
            value = key_value_it->second;
        }
    };

    auto assign_if_present_alias = [&](std::string_view primary_key, std::string_view secondary_key, std::string& value) {
        if (const auto primary_it = key_values.find(std::string(primary_key)); primary_it != key_values.end()) {
            value = primary_it->second;
            return;
        }
        if (const auto secondary_it = key_values.find(std::string(secondary_key)); secondary_it != key_values.end()) {
            value = secondary_it->second;
        }
    };

    assign_if_present("whisper_base_url", configuration.whisper_base_url);
    assign_if_present("whisper_api_key", configuration.whisper_api_key);
    assign_if_present("whisper_model", configuration.whisper_model);
    assign_if_present_alias("llm_base_url", "language_model_base_url", configuration.language_model_base_url);
    assign_if_present_alias("llm_api_key", "language_model_api_key", configuration.language_model_api_key);
    assign_if_present_alias("llm_model", "language_model_name", configuration.language_model_name);
    if (const auto retry_count_it = key_values.find("retry_count"); retry_count_it != key_values.end()) {
        if (const std::optional<int> retry_count = parse_int(retry_count_it->second); retry_count.has_value() && *retry_count >= 0) {
            configuration.retry_count = *retry_count;
        }
    }
    assign_if_present("source_lang", configuration.source_lang);
    assign_if_present("target_lang", configuration.target_lang);
    assign_if_present("output_dir", configuration.output_dir);
    assign_if_present("hard_subtitle_font", configuration.hard_subtitle_style.font_name);

    if (const auto bilingual_it = key_values.find("bilingual_subtitles"); bilingual_it != key_values.end()) {
        configuration.bilingual_subtitles = parse_bool(bilingual_it->second);
    }
    if (const auto chinese_font_size_it = key_values.find("hard_subtitle_font_size_zh"); chinese_font_size_it != key_values.end()) {
        if (const std::optional<int> font_size = parse_int(chinese_font_size_it->second); font_size.has_value() && *font_size > 0) {
            configuration.hard_subtitle_style.chinese_font_size = *font_size;
        }
    }
    if (const auto english_font_size_it = key_values.find("hard_subtitle_font_size_en"); english_font_size_it != key_values.end()) {
        if (const std::optional<int> font_size = parse_int(english_font_size_it->second); font_size.has_value() && *font_size > 0) {
            configuration.hard_subtitle_style.english_font_size = *font_size;
        }
    }
    if (const auto bottom_margin_it = key_values.find("hard_subtitle_bottom_margin"); bottom_margin_it != key_values.end()) {
        if (const std::optional<int> bottom_margin = parse_int(bottom_margin_it->second); bottom_margin.has_value() && *bottom_margin >= 0) {
            configuration.hard_subtitle_style.bottom_margin = *bottom_margin;
        }
    }
    if (const auto fill_color_it = key_values.find("hard_subtitle_fill_color"); fill_color_it != key_values.end()) {
        if (const std::optional<SubtitleColor> color = parse_color(fill_color_it->second); color.has_value()) {
            configuration.hard_subtitle_style.fill_color = *color;
        }
    }
    if (const auto outline_color_it = key_values.find("hard_subtitle_outline_color"); outline_color_it != key_values.end()) {
        if (const std::optional<SubtitleColor> color = parse_color(outline_color_it->second); color.has_value()) {
            configuration.hard_subtitle_style.outline_color = *color;
        }
    }
    if (const auto outline_thickness_it = key_values.find("hard_subtitle_outline_thickness"); outline_thickness_it != key_values.end()) {
        if (const std::optional<float> thickness = parse_non_negative_float(outline_thickness_it->second); thickness.has_value()) {
            configuration.hard_subtitle_style.outline_thickness = *thickness;
        }
    }
    if (const auto bilingual_gap_it = key_values.find("hard_subtitle_bilingual_gap"); bilingual_gap_it != key_values.end()) {
        if (const std::optional<float> gap = parse_non_negative_float(bilingual_gap_it->second); gap.has_value()) {
            configuration.hard_subtitle_style.bilingual_line_gap = *gap;
        }
    }
    if (const auto preview_background_color_it = key_values.find("hard_subtitle_preview_background_color"); preview_background_color_it != key_values.end()) {
        if (const std::optional<SubtitleColor> color = parse_color(preview_background_color_it->second); color.has_value()) {
            configuration.hard_subtitle_style.preview_background_color = *color;
        }
    }

    return configuration;
}

void save_config(const Config& configuration, const std::filesystem::path& path) {
    std::error_code error_code;
    std::filesystem::create_directories(path.parent_path(), error_code);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return;
    }

    output << "whisper_base_url=" << configuration.whisper_base_url << '\n'
           << "whisper_api_key=" << configuration.whisper_api_key << '\n'
           << "whisper_model=" << configuration.whisper_model << '\n'
           << "llm_base_url=" << configuration.language_model_base_url << '\n'
           << "llm_api_key=" << configuration.language_model_api_key << '\n'
           << "llm_model=" << configuration.language_model_name << '\n'
           << "retry_count=" << std::max(configuration.retry_count, 0) << '\n'
           << "source_lang=" << configuration.source_lang << '\n'
           << "target_lang=" << configuration.target_lang << '\n'
            << "output_dir=" << configuration.output_dir << '\n'
            << "bilingual_subtitles=" << (configuration.bilingual_subtitles ? "1" : "0") << '\n'
            << "hard_subtitle_font=" << configuration.hard_subtitle_style.font_name << '\n'
            << "hard_subtitle_font_size_zh=" << configuration.hard_subtitle_style.chinese_font_size << '\n'
            << "hard_subtitle_font_size_en=" << configuration.hard_subtitle_style.english_font_size << '\n'
            << "hard_subtitle_bottom_margin=" << std::max(configuration.hard_subtitle_style.bottom_margin, 0) << '\n'
            << "hard_subtitle_fill_color=" << serialize_color(configuration.hard_subtitle_style.fill_color) << '\n'
            << "hard_subtitle_outline_color=" << serialize_color(configuration.hard_subtitle_style.outline_color) << '\n'
            << "hard_subtitle_outline_thickness=" << configuration.hard_subtitle_style.outline_thickness << '\n'
            << "hard_subtitle_bilingual_gap=" << configuration.hard_subtitle_style.bilingual_line_gap << '\n'
            << "hard_subtitle_preview_background_color=" << serialize_color(configuration.hard_subtitle_style.preview_background_color) << '\n';
}

}
