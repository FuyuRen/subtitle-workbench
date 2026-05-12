#include "swb/cli.h"

#include "swb/text.h"
#include "swb/workspace.h"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <ostream>
#include <string_view>
#include <thread>

namespace swb {

namespace {

constexpr std::string_view output_filename = "output.mp4";

[[nodiscard]] bool is_help_argument(std::string_view argument) {
    return argument == "help" || argument == "--help" || argument == "-h";
}

[[nodiscard]] const char* step_token(StepId step) noexcept {
    switch (step) {
    case StepId::download_video:
        return "download_video";
    case StepId::fetch_source_subtitle:
        return "fetch_source_subtitle";
    case StepId::translate:
        return "translate";
    case StepId::output:
        return "output";
    case StepId::count:
        break;
    }
    return "unknown";
}

[[nodiscard]] const char* state_token(StepState state) noexcept {
    switch (state) {
    case StepState::waiting:
        return "waiting";
    case StepState::in_progress:
        return "in_progress";
    case StepState::completed:
        return "completed";
    case StepState::failed:
        return "failed";
    }
    return "unknown";
}

[[nodiscard]] std::optional<int> parse_non_negative_int(std::string_view text) {
    int value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [parsed_end, error_code] = std::from_chars(begin, end, value);
    if (error_code != std::errc{} || parsed_end != end || value < 0) {
        return std::nullopt;
    }
    return value;
}

void apply_cli_overrides(Config& configuration, const CliInvocation& invocation) {
    if (invocation.output_directory.has_value()) {
        configuration.output_dir = *invocation.output_directory;
    }
    if (invocation.source_language.has_value()) {
        configuration.source_lang = *invocation.source_language;
    }
    if (invocation.target_language.has_value()) {
        configuration.target_lang = *invocation.target_language;
    }
    if (invocation.retry_count.has_value()) {
        configuration.retry_count = *invocation.retry_count;
    }
    if (invocation.bilingual_subtitles.has_value()) {
        configuration.bilingual_subtitles = *invocation.bilingual_subtitles;
    }
}

void report_step_changes(
    const Task::Snapshot snapshot,
    const Task::Snapshot& previous_snapshot,
    bool has_previous_snapshot,
    const CliProgressCallback& on_progress) {
    if (!on_progress) {
        return;
    }
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        const StepInfo& current = snapshot[index];
        const bool changed = !has_previous_snapshot
            ? current.state != StepState::waiting || !current.status.empty()
            : current.state != previous_snapshot[index].state || current.status != previous_snapshot[index].status;
        if (changed) {
            on_progress(static_cast<StepId>(index), current);
        }
    }
}

void append_json_string(std::string& json, std::string_view text) {
        json.push_back('"');
        for (const unsigned char character : text) {
            switch (character) {
            case '\\':
                json.append("\\\\");
                break;
            case '"':
                json.append("\\\"");
                break;
            case '\b':
                json.append("\\b");
                break;
            case '\f':
                json.append("\\f");
                break;
            case '\n':
                json.append("\\n");
                break;
            case '\r':
                json.append("\\r");
                break;
            case '\t':
                json.append("\\t");
                break;
            default:
                if (character < 0x20u) {
                    constexpr std::string_view hex = "0123456789ABCDEF";
                    json.append("\\u00");
                    json.push_back(hex[(character >> 4u) & 0x0Fu]);
                    json.push_back(hex[character & 0x0Fu]);
                } else {
                    json.push_back(static_cast<char>(character));
                }
                break;
            }
        }
        json.push_back('"');
    }

[[nodiscard]] std::string build_summary_json(const CliRunSummary& summary) {
        std::string json;
        json.append("{\"success\":");
        json.append(summary.success ? "true" : "false");
        json.append(",\"detected_title\":");
        append_json_string(json, summary.detected_title);
        json.append(",\"working_directory\":");
        append_json_string(json, path_to_utf8(summary.working_directory));
        json.append(",\"output_path\":");
        if (summary.output_path.has_value()) {
            append_json_string(json, path_to_utf8(*summary.output_path));
        } else {
            json.append("null");
        }
        json.append(",\"steps\":[");
        for (std::size_t index = 0; index < summary.steps.size(); ++index) {
            if (index > 0) {
                json.push_back(',');
            }
            const StepId step = static_cast<StepId>(index);
            const StepInfo& step_info = summary.steps[index];
            json.append("{\"id\":");
            append_json_string(json, step_token(step));
            json.append(",\"label\":");
            append_json_string(json, step_label(step));
            json.append(",\"state\":");
            append_json_string(json, state_token(step_info.state));
            json.append(",\"status\":");
            append_json_string(json, step_info.status);
            json.push_back('}');
        }
        json.append("]}");
        return json;
}

[[nodiscard]] std::optional<std::string> resolve_task_input(const CliInvocation& invocation) {
    if (invocation.task_options.reuse_working_directory) {
        if (!invocation.working_directory.has_value()) {
            return std::nullopt;
        }
        return path_to_utf8(*invocation.working_directory);
    }
    return invocation.source;
}
}

