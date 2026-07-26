#include "swb/task.h"

#include "swb/audio.h"
#include "swb/output.h"
#include "swb/segment.h"
#include "swb/source_subtitle_resolution.h"
#include "swb/srt.h"
#include "swb/translate.h"
#include "swb/transcribe.h"
#include "swb/workspace.h"
#include "swb/ytdlp.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace swb {

namespace {

constexpr std::array<std::string_view, step_count> step_labels{
    std::string_view{"下载视频"},
    std::string_view{"获取源语言字幕"},
    std::string_view{"翻译为中文"},
    std::string_view{"输出"},
};

constexpr std::string_view audio_filename = "audio.wav";
constexpr std::string_view transcript_filename = "transcript.json";
constexpr std::string_view source_subtitle_filename = "en.srt";
constexpr std::string_view manual_subtitle_filename = "manual.en.srt";
constexpr std::string_view automatic_subtitle_filename = "auto.en.srt";
constexpr std::string_view translated_subtitle_filename = "zh.srt";
constexpr int retry_delay_ms = 600;
constexpr std::uint64_t task_progress_scale = 1000;

[[nodiscard]] double clamp_progress(double progress) noexcept {
    return std::clamp(progress, 0.0, 1.0);
}

[[nodiscard]] double step_progress_value(const StepInfo& step) noexcept {
    switch (step.state) {
    case StepState::waiting:
        return 0.0;
    case StepState::completed:
        return 1.0;
    case StepState::in_progress:
    case StepState::failed:
        return clamp_progress(step.progress);
    }
    return 0.0;
}

[[nodiscard]] double fraction_from_count(std::size_t position, std::size_t count) noexcept {
    if (count == 0) {
        return 0.0;
    }
    return clamp_progress(static_cast<double>(position) / static_cast<double>(count));
}

[[nodiscard]] double download_step_progress(const DownloadProgress& progress) noexcept {
    if (progress.downloaded_bytes < 0 || progress.total_bytes <= 0) {
        return 0.0;
    }
    return clamp_progress(static_cast<double>(progress.downloaded_bytes) / static_cast<double>(progress.total_bytes));
}

[[nodiscard]] double translate_step_progress(std::size_t position, std::size_t count) noexcept {
    return fraction_from_count(position, count);
}

[[nodiscard]] std::string format_retry_status(std::string_view status, int retry_index, int retry_count) {
    std::string formatted;
    if (!status.empty()) {
        formatted.assign(status);
        formatted.append("，");
    }
    formatted.append("重试中（");
    formatted.append(std::to_string(retry_index));
    formatted.push_back('/');
    formatted.append(std::to_string(retry_count));
    formatted.append("）");
    return formatted;
}

[[nodiscard]] bool copy_file_replace(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error_code;
    std::filesystem::remove(destination, error_code);
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error_code);
    return !error_code;
}

}

std::string_view step_label(StepId step) {
    return step_labels[static_cast<std::size_t>(step)];
}

TaskProgress summarize_task_progress(std::span<const StepInfo, step_count> steps) noexcept {
    TaskProgress progress{
        .total_units = static_cast<std::uint64_t>(steps.size()) * task_progress_scale,
    };
    for (const StepInfo& step : steps) {
        progress.completed_units += static_cast<std::uint64_t>(step_progress_value(step) * static_cast<double>(task_progress_scale));
    }
    return progress;
}

