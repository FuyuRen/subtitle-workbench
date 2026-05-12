#include "swb/app.h"
#include "swb/cli.h"
#include "swb/text.h"
#include "swb/win32_headers.h"

#include <shellapi.h>

#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
[[nodiscard]] std::vector<std::string> read_command_line_arguments() {
    int argument_count = 0;
    LPWSTR* argument_values = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (argument_values == nullptr) {
        return {};
    }

    std::unique_ptr<wchar_t*, decltype(&LocalFree)> holder(argument_values, LocalFree);
    std::vector<std::string> arguments;
    arguments.reserve(argument_count > 1 ? static_cast<std::size_t>(argument_count - 1) : 0);
    for (int index = 1; index < argument_count; ++index) {
        arguments.push_back(swb::wide_to_utf8(argument_values[index]));
    }
    return arguments;
}

[[nodiscard]] bool is_cli_mode(std::span<const std::string> arguments) {
    if (arguments.empty()) {
        return false;
    }
    const std::string_view first = arguments.front();
    return first == "run"
        || first == "help"
        || first == "--help"
        || first == "-h"
        || first.starts_with("--");
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::vector<std::string> arguments = read_command_line_arguments();
    const bool cli_mode = is_cli_mode(arguments);
    try {
        if (cli_mode) {
            return swb::run_cli(arguments, std::cout, std::cerr);
        }
        App application;
        return application.run();
    } catch (const std::exception& exception) {
        if (cli_mode) {
            std::cerr << exception.what() << '\n';
            return 1;
        }
        MessageBoxA(nullptr, exception.what(), "subtitle-workbench", MB_ICONERROR | MB_OK);
        return 1;
    }
}
