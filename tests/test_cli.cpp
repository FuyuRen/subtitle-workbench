#include "swb_test.h"
#include "test_support.h"
#include "swb/cli.h"
#include "swb/workspace.h"

#include <array>
#include <sstream>
#include <vector>

using swb::test::contains_text;
using swb::test::expect_eq;
using swb::test::expect_true;
using swb::test::make_temp_directory;
using swb::test::write_text_file;

namespace {

const swb::test::Registrar case_1{
    "cli: help command writes usage",
    [] {
        std::ostringstream output;
        std::ostringstream error;
        const std::array arguments{std::string{"help"}};

        expect_eq(swb::run_cli(arguments, output, error), 0);
        expect_true(contains_text(output.str(), "subtitle-workbench.exe run --source"));
        expect_true(error.str().empty());
    },
};

const swb::test::Registrar case_2{
    "cli: parses source invocation with overrides",
    [] {
        const std::array arguments{
            std::string{"run"},
            std::string{"--source"},
            std::string{"https://example.test/video"},
            std::string{"--output-name"},
            std::string{"episode-01"},
            std::string{"--retry-count"},
            std::string{"2"},
            std::string{"--bilingual"},
            std::string{"--target-lang"},
            std::string{"ja"},
        };

        const swb::CliParseResult result = swb::parse_cli_arguments(arguments);

        expect_true(result.ok);
        expect_true(!result.show_help);
        expect_true(!result.invocation.task_options.reuse_working_directory);
        expect_true(result.invocation.source.has_value());
        expect_eq(*result.invocation.source, std::string{"https://example.test/video"});
        expect_true(!result.invocation.working_directory.has_value());
        expect_eq(result.invocation.task_options.output_name, std::string{"episode-01"});
        expect_true(result.invocation.retry_count.has_value());
        expect_eq(*result.invocation.retry_count, 2);
        expect_true(result.invocation.bilingual_subtitles.has_value());
        expect_true(*result.invocation.bilingual_subtitles);
        expect_true(result.invocation.target_language.has_value());
        expect_eq(*result.invocation.target_language, std::string{"ja"});
    },
};

const swb::test::Registrar case_3{
    "cli: rejects source and workdir together",
    [] {
        const std::array arguments{
            std::string{"--source"},
            std::string{"a.mp4"},
            std::string{"--workdir"},
            std::string{"work"},
        };

        const swb::CliParseResult result = swb::parse_cli_arguments(arguments);

        expect_true(!result.ok);
        expect_true(contains_text(result.error_message, "--source或--workdir"));
    },
};

const swb::test::Registrar case_4{
    "cli: rejects output name in reuse mode",
    [] {
        const std::array arguments{
            std::string{"run"},
            std::string{"--workdir"},
            std::string{"work"},
            std::string{"--output-name"},
            std::string{"episode-01"},
        };

        const swb::CliParseResult result = swb::parse_cli_arguments(arguments);

        expect_true(!result.ok);
        expect_true(contains_text(result.error_message, "--output-name"));
    },
};

const swb::test::Registrar case_5{
    "cli: execute invocation returns task summary for reuse workdir failure",
    [] {
        const std::filesystem::path directory = make_temp_directory("swb-cli", "reuse-workdir-failure");
        write_text_file(directory / "en.srt", "1\n00:00:00,000 --> 00:00:01,000\nhello\n\n");

        swb::CliInvocation invocation;
        invocation.config_path = directory / "missing.ini";
        invocation.working_directory = directory;
        invocation.task_options.reuse_working_directory = true;

        std::vector<std::string> progress_updates;
        const swb::CliRunSummary summary = swb::execute_cli_invocation(
            invocation,
            [&](swb::StepId step, const swb::StepInfo& info) {
                progress_updates.push_back(std::string{swb::step_label(step)} + ":" + info.status);
            });

        expect_true(!summary.success);
        expect_eq(summary.working_directory, directory);
        expect_true(!progress_updates.empty());

        const auto& fetch_step = summary.steps[static_cast<std::size_t>(swb::StepId::fetch_source_subtitle)];
        const auto& translate_step = summary.steps[static_cast<std::size_t>(swb::StepId::translate)];
        expect_true(fetch_step.state == swb::StepState::completed);
        expect_eq(fetch_step.status, std::string{"复用现有字幕"});
        expect_true(translate_step.state == swb::StepState::failed);
        expect_eq(translate_step.status, std::string{"未配置LLM Base URL"});
    },
};

}