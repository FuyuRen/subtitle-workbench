#pragma once

#include "swb/operation_status.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swb {

struct SubtitleCue {
    double start_seconds{0.0};
    double end_seconds{0.0};
    std::string text;
};

struct SrtReadResult : OperationStatus {
    std::vector<SubtitleCue> cues;

    SrtReadResult() = default;

    SrtReadResult(bool operation_succeeded, std::string status_message, std::vector<SubtitleCue> parsed_cues = {})
        : OperationStatus(operation_succeeded, std::move(status_message)),
          cues(std::move(parsed_cues)) {}
};

[[nodiscard]] std::string format_srt_timestamp(double seconds);

[[nodiscard]] std::string serialize_srt(std::span<const SubtitleCue> cues);

void normalize_sequential_cue_timings(std::span<SubtitleCue> cues);

[[nodiscard]] SrtReadResult parse_srt(std::string_view content);

[[nodiscard]] SrtReadResult load_srt(const std::filesystem::path& path);

[[nodiscard]] bool save_srt(std::span<const SubtitleCue> cues, const std::filesystem::path& path);

}