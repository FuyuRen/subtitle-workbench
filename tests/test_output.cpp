#include "swb_test.h"
#include "test_support.h"
#include "swb/output.h"

#include <atomic>
#include <filesystem>
#include <optional>

using swb::test::contains_argument;
using swb::test::contains_text;
using swb::test::create_file;
using swb::test::expect_eq;
using swb::test::expect_true;
using swb::test::make_temp_directory;
using swb::test::read_text_file;

namespace {

const swb::test::Registrar case_1{
    "output: serializes hard subtitle script with bilingual sizing",
    [] {
        swb::Config configuration;
        configuration.bilingual_subtitles = true;
        configuration.hard_subtitle_style.font_name = "Microsoft YaHei";
        configuration.hard_subtitle_style.chinese_font_size = 30;
        configuration.hard_subtitle_style.english_font_size = 18;
        configuration.hard_subtitle_style.bottom_margin = 72;
        configuration.hard_subtitle_style.outline_thickness = 3.0f;
        configuration.hard_subtitle_style.bilingual_line_gap = 14.0f;

        const std::vector<swb::SubtitleCue> translated{{0.0, 1.0, "你好"}};
        const std::vector<swb::SubtitleCue> source{{0.0, 1.0, "hello"}};

        const auto script = swb::serialize_hard_subtitle_script(translated, source, configuration);

        expect_true(script.has_value());
        expect_true(contains_text(*script, "Style: DefaultZh,Microsoft YaHei,30"));
        expect_true(contains_text(*script, "Style: DefaultEn,Microsoft YaHei,18"));
        expect_true(contains_text(*script, "Dialogue: 0,0:00:00.00,0:00:01.00,DefaultEn,,0,0,72,,hello"));
        expect_true(contains_text(*script, "Dialogue: 0,0:00:00.00,0:00:01.00,DefaultZh,,0,0,104,,你好"));
        expect_true(
            script->find("Dialogue: 0,0:00:00.00,0:00:01.00,DefaultEn,,0,0,72,,hello")
            < script->find("Dialogue: 0,0:00:00.00,0:00:01.00,DefaultZh,,0,0,104,,你好"));
    },
};

const swb::test::Registrar case_2{
    "output: stacks overlapping bilingual cues onto separate rows",
    [] {
        swb::Config configuration;
        configuration.bilingual_subtitles = true;
        configuration.hard_subtitle_style.font_name = "Microsoft YaHei";
        configuration.hard_subtitle_style.chinese_font_size = 30;
        configuration.hard_subtitle_style.english_font_size = 18;
        configuration.hard_subtitle_style.bilingual_line_gap = 14.0f;

        const std::vector<swb::SubtitleCue> translated{{0.0, 2.0, "第一句"}, {1.0, 3.0, "第二句"}};
        const std::vector<swb::SubtitleCue> source{{0.0, 2.0, "first line"}, {1.0, 3.0, "second line"}};

        const auto script = swb::serialize_hard_subtitle_script(translated, source, configuration);

        expect_true(script.has_value());
        expect_true(contains_text(*script, "Dialogue: 0,0:00:00.00,0:00:02.00,DefaultEn,,0,0,60,,first line"));
        expect_true(contains_text(*script, "Dialogue: 0,0:00:00.00,0:00:02.00,DefaultZh,,0,0,92,,第一句"));
        expect_true(contains_text(*script, "Dialogue: 0,0:00:01.00,0:00:03.00,DefaultEn,,0,0,134,,second line"));
        expect_true(contains_text(*script, "Dialogue: 0,0:00:01.00,0:00:03.00,DefaultZh,,0,0,166,,第二句"));
    },
};

const swb::test::Registrar case_3{
    "output: hard mode writes ass script and ffmpeg filter",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-output-hard");
        const std::filesystem::path video_path = directory / "video.mp4";
        create_file(video_path, "video");

        std::vector<swb::ProcessOptions> captured_options;
        const swb::SubtitleOutputRenderer renderer{
            std::filesystem::path{"ffmpeg.exe"},
            [&](const swb::ProcessOptions& options) {
                captured_options.push_back(options);
                if (options.executable.filename() == L"ffprobe.exe") {
                    return swb::ProcessResult{
                        .exit_code = 0,
                        .stdout_data = "20.0\n",
                    };
                }
                if (contains_argument(options.arguments, "lavfi")) {
                    return swb::ProcessResult{
                        .exit_code = 0,
                    };
                }
                if (options.on_stdout) {
                    options.on_stdout("out_time=00:00:00.50\nspeed=1.8x\nprogress=continue\nout_time=00:00:01.00\nspeed=2.0x\nprogress=end\n");
                }
                create_file(std::filesystem::u8path(options.arguments.back()), "encoded");
                return swb::ProcessResult{
                    .exit_code = 0,
                };
            }};

        swb::Config configuration;
        configuration.bilingual_subtitles = true;
        configuration.hard_subtitle_style.font_name = "SimHei";
        configuration.hard_subtitle_style.chinese_font_size = 32;
        configuration.hard_subtitle_style.english_font_size = 20;

        const std::vector<swb::SubtitleCue> translated{{0.0, 1.0, "你好"}};
        const std::vector<swb::SubtitleCue> source{{0.0, 1.0, "hello"}};
        const std::atomic<bool> cancel{false};
        std::vector<std::string> statuses;
        const swb::OutputResult result = renderer.render(
            configuration,
            video_path,
            source,
            translated,
            directory,
            cancel,
            [&](const swb::OutputProgress& progress) {
                statuses.push_back(progress.status);
            });

        expect_true(result.success);
        expect_eq(result.message, std::string{"硬编码完成（NVENC）"});
        expect_eq(captured_options.size(), std::size_t{3});
        expect_true(contains_argument(captured_options[2].arguments, "subtitles=output.hard.ass"));
        expect_true(contains_argument(captured_options[2].arguments, "h264_nvenc"));
        expect_eq(statuses.front(), std::string{"硬编码中（0%，NVENC）"});
        expect_true(statuses[1].find("3%") != std::string::npos);
        expect_true(statuses.back().find("100%") != std::string::npos);
        expect_true(std::filesystem::exists(directory / "output.hard.ass"));
        expect_true(std::filesystem::exists(result.output_path));
        expect_true(contains_text(read_text_file(directory / "output.hard.ass"), "SimHei"));

        std::filesystem::remove_all(directory);
    },
};