TaskTerminalSummary summarize_task_state(std::span<const StepInfo, step_count> steps) {
    bool all_waiting = true;
    bool all_failed_steps_canceled = true;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const StepInfo& step = steps[index];
        if (step.state != StepState::waiting) {
            all_waiting = false;
        }
        if (step.state == StepState::in_progress) {
            return {
                .state = TaskState::running,
                .step = static_cast<StepId>(index),
                .status = step.status,
            };
        }
        if (step.state == StepState::failed) {
            if (step.status != "已取消") {
                all_failed_steps_canceled = false;
            }
            return {
                .state = step.status == "已取消" && all_failed_steps_canceled ? TaskState::canceled : TaskState::failed,
                .step = static_cast<StepId>(index),
                .status = step.status,
            };
        }
    }

    if (all_waiting) {
        return {};
    }
    if (steps.back().state == StepState::completed) {
        return {
            .state = TaskState::succeeded,
            .step = StepId::output,
            .status = steps.back().status,
        };
    }
    return {
        .state = TaskState::failed,
    };
}

struct Task::ExecutionContext {
    YtDlp yt_dlp{YtDlp::resolve_executable()};
    Ffmpeg audio_converter{Ffmpeg::resolve_executable()};
    SubtitleOutputRenderer output_renderer{audio_converter.executable()};
    WhisperApiTranscriber api_transcriber{};
    std::unique_ptr<WhisperCppTranscriber> local_transcriber;
    SubtitleTranslator translator{};
    std::filesystem::path video_path;
    bool is_remote_source{false};
    std::string title;
    std::string source_key;
    WorkingDirectoryState working_directory_state;
    bool has_platform_manual_subtitle{false};
    bool has_transcription{false};
    bool has_platform_automatic_subtitle{false};
    std::string subtitle_status;
    std::string fetch_failure_status{"无可用字幕"};
};

Task::Task() = default;

Task::~Task() {
    cancel_.store(true, std::memory_order_release);
    join_worker();
}

void Task::start(std::string source, Config configuration, TaskStartOptions options) {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    join_worker();

    source_ = std::move(source);
    configuration_ = std::move(configuration);
    output_name_ = std::move(options.output_name);
    reuse_working_directory_ = options.reuse_working_directory;
    {
        std::scoped_lock lock(mutex_);
        detected_title_.clear();
        transcription_runtime_ = {};
    }
    working_directory_.clear();
    cancel_.store(false, std::memory_order_release);
    reset_steps();
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&Task::run, this);
}

void Task::cancel() {
    cancel_.store(true, std::memory_order_release);
}

Task::Snapshot Task::read() const {
    std::scoped_lock lock(mutex_);
    return steps_;
}

std::string Task::detected_title() const {
    std::scoped_lock lock(mutex_);
    return detected_title_;
}

std::filesystem::path Task::working_directory() const {
    return working_directory_;
}

TranscriptionRuntime Task::transcription_runtime() const {
    std::scoped_lock lock(mutex_);
    return transcription_runtime_;
}

void Task::set_transcription_runtime(TranscriptionRuntime runtime) {
    std::scoped_lock lock(mutex_);
    transcription_runtime_ = std::move(runtime);
}

void Task::reset_steps() {
    std::scoped_lock lock(mutex_);
    for (StepInfo& step_info : steps_) {
        step_info.state = StepState::waiting;
        step_info.status.clear();
        step_info.progress = 0.0;
    }
}

void Task::set_step(StepId step, StepState state, std::string status, std::optional<double> progress) {
    std::scoped_lock lock(mutex_);
    StepInfo& step_info = steps_[static_cast<std::size_t>(step)];
    const StepState previous_state = step_info.state;
    step_info.state = state;
    step_info.status = std::move(status);
    if (progress.has_value()) {
        const double next_progress = clamp_progress(*progress);
        step_info.progress = state == StepState::in_progress && previous_state == StepState::in_progress
            ? std::max(step_info.progress, next_progress)
            : next_progress;
        return;
    }
    switch (state) {
    case StepState::waiting:
        step_info.progress = 0.0;
        break;
    case StepState::completed:
        step_info.progress = 1.0;
        break;
    case StepState::in_progress:
    case StepState::failed:
        break;
    }
}

void Task::set_detected_title(std::string detected_title) {
    std::scoped_lock lock(mutex_);
    detected_title_ = std::move(detected_title);
}

