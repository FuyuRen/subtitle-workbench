#include "swb_test.h"
#include "test_support.h"
#include "swb/translate.h"

#include <atomic>
#include <filesystem>
#include <optional>

using swb::test::contains_text;
using swb::test::expect_eq;
using swb::test::expect_true;
using swb::test::has_header_prefix;
using swb::test::make_temp_directory;

namespace {

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

[[nodiscard]] std::string make_translation_payload(std::size_t first_id, std::size_t last_id) {
    std::string payload{"{\"translations\":["};
    for (std::size_t identifier = first_id; identifier <= last_id; ++identifier) {
        if (identifier > first_id) {
            payload.push_back(',');
        }
        payload.append("{\"id\":");
        payload.append(std::to_string(identifier));
        payload.append(",\"text\":\"译文");
        payload.append(std::to_string(identifier));
        payload.append("\"}");
    }
    payload.append("]}");
    return payload;
}

const swb::test::Registrar case_1{
    "translate: writes zh srt and builds openai request",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-translate-openai");

        std::optional<swb::http::Request> captured_request;
        std::vector<std::string> progress_updates;
        const swb::SubtitleTranslator translator{
            [&](const swb::http::Request& request) {
                captured_request = request;
                return swb::http::Response{
                    .status = 200,
                    .body = R"({"choices":[{"message":{"content":"{\"translations\":[{\"id\":1,\"text\":\"你好\"},{\"id\":2,\"text\":\"世界\"}]}"}}]})",
                };
            }};

        swb::Config configuration;
        configuration.language_model_base_url = "https://api.example.test";
        configuration.language_model_api_key = "secret-token";
        configuration.language_model_name = "gpt-4o-mini";

        const std::vector<swb::SubtitleCue> source_cues{{0.0, 1.3, "hello"}, {1.1, 2.0, "world"}};
        const std::atomic<bool> cancel{false};
        const swb::TranslationOutcome outcome = translator.translate(
            configuration,
            source_cues,
            directory,
            "en",
            "zh",
            cancel,
            [&](std::size_t chunk_position, std::size_t chunk_count) {
                progress_updates.push_back(swb::format_translation_progress(chunk_position, chunk_count));
            });

        expect_true(outcome.success);
        expect_eq(outcome.message, std::string{"LLM翻译"});
        expect_eq(progress_updates.size(), std::size_t{1});
        expect_eq(progress_updates.front(), std::string{"进行中（0/1）"});
        expect_true(captured_request.has_value());
        expect_eq(captured_request->url, std::string{"https://api.example.test/v1/chat/completions"});
        expect_true(has_header_prefix(captured_request->headers, "Content-Type: application/json"));
        expect_true(has_header_prefix(captured_request->headers, "Authorization: Bearer secret-token"));
        expect_true(captured_request->cancel == &cancel);
        expect_true(contains_text(captured_request->body, "\"model\":\"gpt-4o-mini\""));
        expect_true(contains_text(captured_request->body, "\"role\":\"system\""));
        expect_true(contains_text(captured_request->body, "\"role\":\"user\""));
        expect_true(contains_text(captured_request->body, "Do not mechanically append a Chinese full stop to every subtitle item"));
        expect_true(contains_text(captured_request->body, "keep it in the original form without transliteration and without added parentheses"));
        expect_true(contains_text(captured_request->body, "hello"));
        expect_true(std::filesystem::exists(outcome.subtitle_path));

        const swb::SrtReadResult translated = swb::load_srt(outcome.subtitle_path);
        expect_true(translated.success);
        expect_eq(translated.cues.size(), std::size_t{2});
        expect_eq(translated.cues[0].text, std::string{"你好"});
        expect_eq(translated.cues[1].text, std::string{"世界"});
        expect_eq(translated.cues[0].start_seconds, 0.0);
        expect_true(translated.cues[0].end_seconds <= translated.cues[1].start_seconds);
        expect_eq(translated.cues[1].end_seconds, 2.0);

        std::filesystem::remove_all(directory);
    },
};

