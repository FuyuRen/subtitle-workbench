#include "swb/srt.h"

#include "swb/file_io.h"
#include "swb/text.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <optional>
#include <sstream>

namespace swb {

namespace {

[[nodiscard]] std::string normalize_line(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

[[nodiscard]] bool is_integer_line(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<double> parse_timestamp(std::string_view text) {
    if (text.size() != 12 || text[2] != ':' || text[5] != ':' || text[8] != ',') {
        return std::nullopt;
    }
    const auto parse_component = [](std::string_view component) -> std::optional<int> {
        int value = 0;
        for (const char character : component) {
            if (character < '0' || character > '9') {
                return std::nullopt;
            }
            value = value * 10 + (character - '0');
        }
        return value;
    };

    const std::optional<int> hours = parse_component(text.substr(0, 2));
    const std::optional<int> minutes = parse_component(text.substr(3, 2));
    const std::optional<int> seconds = parse_component(text.substr(6, 2));
    const std::optional<int> milliseconds = parse_component(text.substr(9, 3));
    if (!hours.has_value() || !minutes.has_value() || !seconds.has_value() || !milliseconds.has_value()) {
        return std::nullopt;
    }
    return static_cast<double>(*hours * 3600 + *minutes * 60 + *seconds) + static_cast<double>(*milliseconds) / 1000.0;
}

[[nodiscard]] bool parse_time_range(std::string_view line, SubtitleCue& cue) {
    const std::size_t separator = line.find("-->");
    if (separator == std::string_view::npos) {
        return false;
    }
    const std::optional<double> start_seconds = parse_timestamp(trim_ascii_whitespace(line.substr(0, separator)));
    const std::optional<double> end_seconds = parse_timestamp(trim_ascii_whitespace(line.substr(separator + 3)));
    if (!start_seconds.has_value() || !end_seconds.has_value() || *end_seconds < *start_seconds) {
        return false;
    }
    cue.start_seconds = *start_seconds;
    cue.end_seconds = *end_seconds;
    return true;
}

}

std::string format_srt_timestamp(double seconds) {
    const long long total_milliseconds = std::max(0LL, static_cast<long long>(std::llround(seconds * 1000.0)));
    const long long hours = total_milliseconds / 3'600'000;
    const long long minutes = (total_milliseconds / 60'000) % 60;
    const long long whole_seconds = (total_milliseconds / 1000) % 60;
    const long long milliseconds = total_milliseconds % 1000;

    std::ostringstream output;
    output << std::setfill('0')
           << std::setw(2) << hours << ':'
           << std::setw(2) << minutes << ':'
           << std::setw(2) << whole_seconds << ','
           << std::setw(3) << milliseconds;
    return output.str();
}

std::string serialize_srt(std::span<const SubtitleCue> cues) {
    std::ostringstream output;
    for (std::size_t index = 0; index < cues.size(); ++index) {
        const SubtitleCue& cue = cues[index];
        output << (index + 1) << '\n'
               << format_srt_timestamp(cue.start_seconds)
               << " --> "
               << format_srt_timestamp(cue.end_seconds)
               << '\n'
               << cue.text
               << "\n\n";
    }
    return output.str();
}

void normalize_sequential_cue_timings(std::span<SubtitleCue> cues) {
    for (std::size_t index = 0; index + 1 < cues.size(); ++index) {
        SubtitleCue& current_cue = cues[index];
        SubtitleCue& next_cue = cues[index + 1];
        if (current_cue.end_seconds <= next_cue.start_seconds) {
            continue;
        }

        const double midpoint_seconds = std::midpoint(current_cue.end_seconds, next_cue.start_seconds);
        const double boundary_seconds = current_cue.start_seconds <= next_cue.end_seconds
            ? std::clamp(midpoint_seconds, current_cue.start_seconds, next_cue.end_seconds)
            : current_cue.start_seconds;
        current_cue.end_seconds = boundary_seconds;
        next_cue.start_seconds = boundary_seconds;
    }
}

SrtReadResult parse_srt(std::string_view content) {
    SrtReadResult result;
    std::istringstream input(std::string{content});
    std::string line;
    std::vector<std::string> block_lines;

    const auto flush_block = [&]() -> bool {
        if (block_lines.empty()) {
            return true;
        }
        const std::size_t time_line_index = (block_lines.size() >= 2 && is_integer_line(block_lines.front())) ? 1u : 0u;
        if (time_line_index >= block_lines.size()) {
            result.message = "SRT格式无效";
            return false;
        }

        SubtitleCue cue;
        if (!parse_time_range(block_lines[time_line_index], cue)) {
            result.message = "SRT时间轴无效";
            return false;
        }

        for (std::size_t index = time_line_index + 1; index < block_lines.size(); ++index) {
            if (!cue.text.empty()) {
                cue.text.push_back('\n');
            }
            cue.text.append(block_lines[index]);
        }
        result.cues.push_back(std::move(cue));
        block_lines.clear();
        return true;
    };

    while (std::getline(input, line)) {
        line = normalize_line(std::move(line));
        if (line.empty()) {
            if (!flush_block()) {
                return result;
            }
            continue;
        }
        block_lines.push_back(std::move(line));
    }

    if (!flush_block()) {
        return result;
    }
    result.success = true;
    return result;
}

SrtReadResult load_srt(const std::filesystem::path& path) {
    const std::optional<std::string> content = read_text_file(path);
    if (!content.has_value()) {
        return {false, "无法读取SRT文件"};
    }
    return parse_srt(*content);
}

bool save_srt(std::span<const SubtitleCue> cues, const std::filesystem::path& path) {
    return write_text_file(path, serialize_srt(cues));
}

}