bool Task::is_canceled() const noexcept {
    return cancel_.load(std::memory_order_acquire);
}

Task::AttemptResult Task::fail_step(StepId step, std::string status) const {
    return {
        .outcome = AttemptOutcome::failed,
        .failure = AttemptFailure{
            .primary_step = step,
            .primary_status = std::move(status),
        },
    };
}

Task::AttemptResult Task::cancel_step(StepId step) const {
    return {
        .outcome = AttemptOutcome::canceled,
        .failure = AttemptFailure{
            .primary_step = step,
            .primary_status = "已取消",
        },
    };
}

Task::AttemptResult Task::fail_source_pipeline(std::string fetch_status, std::string translate_status) const {
    return {
        .outcome = AttemptOutcome::failed,
        .failure = AttemptFailure{
            .primary_step = StepId::fetch_source_subtitle,
            .primary_status = std::move(fetch_status),
            .secondary_step = StepId::translate,
            .secondary_status = std::move(translate_status),
        },
    };
}

void Task::refresh_working_directory_state(ExecutionContext& context) const {
    context.working_directory_state = inspect_working_directory_state(working_directory_);
}

bool Task::build_source_subtitle_from_transcript(ExecutionContext& context) {
    if (!context.working_directory_state.transcript_path.has_value()) {
        return false;
    }
    set_step(StepId::fetch_source_subtitle, StepState::in_progress, "切分字幕", 0.92);
    const TranscriptReadResult transcript = load_transcript_words(*context.working_directory_state.transcript_path);
    if (!transcript.success) {
        return false;
    }
    const std::vector<SubtitleCue> cues = segment_transcript(transcript.words);
    if (cues.empty()) {
        return false;
    }
    set_step(StepId::fetch_source_subtitle, StepState::in_progress, "写入SRT", 0.97);
    if (!save_srt(cues, working_directory_ / std::string{source_subtitle_filename})) {
        return false;
    }
    if (context.working_directory_state.automatic_subtitle_path.has_value()) {
        std::error_code cleanup_error;
        std::filesystem::remove(*context.working_directory_state.automatic_subtitle_path, cleanup_error);
    }
    refresh_working_directory_state(context);
    return true;
}

bool Task::promote_existing_subtitle(ExecutionContext& context, const std::filesystem::path& source_subtitle) {
    const std::filesystem::path destination = working_directory_ / std::string{source_subtitle_filename};
    if (source_subtitle == destination) {
        refresh_working_directory_state(context);
        return true;
    }
    const bool copied = copy_file_replace(source_subtitle, destination);
    if (copied) {
        refresh_working_directory_state(context);
    }
    return copied;
}