const swb::test::Registrar case_2{
    "translate: supports anthropic responses and splits large batches",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-translate-anthropic");

        std::vector<swb::http::Request> requests;
        std::vector<std::string> progress_updates;
        const swb::SubtitleTranslator translator{
            [&](const swb::http::Request& request) {
                requests.push_back(request);
                const std::size_t request_index = requests.size();
                const std::string payload = request_index == 1 ? make_translation_payload(1, 24) : make_translation_payload(25, 25);
                return swb::http::Response{
                    .status = 200,
                    .body = "{\"content\":[{\"type\":\"text\",\"text\":\"```json\\n" + escape_json_string(payload) + "\\n```\"}]}",
                };
            }};

        swb::Config configuration;
        configuration.language_model_base_url = "https://api.anthropic.com";
        configuration.language_model_api_key = "anthropic-secret";
        configuration.language_model_name = "claude-3-haiku";

        std::vector<swb::SubtitleCue> source_cues;
        for (int index = 0; index < 25; ++index) {
            source_cues.push_back({
                .start_seconds = static_cast<double>(index),
                .end_seconds = static_cast<double>(index) + 0.8,
                .text = "line " + std::to_string(index + 1),
            });
        }

        const std::atomic<bool> cancel{false};
        const swb::TranslationOutcome outcome = translator.translate(
            configuration,
            source_cues,
            directory,
            "en",
            "zh",
            cancel,
            [&](std::size_t chunk_position, std::size_t chunk_count) {
                progress_updates.push_back(swb::format_translation_progress(chunk_position, chunk_count));
            });

        expect_true(outcome.success);
        expect_eq(requests.size(), std::size_t{2});
        expect_eq(progress_updates.size(), std::size_t{2});
        expect_eq(progress_updates[0], std::string{"进行中（0/2）"});
        expect_eq(progress_updates[1], std::string{"进行中（1/2）"});
        expect_eq(requests.front().url, std::string{"https://api.anthropic.com/v1/messages"});
        expect_true(has_header_prefix(requests.front().headers, "Content-Type: application/json"));
        expect_true(has_header_prefix(requests.front().headers, "anthropic-version: 2023-06-01"));
        expect_true(has_header_prefix(requests.front().headers, "x-api-key: anthropic-secret"));
        expect_true(contains_text(requests.front().body, "\"model\":\"claude-3-haiku\""));
        expect_true(contains_text(requests.front().body, "\"system\":"));

        const swb::SrtReadResult translated = swb::load_srt(outcome.subtitle_path);
        expect_true(translated.success);
        expect_eq(translated.cues.size(), std::size_t{25});
        expect_eq(translated.cues.front().text, std::string{"译文1"});
        expect_eq(translated.cues.back().text, std::string{"译文25"});

        std::filesystem::remove_all(directory);
    },
};

const swb::test::Registrar case_3{
    "translate: retries only the failing chunk",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-translate-retry-failing-chunk");

        std::vector<swb::http::Request> requests;
        std::vector<std::string> progress_updates;
        const swb::SubtitleTranslator translator{
            [&](const swb::http::Request& request) {
                requests.push_back(request);
                if (requests.size() == std::size_t{1}) {
                    return swb::http::Response{
                        .status = 200,
                        .body = "{\"choices\":[{\"message\":{\"content\":\"" + escape_json_string(make_translation_payload(1, 24)) + "\"}}]}",
                    };
                }
                if (requests.size() == std::size_t{2}) {
                    return swb::http::Response{
                        .status = 200,
                        .body = R"({"choices":[{"message":{"content":"not-json"}}]})",
                    };
                }
                return swb::http::Response{
                    .status = 200,
                    .body = "{\"choices\":[{\"message\":{\"content\":\"" + escape_json_string(make_translation_payload(25, 25)) + "\"}}]}",
                };
            }};

        swb::Config configuration;
        configuration.language_model_base_url = "https://api.example.test";
        configuration.language_model_api_key = "secret-token";
        configuration.language_model_name = "gpt-4o-mini";

        std::vector<swb::SubtitleCue> source_cues;
        for (int index = 0; index < 25; ++index) {
            source_cues.push_back({
                .start_seconds = static_cast<double>(index),
                .end_seconds = static_cast<double>(index) + 0.8,
                .text = "line " + std::to_string(index + 1),
            });
        }

        const std::atomic<bool> cancel{false};
        const swb::TranslationOutcome outcome = translator.translate(
            configuration,
            source_cues,
            directory,
            "en",
            "zh",
            cancel,
            [&](std::size_t chunk_position, std::size_t chunk_count) {
                progress_updates.push_back(swb::format_translation_progress(chunk_position, chunk_count));
            });

        expect_true(outcome.success);
        expect_eq(requests.size(), std::size_t{3});
        expect_eq(progress_updates.size(), std::size_t{2});
        expect_eq(progress_updates[0], std::string{"进行中（0/2）"});
        expect_eq(progress_updates[1], std::string{"进行中（1/2）"});
        expect_true(requests[0].body != requests[1].body);
        expect_eq(requests[1].body, requests[2].body);

        const swb::SrtReadResult translated = swb::load_srt(outcome.subtitle_path);
        expect_true(translated.success);
        expect_eq(translated.cues.size(), std::size_t{25});
        expect_eq(translated.cues.back().text, std::string{"译文25"});

        std::filesystem::remove_all(directory);
    },
};

