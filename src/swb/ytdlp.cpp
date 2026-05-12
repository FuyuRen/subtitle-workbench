#include "swb/ytdlp.h"

#include "swb/process.h"
#include "swb/text.h"
#include "swb/workspace.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <system_error>
#include <utility>

namespace swb {

namespace {

constexpr int title_timeout_ms = 60'000;
constexpr int video_timeout_ms = 0;
constexpr int subtitle_timeout_ms = 90'000;

[[nodiscard]] std::filesystem::path subtitle_destination_path(
    const std::filesystem::path& working_directory,
    SubtitleOrigin origin) {
    return working_directory / (origin == SubtitleOrigin::manual ? "manual.en.srt" : "auto.en.srt");
}

std::optional<std::filesystem::path> find_first_matching_file(
    const std::filesystem::path& directory,
    std::string_view stem_prefix,
    std::string_view extension) {
    std::error_code error_code;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error_code)) {
        if (!entry.is_regular_file(error_code)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.size() < extension.size()) {
            continue;
        }
        const bool extension_matches = std::equal(
            extension.rbegin(),
            extension.rend(),
            name.rbegin(),
            [](char left, char right) {
                return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
            });
        if (!extension_matches) {
            continue;
        }
        if (!stem_prefix.empty() && !name.starts_with(stem_prefix)) {
            continue;
        }
        return entry.path();
    }
    return std::nullopt;
}

void remove_matching_files(const std::filesystem::path& directory, std::string_view stem_prefix) {
    std::error_code error_code;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error_code)) {
        if (!entry.is_regular_file(error_code)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.starts_with(stem_prefix)) {
            std::filesystem::remove(entry.path(), error_code);
        }
    }
}

