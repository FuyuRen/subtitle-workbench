#pragma once

#include "swb/config.h"
#include "swb/http.h"
#include "swb/operation_status.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
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

struct TranscriptToken {
    std::string text;
    double start_seconds{0.0};
    double end_seconds{0.0};
    bool control{false};
};

struct TokenAggregationResult {
    std::vector<TranscriptWord> words;
    std::vector<std::string> diagnostics;
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

class WhisperApiTranscriber {
public:
    using Sender = std::function<http::Response(const http::Request&)>;
    using ProgressCallback = std::function<void(TranscriptionStage)>;

    explicit WhisperApiTranscriber(Sender sender = http::send);

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

enum class ActualTranscriptionBackend : int {
    not_initialized,
    api,
    gpu,
    cpu,
};

struct TranscriptionRuntime {
    ActualTranscriptionBackend backend{ActualTranscriptionBackend::not_initialized};
    std::string backend_name;
    std::string fallback_reason;
};

struct TranscriptionResult : OperationStatus {
    std::vector<TranscriptWord> words;
    TranscriptionRuntime runtime;
    std::vector<std::string> diagnostics;
};

using TranscriptionProgressCallback = std::function<void(std::string_view, double)>;

struct LocalAsrGpuDevice {
    int index{0};
    std::string name;
};

[[nodiscard]] std::vector<LocalAsrGpuDevice> enumerate_local_asr_gpu_devices();

struct WhisperCppContextAttempt {
    std::shared_ptr<void> context;
    std::string backend_name;
    std::string error;
    bool gpu{false};
};

using WhisperCppContextFactory = std::function<WhisperCppContextAttempt(
    const std::filesystem::path&,
    bool,
    int)>;

struct WhisperCppContextSelection {
    std::shared_ptr<void> context;
    TranscriptionRuntime runtime;
    std::string error;
    int creation_attempts{0};
};

[[nodiscard]] WhisperCppContextSelection select_whisper_cpp_context(
    const std::filesystem::path& model_path,
    LocalAsrCompute compute,
    int gpu_device,
    const WhisperCppContextFactory& factory);

class WhisperCppTranscriber {
public:
    explicit WhisperCppTranscriber(
        const Config& configuration,
        WhisperCppContextFactory context_factory = {});
    ~WhisperCppTranscriber();

    WhisperCppTranscriber(const WhisperCppTranscriber&) = delete;
    WhisperCppTranscriber& operator=(const WhisperCppTranscriber&) = delete;
    WhisperCppTranscriber(WhisperCppTranscriber&&) noexcept;
    WhisperCppTranscriber& operator=(WhisperCppTranscriber&&) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::string_view error() const noexcept;
    [[nodiscard]] const TranscriptionRuntime& runtime() const noexcept;
    [[nodiscard]] int model_load_attempts() const noexcept;

    [[nodiscard]] TranscriptionResult transcribe_wav(
        const Config& configuration,
        const std::filesystem::path& audio_path,
        std::string_view source_language,
        const std::atomic<bool>& cancel,
        TranscriptionProgressCallback on_progress = {});

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] TranscriptionResult transcribe_wav_with_api(
    const WhisperApiTranscriber& transcriber,
    const Config& configuration,
    const std::filesystem::path& audio_path,
    const std::filesystem::path& working_directory,
    std::string_view source_language,
    const std::atomic<bool>& cancel,
    TranscriptionProgressCallback on_progress = {});

[[nodiscard]] TokenAggregationResult aggregate_transcript_tokens(
    std::span<const TranscriptToken> tokens,
    std::string_view language,
    double audio_duration_seconds);

[[nodiscard]] std::vector<TranscriptWord> merge_overlapping_transcript_words(
    std::span<const TranscriptWord> existing,
    std::span<const TranscriptWord> incoming,
    double incoming_window_start_seconds);

[[nodiscard]] TranscriptReadResult parse_transcript_words(std::string_view json_text);

[[nodiscard]] TranscriptReadResult load_transcript_words(const std::filesystem::path& path);

[[nodiscard]] bool save_transcript_words(std::span<const TranscriptWord> words, const std::filesystem::path& path);

[[nodiscard]] bool save_transcript_words_atomic(
    std::span<const TranscriptWord> words,
    const std::filesystem::path& path);

}
