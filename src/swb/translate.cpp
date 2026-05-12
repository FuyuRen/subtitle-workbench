#include "swb/translate.h"

#include "swb/json.h"
#include "swb/text.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace swb {

namespace {

constexpr std::string_view translated_subtitle_filename = "zh.srt";
constexpr std::string_view openai_endpoint_suffix = "/chat/completions";
constexpr std::string_view anthropic_endpoint_suffix = "/messages";
constexpr std::string_view default_openai_endpoint = "/v1/chat/completions";
constexpr std::string_view default_anthropic_endpoint = "/v1/messages";
constexpr std::string_view anthropic_version = "2023-06-01";
constexpr int translation_timeout_ms = 300'000;
constexpr int translation_chunk_retry_attempts = 1;
constexpr std::size_t max_chunk_cues = 24;
constexpr std::size_t max_chunk_characters = 1'800;

enum class ApiFlavor : int {
    openai_chat,
    anthropic_messages,
};

struct EndpointConfig {
    std::string url;
    ApiFlavor flavor{ApiFlavor::openai_chat};
};

struct ChunkRange {
    std::size_t begin_index{0};
    std::size_t end_index{0};
};

struct TranslationEntry {
    std::size_t id{0};
    std::string text;
};

struct ChunkTranslationResult {
    bool success{false};
    std::vector<TranslationEntry> entries;
    std::string message;
    int http_status{0};
};

[[nodiscard]] std::size_t displayed_chunk_index(std::size_t chunk_position, std::size_t) noexcept {
    return chunk_position;
}

[[nodiscard]] bool is_success_status(int status) noexcept {
    return status >= 200 && status < 300;
}

[[nodiscard]] bool is_chunk_split_candidate(std::string_view message) noexcept {
    return message == "LLM API响应为空"
        || message == "LLM响应格式无效"
        || message == "LLM返回条目数量不匹配"
        || message == "LLM返回条目标识不匹配";
}

[[nodiscard]] std::string lower_ascii(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char character : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return lowered;
}

[[nodiscard]] EndpointConfig normalize_endpoint(std::string_view base_url) {
    std::string endpoint{trim_ascii_whitespace(base_url)};
    while (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }
    if (endpoint.empty()) {
        return {};
    }

    const std::string lowered = lower_ascii(endpoint);
    if (lowered.ends_with(std::string{openai_endpoint_suffix})) {
        return {
            .url = std::move(endpoint),
            .flavor = ApiFlavor::openai_chat,
        };
    }
    if (lowered.ends_with(std::string{anthropic_endpoint_suffix})) {
        return {
            .url = std::move(endpoint),
            .flavor = ApiFlavor::anthropic_messages,
        };
    }

    const bool is_anthropic = lowered.find("anthropic") != std::string::npos;
    if (lowered.ends_with("/v1")) {
        endpoint.append(is_anthropic ? std::string{anthropic_endpoint_suffix} : std::string{openai_endpoint_suffix});
    } else {
        endpoint.append(is_anthropic ? std::string{default_anthropic_endpoint} : std::string{default_openai_endpoint});
    }

    return {
        .url = std::move(endpoint),
        .flavor = is_anthropic ? ApiFlavor::anthropic_messages : ApiFlavor::openai_chat,
    };
}

[[nodiscard]] std::string escape_json_string(std::string_view text) {
    constexpr std::array<char, 16> hex_digits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

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
            if (character < 0x20u) {
                escaped.append("\\u00");
                escaped.push_back(hex_digits[(character >> 4u) & 0x0Fu]);
                escaped.push_back(hex_digits[character & 0x0Fu]);
            } else {
                escaped.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return escaped;
}

[[nodiscard]] std::string build_system_prompt(std::string_view source_language, std::string_view target_language) {
    std::string prompt{"You translate subtitle entries. Return only valid JSON in the form {\"translations\":[{\"id\":1,\"text\":\"...\"}]}. Keep the same item count, the same ids, and the same order. Translate naturally from "};
    prompt.append(source_language.empty() ? "the source language" : std::string{source_language});
    prompt.append(" to ");
    prompt.append(target_language.empty() ? "the target language" : std::string{target_language});
    prompt.append(
        ". Do not merge items, do not omit items, do not add notes, and preserve intentional "
        "line breaks inside each subtitle item. Preserve natural subtitle punctuation rhythm. "
        "Do not mechanically append a Chinese full stop to every subtitle item. Commas, "
        "exclamation marks, question marks, enumeration commas, and colons are fine when "
        "natural, and incomplete lines may end without a full stop. Unless a person name, "
        "screen name, fictional name, or place name has an official or authoritative "
        "translation, keep it in the original form without transliteration and without added "
        "parentheses."
    );
    return prompt;
}

[[nodiscard]] std::string build_user_prompt(std::span<const SubtitleCue> source_cues, const ChunkRange& range) {
    std::string prompt{"Translate the following subtitle items and return strict JSON only.\n{"};
    prompt.append("\"items\":[");
    for (std::size_t index = range.begin_index; index < range.end_index; ++index) {
        if (index > range.begin_index) {
            prompt.push_back(',');
        }
        prompt.append("{\"id\":");
        prompt.append(std::to_string(index + 1));
        prompt.append(",\"text\":\"");
        prompt.append(escape_json_string(source_cues[index].text));
        prompt.append("\"}");
    }
    prompt.append("]}");
    return prompt;
}

[[nodiscard]] std::vector<std::string> build_headers(ApiFlavor flavor, std::string_view api_key) {
    std::vector<std::string> headers;
    headers.emplace_back("Content-Type: application/json");

    const std::string_view trimmed_api_key = trim_ascii_whitespace(api_key);
    if (flavor == ApiFlavor::anthropic_messages) {
        headers.emplace_back("anthropic-version: " + std::string{anthropic_version});
        if (!trimmed_api_key.empty()) {
            headers.emplace_back("x-api-key: " + std::string{trimmed_api_key});
        }
        return headers;
    }

    if (!trimmed_api_key.empty()) {
        headers.emplace_back("Authorization: Bearer " + std::string{trimmed_api_key});
    }
    return headers;
}

[[nodiscard]] std::string build_request_body(
    ApiFlavor flavor,
    std::string_view model,
    std::string_view system_prompt,
    std::string_view user_prompt) {
    std::string body;
    if (flavor == ApiFlavor::anthropic_messages) {
        body.reserve(model.size() + system_prompt.size() + user_prompt.size() + 128);
        body.append("{\"model\":\"");
        body.append(escape_json_string(model));
        body.append("\",\"max_tokens\":4096,\"temperature\":0.2,\"system\":\"");
        body.append(escape_json_string(system_prompt));
        body.append("\",\"messages\":[{\"role\":\"user\",\"content\":\"");
        body.append(escape_json_string(user_prompt));
        body.append("\"}]}");
        return body;
    }

    body.reserve(model.size() + system_prompt.size() + user_prompt.size() + 160);
    body.append("{\"model\":\"");
    body.append(escape_json_string(model));
    body.append("\",\"temperature\":0.2,\"messages\":[{\"role\":\"system\",\"content\":\"");
    body.append(escape_json_string(system_prompt));
    body.append("\"},{\"role\":\"user\",\"content\":\"");
    body.append(escape_json_string(user_prompt));
    body.append("\"}]}");
    return body;
}

[[nodiscard]] std::vector<ChunkRange> build_chunk_ranges(std::span<const SubtitleCue> source_cues) {
    std::vector<ChunkRange> ranges;
    if (source_cues.empty()) {
        return ranges;
    }

    std::size_t begin_index = 0;
    std::size_t character_count = 0;
    for (std::size_t index = 0; index < source_cues.size(); ++index) {
        const std::size_t cue_characters = source_cues[index].text.size();
        if (begin_index < index
            && ((index - begin_index) >= max_chunk_cues || (character_count + cue_characters) > max_chunk_characters)) {
            ranges.push_back({
                .begin_index = begin_index,
                .end_index = index,
            });
            begin_index = index;
            character_count = 0;
        }
        character_count += cue_characters;
    }

    ranges.push_back({
        .begin_index = begin_index,
        .end_index = source_cues.size(),
    });
    return ranges;
}

using JsonCursor = json::Cursor;

[[nodiscard]] bool parse_translation_id(JsonCursor& cursor, std::size_t& identifier) {
    if (cursor.peek() == '"') {
        const std::optional<std::string> value = cursor.parse_string();
        if (!value.has_value()) {
            return false;
        }
        std::size_t parsed_identifier = 0;
        const char* begin = value->data();
        const char* end = value->data() + value->size();
        const auto [parsed_end, error_code] = std::from_chars(begin, end, parsed_identifier);
        if (error_code != std::errc{} || parsed_end != end || parsed_identifier == 0) {
            return false;
        }
        identifier = parsed_identifier;
        return true;
    }

    const std::optional<double> numeric_value = cursor.parse_number();
    if (!numeric_value.has_value()) {
        return false;
    }
    if (*numeric_value < 1.0 || std::floor(*numeric_value) != *numeric_value) {
        return false;
    }
    identifier = static_cast<std::size_t>(*numeric_value);
    return true;
}

[[nodiscard]] bool parse_translation_entry(JsonCursor& cursor, TranslationEntry& entry) {
    if (!cursor.consume('{')) {
        return false;
    }

    bool has_id = false;
    bool has_text = false;
    for (;;) {
        if (cursor.consume('}')) {
            break;
        }
        const std::optional<std::string> key = cursor.parse_string();
        if (!key.has_value() || !cursor.consume(':')) {
            return false;
        }
        if (*key == "id") {
            if (!parse_translation_id(cursor, entry.id)) {
                return false;
            }
            has_id = true;
        } else if (*key == "text") {
            const std::optional<std::string> value = cursor.parse_string();
            if (!value.has_value()) {
                return false;
            }
            entry.text = *value;
            has_text = true;
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

    return has_id && has_text;
}

[[nodiscard]] bool parse_translation_entries_array(JsonCursor& cursor, std::vector<TranslationEntry>& entries) {
    if (!cursor.consume('[')) {
        return false;
    }
    cursor.skip_whitespace();
    if (cursor.consume(']')) {
        return true;
    }

    for (;;) {
        TranslationEntry entry;
        if (!parse_translation_entry(cursor, entry)) {
            return false;
        }
        entries.push_back(std::move(entry));
        if (cursor.consume(']')) {
            return true;
        }
        if (!cursor.consume(',')) {
            return false;
        }
    }
}

[[nodiscard]] std::string_view isolate_json_payload(std::string_view text) {
    text = trim_ascii_whitespace(text);
    if (text.starts_with("```")) {
        const std::size_t first_newline = text.find('\n');
        if (first_newline != std::string_view::npos) {
            text.remove_prefix(first_newline + 1);
            const std::size_t closing_fence = text.rfind("```");
            if (closing_fence != std::string_view::npos) {
                text = text.substr(0, closing_fence);
            }
            text = trim_ascii_whitespace(text);
        }
    }
    if (!text.empty() && (text.front() == '{' || text.front() == '[')) {
        return text;
    }

    const std::size_t object_begin = text.find('{');
    const std::size_t object_end = text.rfind('}');
    if (object_begin != std::string_view::npos && object_end != std::string_view::npos && object_begin <= object_end) {
        return text.substr(object_begin, object_end - object_begin + 1);
    }

    const std::size_t array_begin = text.find('[');
    const std::size_t array_end = text.rfind(']');
    if (array_begin != std::string_view::npos && array_end != std::string_view::npos && array_begin <= array_end) {
        return text.substr(array_begin, array_end - array_begin + 1);
    }

    return text;
}

[[nodiscard]] std::optional<std::vector<TranslationEntry>> parse_translation_payload(std::string_view text) {
    const std::string_view payload = isolate_json_payload(text);
    if (payload.empty()) {
        return std::nullopt;
    }

    JsonCursor cursor{payload};
    std::vector<TranslationEntry> entries;
    const char first_token = cursor.peek();
    if (first_token == '{') {
        if (!cursor.consume('{')) {
            return std::nullopt;
        }
        for (;;) {
            if (cursor.consume('}')) {
                break;
            }
            const std::optional<std::string> key = cursor.parse_string();
            if (!key.has_value() || !cursor.consume(':')) {
                return std::nullopt;
            }
            if (*key == "translations") {
                if (!parse_translation_entries_array(cursor, entries)) {
                    return std::nullopt;
                }
            } else if (!cursor.skip_value()) {
                return std::nullopt;
            }
            if (cursor.consume('}')) {
                break;
            }
            if (!cursor.consume(',')) {
                return std::nullopt;
            }
        }
    } else if (first_token == '[') {
        if (!parse_translation_entries_array(cursor, entries)) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    if (entries.empty()) {
        return std::nullopt;
    }
    return entries;
}

[[nodiscard]] bool parse_content_field(JsonCursor& cursor, std::string& content);

[[nodiscard]] bool parse_content_parts_array(JsonCursor& cursor, std::string& content) {
    if (!cursor.consume('[')) {
        return false;
    }
    cursor.skip_whitespace();
    if (cursor.consume(']')) {
        return true;
    }

    for (;;) {
        const char token = cursor.peek();
        if (token == '{') {
            if (!cursor.consume('{')) {
                return false;
            }

            std::string text_part;
            std::string type;
            for (;;) {
                if (cursor.consume('}')) {
                    break;
                }
                const std::optional<std::string> key = cursor.parse_string();
                if (!key.has_value() || !cursor.consume(':')) {
                    return false;
                }
                if (*key == "text") {
                    const std::optional<std::string> value = cursor.parse_string();
                    if (!value.has_value()) {
                        return false;
                    }
                    text_part = *value;
                } else if (*key == "type") {
                    const std::optional<std::string> value = cursor.parse_string();
                    if (!value.has_value()) {
                        return false;
                    }
                    type = *value;
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
            if (!text_part.empty() && (type.empty() || type == "text")) {
                content.append(text_part);
            }
        } else if (token == '"') {
            const std::optional<std::string> value = cursor.parse_string();
            if (!value.has_value()) {
                return false;
            }
            content.append(*value);
        } else if (!cursor.skip_value()) {
            return false;
        }

        if (cursor.consume(']')) {
            return true;
        }
        if (!cursor.consume(',')) {
            return false;
        }
    }
}

[[nodiscard]] bool parse_content_field(JsonCursor& cursor, std::string& content) {
    const char token = cursor.peek();
    if (token == '"') {
        const std::optional<std::string> value = cursor.parse_string();
        if (!value.has_value()) {
            return false;
        }
        content = *value;
        return true;
    }
    if (token == '[') {
        content.clear();
        return parse_content_parts_array(cursor, content);
    }
    return cursor.skip_value();
}

[[nodiscard]] bool parse_message_object_for_content(JsonCursor& cursor, std::string& content) {
    if (!cursor.consume('{')) {
        return false;
    }

    for (;;) {
        if (cursor.consume('}')) {
            return true;
        }
        const std::optional<std::string> key = cursor.parse_string();
        if (!key.has_value() || !cursor.consume(':')) {
            return false;
        }
        if (*key == "content") {
            if (!parse_content_field(cursor, content)) {
                return false;
            }
        } else if (!cursor.skip_value()) {
            return false;
        }
        if (cursor.consume('}')) {
            return true;
        }
        if (!cursor.consume(',')) {
            return false;
        }
    }
}

[[nodiscard]] bool parse_choice_object_for_content(JsonCursor& cursor, std::string& content) {
    if (!cursor.consume('{')) {
        return false;
    }

    for (;;) {
        if (cursor.consume('}')) {
            return true;
        }
        const std::optional<std::string> key = cursor.parse_string();
        if (!key.has_value() || !cursor.consume(':')) {
            return false;
        }
        if (*key == "message") {
            if (!parse_message_object_for_content(cursor, content)) {
                return false;
            }
        } else if (*key == "content") {
            if (!parse_content_field(cursor, content)) {
                return false;
            }
        } else if (!cursor.skip_value()) {
            return false;
        }
        if (cursor.consume('}')) {
            return true;
        }
        if (!cursor.consume(',')) {
            return false;
        }
    }
}

[[nodiscard]] bool parse_choices_array_for_content(JsonCursor& cursor, std::string& content) {
    if (!cursor.consume('[')) {
        return false;
    }
    cursor.skip_whitespace();
    if (cursor.consume(']')) {
        return true;
    }

    for (;;) {
        std::string choice_content;
        if (!parse_choice_object_for_content(cursor, choice_content)) {
            return false;
        }
        if (content.empty() && !choice_content.empty()) {
            content = std::move(choice_content);
        }
        if (cursor.consume(']')) {
            return true;
        }
        if (!cursor.consume(',')) {
            return false;
        }
    }
}

[[nodiscard]] std::optional<std::string> extract_assistant_content(std::string_view json_text) {
    JsonCursor cursor{json_text};
    if (!cursor.consume('{')) {
        return std::nullopt;
    }

    std::string content;
    for (;;) {
        if (cursor.consume('}')) {
            break;
        }
        const std::optional<std::string> key = cursor.parse_string();
        if (!key.has_value() || !cursor.consume(':')) {
            return std::nullopt;
        }
        if (*key == "choices") {
            if (!parse_choices_array_for_content(cursor, content)) {
                return std::nullopt;
            }
        } else if (*key == "content") {
            if (!parse_content_field(cursor, content)) {
                return std::nullopt;
            }
        } else if (!cursor.skip_value()) {
            return std::nullopt;
        }
        if (cursor.consume('}')) {
            break;
        }
        if (!cursor.consume(',')) {
            return std::nullopt;
        }
    }

    if (content.empty()) {
        return std::nullopt;
    }
    return content;
}

[[nodiscard]] std::optional<std::vector<TranslationEntry>> extract_translation_entries(std::string_view response_body) {
    if (const std::optional<std::vector<TranslationEntry>> direct = parse_translation_payload(response_body); direct.has_value()) {
        return direct;
    }
    const std::optional<std::string> assistant_content = extract_assistant_content(response_body);
    if (!assistant_content.has_value()) {
        return std::nullopt;
    }
    return parse_translation_payload(*assistant_content);
}

[[nodiscard]] ChunkTranslationResult request_translation_chunk(
    const SubtitleTranslator::Sender& sender,
    const EndpointConfig& endpoint,
    const std::vector<std::string>& headers,
    std::string_view model,
    std::string_view system_prompt,
    std::span<const SubtitleCue> source_cues,
    const ChunkRange& chunk,
    const std::atomic<bool>& cancel) {
    if (cancel.load(std::memory_order_acquire)) {
        return {
            .message = "已取消",
        };
    }

    http::Request request;
    request.url = endpoint.url;
    request.headers = headers;
    request.body = build_request_body(
        endpoint.flavor,
        model,
        system_prompt,
        build_user_prompt(source_cues, chunk));
    request.timeout_ms = translation_timeout_ms;
    request.cancel = &cancel;

    http::Response response;
    try {
        response = sender(request);
    } catch (const std::exception& exception) {
        return {
            .message = std::string{"LLM请求失败: "} + exception.what(),
        };
    }

    if (cancel.load(std::memory_order_acquire)) {
        return {
            .message = "已取消",
            .http_status = response.status,
        };
    }
    if (!is_success_status(response.status)) {
        return {
            .message = "LLM API返回" + std::to_string(response.status),
            .http_status = response.status,
        };
    }
    if (response.body.empty()) {
        return {
            .message = "LLM API响应为空",
            .http_status = response.status,
        };
    }

    const std::optional<std::vector<TranslationEntry>> entries = extract_translation_entries(response.body);
    if (!entries.has_value()) {
        return {
            .message = "LLM响应格式无效",
            .http_status = response.status,
        };
    }
    if (entries->size() != (chunk.end_index - chunk.begin_index)) {
        return {
            .message = "LLM返回条目数量不匹配",
            .http_status = response.status,
        };
    }

    for (std::size_t local_index = 0; local_index < entries->size(); ++local_index) {
        const std::size_t expected_id = chunk.begin_index + local_index + 1;
        if ((*entries)[local_index].id != expected_id) {
            return {
                .message = "LLM返回条目标识不匹配",
                .http_status = response.status,
            };
        }
    }

    return {
        .success = true,
        .entries = *entries,
        .http_status = response.status,
    };
}

[[nodiscard]] ChunkTranslationResult translate_chunk_with_recovery(
    const SubtitleTranslator::Sender& sender,
    const EndpointConfig& endpoint,
    const std::vector<std::string>& headers,
    std::string_view model,
    std::string_view system_prompt,
    std::span<const SubtitleCue> source_cues,
    const ChunkRange& chunk,
    const std::atomic<bool>& cancel) {
    ChunkTranslationResult last_failure;
    for (int attempt_index = 0; attempt_index <= translation_chunk_retry_attempts; ++attempt_index) {
        ChunkTranslationResult result = request_translation_chunk(
            sender,
            endpoint,
            headers,
            model,
            system_prompt,
            source_cues,
            chunk,
            cancel);
        if (result.success || result.message == "已取消") {
            return result;
        }
        last_failure = std::move(result);
    }

    const std::size_t chunk_size = chunk.end_index - chunk.begin_index;
    if (chunk_size > std::size_t{1} && is_chunk_split_candidate(last_failure.message)) {
        const std::size_t middle_index = chunk.begin_index + chunk_size / std::size_t{2};
        const ChunkTranslationResult left = translate_chunk_with_recovery(
            sender,
            endpoint,
            headers,
            model,
            system_prompt,
            source_cues,
            ChunkRange{.begin_index = chunk.begin_index, .end_index = middle_index},
            cancel);
        if (!left.success) {
            return left;
        }

        const ChunkTranslationResult right = translate_chunk_with_recovery(
            sender,
            endpoint,
            headers,
            model,
            system_prompt,
            source_cues,
            ChunkRange{.begin_index = middle_index, .end_index = chunk.end_index},
            cancel);
        if (!right.success) {
            return right;
        }

        ChunkTranslationResult combined;
        combined.success = true;
        combined.http_status = right.http_status != 0 ? right.http_status : left.http_status;
        combined.entries.reserve(left.entries.size() + right.entries.size());
        combined.entries.insert(combined.entries.end(), left.entries.begin(), left.entries.end());
        combined.entries.insert(combined.entries.end(), right.entries.begin(), right.entries.end());
        return combined;
    }

    return last_failure;
}

}

SubtitleTranslator::SubtitleTranslator(Sender sender) : sender_(std::move(sender)) {}

std::string format_translation_progress(std::size_t chunk_position, std::size_t chunk_count) {
    const std::size_t normalized_chunk_count = chunk_count == 0 ? 1 : chunk_count;
    return "进行中（"
        + std::to_string(displayed_chunk_index(chunk_position, normalized_chunk_count))
        + "/"
        + std::to_string(normalized_chunk_count)
        + "）";
}

TranslationOutcome SubtitleTranslator::translate(
    const Config& configuration,
    std::span<const SubtitleCue> source_cues,
    const std::filesystem::path& working_directory,
    std::string_view source_language,
    std::string_view target_language,
    const std::atomic<bool>& cancel,
    ProgressCallback on_progress) const {
    if (cancel.load(std::memory_order_acquire)) {
        return {false, "已取消"};
    }
    if (!sender_) {
        return {false, "LLM发送器不可用"};
    }
    if (source_cues.empty()) {
        return {false, "源字幕为空"};
    }

    const EndpointConfig endpoint = normalize_endpoint(configuration.language_model_base_url);
    if (endpoint.url.empty()) {
        return {false, "未配置LLM Base URL"};
    }

    const std::string_view configured_model = trim_ascii_whitespace(configuration.language_model_name);
    if (configured_model.empty()) {
        return {false, "未配置LLM Model"};
    }

    const std::string system_prompt = build_system_prompt(source_language, target_language);
    const std::vector<std::string> headers = build_headers(endpoint.flavor, configuration.language_model_api_key);
    const std::vector<ChunkRange> chunks = build_chunk_ranges(source_cues);
    std::vector<SubtitleCue> translated_cues;
    translated_cues.reserve(source_cues.size());

    int last_http_status = 0;
    for (std::size_t chunk_position = 0; chunk_position < chunks.size(); ++chunk_position) {
        const ChunkRange& chunk = chunks[chunk_position];
        if (cancel.load(std::memory_order_acquire)) {
            return {false, "已取消"};
        }
        if (on_progress) {
            on_progress(chunk_position, chunks.size());
        }

        const ChunkTranslationResult chunk_result = translate_chunk_with_recovery(
            sender_,
            endpoint,
            headers,
            configured_model,
            system_prompt,
            source_cues,
            chunk,
            cancel);
        if (!chunk_result.success) {
            return {false, std::move(chunk_result.message), chunk_result.http_status};
        }

        last_http_status = chunk_result.http_status;
        for (std::size_t local_index = 0; local_index < chunk_result.entries.size(); ++local_index) {
            const TranslationEntry& entry = chunk_result.entries[local_index];
            const SubtitleCue& source_cue = source_cues[chunk.begin_index + local_index];
            translated_cues.push_back({
                .start_seconds = source_cue.start_seconds,
                .end_seconds = source_cue.end_seconds,
                .text = entry.text,
            });
        }
    }

    std::error_code error_code;
    std::filesystem::create_directories(working_directory, error_code);
    if (error_code) {
        return {false, "无法创建翻译输出目录", last_http_status};
    }

    const std::filesystem::path subtitle_path = working_directory / std::filesystem::u8path(std::string{translated_subtitle_filename});
    normalize_sequential_cue_timings(translated_cues);
    if (!save_srt(translated_cues, subtitle_path)) {
        return {false, "无法写入zh.srt", last_http_status};
    }

    return {true, "LLM翻译", last_http_status, subtitle_path};
}

}