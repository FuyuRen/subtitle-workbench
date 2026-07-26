#include "swb/transcribe.h"

#include "swb/audio.h"
#include "swb/model_manager.h"
#include "swb/workspace.h"

#include "ggml-backend.h"
#include "whisper.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <thread>
#include <utility>

namespace swb {

namespace {

constexpr int local_window_seconds = 30;
constexpr int api_window_seconds = 300;
constexpr int window_overlap_seconds = 2;

struct ModelFileLoader {
    std::ifstream input;
};

[[nodiscard]] std::size_t read_model(void* context, void* output, std::size_t read_size) noexcept {
    auto& loader = *static_cast<ModelFileLoader*>(context);
    loader.input.read(static_cast<char*>(output), static_cast<std::streamsize>(read_size));
    return static_cast<std::size_t>(loader.input.gcount());
}

[[nodiscard]] bool model_eof(void* context) noexcept {
    return static_cast<ModelFileLoader*>(context)->input.eof();
}

void close_model(void* context) noexcept {
    static_cast<ModelFileLoader*>(context)->input.close();
}

[[nodiscard]] bool is_gpu_device(ggml_backend_dev_t device) noexcept {
    const enum ggml_backend_dev_type type = ggml_backend_dev_type(device);
    return type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU;
}

[[nodiscard]] ggml_backend_dev_t find_gpu_device(int requested_device) {
    int gpu_index = 0;
    for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
        ggml_backend_dev_t device = ggml_backend_dev_get(index);
        if (!is_gpu_device(device)) {
            continue;
        }
        if (gpu_index == requested_device) {
            return device;
        }
        ++gpu_index;
    }
    return nullptr;
}

[[nodiscard]] std::string gpu_device_name(ggml_backend_dev_t device) {
    const char* name = ggml_backend_dev_name(device);
    const char* description = ggml_backend_dev_description(device);
    std::string result = name == nullptr ? std::string{"GPU"} : std::string{name};
    if (description != nullptr && !std::string_view{description}.empty() && result != description) {
        result.append(" - ");
        result.append(description);
    }
    return result;
}

[[nodiscard]] WhisperCppContextAttempt create_native_context(
    const std::filesystem::path& model_path,
    bool use_gpu,
    int gpu_device) {
    std::string backend_name = "CPU";
    if (use_gpu) {
        ggml_backend_dev_t device = find_gpu_device(gpu_device);
        if (device == nullptr) {
            return {.error = "未发现可用GPU设备", .gpu = true};
        }
        backend_name = gpu_device_name(device);
        ggml_backend_t probe = ggml_backend_dev_init(device, nullptr);
        if (probe == nullptr) {
            return {.error = backend_name + "初始化失败", .gpu = true};
        }
        ggml_backend_free(probe);
    }

    ModelFileLoader file_loader{
        .input = std::ifstream{model_path, std::ios::binary},
    };
    if (!file_loader.input) {
        return {.error = "无法打开本地模型", .gpu = use_gpu};
    }
    whisper_model_loader loader{
        .context = &file_loader,
        .read = read_model,
        .eof = model_eof,
        .close = close_model,
    };
    whisper_context_params parameters = whisper_context_default_params();
    parameters.use_gpu = use_gpu;
    parameters.gpu_device = gpu_device;
    parameters.dtw_token_timestamps = true;
    parameters.dtw_aheads_preset = WHISPER_AHEADS_BASE;
    parameters.flash_attn = false;

    whisper_context* raw_context = whisper_init_with_params(&loader, parameters);
    if (raw_context == nullptr) {
        return {
            .error = use_gpu ? backend_name + "模型上下文初始化失败" : "CPU模型上下文初始化失败",
            .gpu = use_gpu,
        };
    }
    std::shared_ptr<void> context{raw_context, [](void* pointer) {
        whisper_free(static_cast<whisper_context*>(pointer));
    }};
    if (whisper_is_multilingual(raw_context) == 0) {
        context.reset();
        return {.error = "本地模型不是多语言Whisper模型", .gpu = use_gpu};
    }
    return {
        .context = std::move(context),
        .backend_name = std::move(backend_name),
        .gpu = use_gpu,
    };
}

struct InferenceCallbacks {
    const std::atomic<bool>* cancel{nullptr};
    const TranscriptionProgressCallback* on_progress{nullptr};
    double window_start_seconds{0.0};
    double window_duration_seconds{0.0};
    double audio_duration_seconds{0.0};
    double* last_progress{nullptr};
};

[[nodiscard]] bool abort_inference(void* user_data) noexcept {
    const auto& callbacks = *static_cast<InferenceCallbacks*>(user_data);
    return callbacks.cancel != nullptr && callbacks.cancel->load(std::memory_order_acquire);
}

void report_inference_progress(whisper_context*, whisper_state*, int progress, void* user_data) noexcept {
    auto& callbacks = *static_cast<InferenceCallbacks*>(user_data);
    if (callbacks.on_progress == nullptr || !*callbacks.on_progress || callbacks.audio_duration_seconds <= 0.0) {
        return;
    }
    const double completed_in_window = callbacks.window_duration_seconds * std::clamp(progress, 0, 100) / 100.0;
    double fraction = (callbacks.window_start_seconds + completed_in_window) / callbacks.audio_duration_seconds;
    fraction = std::clamp(fraction, 0.0, 0.99);
    if (callbacks.last_progress != nullptr) {
        fraction = std::max(fraction, *callbacks.last_progress);
        *callbacks.last_progress = fraction;
    }
    try {
        (*callbacks.on_progress)("本地转写", fraction);
    } catch (...) {
    }
}

[[nodiscard]] int configured_thread_count(int configured_threads) {
    if (configured_threads > 0) {
        return configured_threads;
    }
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    return static_cast<int>(std::max(1u, hardware_threads == 0 ? 4u : hardware_threads));
}

[[nodiscard]] std::vector<TranscriptToken> read_context_tokens(whisper_context* context) {
    std::vector<TranscriptToken> tokens;
    const whisper_token first_control_token = whisper_token_eot(context);
    const int segment_count = whisper_full_n_segments(context);
    for (int segment_index = 0; segment_index < segment_count; ++segment_index) {
        const int token_count = whisper_full_n_tokens(context, segment_index);
        for (int token_index = 0; token_index < token_count; ++token_index) {
            const whisper_token token_id = whisper_full_get_token_id(context, segment_index, token_index);
            const whisper_token_data token_data = whisper_full_get_token_data(context, segment_index, token_index);
            const char* token_text = whisper_full_get_token_text(context, segment_index, token_index);
            tokens.push_back({
                .text = token_text == nullptr ? std::string{} : std::string{token_text},
                .start_seconds = static_cast<double>(token_data.t0) / 100.0,
                .end_seconds = static_cast<double>(token_data.t1) / 100.0,
                .control = token_id >= first_control_token,
            });
        }
    }
    return tokens;
}

void report_progress(
    const TranscriptionProgressCallback& on_progress,
    std::string_view status,
    double fraction,
    double& last_progress) {
    if (!on_progress) {
        return;
    }
    last_progress = std::max(last_progress, std::clamp(fraction, 0.0, 1.0));
    on_progress(status, last_progress);
}

class ScopedApiWindowFiles {
public:
    ScopedApiWindowFiles(std::filesystem::path audio_path, std::filesystem::path result_directory)
        : audio_path_(std::move(audio_path)),
          result_directory_(std::move(result_directory)) {}

