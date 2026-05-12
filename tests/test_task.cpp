#include "swb_test.h"
#include "test_support.h"
#include "swb/source_subtitle_resolution.h"
#include "swb/task.h"
#include "swb/workspace.h"

#include <chrono>
#include <fstream>
#include <thread>

using swb::test::contains_text;
using swb::test::create_file;
using swb::test::expect_eq;
using swb::test::expect_true;
using swb::test::make_temp_directory;
using swb::test::write_text_file;

namespace {

void wait_for_task(swb::Task& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{6};
    while (task.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
}

[[nodiscard]] bool wait_for_step_status(
    swb::Task& task,
    swb::StepId step_identifier,
    std::string_view needle,
    std::chrono::milliseconds timeout = std::chrono::seconds{3}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = task.read();
        const auto& step = snapshot[static_cast<std::size_t>(step_identifier)];
        if (contains_text(step.status, needle)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return false;
}

const swb::test::Registrar case_1{
    "task: starts in waiting state",
    [] {
        swb::Task task;
        const auto snapshot = task.read();
        for (const auto& step : snapshot) {
            expect_true(step.state == swb::StepState::waiting);
        }
        expect_true(!task.running());
    },
};

const swb::test::Registrar case_2{
    "task: cancel terminates worker quickly",
    [] {
        swb::Task task;
        task.start("https://invalid.example.test/video", swb::Config{});
        std::this_thread::sleep_for(std::chrono::milliseconds{80});
        task.cancel();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (task.running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        expect_true(!task.running());
    },
};

const swb::test::Registrar case_3{
    "task: step labels match step ids",
    [] {
        expect_eq(swb::step_label(swb::StepId::download_video), std::string_view{"下载视频"});
        expect_eq(swb::step_label(swb::StepId::fetch_source_subtitle), std::string_view{"获取源语言字幕"});
        expect_eq(swb::step_label(swb::StepId::translate), std::string_view{"翻译为中文"});
        expect_eq(swb::step_label(swb::StepId::output), std::string_view{"输出"});
    },
};

const swb::test::Registrar case_4{
    "task: summarize progress aggregates completed work and active step",
    [] {
        swb::Task::Snapshot snapshot;
        snapshot[static_cast<std::size_t>(swb::StepId::download_video)] = {
            .state = swb::StepState::completed,
            .status = "完成",
        };
        snapshot[static_cast<std::size_t>(swb::StepId::fetch_source_subtitle)] = {
            .state = swb::StepState::in_progress,
            .status = "处理中",
            .progress = 0.25,
        };

        const swb::TaskProgress progress = swb::summarize_task_progress(snapshot);

        expect_eq(progress.total_units, std::uint64_t{4000});
        expect_eq(progress.completed_units, std::uint64_t{1250});
    },
};

const swb::test::Registrar case_5{
    "task: summarize state reports success from completed output",
    [] {
        swb::Task::Snapshot snapshot;
        for (auto& step : snapshot) {
            step.state = swb::StepState::completed;
            step.status = "完成";
        }

        const swb::TaskTerminalSummary summary = swb::summarize_task_state(snapshot);

        expect_true(summary.state == swb::TaskState::succeeded);
        expect_true(summary.step.has_value());
        expect_true(*summary.step == swb::StepId::output);
        expect_eq(summary.status, std::string{"完成"});
    },
};

const swb::test::Registrar case_6{
    "task: summarize state reports cancellation from canceled failure",
    [] {
        swb::Task::Snapshot snapshot;
        snapshot[static_cast<std::size_t>(swb::StepId::translate)] = {
            .state = swb::StepState::failed,
            .status = "已取消",
        };

        const swb::TaskTerminalSummary summary = swb::summarize_task_state(snapshot);

        expect_true(summary.state == swb::TaskState::canceled);
        expect_true(summary.step.has_value());
        expect_true(*summary.step == swb::StepId::translate);
        expect_eq(summary.status, std::string{"已取消"});
    },
};

const swb::test::Registrar case_7{
    "task: summarize state reports failure from failed step",
    [] {
        swb::Task::Snapshot snapshot;
        snapshot[static_cast<std::size_t>(swb::StepId::fetch_source_subtitle)] = {
            .state = swb::StepState::failed,
            .status = "API转写失败",
        };

        const swb::TaskTerminalSummary summary = swb::summarize_task_state(snapshot);

        expect_true(summary.state == swb::TaskState::failed);
        expect_true(summary.step.has_value());
        expect_true(*summary.step == swb::StepId::fetch_source_subtitle);
        expect_eq(summary.status, std::string{"API转写失败"});
    },
};

const swb::test::Registrar case_8{
    "task: source subtitle selection prefers manual then api then automatic",
    [] {
        expect_eq(swb::select_source_subtitle(true, false, false), swb::SourceSubtitleSelection::platform_manual_subtitle);
        expect_eq(swb::select_source_subtitle(true, true, true), swb::SourceSubtitleSelection::platform_manual_subtitle);
        expect_eq(swb::select_source_subtitle(false, true, true), swb::SourceSubtitleSelection::api_transcription);
        expect_eq(swb::select_source_subtitle(false, false, true), swb::SourceSubtitleSelection::platform_automatic_subtitle);
        expect_eq(swb::select_source_subtitle(false, false, false), swb::SourceSubtitleSelection::unavailable);
    },
};

const swb::test::Registrar case_9{
    "task: inspect working directory finds pipeline artifacts",
    [] {
        const std::filesystem::path directory = make_temp_directory("task", "artifacts");
        create_file(directory / "video.mp4");
        create_file(directory / "audio.wav");
        create_file(directory / "transcript.json");
        create_file(directory / "en.srt");
        create_file(directory / "manual.en.srt");
        create_file(directory / "auto.en.srt");
        create_file(directory / "zh.srt");

        const swb::WorkingDirectoryState working_directory_state = swb::inspect_working_directory_state(directory);

        expect_true(working_directory_state.video_path.has_value());
        expect_true(working_directory_state.audio_path.has_value());
        expect_true(working_directory_state.transcript_path.has_value());
        expect_true(working_directory_state.source_subtitle_path.has_value());
        expect_true(working_directory_state.manual_subtitle_path.has_value());
        expect_true(working_directory_state.automatic_subtitle_path.has_value());
        expect_true(working_directory_state.translated_subtitle_path.has_value());
    },
};

const swb::test::Registrar case_10{
    "task: inspect working directory ignores yt-dlp fragment residue",
    [] {
        const std::filesystem::path directory = make_temp_directory("task", "ignore-video-fragment");
        create_file(directory / "video.f30080.mp4");

        const swb::WorkingDirectoryState working_directory_state = swb::inspect_working_directory_state(directory);

        expect_true(!working_directory_state.video_path.has_value());
    },
};

const swb::test::Registrar case_11{
    "task: classify source subtitle stage retries automatic fallback when richer artifacts exist",
    [] {
        swb::WorkingDirectoryState working_directory_state;
        working_directory_state.source_subtitle_path = std::filesystem::path{"en.srt"};
        working_directory_state.automatic_subtitle_path = std::filesystem::path{"auto.en.srt"};
        working_directory_state.audio_path = std::filesystem::path{"audio.wav"};
        expect_eq(
            swb::classify_source_subtitle_resume_stage(working_directory_state),
            swb::SourceSubtitleResumeStage::needs_transcription);

        working_directory_state.audio_path.reset();
        working_directory_state.video_path = std::filesystem::path{"video.mp4"};
        expect_eq(
            swb::classify_source_subtitle_resume_stage(working_directory_state),
            swb::SourceSubtitleResumeStage::needs_audio_extraction);

        working_directory_state.video_path.reset();
        expect_eq(
            swb::classify_source_subtitle_resume_stage(working_directory_state),
            swb::SourceSubtitleResumeStage::ready_source_subtitle);
    },
};

const swb::test::Registrar case_12{
    "task: classify source subtitle stage treats transcript with final srt as complete",
    [] {
        swb::WorkingDirectoryState working_directory_state;
        working_directory_state.transcript_path = std::filesystem::path{"transcript.json"};
        working_directory_state.source_subtitle_path = std::filesystem::path{"en.srt"};
        expect_eq(
            swb::classify_source_subtitle_resume_stage(working_directory_state),
            swb::SourceSubtitleResumeStage::ready_source_subtitle);
    },
};

const swb::test::Registrar case_13{
    "task: transcript-only working directory generates en srt",
    [] {
        const std::filesystem::path directory = make_temp_directory("task", "transcript-only");
        {
            std::ofstream output(directory / "transcript.json", std::ios::binary | std::ios::trunc);
            output << R"({
            "task":"transcribe",
            "segments":[{"id":0,"tokens":[1,2,3],"text":"hello world"}],
            "words":[
                {"word":"hello","start":0.0,"end":0.5},
                {"word":"world.","start":0.5,"end":1.6}
            ]
        })";
        }
        write_text_file(directory / "zh.srt", "1\n00:00:00,000 --> 00:00:01,000\n你好\n\n");

        swb::Task task;
        task.start(swb::path_to_utf8(directory), swb::Config{}, {
            .reuse_working_directory = true,
        });

        wait_for_task(task);

        expect_true(!task.running());
        expect_true(std::filesystem::exists(directory / "en.srt"));

        const auto snapshot = task.read();
        const auto& fetch_step = snapshot[static_cast<std::size_t>(swb::StepId::fetch_source_subtitle)];
        const auto& translate_step = snapshot[static_cast<std::size_t>(swb::StepId::translate)];
        expect_true(fetch_step.state == swb::StepState::completed);
        expect_eq(fetch_step.status, std::string{"API转写"});
        expect_true(translate_step.state == swb::StepState::completed);
        expect_eq(translate_step.status, std::string{"复用现有翻译"});
    },
};

const swb::test::Registrar case_14{
    "task: source subtitle without llm config fails translate stage",
    [] {
        const std::filesystem::path directory = make_temp_directory("task", "missing-translation-config");
        write_text_file(directory / "en.srt", "1\n00:00:00,000 --> 00:00:01,000\nhello\n\n");

        swb::Config configuration;
        configuration.retry_count = 0;

        swb::Task task;
        task.start(swb::path_to_utf8(directory), configuration, {
            .reuse_working_directory = true,
        });

        wait_for_task(task);

        expect_true(!task.running());

        const auto snapshot = task.read();
        const auto& fetch_step = snapshot[static_cast<std::size_t>(swb::StepId::fetch_source_subtitle)];
        const auto& translate_step = snapshot[static_cast<std::size_t>(swb::StepId::translate)];
        expect_true(fetch_step.state == swb::StepState::completed);
        expect_eq(fetch_step.status, std::string{"复用现有字幕"});
        expect_true(translate_step.state == swb::StepState::failed);
        expect_eq(translate_step.status, std::string{"未配置LLM Base URL"});
    },
};

const swb::test::Registrar case_15{
    "task: global retry budget is shared across all steps",
    [] {
        const std::filesystem::path directory = make_temp_directory("task", "global-retry-budget");
        write_text_file(directory / "en.srt", "1\n00:00:00,000 --> 00:00:01,000\nhello\n\n");

        swb::Config configuration;
        configuration.retry_count = 1;

        swb::Task task;
        task.start(swb::path_to_utf8(directory), configuration, {
            .reuse_working_directory = true,
        });

        expect_true(wait_for_step_status(task, swb::StepId::translate, "重试中（1/1）"));
        write_text_file(directory / "zh.srt", "1\n00:00:00,000 --> 00:00:01,000\n你好\n\n");

        wait_for_task(task);

        expect_true(!task.running());

        const auto snapshot = task.read();
        const auto& translate_step = snapshot[static_cast<std::size_t>(swb::StepId::translate)];
        const auto& output_step = snapshot[static_cast<std::size_t>(swb::StepId::output)];
        expect_true(translate_step.state == swb::StepState::completed);
        expect_eq(translate_step.status, std::string{"复用现有翻译"});
        expect_true(output_step.state == swb::StepState::failed);
        expect_eq(output_step.status, std::string{"工作目录缺少视频文件"});
        expect_true(!contains_text(output_step.status, "重试中"));
    },
};

}