CliParseResult parse_cli_arguments(std::span<const std::string> arguments) {
    CliParseResult result;
    result.invocation.config_path = default_config_path();

    if (arguments.empty()) {
        result.ok = true;
        result.show_help = true;
        return result;
    }

    std::span<const std::string> option_arguments = arguments;
    if (is_help_argument(arguments.front())) {
        result.ok = true;
        result.show_help = true;
        return result;
    }
    if (arguments.front() == "run") {
        option_arguments = arguments.subspan(1);
    }

    bool has_source = false;
    bool has_workdir = false;

    const auto take_value = [&](std::size_t &index, std::string_view option) -> std::optional<std::string> {
        if (index + 1 >= option_arguments.size()) {
            result.error_message = "缺少参数值: " + std::string{option};
            return std::nullopt;
        }
        ++index;
        return option_arguments[index];
    };

    for (std::size_t index = 0; index < option_arguments.size(); ++index) {
        const std::string_view argument = option_arguments[index];

        if (argument == "--help" || argument == "-h") {
            result.ok = true;
            result.show_help = true;
            return result;
        }
        if (argument == "--source") {
            const auto value = take_value(index, argument);
            if (!value.has_value()) {
                return result;
            }
            result.invocation.source = *value;
            has_source = true;
            continue;
        }
        if (argument == "--workdir") {
            const auto value = take_value(index, argument);
            if (!value.has_value()) {
                return result;
            }
            result.invocation.working_directory = std::filesystem::u8path(*value);
            result.invocation.task_options.reuse_working_directory = true;
            has_workdir = true;
            continue;
        }
        if (argument == "--output-name") {
            const auto value = take_value(index, argument);
            if (!value.has_value()) {
                return result;
            }
            result.invocation.task_options.output_name = *value;
            continue;
        }
        if (argument == "--config") {
            const auto value = take_value(index, argument);
            if (!value.has_value()) {
                return result;
            }
            result.invocation.config_path = std::filesystem::u8path(*value);
            continue;
        }
        if (argument == "--output-dir") {
            const auto value = take_value(index, argument);
            if (!value.has_value()) {
                return result;
            }
            result.invocation.output_directory = *value;
            continue;
        }
        if (argument == "--source-lang") {
            const auto value = take_value(index, argument);
            if (!value.has_value()) {
                return result;
            }
            result.invocation.source_language = *value;
            continue;
        }
        if (argument == "--target-lang") {
            const auto value = take_value(index, argument);
            if (!value.has_value()) {
                return result;
            }
            result.invocation.target_language = *value;
            continue;
        }
        if (argument == "--retry-count") {
            const auto value = take_value(index, argument);
            if (!value.has_value()) {
                return result;
            }
            const std::optional<int> retry_count = parse_non_negative_int(*value);
            if (!retry_count.has_value()) {
                result.error_message = "retry-count必须是非负整数";
                return result;
            }
            result.invocation.retry_count = *retry_count;
            continue;
        }
        if (argument == "--bilingual") {
            result.invocation.bilingual_subtitles = true;
            continue;
        }
        if (argument == "--no-bilingual") {
            result.invocation.bilingual_subtitles = false;
            continue;
        }

        result.error_message = "未知选项: " + std::string{argument};
        return result;
    }

    if (has_source == has_workdir) {
        result.error_message = "必须且只能提供一个输入：--source或--workdir";
        return result;
    }
    if (result.invocation.task_options.reuse_working_directory && !result.invocation.task_options.output_name.empty()) {
        result.error_message = "继续模式下不能再传--output-name";
        return result;
    }

    result.ok = true;
    return result;
}

