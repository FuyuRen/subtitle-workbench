#include "swb_test.h"
#include "swb/config.h"

#include <filesystem>
#include <fstream>
#include <iterator>

using swb::test::expect_eq;
using swb::test::expect_true;

namespace {

std::filesystem::path test_root_directory() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "subtitle-workbench-tests";
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path temp_config_path(std::string_view tag) {
    const std::filesystem::path directory = test_root_directory() / "config";
    std::filesystem::create_directories(directory);
    return directory / (std::string(tag) + ".ini");
}

const swb::test::Registrar case_1{
    "config: defaults when file missing",
    [] {
        const auto path = temp_config_path("missing");
        std::filesystem::remove(path);

        const auto configuration = swb::load_config(path);
        expect_eq(configuration.source_lang, std::string{"en"});
        expect_eq(configuration.target_lang, std::string{"zh"});
        expect_eq(configuration.retry_count, 0);
        expect_true(configuration.transcription_backend == swb::TranscriptionBackend::local);
        expect_eq(configuration.local_asr_model, std::string{"base-q5_1"});
        expect_true(configuration.local_asr_compute == swb::LocalAsrCompute::automatic);
        expect_true(configuration.local_asr_use_vad);
        expect_true(!configuration.bilingual_subtitles);
        expect_eq(configuration.hard_subtitle_style.font_name, std::string{"Microsoft YaHei"});
        expect_eq(configuration.hard_subtitle_style.chinese_font_size, 28);
        expect_eq(configuration.hard_subtitle_style.english_font_size, 20);
        expect_eq(configuration.hard_subtitle_style.bottom_margin, 60);
        expect_true(configuration.hard_subtitle_style.bilingual_line_gap == 12.0f);
        expect_eq(configuration.hard_subtitle_style.preview_background_color.red, 22);
        expect_eq(configuration.hard_subtitle_style.preview_background_color.green, 24);
        expect_eq(configuration.hard_subtitle_style.preview_background_color.blue, 28);
        expect_eq(configuration.hard_subtitle_style.preview_background_color.alpha, 255);
    },
};

const swb::test::Registrar case_2{
    "config: round trip preserves all fields",
    [] {
        swb::Config original;
        original.transcription_backend = swb::TranscriptionBackend::api;
        original.local_asr_model_dir = "custom-models";
        original.local_asr_compute = swb::LocalAsrCompute::gpu;
        original.local_asr_gpu_device = 2;
        original.local_asr_threads = 6;
        original.local_asr_use_vad = false;
        original.whisper_base_url = "https://api.openai.com/v1";
        original.whisper_api_key = "sk-w";
        original.whisper_model = "gpt-4o-mini-transcribe";
        original.language_model_base_url = "https://api.anthropic.com";
        original.language_model_api_key = "sk-l";
        original.language_model_name = "claude-3-haiku";
        original.retry_count = 3;
        original.source_lang = "ja";
        original.target_lang = "en";
        original.bilingual_subtitles = true;
        original.hard_subtitle_style.font_name = "SimHei";
        original.hard_subtitle_style.chinese_font_size = 34;
        original.hard_subtitle_style.english_font_size = 21;
        original.hard_subtitle_style.bottom_margin = 84;
        original.hard_subtitle_style.fill_color = {240, 230, 210, 255};
        original.hard_subtitle_style.outline_color = {12, 18, 36, 255};
        original.hard_subtitle_style.outline_thickness = 0.0f;
        original.hard_subtitle_style.bilingual_line_gap = 15.0f;
        original.hard_subtitle_style.preview_background_color = {48, 52, 60, 255};

        const auto path = temp_config_path("roundtrip");
        swb::save_config(original, path);

        const auto loaded_configuration = swb::load_config(path);
        expect_true(loaded_configuration.transcription_backend == original.transcription_backend);
        expect_eq(loaded_configuration.local_asr_model_dir, original.local_asr_model_dir);
        expect_true(loaded_configuration.local_asr_compute == original.local_asr_compute);
        expect_eq(loaded_configuration.local_asr_gpu_device, original.local_asr_gpu_device);
        expect_eq(loaded_configuration.local_asr_threads, original.local_asr_threads);
        expect_true(loaded_configuration.local_asr_use_vad == original.local_asr_use_vad);
        expect_eq(loaded_configuration.whisper_base_url, original.whisper_base_url);
        expect_eq(loaded_configuration.whisper_api_key, original.whisper_api_key);
        expect_eq(loaded_configuration.whisper_model, original.whisper_model);
        expect_eq(loaded_configuration.language_model_base_url, original.language_model_base_url);
        expect_eq(loaded_configuration.language_model_api_key, original.language_model_api_key);
        expect_eq(loaded_configuration.language_model_name, original.language_model_name);
        expect_eq(loaded_configuration.retry_count, original.retry_count);
        expect_eq(loaded_configuration.source_lang, original.source_lang);
        expect_eq(loaded_configuration.target_lang, original.target_lang);
        expect_true(loaded_configuration.bilingual_subtitles == original.bilingual_subtitles);
        expect_eq(loaded_configuration.hard_subtitle_style.font_name, original.hard_subtitle_style.font_name);
        expect_eq(loaded_configuration.hard_subtitle_style.chinese_font_size, original.hard_subtitle_style.chinese_font_size);
        expect_eq(loaded_configuration.hard_subtitle_style.english_font_size, original.hard_subtitle_style.english_font_size);
        expect_eq(loaded_configuration.hard_subtitle_style.bottom_margin, original.hard_subtitle_style.bottom_margin);
        expect_eq(loaded_configuration.hard_subtitle_style.fill_color.red, original.hard_subtitle_style.fill_color.red);
        expect_eq(loaded_configuration.hard_subtitle_style.fill_color.green, original.hard_subtitle_style.fill_color.green);
        expect_eq(loaded_configuration.hard_subtitle_style.fill_color.blue, original.hard_subtitle_style.fill_color.blue);
        expect_eq(loaded_configuration.hard_subtitle_style.fill_color.alpha, original.hard_subtitle_style.fill_color.alpha);
        expect_eq(loaded_configuration.hard_subtitle_style.outline_color.red, original.hard_subtitle_style.outline_color.red);
        expect_eq(loaded_configuration.hard_subtitle_style.outline_color.green, original.hard_subtitle_style.outline_color.green);
        expect_eq(loaded_configuration.hard_subtitle_style.outline_color.blue, original.hard_subtitle_style.outline_color.blue);
        expect_eq(loaded_configuration.hard_subtitle_style.outline_color.alpha, original.hard_subtitle_style.outline_color.alpha);
        expect_true(loaded_configuration.hard_subtitle_style.outline_thickness == original.hard_subtitle_style.outline_thickness);
        expect_true(loaded_configuration.hard_subtitle_style.bilingual_line_gap == original.hard_subtitle_style.bilingual_line_gap);
        expect_eq(loaded_configuration.hard_subtitle_style.preview_background_color.red, original.hard_subtitle_style.preview_background_color.red);
        expect_eq(loaded_configuration.hard_subtitle_style.preview_background_color.green, original.hard_subtitle_style.preview_background_color.green);
        expect_eq(loaded_configuration.hard_subtitle_style.preview_background_color.blue, original.hard_subtitle_style.preview_background_color.blue);
        expect_eq(loaded_configuration.hard_subtitle_style.preview_background_color.alpha, original.hard_subtitle_style.preview_background_color.alpha);
    },
};

const swb::test::Registrar case_3{
    "config: ignores comments and blank lines",
    [] {
        const auto path = temp_config_path("comments");
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "# this is a comment\n"
                   << "\n"
                   << "  llm_model = gpt-4o-mini  \n"
                   << "source_lang=ko\n";
        }

        const auto configuration = swb::load_config(path);
        expect_eq(configuration.language_model_name, std::string{"gpt-4o-mini"});
        expect_eq(configuration.source_lang, std::string{"ko"});
    },
};