    ~ScopedApiWindowFiles() {
        std::error_code error_code;
        std::filesystem::remove(audio_path_, error_code);
        std::filesystem::remove_all(result_directory_, error_code);
    }

    ScopedApiWindowFiles(const ScopedApiWindowFiles&) = delete;
    ScopedApiWindowFiles& operator=(const ScopedApiWindowFiles&) = delete;

private:
    std::filesystem::path audio_path_;
    std::filesystem::path result_directory_;
};

}

std::vector<LocalAsrGpuDevice> enumerate_local_asr_gpu_devices() {
    std::vector<LocalAsrGpuDevice> devices;
    int gpu_index = 0;
    for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
        ggml_backend_dev_t device = ggml_backend_dev_get(index);
        if (!is_gpu_device(device)) {
            continue;
        }
        devices.push_back({
            .index = gpu_index,
            .name = gpu_device_name(device),
        });
        ++gpu_index;
    }
    return devices;
}

WhisperCppContextSelection select_whisper_cpp_context(
    const std::filesystem::path& model_path,
    LocalAsrCompute compute,
    int gpu_device,
    const WhisperCppContextFactory& factory) {
    WhisperCppContextSelection selection;
    if (!factory) {
        selection.error = "Whisper上下文工厂不可用";
        return selection;
    }

    if (compute != LocalAsrCompute::cpu) {
        ++selection.creation_attempts;
        WhisperCppContextAttempt gpu_attempt = factory(model_path, true, std::max(gpu_device, 0));
        if (gpu_attempt.context && gpu_attempt.error.empty() && gpu_attempt.gpu) {
            selection.context = std::move(gpu_attempt.context);
            selection.runtime.backend = ActualTranscriptionBackend::gpu;
            selection.runtime.backend_name = std::move(gpu_attempt.backend_name);
            return selection;
        }
        selection.runtime.fallback_reason = gpu_attempt.error.empty()
            ? std::string{"GPU上下文初始化失败"}
            : std::move(gpu_attempt.error);
        gpu_attempt.context.reset();
    }

    ++selection.creation_attempts;
    WhisperCppContextAttempt cpu_attempt = factory(model_path, false, 0);
    if (!cpu_attempt.context || !cpu_attempt.error.empty()) {
        selection.error = cpu_attempt.error.empty() ? std::string{"CPU上下文初始化失败"} : std::move(cpu_attempt.error);
        return selection;
    }
    selection.context = std::move(cpu_attempt.context);
    selection.runtime.backend = ActualTranscriptionBackend::cpu;
    selection.runtime.backend_name = cpu_attempt.backend_name.empty() ? std::string{"CPU"} : std::move(cpu_attempt.backend_name);
    return selection;
}

