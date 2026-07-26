#include "swb_test.h"
#include "test_support.h"
#include "swb/audio.h"
#include "swb/model_manager.h"
#include "swb/transcribe.h"
#include "swb/workspace.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using swb::test::expect_eq;
using swb::test::expect_true;
using swb::test::make_temp_directory;
using swb::test::write_text_file;

namespace {

[[nodiscard]] std::shared_ptr<void> tracked_context(int& live_contexts) {
    ++live_contexts;
    return std::shared_ptr<void>{new int{1}, [&live_contexts](void* value) {
        delete static_cast<int*>(value);
        --live_contexts;
    }};
}

void expect_valid_words(const swb::TranscriptionResult& result) {
    expect_true(result.success);
    expect_true(!result.words.empty());
    double previous_start = 0.0;
    double previous_end = 0.0;
    for (const swb::TranscriptWord& word : result.words) {
        expect_true(!word.word.empty());
        expect_true(std::isfinite(word.start_seconds));
        expect_true(std::isfinite(word.end_seconds));
        expect_true(word.start_seconds <= word.end_seconds);
        expect_true(word.start_seconds >= previous_start);
        expect_true(word.end_seconds >= previous_end);
        previous_start = word.start_seconds;
        previous_end = word.end_seconds;
    }
}

const swb::test::Registrar case_1{
    "local asr: gpu failure destroys context before cpu fallback",
    [] {
        int live_contexts = 0;
        int calls = 0;
        bool cpu_saw_gpu_released = false;
        swb::WhisperCppContextSelection selection;
        {
            selection = swb::select_whisper_cpp_context(
                "model.bin",
                swb::LocalAsrCompute::automatic,
                1,
                [&](const std::filesystem::path&, bool use_gpu, int device) {
                    ++calls;
                    if (use_gpu) {
                        expect_eq(device, 1);
                        return swb::WhisperCppContextAttempt{
                            .context = tracked_context(live_contexts),
                            .backend_name = "Vulkan GPU",
                            .error = "Vulkan初始化失败",
                            .gpu = true,
                        };
                    }
                    cpu_saw_gpu_released = live_contexts == 0;
                    return swb::WhisperCppContextAttempt{
                        .context = tracked_context(live_contexts),
                        .backend_name = "CPU",
                    };
                });

            expect_true(cpu_saw_gpu_released);
            expect_eq(calls, 2);
            expect_eq(selection.creation_attempts, 2);
            expect_true(selection.runtime.backend == swb::ActualTranscriptionBackend::cpu);
            expect_eq(selection.runtime.fallback_reason, std::string{"Vulkan初始化失败"});
            expect_eq(live_contexts, 1);
        }
        selection.context.reset();
        expect_eq(live_contexts, 0);
    },
};

const swb::test::Registrar case_2{
    "local asr: successful gpu initialization reports actual device",
    [] {
        int calls = 0;
        const swb::WhisperCppContextSelection selection = swb::select_whisper_cpp_context(
            "model.bin",
            swb::LocalAsrCompute::gpu,
            0,
            [&](const std::filesystem::path&, bool use_gpu, int) {
                ++calls;
                expect_true(use_gpu);
                return swb::WhisperCppContextAttempt{
                    .context = std::make_shared<int>(1),
                    .backend_name = "Vulkan: Test GPU",
                    .gpu = true,
                };
            });

        expect_eq(calls, 1);
        expect_true(selection.runtime.backend == swb::ActualTranscriptionBackend::gpu);
        expect_eq(selection.runtime.backend_name, std::string{"Vulkan: Test GPU"});
        expect_true(selection.runtime.fallback_reason.empty());
    },
};

const swb::test::Registrar case_3{
    "local asr: cpu mode never probes gpu",
    [] {
        int calls = 0;
        const swb::WhisperCppContextSelection selection = swb::select_whisper_cpp_context(
            "model.bin",
            swb::LocalAsrCompute::cpu,
            8,
            [&](const std::filesystem::path&, bool use_gpu, int device) {
                ++calls;
                expect_true(!use_gpu);
                expect_eq(device, 0);
                return swb::WhisperCppContextAttempt{
                    .context = std::make_shared<int>(1),
                    .backend_name = "CPU",
                };
            });

        expect_eq(calls, 1);
        expect_eq(selection.creation_attempts, 1);
        expect_true(selection.runtime.backend == swb::ActualTranscriptionBackend::cpu);
    },
};

const swb::test::Registrar case_4{
    "local asr: english tokens aggregate words punctuation and whitespace",
    [] {
        const std::vector<swb::TranscriptToken> tokens{
            {.text = " Hello", .start_seconds = 0.0, .end_seconds = 0.2},
            {.text = " world", .start_seconds = 0.2, .end_seconds = 0.5},
            {.text = " !", .start_seconds = 0.5, .end_seconds = 0.6},
            {.text = "ignored", .start_seconds = 0.6, .end_seconds = 0.7, .control = true},
        };

        const swb::TokenAggregationResult result = swb::aggregate_transcript_tokens(tokens, "en", 1.0);
        expect_eq(result.words.size(), std::size_t{2});
        expect_eq(result.words[0].word, std::string{"Hello"});
        expect_eq(result.words[1].word, std::string{"world!"});
        expect_true(result.words[1].start_seconds == 0.2);
        expect_true(result.words[1].end_seconds == 0.6);
    },
};

const swb::test::Registrar case_5{
    "local asr: cjk aggregation preserves split utf8 and punctuation",
    [] {
        const std::vector<swb::TranscriptToken> tokens{
            {.text = std::string{"\xE4", 1}, .start_seconds = 0.0, .end_seconds = 0.05},
            {.text = std::string{"\xBD\xA0", 2}, .start_seconds = 0.05, .end_seconds = 0.1},
            {.text = "好", .start_seconds = 0.1, .end_seconds = 0.2},
            {.text = "，", .start_seconds = 0.2, .end_seconds = 0.25},
            {.text = "世界", .start_seconds = 0.25, .end_seconds = 0.45},
        };

        const swb::TokenAggregationResult result = swb::aggregate_transcript_tokens(tokens, "zh", 1.0);
        expect_eq(result.words.size(), std::size_t{3});
        expect_eq(result.words[0].word, std::string{"你"});
        expect_eq(result.words[1].word, std::string{"好，"});
        expect_eq(result.words[2].word, std::string{"世界"});
        expect_true(result.words[0].start_seconds == 0.0);
        expect_true(result.words[2].end_seconds == 0.45);
    },
};

const swb::test::Registrar case_6{
    "local asr: invalid timestamps are corrected or discarded monotonically",
    [] {
        const std::vector<swb::TranscriptToken> tokens{
            {.text = " one", .start_seconds = 0.1, .end_seconds = 0.2},
            {.text = " two", .start_seconds = 0.15, .end_seconds = 0.1},
            {.text = " bad", .start_seconds = std::numeric_limits<double>::quiet_NaN(), .end_seconds = 0.5},
            {.text = " outside", .start_seconds = 2.0, .end_seconds = 2.1},
        };

        const swb::TokenAggregationResult result = swb::aggregate_transcript_tokens(tokens, "en", 1.0);
        expect_eq(result.words.size(), std::size_t{2});
        expect_true(result.words[0].start_seconds <= result.words[0].end_seconds);
        expect_true(result.words[1].start_seconds >= result.words[0].end_seconds);
        expect_true(!result.diagnostics.empty());
    },
};

const swb::test::Registrar case_7{
    "local asr: overlap merge removes matching words but keeps distinct speech",
    [] {
        const std::vector<swb::TranscriptWord> existing{
            {.word = "before", .start_seconds = 25.0, .end_seconds = 26.0},
            {.word = "Hello", .start_seconds = 27.0, .end_seconds = 27.5},
            {.word = "there", .start_seconds = 28.0, .end_seconds = 28.4},
        };
        const std::vector<swb::TranscriptWord> incoming{
            {.word = "hello", .start_seconds = 27.1, .end_seconds = 27.6},
            {.word = "again", .start_seconds = 27.2, .end_seconds = 27.7},
            {.word = "world", .start_seconds = 28.5, .end_seconds = 29.0},
        };

        const std::vector<swb::TranscriptWord> merged =
            swb::merge_overlapping_transcript_words(existing, incoming, 27.0);
        expect_eq(merged.size(), std::size_t{5});
        expect_eq(merged[0].word, std::string{"before"});
        expect_eq(merged[1].word, std::string{"Hello"});
        expect_eq(merged[2].word, std::string{"again"});
        expect_eq(merged[4].word, std::string{"world"});
        double previous_start = 0.0;
        double previous_end = 0.0;
        for (const swb::TranscriptWord& word : merged) {
            expect_true(word.start_seconds >= previous_start);
            expect_true(word.end_seconds >= previous_end);
            previous_start = word.start_seconds;
            previous_end = word.end_seconds;
        }
    },
};

const swb::test::Registrar case_8{
    "local asr: missing and corrupt models fail before context creation",
    [] {
        int factory_calls = 0;
        const auto factory = [&](const std::filesystem::path&, bool, int) {
            ++factory_calls;
            return swb::WhisperCppContextAttempt{};
        };

        swb::Config configuration;
        configuration.local_asr_compute = swb::LocalAsrCompute::cpu;
        const std::filesystem::path missing_directory = make_temp_directory("local-model-missing");
        configuration.local_asr_model_dir = swb::path_to_utf8(missing_directory);
        swb::WhisperCppTranscriber missing{configuration, factory};
        expect_true(!missing.ready());
        expect_true(missing.error().find("未下载") != std::string_view::npos);

        const std::filesystem::path corrupt_directory = make_temp_directory("local-model-corrupt");
        write_text_file(swb::model_file_path(swb::default_local_asr_model(), corrupt_directory), "corrupt");
        configuration.local_asr_model_dir = swb::path_to_utf8(corrupt_directory);
        swb::WhisperCppTranscriber corrupt{configuration, factory};
        expect_true(!corrupt.ready());
        expect_true(corrupt.error().find("损坏") != std::string_view::npos);
        expect_eq(factory_calls, 0);
    },
};

const swb::test::Registrar case_9{
    "local asr: api cancellation cleans temporary results and commits no transcript",
    [] {
        const std::filesystem::path directory = make_temp_directory("api-transcribe-cancel-cleanup");
        const std::filesystem::path audio_path = directory / "audio.wav";
        expect_true(swb::write_pcm16_wav(audio_path, std::vector<std::int16_t>(16'000)));
        std::atomic<bool> cancel{false};
        const swb::WhisperApiTranscriber transcriber{[&](const swb::http::Request&) {
            cancel.store(true, std::memory_order_release);
            return swb::http::Response{
                .status = 200,
                .body = R"({"words":[{"word":"partial","start":0.0,"end":0.5}]})",
            };
        }};
        swb::Config configuration;
        configuration.whisper_base_url = "https://api.example.test";

        const swb::TranscriptionResult result = swb::transcribe_wav_with_api(
            transcriber,
            configuration,
            audio_path,
            directory,
            "en",
            cancel);

        expect_true(!result.success);
        expect_eq(result.message, std::string{"已取消"});
        expect_true(!std::filesystem::exists(directory / "transcript.json"));
        expect_true(!std::filesystem::exists(directory / ".api-transcribe-window.wav"));
        expect_true(!std::filesystem::exists(directory / ".api-transcribe-window-result"));
    },
};

const swb::test::Registrar case_10{
    "local asr: atomic transcript json roundtrip preserves monotonic timestamps",
    [] {
        const std::filesystem::path directory = make_temp_directory("transcript-atomic-roundtrip");
        const std::filesystem::path path = directory / "transcript.json";
        const std::vector<swb::TranscriptWord> words{
            {.word = "你", .start_seconds = 0.0123456789, .end_seconds = 0.123456789},
            {.word = "hello", .start_seconds = 0.123456789, .end_seconds = 0.987654321},
        };
        expect_true(swb::save_transcript_words_atomic(words, path));
        const swb::TranscriptReadResult loaded = swb::load_transcript_words(path);
        expect_true(loaded.success);
        expect_eq(loaded.words.size(), words.size());
        expect_eq(loaded.words[0].word, words[0].word);
        expect_true(loaded.words[0].start_seconds <= loaded.words[0].end_seconds);
        expect_true(loaded.words[1].start_seconds >= loaded.words[0].end_seconds);
        std::filesystem::path part = path;
        part += L".part";
        expect_true(!std::filesystem::exists(part));
    },
};

const swb::test::Registrar case_11{
    "local asr: gpu devices have stable indices and display names",
    [] {
        const std::vector<swb::LocalAsrGpuDevice> devices = swb::enumerate_local_asr_gpu_devices();
        for (std::size_t position = 0; position < devices.size(); ++position) {
            expect_eq(devices[position].index, static_cast<int>(position));
            expect_true(!devices[position].name.empty());
        }
    },
};

const swb::test::Registrar case_12{
    "local asr: environment model integration transcribes english and chinese",
    [] {
        const char* model_directory = std::getenv("SWB_LOCAL_ASR_MODEL_DIR");
        const char* english_audio = std::getenv("SWB_LOCAL_ASR_EN_WAV");
        const char* chinese_audio = std::getenv("SWB_LOCAL_ASR_ZH_WAV");
        if (model_directory == nullptr || english_audio == nullptr || chinese_audio == nullptr) {
            return;
        }

        const bool expect_gpu = std::getenv("SWB_EXPECT_LOCAL_ASR_GPU") != nullptr;
        swb::Config configuration;
        configuration.local_asr_model_dir = model_directory;
        configuration.local_asr_compute = expect_gpu
            ? swb::LocalAsrCompute::automatic
            : swb::LocalAsrCompute::cpu;
        configuration.local_asr_use_vad = std::getenv("SWB_LOCAL_ASR_USE_VAD") != nullptr;
        std::atomic<bool> cancel{false};
        swb::WhisperCppTranscriber transcriber{configuration};
        expect_true(transcriber.ready());
        expect_eq(transcriber.model_load_attempts(), 1);
        if (expect_gpu) {
            expect_true(transcriber.runtime().backend == swb::ActualTranscriptionBackend::gpu);
        }

        const swb::TranscriptionResult english = transcriber.transcribe_wav(
            configuration,
            std::filesystem::u8path(english_audio),
            "en",
            cancel);
        const swb::TranscriptionResult chinese = transcriber.transcribe_wav(
            configuration,
            std::filesystem::u8path(chinese_audio),
            "zh",
            cancel);
        expect_valid_words(english);
        expect_valid_words(chinese);
        expect_eq(transcriber.model_load_attempts(), 1);

        const swb::TranscriptionResult unsupported = transcriber.transcribe_wav(
            configuration,
            std::filesystem::u8path(english_audio),
            "not-a-language",
            cancel);
        expect_true(!unsupported.success);
        expect_true(unsupported.message.find("不支持源语言") != std::string::npos);
    },
};

}
