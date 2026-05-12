#pragma once

#include "swb/config.h"
#include "swb/operation_status.h"
#include "swb/process.h"
#include "swb/srt.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swb {

struct OutputResult : OperationStatus {
    std::filesystem::path output_path;

    OutputResult() = default;

    OutputResult(bool operation_succeeded, std::string status_message, std::filesystem::path rendered_output_path = {})
        : OperationStatus(operation_succeeded, std::move(status_message)),
          output_path(std::move(rendered_output_path)) {}
};

struct OutputProgress {
    std::string status;
    double fraction{0.0};
};

[[nodiscard]] std::optional<std::string> serialize_hard_subtitle_script(
    std::span<const SubtitleCue> translated_cues,
    std::span<const SubtitleCue> source_cues,
    const Config& configuration);

class SubtitleOutputRenderer {
public:
    using Runner = std::function<ProcessResult(const ProcessOptions&)>;
    using ProgressCallback = std::function<void(const OutputProgress&)>;

    explicit SubtitleOutputRenderer(std::filesystem::path ffmpeg_executable, Runner runner = run_process);

    [[nodiscard]] OutputResult render(
        const Config& configuration,
        const std::filesystem::path& video_path,
        std::span<const SubtitleCue> source_cues,
        std::span<const SubtitleCue> translated_cues,
        const std::filesystem::path& working_directory,
        const std::atomic<bool>& cancel,
        ProgressCallback on_progress = {}) const;

private:
    std::filesystem::path ffmpeg_executable_;
    Runner runner_;
};

}