Task::AttemptResult Task::prepare_execution_context(ExecutionContext& context) {
    if (is_canceled()) {
        return cancel_step(StepId::download_video);
    }

    set_step(StepId::download_video, StepState::in_progress, "准备工作目录", 0.0);
    context.is_remote_source = !reuse_working_directory_ && is_url(source_);

    if (reuse_working_directory_) {
        std::error_code error_code;
        std::filesystem::path selected_working_directory{std::filesystem::u8path(source_)};
        if (!std::filesystem::exists(selected_working_directory, error_code) || !std::filesystem::is_directory(selected_working_directory, error_code)) {
            return fail_step(StepId::download_video, "工作目录不存在");
        }
        selected_working_directory = std::filesystem::absolute(selected_working_directory, error_code);
        if (!error_code) {
            working_directory_ = std::move(selected_working_directory);
        } else {
            working_directory_ = std::filesystem::u8path(source_);
        }
        context.title = path_to_utf8(working_directory_.filename());
        if (context.title.empty()) {
            context.title = "workspace";
        }
        set_detected_title(context.title);
        context.source_key = path_to_utf8(working_directory_);
    } else if (context.is_remote_source) {
        if (context.yt_dlp.executable().empty()) {
            return fail_step(StepId::download_video, "未找到yt-dlp.exe");
        }
        const std::optional<std::string> fetched_title = context.yt_dlp.fetch_title(source_, cancel_);
        if (!fetched_title) {
            if (is_canceled()) {
                return cancel_step(StepId::download_video);
            }
            return fail_step(StepId::download_video, "无法获取视频标题");
        }
        context.title = *fetched_title;
        set_detected_title(context.title);
        context.source_key = source_;
    } else {
        std::error_code error_code;
        const std::filesystem::path local_path{std::filesystem::u8path(source_)};
        if (!std::filesystem::exists(local_path, error_code)) {
            return fail_step(StepId::download_video, "文件不存在");
        }
        context.title = path_to_utf8(local_path.stem());
        set_detected_title(context.title);
        context.source_key = path_to_utf8(std::filesystem::absolute(local_path, error_code));
        context.video_path = local_path;
    }

    if (!reuse_working_directory_) {
        const std::string_view working_directory_stem = output_name_.empty() ? std::string_view{context.title} : std::string_view{output_name_};
        const std::string directory_name = make_workdir_name(working_directory_stem, context.source_key);
        working_directory_ = resolve_output_root(configuration_.output_dir) / std::filesystem::u8path(directory_name);
        std::error_code error_code;
        std::filesystem::create_directories(working_directory_, error_code);
        if (error_code) {
            return fail_step(StepId::download_video, "无法创建工作目录");
        }
    }

    refresh_working_directory_state(context);
    const SourceSubtitleResumeStage source_stage_before_download = classify_source_subtitle_resume_stage(context.working_directory_state);

    if (context.working_directory_state.video_path.has_value()) {
        context.video_path = *context.working_directory_state.video_path;
        set_step(StepId::download_video, StepState::completed, "复用现有文件");
        return {};
    }
    if (source_stage_before_download != SourceSubtitleResumeStage::unavailable) {
        set_step(StepId::download_video, StepState::completed, "复用现有工件");
        return {};
    }
    if (context.is_remote_source) {
        if (is_canceled()) {
            return cancel_step(StepId::download_video);
        }
        if (context.audio_converter.executable().empty()) {
            return fail_step(StepId::download_video, "未找到ffmpeg");
        }
        set_step(StepId::download_video, StepState::in_progress, "下载视频", 0.0);
        const auto progress_callback = [this](const DownloadProgress& progress) {
            std::string status_message;
            if (progress.speed_bps >= 0.0) {
                status_message.append(format_bytes(static_cast<std::int64_t>(progress.speed_bps)));
                status_message.append("/s");
            }
            if (progress.downloaded_bytes >= 0) {
                if (!status_message.empty()) {
                    status_message.append(" | ");
                }
                status_message.append(format_bytes(progress.downloaded_bytes));
                if (progress.total_bytes >= 0) {
                    status_message.append(" / ");
                    status_message.append(format_bytes(progress.total_bytes));
                }
            }
            std::string status = status_message.empty() ? std::string{"下载中"} : ("下载中（" + status_message + "）");
            set_step(StepId::download_video, StepState::in_progress, std::move(status), download_step_progress(progress));
        };
        const std::optional<std::filesystem::path> downloaded_video = context.yt_dlp.download_video(source_, working_directory_, cancel_, progress_callback);
        if (!downloaded_video) {
            if (is_canceled()) {
                return cancel_step(StepId::download_video);
            }
            return fail_step(StepId::download_video, "下载失败");
        }
        context.video_path = *downloaded_video;
        refresh_working_directory_state(context);
        set_step(StepId::download_video, StepState::completed, "完成");
        return {};
    }
    if (!reuse_working_directory_) {
        set_step(StepId::download_video, StepState::completed, "本地文件");
        return {};
    }
    return fail_step(StepId::download_video, "工作目录缺少可继续工件");
}

