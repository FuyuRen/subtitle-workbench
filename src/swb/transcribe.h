#pragma once

#include "swb/config.h"
#include "swb/http.h"
#include "swb/operation_status.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swb {

struct TranscriptWord {
    std::string word;
    double start_seconds{0.0};
    double end_seconds{0.0};
};

struct TranscriptReadResult : OperationStatus {
    std::vector<TranscriptWord> words;

    TranscriptReadResult() = default;

    TranscriptReadResult(bool operation_succeeded, std::string status_message, std::vector<TranscriptWord> transcript_words = {})
        : OperationStatus(operation_succeeded, std::move(status_message)),
          words(std::move(transcript_words)) {}
};

struct TranscriptionOutcome : HttpOperationStatus {
    std::filesystem::path transcript_path;

    TranscriptionOutcome() = default;

    TranscriptionOutcome(
        bool operation_succeeded,
        std::string status_message,
        int http_status_code = 0,
        std::filesystem::path transcript_file_path = {})
        : HttpOperationStatus(operation_succeeded, std::move(status_message), http_status_code),
          transcript_path(std::move(transcript_file_path)) {}
};

enum class TranscriptionStage {
    reading_audio,
    preparing_request,
    requesting,
    writing_result,
};

[[nodiscard]] std::string format_transcription_progress(TranscriptionStage stage);
[[nodiscard]] std::string format_transcription_batch_progress(std::size_t chunk_position, std::size_t chunk_count);

class WhisperTranscriber {
public:
    using Sender = std::function<http::Response(const http::Request&)>;
    using ProgressCallback = std::function<void(TranscriptionStage)>;

    explicit WhisperTranscriber(Sender sender = http::send);

    [[nodiscard]] TranscriptionOutcome transcribe(
        const Config& configuration,
        const std::filesystem::path& audio_path,
        const std::filesystem::path& working_directory,
        std::string_view source_language,
        const std::atomic<bool>& cancel,
        ProgressCallback on_progress = {}) const;

private:
    Sender sender_;
};

[[nodiscard]] TranscriptReadResult parse_transcript_words(std::string_view json_text);

[[nodiscard]] TranscriptReadResult load_transcript_words(const std::filesystem::path& path);

[[nodiscard]] bool save_transcript_words(std::span<const TranscriptWord> words, const std::filesystem::path& path);

}