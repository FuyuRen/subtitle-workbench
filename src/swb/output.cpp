#include "swb/output.h"

#include "swb/file_io.h"
#include "swb/text.h"
#include "swb/workspace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace swb {

namespace {

constexpr std::string_view output_filename = "output.mp4";
constexpr std::string_view hard_subtitle_filename = "output.hard.ass";
constexpr std::string_view encoder_probe_input = "color=c=black:s=640x360:d=0.1";
constexpr int output_timeout_ms = 1'800'000;
constexpr int stacked_cue_gap = 12;
constexpr int encoder_probe_timeout_ms = 15'000;
constexpr double cue_overlap_tolerance_seconds = 0.08;

struct VideoEncoderProfile {
    std::string_view ffmpeg_name;
    std::string_view label;
};

constexpr VideoEncoderProfile software_encoder_profile{
    .ffmpeg_name = "libx264",
    .label = "CPU",
};

constexpr std::array hardware_encoder_profiles{
    VideoEncoderProfile{.ffmpeg_name = "h264_nvenc", .label = "NVENC"},
    VideoEncoderProfile{.ffmpeg_name = "h264_qsv", .label = "QSV"},
    VideoEncoderProfile{.ffmpeg_name = "h264_amf", .label = "AMF"},
};

[[nodiscard]] bool path_exists(const std::filesystem::path& path) {
    std::error_code error_code;
    return std::filesystem::exists(path, error_code) && !error_code;
}

[[nodiscard]] std::string first_non_empty_line(std::string_view text) {
    std::size_t line_begin = 0;
    while (line_begin < text.size()) {
        std::size_t line_end = text.find_first_of("\r\n", line_begin);
        if (line_end == std::string_view::npos) {
            line_end = text.size();
        }
        const std::string_view line = trim_ascii_whitespace(text.substr(line_begin, line_end - line_begin));
        if (!line.empty()) {
            return std::string{line};
        }
        line_begin = text.find_first_not_of("\r\n", line_end);
        if (line_begin == std::string_view::npos) {
            break;
        }
    }
    return {};
}

[[nodiscard]] std::string process_failure_message(const ProcessResult& result) {
    if (!result.error.empty()) {
        return "编码失败: " + result.error;
    }
    if (const std::string line = first_non_empty_line(result.stderr_data); !line.empty()) {
        return "编码失败: " + line;
    }
    if (const std::string line = first_non_empty_line(result.stdout_data); !line.empty()) {
        return "编码失败: " + line;
    }
    return "编码失败";
}

[[nodiscard]] std::vector<std::string> build_h264_encoder_arguments(const VideoEncoderProfile& encoder) {
    if (encoder.ffmpeg_name == "h264_nvenc") {
        return {
            "-c:v", std::string{encoder.ffmpeg_name},
            "-preset", "p3",
            "-cq", "23",
            "-pix_fmt", "yuv420p",
        };
    }
    if (encoder.ffmpeg_name == "h264_qsv") {
        return {
            "-c:v", std::string{encoder.ffmpeg_name},
            "-preset", "faster",
            "-global_quality", "23",
            "-pix_fmt", "nv12",
        };
    }
    if (encoder.ffmpeg_name == "h264_amf") {
        return {
            "-c:v", std::string{encoder.ffmpeg_name},
            "-quality", "speed",
            "-qp_i", "23",
            "-qp_p", "23",
            "-pix_fmt", "yuv420p",
        };
    }
    return {
        "-c:v", std::string{software_encoder_profile.ffmpeg_name},
        "-preset", "medium",
        "-crf", "20",
        "-pix_fmt", "yuv420p",
    };
}

[[nodiscard]] bool probe_h264_encoder(
    const std::filesystem::path& ffmpeg_executable,
    const SubtitleOutputRenderer::Runner& runner,
    const VideoEncoderProfile& encoder,
    const std::atomic<bool>& cancel) {
    ProcessOptions process_options;
    process_options.executable = ffmpeg_executable;
    process_options.cancel = &cancel;
    process_options.timeout_ms = encoder_probe_timeout_ms;
    process_options.arguments = {
        "-hide_banner",
        "-loglevel", "error",
        "-f", "lavfi",
        "-i", std::string{encoder_probe_input},
        "-frames:v", "1",
    };
    const std::vector<std::string> encoder_arguments = build_h264_encoder_arguments(encoder);
    process_options.arguments.insert(process_options.arguments.end(), encoder_arguments.begin(), encoder_arguments.end());
    process_options.arguments.insert(process_options.arguments.end(), {"-f", "null", "-"});

    const ProcessResult result = runner(process_options);
    return result.launched && !result.canceled && !result.timed_out && result.exit_code == 0;
}

[[nodiscard]] VideoEncoderProfile select_h264_encoder(
    const std::filesystem::path& ffmpeg_executable,
    const SubtitleOutputRenderer::Runner& runner,
    const std::atomic<bool>& cancel) {
    for (const VideoEncoderProfile& encoder : hardware_encoder_profiles) {
        if (probe_h264_encoder(ffmpeg_executable, runner, encoder, cancel)) {
            return encoder;
        }
    }
    return software_encoder_profile;
}

[[nodiscard]] std::filesystem::path resolve_ffprobe_executable(const std::filesystem::path& ffmpeg_executable) {
    const std::filesystem::path sibling = ffmpeg_executable.parent_path() / L"ffprobe.exe";
    if (path_exists(sibling)) {
        return sibling;
    }
    return std::filesystem::path{L"ffprobe.exe"};
}

[[nodiscard]] std::optional<double> probe_media_duration_seconds(
    const std::filesystem::path& ffprobe_executable,
    const SubtitleOutputRenderer::Runner& runner,
    const std::filesystem::path& media_path,
    const std::atomic<bool>& cancel) {
    ProcessOptions process_options;
    process_options.executable = ffprobe_executable;
    process_options.cancel = &cancel;
    process_options.timeout_ms = encoder_probe_timeout_ms;
    process_options.arguments = {
        "-v", "error",
        "-show_entries", "format=duration",
        "-of", "default=noprint_wrappers=1:nokey=1",
        path_to_utf8(media_path),
    };

    const ProcessResult result = runner(process_options);
    if (!result.launched || result.canceled || result.timed_out) {
        return std::nullopt;
    }

    const std::string duration_text = first_non_empty_line(result.stdout_data);
    if (duration_text.empty()) {
        return std::nullopt;
    }

    try {
        const double duration_seconds = std::stod(duration_text);
        if (!std::isfinite(duration_seconds) || duration_seconds <= 0.0) {
            return std::nullopt;
        }
        return duration_seconds;
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}


[[nodiscard]] double estimated_output_duration_seconds(
    std::span<const SubtitleCue> source_cues,
    std::span<const SubtitleCue> translated_cues) noexcept {
    double duration_seconds = 0.0;
    for (const SubtitleCue& cue : source_cues) {
        duration_seconds = std::max(duration_seconds, cue.end_seconds);
    }
    for (const SubtitleCue& cue : translated_cues) {
        duration_seconds = std::max(duration_seconds, cue.end_seconds);
    }
    return std::max(duration_seconds, 1.0);
}

[[nodiscard]] std::optional<double> parse_ffmpeg_progress_time(std::string_view value) {
    const std::size_t first_separator = value.find(':');
    if (first_separator == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t second_separator = value.find(':', first_separator + 1);
    if (second_separator == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view hours_text = value.substr(0, first_separator);
    const std::string_view minutes_text = value.substr(first_separator + 1, second_separator - first_separator - 1);
    const std::string_view seconds_text = value.substr(second_separator + 1);

    try {
        const int hours = std::stoi(std::string{hours_text});
        const int minutes = std::stoi(std::string{minutes_text});
        const double seconds = std::stod(std::string{seconds_text});
        return static_cast<double>(hours * 3600 + minutes * 60) + seconds;
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<double> parse_ffmpeg_progress_microseconds(std::string_view value) {
    try {
        const double microseconds = std::stod(std::string{trim_ascii_whitespace(value)});
        if (!std::isfinite(microseconds) || microseconds < 0.0) {
            return std::nullopt;
        }
        return microseconds / 1'000'000.0;
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::string format_output_progress(
    std::string_view stage_label,
    int percent,
    std::string_view speed,
    std::string_view encoder_label) {
    std::string status{stage_label};
    status.append("（");
    status.append(std::to_string(std::clamp(percent, 0, 100)));
    status.append("%");
    if (!speed.empty() && speed != "N/A") {
        status.append("，");
        status.append(speed);
    }
    if (!encoder_label.empty()) {
        status.append("，");
        status.append(encoder_label);
    }
    status.append("）");
    return status;
}

[[nodiscard]] OutputProgress make_output_progress(
    std::string_view stage_label,
    int percent,
    std::string_view speed,
    std::string_view encoder_label) {
    return {
        .status = format_output_progress(stage_label, percent, speed, encoder_label),
        .fraction = static_cast<double>(std::clamp(percent, 0, 100)) / 100.0,
    };
}

[[nodiscard]] std::string format_ass_timestamp(double seconds) {
    const long long total_centiseconds = std::max(0LL, static_cast<long long>(std::llround(seconds * 100.0)));
    const long long hours = total_centiseconds / 360'000;
    const long long minutes = (total_centiseconds / 6'000) % 60;
    const long long whole_seconds = (total_centiseconds / 100) % 60;
    const long long centiseconds = total_centiseconds % 100;

    std::ostringstream output;
    output << hours << ':'
           << std::setfill('0') << std::setw(2) << minutes << ':'
           << std::setw(2) << whole_seconds << '.'
           << std::setw(2) << centiseconds;
    return output.str();
}

[[nodiscard]] std::string escape_ass_text(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (const char character : text) {
        switch (character) {
        case '\\':
            escaped.append("\\\\");
            break;
        case '{':
            escaped.append("\\{");
            break;
        case '}':
            escaped.append("\\}");
            break;
        case '\r':
            break;
        case '\n':
            escaped.append("\\N");
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

[[nodiscard]] std::string ass_color(const SubtitleColor& color) {
    std::ostringstream output;
    output << "&H"
           << std::uppercase << std::hex << std::setfill('0')
           << std::setw(2) << (255 - static_cast<int>(color.alpha))
           << std::setw(2) << static_cast<int>(color.blue)
           << std::setw(2) << static_cast<int>(color.green)
           << std::setw(2) << static_cast<int>(color.red);
    return output.str();
}

[[nodiscard]] std::string configured_font_name(std::string_view font_name) {
    const std::string_view trimmed = trim_ascii_whitespace(font_name);
    if (trimmed.empty()) {
        return "Microsoft YaHei";
    }
    return std::string{trimmed};
}

[[nodiscard]] int normalized_font_size(int font_size) noexcept {
    return std::max(font_size, 1);
}

[[nodiscard]] float normalized_outline_thickness(float outline_thickness) noexcept {
    return std::max(outline_thickness, 0.0f);
}

[[nodiscard]] int normalized_bilingual_line_gap(float bilingual_line_gap) noexcept {
    return std::max(0, static_cast<int>(std::lround(bilingual_line_gap)));
}

[[nodiscard]] std::vector<int> assign_cue_stack_levels(std::span<const SubtitleCue> cues) {
    std::vector<int> stack_levels(cues.size(), 0);
    std::vector<double> lane_end_seconds;
    lane_end_seconds.reserve(cues.size());

    for (std::size_t index = 0; index < cues.size(); ++index) {
        const SubtitleCue& cue = cues[index];
        std::size_t lane_index = 0;
        for (; lane_index < lane_end_seconds.size(); ++lane_index) {
            if (cue.start_seconds >= (lane_end_seconds[lane_index] - cue_overlap_tolerance_seconds)) {
                lane_end_seconds[lane_index] = cue.end_seconds;
                break;
            }
        }
        if (lane_index == lane_end_seconds.size()) {
            lane_end_seconds.push_back(cue.end_seconds);
        }
        stack_levels[index] = static_cast<int>(lane_index);
    }

    return stack_levels;
}

}

std::optional<std::string> serialize_hard_subtitle_script(
    std::span<const SubtitleCue> translated_cues,
    std::span<const SubtitleCue> source_cues,
    const Config& configuration) {
    if (configuration.bilingual_subtitles && translated_cues.size() != source_cues.size()) {
        return std::nullopt;
    }

    const HardSubtitleStyle& style = configuration.hard_subtitle_style;
    const int chinese_font_size = normalized_font_size(style.chinese_font_size);
    const int english_font_size = normalized_font_size(style.english_font_size);
    const int bottom_margin = std::max(style.bottom_margin, 0);
    const float outline_thickness = normalized_outline_thickness(style.outline_thickness);
    const int bilingual_line_gap = normalized_bilingual_line_gap(style.bilingual_line_gap);
    const int stacked_cue_vertical_spacing = configuration.bilingual_subtitles
        ? chinese_font_size + english_font_size + bilingual_line_gap + stacked_cue_gap
        : chinese_font_size + stacked_cue_gap;
    const std::vector<int> stack_levels = assign_cue_stack_levels(translated_cues);

    std::ostringstream output;
    output << "[Script Info]\n"
           << "ScriptType: v4.00+\n"
           << "Collisions: Normal\n"
           << "WrapStyle: 0\n"
           << "ScaledBorderAndShadow: yes\n"
           << "YCbCr Matrix: TV.601\n"
           << "PlayResX: 1920\n"
           << "PlayResY: 1080\n\n"
           << "[V4+ Styles]\n"
           << "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
           << "Style: DefaultZh,"
           << configured_font_name(style.font_name) << ','
           << chinese_font_size << ','
           << ass_color(style.fill_color) << ','
           << ass_color(style.fill_color) << ','
           << ass_color(style.outline_color) << ",&H64000000,0,0,0,0,100,100,0,0,1,"
           << outline_thickness
           << ",0,2,60,60,"
           << bottom_margin
           << ",1\n"
           << "Style: DefaultEn,"
           << configured_font_name(style.font_name) << ','
           << english_font_size << ','
           << ass_color(style.fill_color) << ','
           << ass_color(style.fill_color) << ','
           << ass_color(style.outline_color) << ",&H64000000,0,0,0,0,100,100,0,0,1,"
           << outline_thickness
           << ",0,2,60,60,"
           << bottom_margin
           << ",1\n\n"
           << "[Events]\n"
           << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

    for (std::size_t index = 0; index < translated_cues.size(); ++index) {
         const bool has_source_line = configuration.bilingual_subtitles && !source_cues[index].text.empty();
        const int base_margin_v = bottom_margin + stack_levels[index] * stacked_cue_vertical_spacing;
         if (has_source_line) {
            output << "Dialogue: 0,"
                   << format_ass_timestamp(translated_cues[index].start_seconds) << ','
                   << format_ass_timestamp(translated_cues[index].end_seconds) << ",DefaultEn,,0,0,"
                   << base_margin_v
                   << ",,"
                   << escape_ass_text(source_cues[index].text) << '\n';
        }

         output << "Dialogue: 0,"
             << format_ass_timestamp(translated_cues[index].start_seconds) << ','
             << format_ass_timestamp(translated_cues[index].end_seconds) << ",DefaultZh,,0,0,"
             << (has_source_line ? base_margin_v + english_font_size + bilingual_line_gap : base_margin_v)
             << ",,"
             << escape_ass_text(translated_cues[index].text) << '\n';
    }

    return output.str();
}

SubtitleOutputRenderer::SubtitleOutputRenderer(std::filesystem::path ffmpeg_executable, Runner runner)
    : ffmpeg_executable_(std::move(ffmpeg_executable)),
      runner_(std::move(runner)) {}

OutputResult SubtitleOutputRenderer::render(
    const Config& configuration,
    const std::filesystem::path& video_path,
    std::span<const SubtitleCue> source_cues,
    std::span<const SubtitleCue> translated_cues,
    const std::filesystem::path& working_directory,
    const std::atomic<bool>& cancel,
    ProgressCallback on_progress) const {
    if (cancel.load(std::memory_order_acquire)) {
        return {false, "已取消"};
    }
    if (!path_exists(video_path)) {
        return {false, "工作目录缺少视频文件"};
    }
    if (ffmpeg_executable_.empty()) {
        return {false, "未找到ffmpeg"};
    }
    if (!runner_) {
        return {false, "输出执行器不可用"};
    }
    if (translated_cues.empty()) {
        return {false, "缺少可输出字幕"};
    }
    if (configuration.bilingual_subtitles && translated_cues.size() != source_cues.size()) {
        return {false, "双语字幕数量不匹配"};
    }

    std::error_code error_code;
    std::filesystem::create_directories(working_directory, error_code);
    if (error_code) {
        return {false, "无法创建输出目录"};
    }

    const std::filesystem::path output_path = working_directory / std::filesystem::u8path(std::string{output_filename});
    std::filesystem::remove(output_path, error_code);

    ProcessOptions process_options;
    process_options.executable = ffmpeg_executable_;
    process_options.working_directory = working_directory;
    process_options.cancel = &cancel;
    process_options.timeout_ms = output_timeout_ms;

    const std::filesystem::path ffprobe_executable = resolve_ffprobe_executable(ffmpeg_executable_);
    const std::string_view stage_label{"硬编码中"};
    const double duration_seconds = probe_media_duration_seconds(ffprobe_executable, runner_, video_path, cancel)
        .value_or(estimated_output_duration_seconds(source_cues, translated_cues));
    const VideoEncoderProfile selected_encoder = select_h264_encoder(ffmpeg_executable_, runner_, cancel);
    const std::optional<std::string> ass_script = serialize_hard_subtitle_script(
        translated_cues,
        source_cues,
        configuration);
    if (!ass_script.has_value()) {
        return {false, "双语字幕数量不匹配"};
    }

    const std::filesystem::path subtitle_path = working_directory / std::filesystem::u8path(std::string{hard_subtitle_filename});
    if (!write_text_file(subtitle_path, *ass_script)) {
        return {false, "无法写入硬字幕脚本"};
    }

    process_options.arguments = {
        "-y",
        "-hide_banner",
        "-loglevel", "error",
        "-progress", "pipe:1",
        "-nostats",
        "-nostdin",
        "-i", path_to_utf8(video_path),
        "-vf", "subtitles=" + std::string{hard_subtitle_filename},
        "-map", "0:v:0",
        "-map", "0:a?",
        "-c:a", "aac",
        "-b:a", "192k",
        "-movflags", "+faststart",
    };
    const std::vector<std::string> encoder_arguments = build_h264_encoder_arguments(selected_encoder);
    process_options.arguments.insert(process_options.arguments.end(), encoder_arguments.begin(), encoder_arguments.end());
    process_options.arguments.push_back(path_to_utf8(output_path));

    if (on_progress) {
        on_progress(make_output_progress(stage_label, 0, {}, selected_encoder.label));
    }

    std::string progress_buffer;
    double encoded_seconds = 0.0;
    std::string speed;
    process_options.on_stdout = [&](std::string_view chunk) {
        progress_buffer.append(chunk);

        std::size_t line_end = progress_buffer.find_first_of("\r\n");
        while (line_end != std::string::npos) {
            std::string line = progress_buffer.substr(0, line_end);
            progress_buffer.erase(0, line_end + 1);
            if (!progress_buffer.empty() && progress_buffer.front() == '\n' && line_end + 1 < chunk.size()) {
                progress_buffer.erase(0, 1);
            }

            const std::string_view trimmed_line = trim_ascii_whitespace(line);
            const std::size_t separator_position = trimmed_line.find('=');
            if (separator_position == std::string_view::npos) {
                line_end = progress_buffer.find_first_of("\r\n");
                continue;
            }

            const std::string_view key = trimmed_line.substr(0, separator_position);
            const std::string_view value = trimmed_line.substr(separator_position + 1);
            if (key == "out_time") {
                if (const std::optional<double> parsed_seconds = parse_ffmpeg_progress_time(value); parsed_seconds.has_value()) {
                    encoded_seconds = *parsed_seconds;
                }
            } else if (key == "out_time_us" || key == "out_time_ms") {
                if (const std::optional<double> parsed_seconds = parse_ffmpeg_progress_microseconds(value); parsed_seconds.has_value()) {
                    encoded_seconds = *parsed_seconds;
                }
            } else if (key == "speed") {
                speed = std::string{trim_ascii_whitespace(value)};
            } else if (key == "progress" && on_progress) {
                const int percent = value == "end"
                    ? 100
                    : std::clamp(static_cast<int>(std::lround((encoded_seconds / duration_seconds) * 100.0)), 0, 99);
                on_progress(make_output_progress(stage_label, percent, speed, selected_encoder.label));
            }

            line_end = progress_buffer.find_first_of("\r\n");
        }
    };

    const ProcessResult result = runner_(process_options);
    if (!result.launched) {
        return {false, process_failure_message(result)};
    }
    if (result.canceled || cancel.load(std::memory_order_acquire)) {
        return {false, "已取消"};
    }
    if (result.timed_out) {
        return {false, "编码超时"};
    }
    if (result.exit_code != 0) {
        return {false, process_failure_message(result)};
    }
    if (!path_exists(output_path)) {
        return {false, "未生成输出文件"};
    }

    return {true, std::string{"硬编码完成（"} + std::string{selected_encoder.label} + "）", output_path};
}

}