Task::AttemptResult Task::acquire_source_subtitles(ExecutionContext& context) {
    if (is_canceled()) {
        return cancel_step(StepId::fetch_source_subtitle);
    }

    context.has_platform_manual_subtitle = false;
    context.has_transcription = false;
    context.has_platform_automatic_subtitle = false;
    context.subtitle_status.clear();
    context.fetch_failure_status = "无可用字幕";

    if (context.working_directory_state.manual_subtitle_path.has_value()) {
        if (!promote_existing_subtitle(context, *context.working_directory_state.manual_subtitle_path)) {
            return fail_source_pipeline("无法写入en.srt", "无可用字幕");
        }
        context.has_platform_manual_subtitle = true;
        context.subtitle_status = "平台人工字幕";
    } else if (context.working_directory_state.transcript_path.has_value() && context.working_directory_state.source_subtitle_path.has_value()) {
        context.has_transcription = true;
        context.subtitle_status = "转写";
    } else if (context.working_directory_state.source_subtitle_path.has_value() && !context.working_directory_state.automatic_subtitle_path.has_value()) {
        context.has_platform_manual_subtitle = true;
        context.subtitle_status = "复用现有字幕";
    }

    if (!context.has_platform_manual_subtitle && !context.has_transcription && context.is_remote_source) {
        set_step(StepId::fetch_source_subtitle, StepState::in_progress, "查询平台人工字幕", 0.05);
        if (const std::optional<SubtitleDownload> subtitle = context.yt_dlp.download_subtitle(
                source_,
                configuration_.source_lang,
                working_directory_,
                cancel_,
                SubtitleOrigin::manual);
            subtitle.has_value()) {
            if (!promote_existing_subtitle(context, subtitle->srt_path)) {
                return fail_source_pipeline("无法写入en.srt", "无可用字幕");
            }
            context.has_platform_manual_subtitle = true;
            context.subtitle_status = "平台人工字幕";
        } else if (!is_canceled()) {
            set_step(StepId::fetch_source_subtitle, StepState::in_progress, "查询平台自动字幕", 0.1);
            if (const std::optional<SubtitleDownload> subtitle = context.yt_dlp.download_subtitle(
                    source_,
                    configuration_.source_lang,
                    working_directory_,
                    cancel_,
                    SubtitleOrigin::automatic);
                subtitle.has_value()) {
                refresh_working_directory_state(context);
            }
        }
    }

    if (!context.has_platform_manual_subtitle && !context.has_transcription) {
        refresh_working_directory_state(context);
        switch (classify_source_subtitle_resume_stage(context.working_directory_state)) {
        case SourceSubtitleResumeStage::ready_source_subtitle:
            if (context.working_directory_state.manual_subtitle_path.has_value()) {
                if (!promote_existing_subtitle(context, *context.working_directory_state.manual_subtitle_path)) {
                    return fail_source_pipeline("无法写入en.srt", "无可用字幕");
                }
                context.has_platform_manual_subtitle = true;
                context.subtitle_status = "平台人工字幕";
            } else if (context.working_directory_state.source_subtitle_path.has_value() && !context.working_directory_state.automatic_subtitle_path.has_value()) {
                context.has_platform_manual_subtitle = true;
                context.subtitle_status = context.subtitle_status.empty() ? "复用现有字幕" : context.subtitle_status;
            }
            break;
        case SourceSubtitleResumeStage::needs_segmentation:
            if (build_source_subtitle_from_transcript(context)) {
                context.has_transcription = true;
                context.subtitle_status = "转写";
            } else {
                context.fetch_failure_status = "transcript.json生成字幕失败";
            }
            break;
        case SourceSubtitleResumeStage::needs_transcription:
        case SourceSubtitleResumeStage::needs_audio_extraction:
            if (!context.working_directory_state.audio_path.has_value()) {
                if (context.audio_converter.executable().empty()) {
                    context.fetch_failure_status = "未找到ffmpeg";
                    break;
                }
                if (context.video_path.empty()) {
                    context.fetch_failure_status = "工作目录缺少视频文件";
                    break;
                }
                set_step(StepId::fetch_source_subtitle, StepState::in_progress, "抽取音频", 0.2);
                if (!context.audio_converter.extract_wav_16k_mono(context.video_path, working_directory_ / std::string{audio_filename}, cancel_)) {
                    if (is_canceled()) {
                        return cancel_step(StepId::fetch_source_subtitle);
                    }
                    context.fetch_failure_status = "音频抽取失败";
                    break;
                }
                refresh_working_directory_state(context);
            }
            if (!context.working_directory_state.transcript_path.has_value() && context.working_directory_state.audio_path.has_value()) {
                const auto progress_callback = [this](std::string_view status, double progress) {
                    set_step(
                        StepId::fetch_source_subtitle,
                        StepState::in_progress,
                        std::string{status},
                        0.25 + clamp_progress(progress) * 0.65);
                };

                TranscriptionResult transcription;
                if (configuration_.transcription_backend == TranscriptionBackend::local) {
                    if (!context.local_transcriber) {
                        set_step(StepId::fetch_source_subtitle, StepState::in_progress, "加载本地语音模型", 0.25);
                        context.local_transcriber = std::make_unique<WhisperCppTranscriber>(configuration_);
                    }
                    set_transcription_runtime(context.local_transcriber->runtime());
                    if (!context.local_transcriber->ready()) {
                        context.fetch_failure_status = std::string{context.local_transcriber->error()};
                        break;
                    }
                    transcription = context.local_transcriber->transcribe_wav(
                        configuration_,
                        *context.working_directory_state.audio_path,
                        configuration_.source_lang,
                        cancel_,
                        progress_callback);
                } else {
                    transcription = transcribe_wav_with_api(
                        context.api_transcriber,
                        configuration_,
                        *context.working_directory_state.audio_path,
                        working_directory_,
                        configuration_.source_lang,
                        cancel_,
                        progress_callback);
                }
                set_transcription_runtime(transcription.runtime);
                if (!transcription.success) {
                    if (transcription.message == "已取消") {
                        return cancel_step(StepId::fetch_source_subtitle);
                    }
                    context.fetch_failure_status = transcription.message;
                    break;
                }
                if (is_canceled()) {
                    return cancel_step(StepId::fetch_source_subtitle);
                }
                set_step(StepId::fetch_source_subtitle, StepState::in_progress, "提交转写结果", 0.91);
                if (!save_transcript_words_atomic(
                        transcription.words,
                        working_directory_ / std::string{transcript_filename})) {
                    context.fetch_failure_status = "无法写入transcript.json";
                    break;
                }
                context.subtitle_status = transcription.message;
                refresh_working_directory_state(context);
            }
            if (context.working_directory_state.transcript_path.has_value() && build_source_subtitle_from_transcript(context)) {
                context.has_transcription = true;
                if (context.subtitle_status.empty()) {
                    context.subtitle_status = "转写";
                }
            } else if (context.fetch_failure_status == "无可用字幕") {
                context.fetch_failure_status = "transcript.json生成字幕失败";
            }
            break;
        case SourceSubtitleResumeStage::unavailable:
            break;
        }
    }

    if (!context.has_platform_manual_subtitle && !context.has_transcription && context.working_directory_state.automatic_subtitle_path.has_value()) {
        if (!promote_existing_subtitle(context, *context.working_directory_state.automatic_subtitle_path)) {
            return fail_source_pipeline("无法写入en.srt", "无可用字幕");
        }
        context.has_platform_automatic_subtitle = true;
        context.subtitle_status = "平台自动字幕";
    }

    if (!context.has_platform_manual_subtitle && !context.has_transcription && !context.has_platform_automatic_subtitle && context.working_directory_state.source_subtitle_path.has_value()) {
        context.has_platform_manual_subtitle = true;
        context.subtitle_status = "复用现有字幕";
    }

    if (!context.has_platform_manual_subtitle && !context.has_transcription && !context.has_platform_automatic_subtitle && context.fetch_failure_status == "无可用字幕") {
        if (context.working_directory_state.transcript_path.has_value()) {
            context.fetch_failure_status = "transcript.json生成字幕失败";
        } else if (context.working_directory_state.audio_path.has_value() || !context.video_path.empty()) {
            context.fetch_failure_status = "语音转写失败";
        }
    }

    const SourceSubtitleSelection source_subtitle = select_source_subtitle(
        context.has_platform_manual_subtitle,
        context.has_transcription,
        context.has_platform_automatic_subtitle);
    switch (source_subtitle) {
    case SourceSubtitleSelection::platform_manual_subtitle:
        if (context.subtitle_status.empty()) {
            context.subtitle_status = "平台人工字幕";
        }
        break;
    case SourceSubtitleSelection::transcription:
        if (context.subtitle_status.empty()) {
            context.subtitle_status = "转写";
        }
        break;
    case SourceSubtitleSelection::platform_automatic_subtitle:
        context.subtitle_status = "平台自动字幕";
        break;
    case SourceSubtitleSelection::unavailable:
        return fail_source_pipeline(std::move(context.fetch_failure_status), "无可用字幕");
    }

    set_step(StepId::fetch_source_subtitle, StepState::completed, std::move(context.subtitle_status));
    return {};
}

