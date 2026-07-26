#pragma once

#include "swb/config.h"
#include "swb/transcribe.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace swb {

enum class StepId : std::size_t {
    download_video = 0,
    fetch_source_subtitle,
    translate,
    output,
    count,
};

inline constexpr std::size_t step_count = static_cast<std::size_t>(StepId::count);

enum class StepState : int {
    waiting,
    in_progress,
    completed,
    failed,
};

struct TaskStartOptions {
    std::string output_name;
    bool reuse_working_directory{false};
};

struct StepInfo {
    StepState state{StepState::waiting};
    std::string status;
    double progress{0.0};
};

struct TaskProgress {
    std::uint64_t completed_units{0};
    std::uint64_t total_units{0};
};

enum class TaskState : int {
    idle,
    running,
    succeeded,
    failed,
    canceled,
};

struct TaskTerminalSummary {
    TaskState state{TaskState::idle};
    std::optional<StepId> step;
    std::string status;
};

[[nodiscard]] std::string_view step_label(StepId step);
[[nodiscard]] TaskProgress summarize_task_progress(std::span<const StepInfo, step_count> steps) noexcept;
[[nodiscard]] TaskTerminalSummary summarize_task_state(std::span<const StepInfo, step_count> steps);

class Task {
public:
    using Snapshot = std::array<StepInfo, step_count>;

    Task();
    ~Task();

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    void start(std::string source, Config configuration, TaskStartOptions options = {});
    void cancel();

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] Snapshot read() const;
    [[nodiscard]] std::string detected_title() const;
    [[nodiscard]] std::filesystem::path working_directory() const;
    [[nodiscard]] TranscriptionRuntime transcription_runtime() const;

private:
    struct ExecutionContext;

    enum class AttemptOutcome : int {
        success,
        canceled,
        failed,
    };

    struct AttemptFailure {
        StepId primary_step{StepId::download_video};
        std::string primary_status;
        std::optional<StepId> secondary_step;
        std::string secondary_status;
    };

    struct AttemptResult {
        AttemptOutcome outcome{AttemptOutcome::success};
        std::optional<AttemptFailure> failure;
    };

    void run();
    void set_detected_title(std::string detected_title);
    void set_transcription_runtime(TranscriptionRuntime runtime);
    [[nodiscard]] bool is_canceled() const noexcept;
    [[nodiscard]] AttemptResult fail_step(StepId step, std::string status) const;
    [[nodiscard]] AttemptResult cancel_step(StepId step) const;
    [[nodiscard]] AttemptResult fail_source_pipeline(std::string fetch_status, std::string translate_status) const;
    void refresh_working_directory_state(ExecutionContext& context) const;
    [[nodiscard]] bool build_source_subtitle_from_transcript(ExecutionContext& context);
    [[nodiscard]] bool promote_existing_subtitle(ExecutionContext& context, const std::filesystem::path& source_subtitle);
    [[nodiscard]] AttemptResult prepare_execution_context(ExecutionContext& context);
    [[nodiscard]] AttemptResult acquire_source_subtitles(ExecutionContext& context);
    [[nodiscard]] AttemptResult translate_source_subtitles(ExecutionContext& context);
    [[nodiscard]] AttemptResult render_output_video(ExecutionContext& context);
    [[nodiscard]] AttemptResult run_once(ExecutionContext& context);
    void reset_steps();
    void set_step(StepId step, StepState state, std::string status, std::optional<double> progress = std::nullopt);
    void apply_attempt_failure(const AttemptFailure& failure);
    void apply_retry_status(const AttemptFailure& failure, int retry_index, int retry_count);
    [[nodiscard]] bool sleep_or_cancel(int milliseconds) noexcept;
    void join_worker();

    mutable std::mutex mutex_;
    Snapshot steps_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::thread worker_;
    std::string source_;
    Config configuration_;
    std::string output_name_;
    bool reuse_working_directory_{false};
    std::string detected_title_;
    std::filesystem::path working_directory_;
    TranscriptionRuntime transcription_runtime_;
};

}
