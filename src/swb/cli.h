#pragma once

#include "swb/config.h"
#include "swb/task.h"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>

namespace swb {
    struct CliInvocation {
        std::filesystem::path config_path;
        std::optional<std::string> source;
        std::optional<std::filesystem::path> working_directory;
        TaskStartOptions task_options;
        std::optional<std::string> output_directory;
        std::optional<std::string> source_language;
        std::optional<std::string> target_language;
        std::optional<int> retry_count;
        std::optional<bool> bilingual_subtitles;
    };

    struct CliParseResult {
        CliInvocation invocation;
        bool ok{false};
        bool show_help{false};
        std::string error_message;
    };

    struct CliRunSummary {
        bool success{false};
        std::string detected_title;
        std::filesystem::path working_directory;
        std::optional<std::filesystem::path> output_path;
        Task::Snapshot steps;
    };

    using CliProgressCallback = std::function<void(StepId, const StepInfo&)>;

    [[nodiscard]] CliParseResult parse_cli_arguments(std::span<const std::string> arguments);
    [[nodiscard]] CliRunSummary execute_cli_invocation(const CliInvocation& invocation, CliProgressCallback on_progress = {});
    void write_cli_help(std::ostream& output);
    int run_cli(std::span<const std::string> arguments, std::ostream& output, std::ostream& error);
}