Task::AttemptResult Task::translate_source_subtitles(ExecutionContext& context) {
    if (is_canceled()) {
        return cancel_step(StepId::translate);
    }

    refresh_working_directory_state(context);
    if (context.working_directory_state.translated_subtitle_path.has_value()) {
        set_step(StepId::translate, StepState::completed, "复用现有翻译");
        return {};
    }
    if (!context.working_directory_state.source_subtitle_path.has_value()) {
        return fail_step(StepId::translate, "缺少en.srt");
    }

    set_step(StepId::translate, StepState::in_progress, "读取源字幕", 0.0);
    const SrtReadResult source_subtitles = load_srt(*context.working_directory_state.source_subtitle_path);
    if (!source_subtitles.success) {
        return fail_step(StepId::translate, source_subtitles.message);
    }

    const TranslationOutcome translation = context.translator.translate(
        configuration_,
        source_subtitles.cues,
        working_directory_,
        configuration_.source_lang,
        configuration_.target_lang,
        cancel_,
        [this](std::size_t chunk_position, std::size_t chunk_count) {
            set_step(
                StepId::translate,
                StepState::in_progress,
                format_translation_progress(chunk_position, chunk_count),
                translate_step_progress(chunk_position, chunk_count));
        });
    if (!translation.success) {
        if (translation.message == "已取消") {
            return cancel_step(StepId::translate);
        }
        return fail_step(StepId::translate, translation.message);
    }

    refresh_working_directory_state(context);
    set_step(StepId::translate, StepState::completed, translation.message);
    return {};
}