struct WhisperCppTranscriber::Implementation {
    std::filesystem::path model_path;
    std::filesystem::path vad_model_path;
    WhisperCppContextSelection context;
    std::string initialization_error;
    std::vector<std::string> initialization_diagnostics;
};

WhisperCppTranscriber::WhisperCppTranscriber(
    const Config& configuration,
    WhisperCppContextFactory context_factory)
    : implementation_(std::make_unique<Implementation>()) {
    const ModelManifestEntry* model = find_model_manifest_entry(configuration.local_asr_model);
    if (model == nullptr || model->kind != ManagedModelKind::speech_recognition) {
        implementation_->initialization_error = "不支持的本地语音模型：" + configuration.local_asr_model;
        return;
    }
    const std::filesystem::path model_directory = resolve_model_directory(configuration.local_asr_model_dir);
    const ModelStatus model_status = inspect_model(*model, model_directory);
    if (model_status.availability == ModelAvailability::missing) {
        implementation_->initialization_error = "本地语音模型未下载，请在设置中下载";
        return;
    }
    if (model_status.availability == ModelAvailability::corrupt) {
        implementation_->initialization_error = "本地语音模型损坏，请重新下载";
        return;
    }
    implementation_->model_path = model_status.path;

    if (configuration.local_asr_use_vad) {
        const ModelStatus vad_status = inspect_model(default_vad_model(), model_directory);
        if (vad_status.availability == ModelAvailability::available) {
            implementation_->vad_model_path = vad_status.path;
        } else {
            implementation_->initialization_diagnostics.emplace_back("VAD模型不可用，本次转写不启用VAD");
        }
    }

    if (!context_factory) {
        context_factory = create_native_context;
    }
    implementation_->context = select_whisper_cpp_context(
        implementation_->model_path,
        configuration.local_asr_compute,
        configuration.local_asr_gpu_device,
        context_factory);
    implementation_->initialization_error = implementation_->context.error;
}

WhisperCppTranscriber::~WhisperCppTranscriber() = default;
WhisperCppTranscriber::WhisperCppTranscriber(WhisperCppTranscriber&&) noexcept = default;
WhisperCppTranscriber& WhisperCppTranscriber::operator=(WhisperCppTranscriber&&) noexcept = default;

bool WhisperCppTranscriber::ready() const noexcept {
    return implementation_ && implementation_->context.context && implementation_->initialization_error.empty();
}

std::string_view WhisperCppTranscriber::error() const noexcept {
    return implementation_ ? std::string_view{implementation_->initialization_error} : std::string_view{"本地转写器未初始化"};
}

const TranscriptionRuntime& WhisperCppTranscriber::runtime() const noexcept {
    static const TranscriptionRuntime not_initialized{};
    return implementation_ ? implementation_->context.runtime : not_initialized;
}