CliRunSummary execute_cli_invocation(const CliInvocation& invocation, CliProgressCallback on_progress) {
    const std::optional<std::string> task_input = resolve_task_input(invocation);
    if (!task_input.has_value()) {
        return {};
    }

    Config configuration = load_config(invocation.config_path.empty() ? default_config_path() : invocation.config_path);
    apply_cli_overrides(configuration, invocation);

    Task task;
    task.start(*task_input, configuration, invocation.task_options);

    Task::Snapshot previous_snapshot{};
    bool has_previous_snapshot = false;

    while (task.running()) {
        const Task::Snapshot snapshot = task.read();
        report_step_changes(snapshot, previous_snapshot, has_previous_snapshot, on_progress);
        previous_snapshot = snapshot;
        has_previous_snapshot = true;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    const Task::Snapshot snapshot = task.read();
    report_step_changes(snapshot, previous_snapshot, has_previous_snapshot, on_progress);

    const std::filesystem::path working_directory = task.working_directory();
    const std::filesystem::path output_path = working_directory / std::string{output_filename};
    std::error_code error_code;
    const bool has_output_path = std::filesystem::exists(output_path, error_code) && !error_code;

    return {
        .success = snapshot[static_cast<std::size_t>(StepId::output)].state == StepState::completed,
        .detected_title = task.detected_title(),
        .working_directory = working_directory,
        .output_path = has_output_path ? std::optional<std::filesystem::path>{output_path} : std::nullopt,
        .steps = snapshot,
    };
}

void write_cli_help(std::ostream& output) {
    output
        << "用法\n"
        << "  subtitle-workbench.exe run --source <url或本地文件> [选项]\n"
        << "  subtitle-workbench.exe --source <url或本地文件> [选项]\n"
        << "  subtitle-workbench.exe run --workdir <工作目录> [选项]\n"
        << "  subtitle-workbench.exe help\n\n"
        << "选项\n"
        << "  --config <path>\n"
        << "  --output-name <name>\n"
        << "  --output-dir <path>\n"
        << "  --source-lang <lang>\n"
        << "  --target-lang <lang>\n"
        << "  --retry-count <n>\n"
        << "  --bilingual\n"
        << "  --no-bilingual\n\n";
}

int run_cli(std::span<const std::string> arguments, std::ostream& output, std::ostream& error) {
    const CliParseResult parsed = parse_cli_arguments(arguments);
    if (!parsed.ok) {
        if (!parsed.error_message.empty()) {
            error << parsed.error_message << '\n';
        }
        write_cli_help(error);
        return 2;
    }
    if (parsed.show_help) {
        write_cli_help(output);
        return 0;
    }

    const CliRunSummary summary = execute_cli_invocation(parsed.invocation, [&](StepId step, const StepInfo& info) {
        error << '[' << step_token(step) << "] " << state_token(info.state);
        if (!info.status.empty()) {
            error << ' ' << info.status;
        }
        error << '\n';
    });

    output << build_summary_json(summary) << '\n';
    return summary.success ? 0 : 1;
}

}