Task::AttemptResult Task::render_output_video(ExecutionContext& context) {
    if (is_canceled()) {
        return cancel_step(StepId::output);
    }

    refresh_working_directory_state(context);
    if (!context.working_directory_state.translated_subtitle_path.has_value()) {
        return fail_step(StepId::output, "缺少zh.srt");
    }
    if (context.video_path.empty() && context.working_directory_state.video_path.has_value()) {
        context.video_path = *context.working_directory_state.video_path;
    }

    set_step(StepId::output, StepState::in_progress, "读取目标字幕", 0.0);
    const SrtReadResult translated_subtitles = load_srt(*context.working_directory_state.translated_subtitle_path);
    if (!translated_subtitles.success) {
        return fail_step(StepId::output, translated_subtitles.message);
    }

    std::vector<SubtitleCue> source_subtitles;
    if (configuration_.bilingual_subtitles) {
        if (!context.working_directory_state.source_subtitle_path.has_value()) {
            return fail_step(StepId::output, "缺少en.srt");
        }
        set_step(StepId::output, StepState::in_progress, "读取源字幕", 0.05);
        const SrtReadResult source_read_result = load_srt(*context.working_directory_state.source_subtitle_path);
        if (!source_read_result.success) {
            return fail_step(StepId::output, source_read_result.message);
        }
        source_subtitles = source_read_result.cues;
    }

    set_step(StepId::output, StepState::in_progress, "硬编码中", 0.0);
    const OutputResult output = context.output_renderer.render(
        configuration_,
        context.video_path,
        source_subtitles,
        translated_subtitles.cues,
        working_directory_,
        cancel_,
        [this](const OutputProgress& progress) {
            set_step(StepId::output, StepState::in_progress, progress.status, progress.fraction);
        });
    if (!output.success) {
        if (output.message == "已取消") {
            return cancel_step(StepId::output);
        }
        return fail_step(StepId::output, output.message);
    }
    set_step(StepId::output, StepState::completed, output.message);
    return {};
}

