#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swb {

struct SubtitleColor {
    std::uint8_t red{255};
    std::uint8_t green{255};
    std::uint8_t blue{255};
    std::uint8_t alpha{255};
};

struct HardSubtitleStyle {
    std::string font_name{"Microsoft YaHei"};
    int chinese_font_size{28};
    int english_font_size{20};
    int bottom_margin{60};
    SubtitleColor fill_color{};
    SubtitleColor outline_color{0, 0, 0, 255};
    float outline_thickness{2.0f};
    float bilingual_line_gap{12.0f};
    SubtitleColor preview_background_color{22, 24, 28, 255};
};

enum class TranscriptionBackend : int {
    local,
    api,
};

enum class LocalAsrCompute : int {
    automatic,
    gpu,
    cpu,
};

struct Config {
    TranscriptionBackend transcription_backend{TranscriptionBackend::local};
    std::string local_asr_model{"base-q5_1"};
    std::string local_asr_model_dir;
    LocalAsrCompute local_asr_compute{LocalAsrCompute::automatic};
    int local_asr_gpu_device{0};
    int local_asr_threads{0};
    bool local_asr_use_vad{true};
    std::string whisper_base_url;
    std::string whisper_api_key;
    std::string whisper_model{"whisper-1"};
    std::string language_model_base_url;
    std::string language_model_api_key;
    std::string language_model_name;
    int retry_count{0};
    std::string source_lang{"en"};
    std::string target_lang{"zh"};
    std::string output_dir;
    bool bilingual_subtitles{false};
    HardSubtitleStyle hard_subtitle_style{};
    std::vector<std::pair<std::string, std::string>> preserved_fields;
};

[[nodiscard]] std::string_view transcription_backend_name(TranscriptionBackend backend) noexcept;

[[nodiscard]] std::string_view local_asr_compute_name(LocalAsrCompute compute) noexcept;

[[nodiscard]] std::filesystem::path default_config_path();

[[nodiscard]] Config load_config(const std::filesystem::path& path);

void save_config(const Config& configuration, const std::filesystem::path& path);

}
