#include "swb/source_subtitle_resolution.h"

#include "swb/workspace.h"

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <system_error>

namespace swb {

namespace {

constexpr std::string_view audio_file_name = "audio.wav";
constexpr std::string_view transcript_file_name = "transcript.json";
constexpr std::string_view source_subtitle_file_name = "en.srt";
constexpr std::string_view manual_subtitle_file_name = "manual.en.srt";
constexpr std::string_view automatic_subtitle_file_name = "auto.en.srt";
constexpr std::string_view translated_subtitle_file_name = "zh.srt";

[[nodiscard]] std::optional<std::filesystem::path> existing_path(const std::filesystem::path& path) {
    std::error_code error_code;
    if (std::filesystem::exists(path, error_code) && !error_code) {
        return path;
    }
    return std::nullopt;
}

[[nodiscard]] bool has_video_extension(std::string_view extension) {
    constexpr std::array video_extensions{
        std::string_view{".mp4"},
        std::string_view{".mkv"},
        std::string_view{".mov"},
        std::string_view{".avi"},
        std::string_view{".webm"},
    };
    for (const std::string_view candidate : video_extensions) {
        if (extension.size() != candidate.size()) {
            continue;
        }
        bool matches = true;
        for (std::size_t index = 0; index < extension.size(); ++index) {
            if (std::tolower(static_cast<unsigned char>(extension[index])) != std::tolower(static_cast<unsigned char>(candidate[index]))) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_yt_dlp_fragment_file(const std::filesystem::path& path) {
    const std::string file_name = path_to_utf8(path.filename());
    return file_name.starts_with("video.f");
}

[[nodiscard]] std::optional<std::filesystem::path> find_video_path(const std::filesystem::path& working_directory) {
    if (const std::optional<std::filesystem::path> preferred_path = existing_path(working_directory / "video.mp4"); preferred_path.has_value()) {
        return preferred_path;
    }

    std::error_code error_code;
    for (const auto& entry : std::filesystem::directory_iterator(working_directory, error_code)) {
        if (error_code || !entry.is_regular_file(error_code)) {
            continue;
        }
        if (is_yt_dlp_fragment_file(entry.path())) {
            continue;
        }
        if (has_video_extension(entry.path().extension().string())) {
            return entry.path();
        }
    }
    return std::nullopt;
}

}

SourceSubtitleSelection select_source_subtitle(
    bool has_platform_manual_subtitle,
    bool has_api_transcription,
    bool has_platform_automatic_subtitle) noexcept {
    if (has_platform_manual_subtitle) {
        return SourceSubtitleSelection::platform_manual_subtitle;
    }
    if (has_api_transcription) {
        return SourceSubtitleSelection::api_transcription;
    }
    if (has_platform_automatic_subtitle) {
        return SourceSubtitleSelection::platform_automatic_subtitle;
    }
    return SourceSubtitleSelection::unavailable;
}

WorkingDirectoryState inspect_working_directory_state(const std::filesystem::path& working_directory) {
    WorkingDirectoryState working_directory_state;
    working_directory_state.video_path = find_video_path(working_directory);
    working_directory_state.audio_path = existing_path(working_directory / std::string{audio_file_name});
    working_directory_state.transcript_path = existing_path(working_directory / std::string{transcript_file_name});
    working_directory_state.source_subtitle_path = existing_path(working_directory / std::string{source_subtitle_file_name});
    working_directory_state.manual_subtitle_path = existing_path(working_directory / std::string{manual_subtitle_file_name});
    working_directory_state.automatic_subtitle_path = existing_path(working_directory / std::string{automatic_subtitle_file_name});
    working_directory_state.translated_subtitle_path = existing_path(working_directory / std::string{translated_subtitle_file_name});
    return working_directory_state;
}

SourceSubtitleResumeStage classify_source_subtitle_resume_stage(const WorkingDirectoryState& working_directory_state) noexcept {
    const bool has_final_source_subtitle = working_directory_state.source_subtitle_path.has_value();
    const bool has_manual_subtitle = working_directory_state.manual_subtitle_path.has_value();
    const bool has_transcript = working_directory_state.transcript_path.has_value();
    const bool has_audio = working_directory_state.audio_path.has_value();
    const bool has_video = working_directory_state.video_path.has_value();
    const bool has_automatic_subtitle = working_directory_state.automatic_subtitle_path.has_value();

    if (has_manual_subtitle || (has_transcript && has_final_source_subtitle) || (has_final_source_subtitle && !has_automatic_subtitle)) {
        return SourceSubtitleResumeStage::ready_source_subtitle;
    }
    if (has_transcript) {
        return SourceSubtitleResumeStage::needs_segmentation;
    }
    if (has_audio) {
        return SourceSubtitleResumeStage::needs_transcription;
    }
    if (has_video) {
        return SourceSubtitleResumeStage::needs_audio_extraction;
    }
    if (has_final_source_subtitle || has_automatic_subtitle) {
        return SourceSubtitleResumeStage::ready_source_subtitle;
    }
    return SourceSubtitleResumeStage::unavailable;
}

}