const swb::test::Registrar case_4{
    "translate: splits malformed chunk before failing whole translation",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-translate-split-malformed-chunk");

        std::vector<swb::http::Request> requests;
        const swb::SubtitleTranslator translator{
            [&](const swb::http::Request& request) {
                requests.push_back(request);
                if (requests.size() <= std::size_t{2}) {
                    return swb::http::Response{
                        .status = 200,
                        .body = R"({"choices":[{"message":{"content":"not-json"}}]})",
                    };
                }
                if (requests.size() == std::size_t{3}) {
                    return swb::http::Response{
                        .status = 200,
                        .body = "{\"choices\":[{\"message\":{\"content\":\"" + escape_json_string(make_translation_payload(1, 2)) + "\"}}]}",
                    };
                }
                return swb::http::Response{
                    .status = 200,
                    .body = "{\"choices\":[{\"message\":{\"content\":\"" + escape_json_string(make_translation_payload(3, 4)) + "\"}}]}",
                };
            }};

        swb::Config configuration;
        configuration.language_model_base_url = "https://api.example.test";
        configuration.language_model_api_key = "secret-token";
        configuration.language_model_name = "gpt-4o-mini";

        const std::vector<swb::SubtitleCue> source_cues{
            {0.0, 1.0, "line 1"},
            {1.0, 2.0, "line 2"},
            {2.0, 3.0, "line 3"},
            {3.0, 4.0, "line 4"},
        };

        const std::atomic<bool> cancel{false};
        const swb::TranslationOutcome outcome = translator.translate(
            configuration,
            source_cues,
            directory,
            "en",
            "zh",
            cancel);

        expect_true(outcome.success);
        expect_eq(requests.size(), std::size_t{4});
        expect_eq(requests[0].body, requests[1].body);
        expect_true(contains_text(requests[2].body, "line 1"));
        expect_true(contains_text(requests[2].body, "line 2"));
        expect_true(!contains_text(requests[2].body, "line 3"));
        expect_true(contains_text(requests[3].body, "line 3"));
        expect_true(contains_text(requests[3].body, "line 4"));
        expect_true(!contains_text(requests[3].body, "line 2"));

        const swb::SrtReadResult translated = swb::load_srt(outcome.subtitle_path);
        expect_true(translated.success);
        expect_eq(translated.cues.size(), std::size_t{4});
        expect_eq(translated.cues[0].text, std::string{"译文1"});
        expect_eq(translated.cues[3].text, std::string{"译文4"});

        std::filesystem::remove_all(directory);
    },
};

const swb::test::Registrar case_5{
    "translate: validates configuration before sending",
    [] {
        bool sender_called = false;
        const swb::SubtitleTranslator translator{
            [&](const swb::http::Request&) {
                sender_called = true;
                return swb::http::Response{};
            }};

        const std::vector<swb::SubtitleCue> source_cues{{0.0, 1.0, "hello"}};
        const std::atomic<bool> cancel{false};
        const swb::TranslationOutcome missing_base_url = translator.translate(swb::Config{}, source_cues, std::filesystem::temp_directory_path(), "en", "zh", cancel);

        expect_true(!missing_base_url.success);
        expect_eq(missing_base_url.message, std::string{"未配置LLM Base URL"});
        expect_true(!sender_called);

        swb::Config configuration;
        configuration.language_model_base_url = "https://api.example.test";
        const swb::TranslationOutcome missing_model = translator.translate(configuration, source_cues, std::filesystem::temp_directory_path(), "en", "zh", cancel);
        expect_true(!missing_model.success);
        expect_eq(missing_model.message, std::string{"未配置LLM Model"});
        expect_true(!sender_called);
    },
};

const swb::test::Registrar case_6{
    "translate: formats progress text as requested",
    [] {
        expect_eq(swb::format_translation_progress(0, 1), std::string{"进行中（0/1）"});
        expect_eq(swb::format_translation_progress(0, 3), std::string{"进行中（0/3）"});
        expect_eq(swb::format_translation_progress(2, 3), std::string{"进行中（2/3）"});
    },
};

}