#pragma once

#include "swb/config.h"
#include "swb/http.h"
#include "swb/operation_status.h"
#include "swb/srt.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace swb {

struct TranslationOutcome : HttpOperationStatus {
    std::filesystem::path subtitle_path;

    TranslationOutcome() = default;

    TranslationOutcome(
        bool operation_succeeded,
        std::string status_message,
        int http_status_code = 0,
        std::filesystem::path subtitle_file_path = {})
        : HttpOperationStatus(operation_succeeded, std::move(status_message), http_status_code),
          subtitle_path(std::move(subtitle_file_path)) {}
};

[[nodiscard]] std::string format_translation_progress(std::size_t chunk_position, std::size_t chunk_count);

class SubtitleTranslator {
public:
    using Sender = std::function<http::Response(const http::Request&)>;
    using ProgressCallback = std::function<void(std::size_t, std::size_t)>;

    explicit SubtitleTranslator(Sender sender = http::send);

    [[nodiscard]] TranslationOutcome translate(
        const Config& configuration,
        std::span<const SubtitleCue> source_cues,
        const std::filesystem::path& working_directory,
        std::string_view source_language,
        std::string_view target_language,
        const std::atomic<bool>& cancel,
        ProgressCallback on_progress = {}) const;

private:
    Sender sender_;
};

}