#pragma once

#include <filesystem>
#include <optional>

namespace swb {

enum class SourceSubtitleSelection : int {
    unavailable,
    platform_manual_subtitle,
    transcription,
    platform_automatic_subtitle,
};

enum class SourceSubtitleResumeStage : int {
    ready_source_subtitle,
    needs_segmentation,
    needs_transcription,
    needs_audio_extraction,
    unavailable,
};

struct WorkingDirectoryState {
    std::optional<std::filesystem::path> video_path;
    std::optional<std::filesystem::path> audio_path;
    std::optional<std::filesystem::path> transcript_path;
    std::optional<std::filesystem::path> source_subtitle_path;
    std::optional<std::filesystem::path> manual_subtitle_path;
    std::optional<std::filesystem::path> automatic_subtitle_path;
    std::optional<std::filesystem::path> translated_subtitle_path;
};

[[nodiscard]] SourceSubtitleSelection select_source_subtitle(
    bool has_platform_manual_subtitle,
    bool has_transcription,
    bool has_platform_automatic_subtitle) noexcept;

[[nodiscard]] WorkingDirectoryState inspect_working_directory_state(const std::filesystem::path& working_directory);

[[nodiscard]] SourceSubtitleResumeStage classify_source_subtitle_resume_stage(
    const WorkingDirectoryState& working_directory_state) noexcept;

}