std::optional<std::int64_t> parse_int_field(std::string_view text) {
    if (text.empty() || text == "NA") {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    auto [parsed_end, error_code] = std::from_chars(begin, end, value);
    if (error_code != std::errc{} || parsed_end != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parse_double_field(std::string_view text) {
    if (text.empty() || text == "NA") {
        return std::nullopt;
    }
    double value = 0.0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    auto [parsed_end, error_code] = std::from_chars(begin, end, value);
    if (error_code != std::errc{} || parsed_end != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<DownloadProgress> parse_progress(std::string_view payload) {
    std::array<std::string_view, 4> fields;
    std::size_t field_count = 0;
    std::size_t field_start = 0;
    for (std::size_t index = 0; index <= payload.size() && field_count < fields.size(); ++index) {
        if (index == payload.size() || payload[index] == '|') {
            fields[field_count++] = payload.substr(field_start, index - field_start);
            field_start = index + 1;
        }
    }
    if (field_count < 4) {
        return std::nullopt;
    }
    DownloadProgress progress;
    if (const auto value = parse_int_field(fields[0])) {
        progress.downloaded_bytes = *value;
    }
    if (const auto value = parse_int_field(fields[1])) {
        progress.total_bytes = *value;
    } else if (const auto value = parse_int_field(fields[2])) {
        progress.total_bytes = *value;
    }
    if (const auto value = parse_double_field(fields[3])) {
        progress.speed_bps = *value;
    }
    if (progress.downloaded_bytes < 0 && progress.total_bytes < 0 && progress.speed_bps < 0) {
        return std::nullopt;
    }
    return progress;
}

bool try_subtitle_pass(
    const std::filesystem::path& executable_path,
    const YtDlp::Runner& runner,
    std::string_view url,
    std::string_view language,
    const std::filesystem::path& working_directory,
    bool automatic,
    const std::atomic<bool>& cancel) {
    constexpr std::string_view stem = "subs";
    remove_matching_files(working_directory, stem);

    ProcessOptions process_options;
    process_options.executable = executable_path;
    process_options.cancel = &cancel;
    process_options.timeout_ms = subtitle_timeout_ms;
    process_options.arguments = {
        "--no-playlist",
        "--skip-download",
        "--encoding", "utf-8",
        "--sub-langs", std::string{language},
        "--convert-subs", "srt",
        "-o", std::string{stem} + ".%(ext)s",
        "-P", path_to_utf8(working_directory),
    };
    if (automatic) {
        process_options.arguments.emplace_back("--write-auto-subs");
        process_options.arguments.emplace_back("--no-write-subs");
    } else {
        process_options.arguments.emplace_back("--write-subs");
        process_options.arguments.emplace_back("--no-write-auto-subs");
    }
    process_options.arguments.emplace_back(std::string{url});

    const ProcessResult result = runner(process_options);
    if (result.canceled || !result.launched) {
        return false;
    }
    return find_first_matching_file(working_directory, stem, ".srt").has_value();
}

}

YtDlp::YtDlp(std::filesystem::path executable, Runner runner)
    : executable_(std::move(executable)),
      runner_(std::move(runner)) {}

std::filesystem::path YtDlp::resolve_executable() {
    return resolve_tool(L"yt-dlp.exe");
}

std::optional<std::string> YtDlp::fetch_title(
    std::string_view url, const std::atomic<bool>& cancel) const {
    if (!runner_) {
        return std::nullopt;
    }

    ProcessOptions process_options;
    process_options.executable = executable_;
    process_options.cancel = &cancel;
    process_options.timeout_ms = title_timeout_ms;
    process_options.arguments = {
        "--no-playlist",
        "--skip-download",
        "--encoding", "utf-8",
        "--print", "%(title)s",
        std::string{url},
    };

    const ProcessResult result = runner_(process_options);
    if (!result.launched || result.canceled || result.exit_code != 0) {
        return std::nullopt;
    }
    std::string title{trim_ascii_whitespace(result.stdout_data)};
    const std::string::size_type newline_pos = title.find('\n');
    if (newline_pos != std::string::npos) {
        title.resize(newline_pos);
        title = std::string{trim_ascii_whitespace(title)};
    }
    if (title.empty()) {
        return std::nullopt;
    }
    return title;
}

std::optional<std::filesystem::path> YtDlp::download_video(
    std::string_view url,
    const std::filesystem::path& working_directory,
    const std::atomic<bool>& cancel,
    const DownloadProgressCallback& on_progress) const {
    if (!runner_) {
        return std::nullopt;
    }

    constexpr std::string_view stem = "video";
    constexpr std::string_view fragment_stem = "video.f";
    constexpr std::string_view progress_prefix = "swb-progress:";
    const std::string progress_template =
        std::string{"download:"} + std::string{progress_prefix} +
        "%(progress.downloaded_bytes)s|%(progress.total_bytes)s|"
        "%(progress.total_bytes_estimate)s|%(progress.speed)s";

    remove_matching_files(working_directory, fragment_stem);

    ProcessOptions process_options;
    process_options.executable = executable_;
    process_options.cancel = &cancel;
    process_options.timeout_ms = video_timeout_ms;
    process_options.arguments = {
        "--no-playlist",
        "--no-part",
        "--newline",
        "--progress",
        "--encoding", "utf-8",
        "--progress-template", progress_template,
        "-f", "bv*+ba/b",
        "--merge-output-format", "mp4",
        "-o", std::string{stem} + ".%(ext)s",
        "-P", path_to_utf8(working_directory),
        std::string{url},
    };

    if (on_progress) {
        std::string output_buffer;
        process_options.on_stdout = [&](std::string_view chunk) {
            output_buffer.append(chunk);
            for (;;) {
                const std::string::size_type newline_pos = output_buffer.find_first_of("\r\n");
                if (newline_pos == std::string::npos) {
                    break;
                }
                const std::string_view line{output_buffer.data(), newline_pos};
                const std::string_view::size_type progress_position = line.find(progress_prefix);
                if (progress_position != std::string_view::npos) {
                    if (const std::optional<DownloadProgress> progress = parse_progress(line.substr(progress_position + progress_prefix.size())); progress.has_value()) {
                        on_progress(*progress);
                    }
                }
                output_buffer.erase(0, newline_pos + 1);
            }
        };
    }

    const ProcessResult result = runner_(process_options);
    if (!result.launched || result.canceled || result.exit_code != 0) {
        return std::nullopt;
    }
    return find_first_matching_file(working_directory, stem, ".mp4");
}

std::optional<SubtitleDownload> YtDlp::download_subtitle(
    std::string_view url,
    std::string_view language,
    const std::filesystem::path& working_directory,
    const std::atomic<bool>& cancel,
    SubtitleOrigin origin) const {
    if (cancel.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    if (!runner_) {
        return std::nullopt;
    }
    const bool automatic = origin == SubtitleOrigin::automatic;
    if (!try_subtitle_pass(executable_, runner_, url, language, working_directory, automatic, cancel)) {
        return std::nullopt;
    }
    const std::optional<std::filesystem::path> source_subtitle = find_first_matching_file(working_directory, "subs", ".srt");
    if (!source_subtitle) {
        return std::nullopt;
    }
    const std::filesystem::path destination = subtitle_destination_path(working_directory, origin);
    std::error_code error_code;
    std::filesystem::remove(destination, error_code);
    std::filesystem::rename(*source_subtitle, destination, error_code);
    if (error_code) {
        std::filesystem::copy_file(*source_subtitle, destination, std::filesystem::copy_options::overwrite_existing, error_code);
        if (error_code) {
            return std::nullopt;
        }
    }
    return SubtitleDownload{
        .srt_path = destination,
        .origin = origin,
    };
}

}
