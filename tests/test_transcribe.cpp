#include "swb_test.h"
#include "test_support.h"
#include "swb/transcribe.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>

using swb::test::contains_text;
using swb::test::expect_eq;
using swb::test::expect_true;
using swb::test::has_header_prefix;
using swb::test::make_temp_directory;
using swb::test::read_text_file;

namespace {

const swb::test::Registrar case_1{
    "transcribe: writes transcript and builds multipart request",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-transcribe-success");
        const std::filesystem::path audio_path = directory / "audio.wav";
        {
            std::ofstream output(audio_path, std::ios::binary | std::ios::trunc);
            output << "RIFFtest-data";
        }

        std::optional<swb::http::Request> captured_request;
        std::vector<std::string> progress_updates;
        const swb::WhisperApiTranscriber transcriber{
            [&](const swb::http::Request& request) {
                captured_request = request;
                return swb::http::Response{
                    .status = 200,
                    .body = R"({"text":"hello"})",
                };
            }};

        swb::Config configuration;
        configuration.whisper_base_url = "https://api.example.test";
        configuration.whisper_api_key = "secret-token";
        configuration.whisper_model = "gpt-4o-mini-transcribe";

        const std::atomic<bool> cancel{false};
        const swb::TranscriptionOutcome outcome = transcriber.transcribe(
            configuration,
            audio_path,
            directory,
            "en",
            cancel,
            [&](swb::TranscriptionStage stage) {
                progress_updates.push_back(swb::format_transcription_progress(stage));
            });

        expect_true(outcome.success);
        expect_eq(outcome.message, std::string{"API转写"});
        expect_eq(progress_updates.size(), std::size_t{4});
        expect_eq(progress_updates[0], std::string{"API转写（读取音频）"});
        expect_eq(progress_updates[1], std::string{"API转写（准备请求）"});
        expect_eq(progress_updates[2], std::string{"API转写（请求中）"});
        expect_eq(progress_updates[3], std::string{"API转写（写入结果）"});
        expect_true(captured_request.has_value());
        expect_eq(captured_request->url, std::string{"https://api.example.test/v1/audio/transcriptions"});
        expect_true(has_header_prefix(captured_request->headers, "Content-Type: multipart/form-data; boundary="));
        expect_true(has_header_prefix(captured_request->headers, "Authorization: Bearer secret-token"));
        expect_true(captured_request->cancel == &cancel);
        expect_true(contains_text(captured_request->body, "name=\"model\"\r\n\r\ngpt-4o-mini-transcribe"));
        expect_true(contains_text(captured_request->body, "name=\"response_format\"\r\n\r\nverbose_json"));
        expect_true(contains_text(captured_request->body, "name=\"timestamp_granularities[]\"\r\n\r\nword"));
        expect_true(contains_text(captured_request->body, "name=\"language\"\r\n\r\nen"));
        expect_true(contains_text(captured_request->body, "filename=\"audio.wav\""));
        expect_true(contains_text(captured_request->body, "RIFFtest-data"));
        expect_true(std::filesystem::exists(outcome.transcript_path));
        expect_eq(read_text_file(outcome.transcript_path), std::string{R"({"text":"hello"})"});

        std::filesystem::remove_all(directory);
    },
};

const swb::test::Registrar case_2{
    "transcribe: preserves explicit endpoint and omits auth header when key is empty",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-transcribe-endpoint");
        const std::filesystem::path audio_path = directory / "audio.wav";
        {
            std::ofstream output(audio_path, std::ios::binary | std::ios::trunc);
            output << "RIFFtest-data";
        }

        std::optional<swb::http::Request> captured_request;
        const swb::WhisperApiTranscriber transcriber{
            [&](const swb::http::Request& request) {
                captured_request = request;
                return swb::http::Response{
                    .status = 200,
                    .body = R"({"text":"ok"})",
                };
            }};

        swb::Config configuration;
        configuration.whisper_base_url = "https://api.example.test/custom/audio/transcriptions/";

        const std::atomic<bool> cancel{false};
        const swb::TranscriptionOutcome outcome = transcriber.transcribe(configuration, audio_path, directory, "", cancel);

        expect_true(outcome.success);
        expect_true(captured_request.has_value());
        expect_eq(captured_request->url, std::string{"https://api.example.test/custom/audio/transcriptions"});
        expect_true(!has_header_prefix(captured_request->headers, "Authorization:"));
        expect_true(contains_text(captured_request->body, "name=\"model\"\r\n\r\nwhisper-1"));
        expect_true(!contains_text(captured_request->body, "name=\"language\""));

        std::filesystem::remove_all(directory);
    },
};

