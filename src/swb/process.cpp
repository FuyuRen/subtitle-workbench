#include "swb/process.h"

#include "swb/text.h"
#include "swb/win32_headers.h"

#include <algorithm>
#include <array>
#include <chrono>

namespace swb {

namespace {

void append_quoted(std::wstring& command_line, std::wstring_view argument) {
    const bool needs_quote = argument.empty()
        || argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quote) {
        command_line.append(argument);
        return;
    }
    command_line.push_back(L'"');
    for (std::wstring_view::const_iterator it = argument.begin(); it != argument.end();) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++backslashes;
            ++it;
        }
        if (it == argument.end()) {
            command_line.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            command_line.append(backslashes * 2 + 1, L'\\');
            command_line.push_back(L'"');
        } else {
            command_line.append(backslashes, L'\\');
            command_line.push_back(*it);
        }
        ++it;
    }
    command_line.push_back(L'"');
}

std::wstring build_command_line(
    const std::filesystem::path& executable_path,
    const std::vector<std::string>& arguments) {
    std::wstring command_line;
    append_quoted(command_line, executable_path.wstring());
    for (const std::string& argument : arguments) {
        command_line.push_back(L' ');
        append_quoted(command_line, utf8_to_wide(argument));
    }
    return command_line;
}

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE h = nullptr) noexcept {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = h;
    }

private:
    HANDLE handle_{nullptr};
};

bool create_pipe_pair(ScopedHandle& read_end, ScopedHandle& write_end) {
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &security_attributes, 0)) {
        return false;
    }
    SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);
    read_end.reset(read_handle);
    write_end.reset(write_handle);
    return true;
}

void drain_pipe(HANDLE pipe, std::string& output_buffer,
                const std::function<void(std::string_view)>& callback) {
    for (;;) {
        DWORD available_byte_count = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available_byte_count, nullptr) || available_byte_count == 0) {
            return;
        }
        std::array<char, 4096> buffer{};
        const DWORD requested_byte_count = std::min<DWORD>(available_byte_count, static_cast<DWORD>(buffer.size()));
        DWORD read_byte_count = 0;
        if (!ReadFile(pipe, buffer.data(), requested_byte_count, &read_byte_count, nullptr) || read_byte_count == 0) {
            return;
        }
        output_buffer.append(buffer.data(), read_byte_count);
        if (callback) {
            callback(std::string_view{buffer.data(), read_byte_count});
        }
    }
}

}

ProcessResult run_process(const ProcessOptions& options) {
    ProcessResult result;

    ScopedHandle stdout_read_end;
    ScopedHandle stdout_write_end;
    ScopedHandle stderr_read_end;
    ScopedHandle stderr_write_end;
    if (!create_pipe_pair(stdout_read_end, stdout_write_end) || !create_pipe_pair(stderr_read_end, stderr_write_end)) {
        result.launched = false;
        result.error = "CreatePipe failed";
        return result;
    }

    std::wstring command_line = build_command_line(options.executable, options.arguments);
    command_line.push_back(L'\0');

    const std::wstring working_directory = options.working_directory.empty()
        ? std::wstring{}
        : options.working_directory.wstring();

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup_info.wShowWindow = SW_HIDE;
    startup_info.hStdInput = nullptr;
    startup_info.hStdOutput = stdout_write_end.get();
    startup_info.hStdError = stderr_write_end.get();

    PROCESS_INFORMATION process_information{};
    const BOOL created = CreateProcessW(
        options.executable.empty() ? nullptr : options.executable.c_str(),
        command_line.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup_info, &process_information);

    if (!created) {
        result.launched = false;
        const DWORD error_code = GetLastError();
        result.error = "CreateProcess failed (" + std::to_string(error_code) + ")";
        return result;
    }

    ScopedHandle process{process_information.hProcess};
    ScopedHandle thread{process_information.hThread};
    stdout_write_end.reset();
    stderr_write_end.reset();

    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = options.timeout_ms > 0
        ? Clock::now() + std::chrono::milliseconds(options.timeout_ms)
        : Clock::time_point::max();

    for (;;) {
        drain_pipe(stdout_read_end.get(), result.stdout_data, options.on_stdout);
        drain_pipe(stderr_read_end.get(), result.stderr_data, options.on_stderr);

        if (options.cancel && options.cancel->load(std::memory_order_acquire)) {
            TerminateProcess(process.get(), 1);
            WaitForSingleObject(process.get(), INFINITE);
            result.canceled = true;
            break;
        }

        if (Clock::now() >= deadline) {
            TerminateProcess(process.get(), 1);
            WaitForSingleObject(process.get(), INFINITE);
            result.timed_out = true;
            break;
        }

        const DWORD wait_result = WaitForSingleObject(process.get(), 25);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
    }

    drain_pipe(stdout_read_end.get(), result.stdout_data, options.on_stdout);
    drain_pipe(stderr_read_end.get(), result.stderr_data, options.on_stderr);

    DWORD exit_code = 1;
    GetExitCodeProcess(process.get(), &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    return result;
}

}