int WhisperCppTranscriber::model_load_attempts() const noexcept {
    return implementation_ ? implementation_->context.creation_attempts : 0;
}

TranscriptionResult WhisperCppTranscriber::transcribe_wav(
    const Config& configuration,
    const std::filesystem::path& audio_path,
    std::string_view source_language,
    const std::atomic<bool>& cancel,
    TranscriptionProgressCallback on_progress) {
    TranscriptionResult result;
    result.runtime = runtime();
    if (!ready()) {
        result.message = std::string{error()};
        return result;
    }
    if (!source_language.empty() && source_language != "auto") {
        const std::string language{source_language};
        if (whisper_lang_id(language.c_str()) < 0) {
            result.message = "本地Whisper不支持源语言：" + language;
            return result;
        }
    }

    WavWindowReader reader{audio_path};
    if (!reader.valid()) {
        result.message = reader.error();
        return result;
    }
    const double audio_duration = reader.duration_seconds();
    if (audio_duration <= 0.0) {
        result.message = "音频为空";
        return result;
    }

    result.diagnostics = implementation_->initialization_diagnostics;
    whisper_context* context = static_cast<whisper_context*>(implementation_->context.context.get());
    const std::string language = source_language.empty() ? std::string{"auto"} : std::string{source_language};
    const std::string vad_model_path = path_to_utf8(implementation_->vad_model_path);
    double last_progress = 0.0;
    while (const std::optional<AudioWindow> window = reader.read_next(local_window_seconds, window_overlap_seconds)) {
        if (cancel.load(std::memory_order_acquire)) {
            result.message = "已取消";
            result.words.clear();
            return result;
        }

        std::vector<float> samples = pcm16_to_float(window->samples);
        InferenceCallbacks callbacks{
            .cancel = &cancel,
            .on_progress = &on_progress,
            .window_start_seconds = window->start_seconds,
            .window_duration_seconds = window->duration_seconds,
            .audio_duration_seconds = audio_duration,
            .last_progress = &last_progress,
        };
        whisper_full_params parameters = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        parameters.n_threads = configured_thread_count(configuration.local_asr_threads);
        parameters.translate = false;
        parameters.no_context = true;
        parameters.no_timestamps = false;
        parameters.print_special = false;
        parameters.print_progress = false;
        parameters.print_realtime = false;
        parameters.print_timestamps = false;
        parameters.token_timestamps = true;
        parameters.language = language.c_str();
        parameters.detect_language = language == "auto";
        parameters.progress_callback = report_inference_progress;
        parameters.progress_callback_user_data = &callbacks;
        parameters.abort_callback = abort_inference;
        parameters.abort_callback_user_data = &callbacks;
        parameters.vad = !vad_model_path.empty();
        parameters.vad_model_path = vad_model_path.empty() ? nullptr : vad_model_path.c_str();

        const int inference_status = whisper_full(
            context,
            parameters,
            samples.data(),
            static_cast<int>(samples.size()));
        samples.clear();
        samples.shrink_to_fit();
        if (cancel.load(std::memory_order_acquire)) {
            result.message = "已取消";
            result.words.clear();
            return result;
        }
        if (inference_status != 0) {
            result.message = "whisper.cpp推理失败，错误码" + std::to_string(inference_status);
            result.words.clear();
            return result;
        }

        const std::vector<TranscriptToken> tokens = read_context_tokens(context);
        std::string detected_language;
        if (language == "auto") {
            const int language_id = whisper_full_lang_id(context);
            if (language_id >= 0) {
                detected_language = whisper_lang_str(language_id);
            }
        }
        const std::string_view aggregation_language = detected_language.empty() ? source_language : detected_language;
        TokenAggregationResult window_result =
            aggregate_transcript_tokens(tokens, aggregation_language, window->duration_seconds);
        for (TranscriptWord& word : window_result.words) {
            word.start_seconds += window->start_seconds;
            word.end_seconds += window->start_seconds;
        }
        result.words = merge_overlapping_transcript_words(result.words, window_result.words, window->start_seconds);
        result.diagnostics.insert(
            result.diagnostics.end(),
            std::make_move_iterator(window_result.diagnostics.begin()),
            std::make_move_iterator(window_result.diagnostics.end()));
    }
    if (!reader.error().empty()) {
        result.message = reader.error();
        result.words.clear();
        return result;
    }
    if (result.words.empty()) {
        result.message = "本地转写未生成带时间戳的文本";
        return result;
    }
    report_progress(on_progress, "本地转写", 1.0, last_progress);
    result.success = true;
    result.message = result.runtime.backend == ActualTranscriptionBackend::gpu ? "本地转写（GPU）" : "本地转写（CPU）";
    return result;
}