const swb::test::Registrar case_4{
    "output: encoder probe avoids false cpu fallback",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-output-probe");
        const std::filesystem::path video_path = directory / "video.mp4";
        create_file(video_path, "video");

        std::vector<swb::ProcessOptions> captured_options;
        const swb::SubtitleOutputRenderer renderer{
            std::filesystem::path{"ffmpeg.exe"},
            [&](const swb::ProcessOptions& options) {
                captured_options.push_back(options);
                if (options.executable.filename() == L"ffprobe.exe") {
                    return swb::ProcessResult{
                        .exit_code = 0,
                        .stdout_data = "20.0\n",
                    };
                }
                if (contains_argument(options.arguments, "lavfi")) {
                    if (contains_argument(options.arguments, "h264_nvenc")) {
                        return swb::ProcessResult{.exit_code = 1};
                    }
                    if (contains_argument(options.arguments, "h264_qsv")) {
                        return swb::ProcessResult{
                            .exit_code = contains_argument(options.arguments, "color=c=black:s=640x360:d=0.1") ? 0 : 1,
                        };
                    }
                    return swb::ProcessResult{.exit_code = 1};
                }
                create_file(std::filesystem::u8path(options.arguments.back()), "encoded");
                return swb::ProcessResult{.exit_code = 0};
            }};

        swb::Config configuration;
        const std::vector<swb::SubtitleCue> translated{{0.0, 1.0, "你好"}};
        const std::vector<swb::SubtitleCue> source{{0.0, 1.0, "hello"}};
        const std::atomic<bool> cancel{false};

        const swb::OutputResult result = renderer.render(
            configuration,
            video_path,
            source,
            translated,
            directory,
            cancel);

        expect_true(result.success);
        expect_eq(result.message, std::string{"硬编码完成（QSV）"});
        expect_true(contains_argument(captured_options[2].arguments, "color=c=black:s=640x360:d=0.1"));
        expect_true(contains_argument(captured_options.back().arguments, "h264_qsv"));

        std::filesystem::remove_all(directory);
    },
};

}