#include "swb/transcribe.h"

#include "swb/file_io.h"
#include "swb/json.h"
#include "swb/text.h"
#include "swb/workspace.h"

#include <charconv>
#include <cstdint>
#include <sstream>
#include <system_error>
#include <utility>

namespace swb {

namespace {

constexpr std::string_view multipart_boundary = "----swb-whisper-boundary-6f2d5e63a7b4";
constexpr std::string_view default_model = "whisper-1";
constexpr std::string_view transcript_filename = "transcript.json";
constexpr int transcription_timeout_ms = 600'000;
constexpr std::size_t error_excerpt_limit = 160;

[[nodiscard]] bool is_success_status(int status) noexcept {
    return status >= 200 && status < 300;
}

[[nodiscard]] std::string collapse_ascii_whitespace(std::string_view text) {
    std::string collapsed;
    collapsed.reserve(text.size());

    bool previous_was_whitespace = false;
    for (const char character : text) {
        const bool is_whitespace = character == ' ' || character == '\t' || character == '\r' || character == '\n';
        if (is_whitespace) {
            if (!collapsed.empty() && !previous_was_whitespace) {
                collapsed.push_back(' ');
            }
            previous_was_whitespace = true;
            continue;
        }
        collapsed.push_back(character);
        previous_was_whitespace = false;
    }

    return std::string{trim_ascii_whitespace(collapsed)};
}

[[nodiscard]] std::optional<std::string> extract_json_string_field(std::string_view json_text, std::string_view field_name) {
    if (const std::optional<std::string> value = json::find_string_field(json_text, field_name); value.has_value()) {
        return collapse_ascii_whitespace(*value);
    }
    return std::nullopt;
}

[[nodiscard]] std::string summarize_error_body(std::string_view body) {
    if (const std::optional<std::string> message = extract_json_string_field(body, "message");
        message.has_value() && !message->empty()) {
        return *message;
    }
    if (const std::optional<std::string> error = extract_json_string_field(body, "error");
        error.has_value() && !error->empty()) {
        return *error;
    }

    std::string summary = collapse_ascii_whitespace(body);
    if (summary.size() > error_excerpt_limit) {
        summary.resize(error_excerpt_limit);
        summary.append("...");
    }
    return summary;
}

[[nodiscard]] std::string format_api_failure_message(std::string_view prefix, int status, std::string_view body) {
    std::string message = std::string{prefix} + std::to_string(status);
    const std::string summary = summarize_error_body(body);
    if (!summary.empty()) {
        message.append("：");
        message.append(summary);
    }
    return message;
}

void report_progress(const WhisperTranscriber::ProgressCallback& on_progress, TranscriptionStage stage) {
    if (on_progress) {
        on_progress(stage);
    }
}

[[nodiscard]] std::string normalize_endpoint(std::string_view base_url) {
    std::string endpoint{trim_ascii_whitespace(base_url)};
    while (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }
    if (endpoint.empty()) {
        return {};
    }
    if (endpoint.ends_with("/audio/transcriptions")) {
        return endpoint;
    }
    if (endpoint.ends_with("/v1")) {
        endpoint.append("/audio/transcriptions");
        return endpoint;
    }
    endpoint.append("/v1/audio/transcriptions");
    return endpoint;
}

[[nodiscard]] std::string configured_model_name(std::string_view configured_model) {
    const std::string_view trimmed_model = trim_ascii_whitespace(configured_model);
    if (trimmed_model.empty()) {
        return std::string{default_model};
    }
    return std::string{trimmed_model};
}

void append_field(std::string& body, std::string_view name, std::string_view value) {
    body.append("--");
    body.append(multipart_boundary);
    body.append("\r\nContent-Disposition: form-data; name=\"");
    body.append(name);
    body.append("\"\r\n\r\n");
    body.append(value);
    body.append("\r\n");
}

void append_file(std::string& body, std::string_view filename, std::string_view bytes) {
    body.append("--");
    body.append(multipart_boundary);
    body.append("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"");
    body.append(filename);
    body.append("\"\r\nContent-Type: audio/wav\r\n\r\n");
    body.append(bytes);
    body.append("\r\n");
}

[[nodiscard]] std::string make_multipart_body(
    std::string_view model,
    std::string_view filename,
    std::string_view source_language,
    std::string_view audio_bytes) {
    std::string body;
    body.reserve(audio_bytes.size() + 512);
    append_field(body, "model", model);
    append_field(body, "response_format", "verbose_json");
    append_field(body, "timestamp_granularities[]", "word");
    if (!source_language.empty()) {
        append_field(body, "language", source_language);
    }
    append_file(body, filename, audio_bytes);
    body.append("--");
    body.append(multipart_boundary);
    body.append("--\r\n");
    return body;
}

[[nodiscard]] std::vector<std::string> make_headers(std::string_view api_key) {
    std::vector<std::string> headers;
    headers.emplace_back("Content-Type: multipart/form-data; boundary=" + std::string{multipart_boundary});
    const std::string_view trimmed_api_key = trim_ascii_whitespace(api_key);
    if (!trimmed_api_key.empty()) {
        headers.emplace_back("Authorization: Bearer " + std::string{trimmed_api_key});
    }
    return headers;
}

using JsonCursor = json::Cursor;

[[nodiscard]] bool parse_words_array(JsonCursor& cursor, std::vector<TranscriptWord>& words) {
    if (!cursor.consume('[')) {
        return false;
    }
    cursor.skip_whitespace();
    if (cursor.consume(']')) {
        return true;
    }

    for (;;) {
        TranscriptWord word;
        bool has_word = false;
        bool has_start = false;
        bool has_end = false;

        if (!cursor.consume('{')) {
            return false;
        }
        for (;;) {
            if (cursor.consume('}')) {
                break;
            }
            const std::optional<std::string> key = cursor.parse_string();
            if (!key.has_value() || !cursor.consume(':')) {
                return false;
            }
            if (*key == "word") {
                const std::optional<std::string> value = cursor.parse_string();
                if (!value.has_value()) {
                    return false;
                }
                word.word = *value;
                has_word = true;
            } else if (*key == "start") {
                const std::optional<double> value = cursor.parse_number();
                if (!value.has_value()) {
                    return false;
                }
                word.start_seconds = *value;
                has_start = true;
            } else if (*key == "end") {
                const std::optional<double> value = cursor.parse_number();
                if (!value.has_value()) {
                    return false;
                }
                word.end_seconds = *value;
                has_end = true;
            } else if (!cursor.skip_value()) {
                return false;
            }

            if (cursor.consume('}')) {
                break;
            }
            if (!cursor.consume(',')) {
                return false;
            }
        }

        if (has_word && has_start && has_end && !word.word.empty() && word.end_seconds >= word.start_seconds) {
            words.push_back(std::move(word));
        }

        if (cursor.consume(']')) {
            return true;
        }
        if (!cursor.consume(',')) {
            return false;
        }
    }
}

[[nodiscard]] std::string escape_json_string(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (const unsigned char character : text) {
        switch (character) {
        case '\\':
            escaped.append("\\\\");
            break;
        case '"':
            escaped.append("\\\"");
            break;
        case '\n':
            escaped.append("\\n");
            break;
        case '\r':
            escaped.append("\\r");
            break;
        case '\t':
            escaped.append("\\t");
            break;
        default:
            escaped.push_back(static_cast<char>(character));
            break;
        }
    }
    return escaped;
}

}

WhisperTranscriber::WhisperTranscriber(Sender sender) : sender_(std::move(sender)) {}

std::string format_transcription_progress(TranscriptionStage stage) {
    switch (stage) {
    case TranscriptionStage::reading_audio:
        return "API转写（读取音频）";
    case TranscriptionStage::preparing_request:
        return "API转写（准备请求）";
    case TranscriptionStage::requesting:
        return "API转写（请求中）";
    case TranscriptionStage::writing_result:
        return "API转写（写入结果）";
    }
    return "API转写";
}

std::string format_transcription_batch_progress(std::size_t chunk_position, std::size_t chunk_count) {
    return "API转写（" + std::to_string(chunk_position) + "/" + std::to_string(chunk_count) + "）";
}

TranscriptionOutcome WhisperTranscriber::transcribe(
    const Config& configuration,
    const std::filesystem::path& audio_path,
    const std::filesystem::path& working_directory,
    std::string_view source_language,
    const std::atomic<bool>& cancel,
    ProgressCallback on_progress) const {
    if (cancel.load(std::memory_order_acquire)) {
        return {false, "已取消"};
    }
    if (!sender_) {
        return {false, "Whisper发送器不可用"};
    }

    const std::string endpoint = normalize_endpoint(configuration.whisper_base_url);
    if (endpoint.empty()) {
        return {false, "未配置Whisper Base URL"};
    }

    std::error_code error_code;
    if (!std::filesystem::exists(audio_path, error_code) || error_code) {
        return {false, "音频文件不存在"};
    }

    report_progress(on_progress, TranscriptionStage::reading_audio);
    const std::optional<std::string> audio_bytes = read_binary_file(audio_path);
    if (!audio_bytes.has_value()) {
        return {false, "无法读取音频文件"};
    }

    report_progress(on_progress, TranscriptionStage::preparing_request);
    http::Request request;
    request.url = endpoint;
    request.headers = make_headers(configuration.whisper_api_key);
    request.body = make_multipart_body(
        configured_model_name(configuration.whisper_model),
        path_to_utf8(audio_path.filename()),
        source_language,
        *audio_bytes);
    request.timeout_ms = transcription_timeout_ms;
    request.cancel = &cancel;

    http::Response response;
    try {
        report_progress(on_progress, TranscriptionStage::requesting);
        response = sender_(request);
    } catch (const std::exception& exception) {
        return {false, std::string{"Whisper请求失败: "} + exception.what()};
    }

    if (cancel.load(std::memory_order_acquire)) {
        return {false, "已取消"};
    }
    if (!is_success_status(response.status)) {
        return {false, format_api_failure_message("Whisper API返回", response.status, response.body), response.status};
    }
    if (response.body.empty()) {
        return {false, "Whisper API响应为空", response.status};
    }

    report_progress(on_progress, TranscriptionStage::writing_result);
    std::filesystem::create_directories(working_directory, error_code);
    if (error_code) {
        return {false, "无法创建转写输出目录", response.status};
    }

    const std::filesystem::path transcript_path = working_directory / std::filesystem::u8path(std::string{transcript_filename});
    if (!write_text_file(transcript_path, response.body)) {
        return {false, "无法写入transcript.json", response.status};
    }

    return {true, "API转写", response.status, transcript_path};
}

TranscriptReadResult parse_transcript_words(std::string_view json_text) {
    JsonCursor cursor{json_text};
    TranscriptReadResult result;
    if (!cursor.consume('{')) {
        result.message = "transcript.json格式无效";
        return result;
    }

    for (;;) {
        if (cursor.consume('}')) {
            break;
        }
        const std::optional<std::string> key = cursor.parse_string();
        if (!key.has_value() || !cursor.consume(':')) {
            result.message = "transcript.json格式无效";
            return result;
        }
        if (*key == "words") {
            if (!parse_words_array(cursor, result.words)) {
                result.message = "transcript.json格式无效";
                return result;
            }
        } else if (!cursor.skip_value()) {
            result.message = "transcript.json格式无效";
            return result;
        }

        if (cursor.consume('}')) {
            break;
        }
        if (!cursor.consume(',')) {
            result.message = "transcript.json格式无效";
            return result;
        }
    }

    if (result.words.empty()) {
        result.message = "transcript.json缺少词时间戳";
        return result;
    }
    result.success = true;
    return result;
}

TranscriptReadResult load_transcript_words(const std::filesystem::path& path) {
    const std::optional<std::string> content = read_text_file(path);
    if (!content.has_value()) {
        return {false, "无法读取transcript.json"};
    }
    return parse_transcript_words(*content);
}

bool save_transcript_words(std::span<const TranscriptWord> words, const std::filesystem::path& path) {
    std::error_code error_code;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error_code);
        if (error_code) {
            return false;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    output << "{\"task\":\"transcribe\",\"words\":[";
    for (std::size_t index = 0; index < words.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        output << "{\"word\":\"" << escape_json_string(words[index].word)
               << "\",\"start\":" << words[index].start_seconds
               << ",\"end\":" << words[index].end_seconds << '}';
    }
    output << "]}";
    return static_cast<bool>(output);
}

}