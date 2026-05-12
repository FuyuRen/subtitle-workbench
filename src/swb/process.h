#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace swb {

struct ProcessOptions {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    int timeout_ms{0};
    const std::atomic<bool>* cancel{nullptr};
    std::function<void(std::string_view)> on_stdout;
    std::function<void(std::string_view)> on_stderr;
};

struct ProcessResult {
    int exit_code{-1};
    std::string stdout_data;
    std::string stderr_data;
    bool launched{true};
    bool canceled{false};
    bool timed_out{false};
    std::string error;
};

[[nodiscard]] ProcessResult run_process(const ProcessOptions& options);

}