const swb::test::Registrar case_3{
    "transcribe: validates base url before sending",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-transcribe-invalid");
        const std::filesystem::path audio_path = directory / "audio.wav";
        {
            std::ofstream output(audio_path, std::ios::binary | std::ios::trunc);
            output << "RIFFtest-data";
        }

        bool sender_called = false;
        const swb::WhisperApiTranscriber transcriber{
            [&](const swb::http::Request&) {
                sender_called = true;
                return swb::http::Response{};
            }};

        const std::atomic<bool> cancel{false};
        const swb::TranscriptionOutcome outcome = transcriber.transcribe(swb::Config{}, audio_path, directory, "en", cancel);

        expect_true(!outcome.success);
        expect_eq(outcome.message, std::string{"未配置Whisper Base URL"});
        expect_true(!sender_called);

        std::filesystem::remove_all(directory);
    },
};

const swb::test::Registrar case_4{
    "transcribe: surfaces server error details from response body",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-transcribe-server-error");
        const std::filesystem::path audio_path = directory / "audio.wav";
        {
            std::ofstream output(audio_path, std::ios::binary | std::ios::trunc);
            output << "RIFFtest-data";
        }

        const swb::WhisperApiTranscriber transcriber{
            [&](const swb::http::Request&) {
                return swb::http::Response{
                    .status = 500,
                    .body = R"({"error":{"message":"model whisper-1 is unavailable for this provider"}})",
                };
            }};

        swb::Config configuration;
        configuration.whisper_base_url = "https://api.example.test";

        const std::atomic<bool> cancel{false};
        const swb::TranscriptionOutcome outcome = transcriber.transcribe(configuration, audio_path, directory, "en", cancel);

        expect_true(!outcome.success);
        expect_eq(outcome.http_status, 500);
        expect_eq(outcome.message, std::string{"Whisper API返回500：model whisper-1 is unavailable for this provider"});

        std::filesystem::remove_all(directory);
    },
};

const swb::test::Registrar case_5{
    "transcribe: saves transcript words with roundtrip",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-transcript-save-roundtrip");
        const std::filesystem::path transcript_path = directory / "transcript.json";
        const std::vector<swb::TranscriptWord> words{
            {.word = "hello", .start_seconds = 0.0, .end_seconds = 0.5},
            {.word = "world", .start_seconds = 300.0, .end_seconds = 300.4},
        };

        expect_true(swb::save_transcript_words(words, transcript_path));

        const swb::TranscriptReadResult loaded = swb::load_transcript_words(transcript_path);
        expect_true(loaded.success);
        expect_eq(loaded.words.size(), std::size_t{2});
        expect_eq(loaded.words[0].word, std::string{"hello"});
        expect_eq(loaded.words[1].word, std::string{"world"});
        expect_eq(loaded.words[1].start_seconds, 300.0);
    },
};

const swb::test::Registrar case_6{
    "transcribe: formats chunk progress as requested",
    [] {
        expect_eq(swb::format_transcription_batch_progress(0, 5), std::string{"API转写（0/5）"});
        expect_eq(swb::format_transcription_batch_progress(3, 5), std::string{"API转写（3/5）"});
    },
};

const swb::test::Registrar case_7{
    "transcribe: parses transcript words from verbose json",
    [] {
        const std::string json = R"({
        "task":"transcribe",
        "words":[
            {"word":"hello","start":1.25,"end":1.5},
            {"word":"say \"hi\"","start":1.5,"end":2.0}
        ]
    })";

        const swb::TranscriptReadResult result = swb::parse_transcript_words(json);

        expect_true(result.success);
        expect_eq(result.words.size(), std::size_t{2});
        expect_eq(result.words[0].word, std::string{"hello"});
        expect_eq(result.words[1].word, std::string{"say \"hi\""});
        expect_eq(result.words[1].start_seconds, 1.5);
        expect_eq(result.words[1].end_seconds, 2.0);
    },
};

const swb::test::Registrar case_8{
    "transcribe: skips nested verbose json fields before words",
    [] {
        const std::string json = R"({
        "task":"transcribe",
        "segments":[
            {
                "id":0,
                "seek":0,
                "tokens":[50364, 400, 995],
                "metadata":{"compression_ratio":1.2,"flags":[true,false,null]},
                "text":"hello world"
            }
        ],
        "words":[
            {"word":"hello","start":0.0,"end":0.5},
            {"word":"world","start":0.5,"end":1.0}
        ]
    })";

        const swb::TranscriptReadResult result = swb::parse_transcript_words(json);

        expect_true(result.success);
        expect_eq(result.words.size(), std::size_t{2});
        expect_eq(result.words[0].word, std::string{"hello"});
        expect_eq(result.words[1].word, std::string{"world"});
    },
};

}