TranscriptionResult transcribe_wav_with_api(
    const WhisperApiTranscriber& transcriber,
    const Config& configuration,
    const std::filesystem::path& audio_path,
    const std::filesystem::path& working_directory,
    std::string_view source_language,
    const std::atomic<bool>& cancel,
    TranscriptionProgressCallback on_progress) {
    TranscriptionResult result;
    result.runtime = {
        .backend = ActualTranscriptionBackend::api,
        .backend_name = "Whisper API",
    };
    WavWindowReader reader{audio_path};
    if (!reader.valid()) {
        result.message = reader.error();
        return result;
    }
    const double audio_duration = reader.duration_seconds();
    if (audio_duration <= 0.0) {
        result.message = "音频为空";
        return result;
    }

    std::error_code error_code;
    std::filesystem::create_directories(working_directory, error_code);
    if (error_code) {
        result.message = "无法创建转写工作目录";
        return result;
    }

    double last_progress = 0.0;
    while (const std::optional<AudioWindow> window = reader.read_next(api_window_seconds, window_overlap_seconds)) {
        if (cancel.load(std::memory_order_acquire)) {
            result.message = "已取消";
            result.words.clear();
            return result;
        }
        const std::filesystem::path window_path = working_directory / ".api-transcribe-window.wav";
        const std::filesystem::path result_directory = working_directory / ".api-transcribe-window-result";
        ScopedApiWindowFiles cleanup{window_path, result_directory};
        if (!write_pcm16_wav(window_path, window->samples)) {
            result.message = "无法写入API转写临时音频";
            result.words.clear();
            return result;
        }

        const double window_base = window->start_seconds / audio_duration;
        const double window_weight = window->duration_seconds / audio_duration;
        const TranscriptionOutcome outcome = transcriber.transcribe(
            configuration,
            window_path,
            result_directory,
            source_language,
            cancel,
            [&](TranscriptionStage stage) {
                double stage_fraction = 0.0;
                switch (stage) {
                case TranscriptionStage::reading_audio:
                    stage_fraction = 0.1;
                    break;
                case TranscriptionStage::preparing_request:
                    stage_fraction = 0.25;
                    break;
                case TranscriptionStage::requesting:
                    stage_fraction = 0.8;
                    break;
                case TranscriptionStage::writing_result:
                    stage_fraction = 0.95;
                    break;
                }
                report_progress(on_progress, format_transcription_progress(stage), window_base + stage_fraction * window_weight, last_progress);
            });
        if (!outcome.success) {
            result.message = outcome.message;
            result.words.clear();
            return result;
        }

        TranscriptReadResult window_result = load_transcript_words(outcome.transcript_path);
        if (!window_result.success) {
            result.message = window_result.message;
            result.words.clear();
            return result;
        }
        std::vector<TranscriptWord> absolute_words;
        absolute_words.reserve(window_result.words.size());
        for (TranscriptWord& word : window_result.words) {
            if (!std::isfinite(word.start_seconds)
                || !std::isfinite(word.end_seconds)
                || word.start_seconds < 0.0
                || word.end_seconds < word.start_seconds
                || word.start_seconds > window->duration_seconds) {
                continue;
            }
            word.end_seconds = std::min(word.end_seconds, window->duration_seconds);
            word.start_seconds += window->start_seconds;
            word.end_seconds += window->start_seconds;
            absolute_words.push_back(std::move(word));
        }
        result.words = merge_overlapping_transcript_words(result.words, absolute_words, window->start_seconds);
    }
    if (!reader.error().empty()) {
        result.message = reader.error();
        result.words.clear();
        return result;
    }
    if (result.words.empty()) {
        result.message = "Whisper API响应缺少可用词时间戳";
        return result;
    }
    report_progress(on_progress, "API转写", 1.0, last_progress);
    result.success = true;
    result.message = "API转写";
    return result;
}

}