const swb::test::Registrar case_4{
    "config: duplicate keys use the last value",
    [] {
        const auto path = temp_config_path("duplicate-keys");
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
                        output << "llm_model=first-model\n"
                                     << "llm_model=second-model\n"
                   << "retry_count=1\n"
                   << "retry_count=3\n";
        }

        const auto configuration = swb::load_config(path);
        expect_eq(configuration.language_model_name, std::string{"second-model"});
        expect_eq(configuration.retry_count, 3);
    },
};

const swb::test::Registrar case_5{
    "config: whisper model defaults to whisper-1",
    [] {
        const auto path = temp_config_path("missing-whisper-model-default");
        std::filesystem::remove(path);
        const auto configuration = swb::load_config(path);
        expect_eq(configuration.whisper_model, std::string{"whisper-1"});
    },
};

const swb::test::Registrar case_6{
    "config: legacy explicit api settings migrate to api backend",
    [] {
        const auto path = temp_config_path("legacy-api-migration");
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "whisper_base_url=https://provider.example/v1\n"
                   << "whisper_api_key=legacy-secret\n"
                   << "whisper_model=whisper-1\n";
        }

        const swb::Config configuration = swb::load_config(path);
        expect_true(configuration.transcription_backend == swb::TranscriptionBackend::api);
        expect_eq(configuration.whisper_api_key, std::string{"legacy-secret"});
    },
};

const swb::test::Registrar case_7{
    "config: explicit local backend overrides legacy api settings",
    [] {
        const auto path = temp_config_path("explicit-local-migration");
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "transcription_backend=local\n"
                   << "whisper_api_key=preserved-secret\n"
                   << "local_asr_compute=cpu\n";
        }

        const swb::Config configuration = swb::load_config(path);
        expect_true(configuration.transcription_backend == swb::TranscriptionBackend::local);
        expect_true(configuration.local_asr_compute == swb::LocalAsrCompute::cpu);
        expect_eq(configuration.whisper_api_key, std::string{"preserved-secret"});
    },
};

const swb::test::Registrar case_8{
    "config: save preserves unknown fields",
    [] {
        const auto path = temp_config_path("preserve-unknown");
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "future_setting=kept\n";
        }

        swb::Config configuration = swb::load_config(path);
        swb::save_config(configuration, path);
        std::ifstream input(path, std::ios::binary);
        const std::string saved{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        expect_true(saved.find("future_setting=kept") != std::string::npos);
    },
};

}