Task::AttemptResult Task::run_once(ExecutionContext& context) {
    if (AttemptResult result = prepare_execution_context(context); result.outcome != AttemptOutcome::success) {
        return result;
    }
    if (AttemptResult result = acquire_source_subtitles(context); result.outcome != AttemptOutcome::success) {
        return result;
    }
    if (AttemptResult result = translate_source_subtitles(context); result.outcome != AttemptOutcome::success) {
        return result;
    }
    return render_output_video(context);
}

bool Task::sleep_or_cancel(int milliseconds) noexcept {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{milliseconds};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel_.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return cancel_.load(std::memory_order_acquire);
}

void Task::join_worker() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Task::run() {
    const int retry_count = std::max(configuration_.retry_count, 0);
    int retries_used = 0;
    ExecutionContext context;

    for (;;) {
        const AttemptResult attempt = run_once(context);
        switch (attempt.outcome) {
        case AttemptOutcome::success:
            running_.store(false, std::memory_order_release);
            return;
        case AttemptOutcome::canceled:
            if (attempt.failure.has_value()) {
                apply_attempt_failure(*attempt.failure);
            }
            running_.store(false, std::memory_order_release);
            return;
        case AttemptOutcome::failed:
            if (!attempt.failure.has_value()) {
                running_.store(false, std::memory_order_release);
                return;
            }
            if (retries_used >= retry_count) {
                apply_attempt_failure(*attempt.failure);
                running_.store(false, std::memory_order_release);
                return;
            }

            ++retries_used;
            apply_retry_status(*attempt.failure, retries_used, retry_count);
            if (sleep_or_cancel(retry_delay_ms)) {
                AttemptFailure canceled_failure = *attempt.failure;
                canceled_failure.primary_status = "已取消";
                if (canceled_failure.secondary_step.has_value()) {
                    canceled_failure.secondary_status = "已取消";
                }
                apply_attempt_failure(canceled_failure);
                running_.store(false, std::memory_order_release);
                return;
            }

            reset_steps();
            break;
        }
    }
}

void Task::apply_attempt_failure(const AttemptFailure& failure) {
    set_step(failure.primary_step, StepState::failed, failure.primary_status);
    if (failure.secondary_step.has_value()) {
        set_step(*failure.secondary_step, StepState::failed, failure.secondary_status);
    }
}

void Task::apply_retry_status(const AttemptFailure& failure, int retry_index, int retry_count) {
    set_step(
        failure.primary_step,
        StepState::failed,
        format_retry_status(failure.primary_status, retry_index, retry_count));
    if (failure.secondary_step.has_value()) {
        set_step(
            *failure.secondary_step,
            StepState::failed,
            format_retry_status(failure.secondary_status, retry_index, retry_count));
    }
}

}
