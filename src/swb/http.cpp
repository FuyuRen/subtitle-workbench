#include "swb/http.h"

#include "swb/text.h"
#include "swb/win32_headers.h"

#include <winhttp.h>

#include <atomic>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace swb::http {

namespace {

using InternetHandleState = std::shared_ptr<std::atomic<HINTERNET>>;

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) noexcept { reset(handle); }
    ~WinHttpHandle() { close(); }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&&) noexcept = default;
    WinHttpHandle& operator=(WinHttpHandle&&) noexcept = default;

    [[nodiscard]] HINTERNET get() const noexcept {
        return handle_state_ ? handle_state_->load(std::memory_order_acquire) : nullptr;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return get() != nullptr; }

    void close() noexcept {
        if (!handle_state_) {
            return;
        }
        if (HINTERNET handle = handle_state_->exchange(nullptr, std::memory_order_acq_rel); handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }

    void reset(HINTERNET handle = nullptr) noexcept {
        close();
        if (handle != nullptr) {
            handle_state_ = std::make_shared<std::atomic<HINTERNET>>(handle);
        } else {
            handle_state_.reset();
        }
    }

    [[nodiscard]] const InternetHandleState& state() const noexcept { return handle_state_; }

private:
    InternetHandleState handle_state_;
};

struct RequestCancelState {
    const std::atomic<bool>* cancel{nullptr};
    InternetHandleState handle_state;
    std::atomic<bool> canceled{false};
};

void cancel_request(const std::shared_ptr<RequestCancelState>& state) noexcept {
    if (!state) {
        return;
    }
    state->canceled.store(true, std::memory_order_release);
    if (!state->handle_state) {
        return;
    }
    if (HINTERNET handle = state->handle_state->exchange(nullptr, std::memory_order_acq_rel); handle != nullptr) {
        WinHttpCloseHandle(handle);
    }
}

class RequestCancelMonitor {
public:
    RequestCancelMonitor() = default;

    RequestCancelMonitor(const std::atomic<bool>* cancel, InternetHandleState handle_state)
        : state_(std::make_shared<RequestCancelState>()) {
        state_->cancel = cancel;
        state_->handle_state = std::move(handle_state);
        if (state_->cancel == nullptr || !state_->handle_state) {
            state_.reset();
            return;
        }
        if (state_->cancel->load(std::memory_order_acquire)) {
            cancel_request(state_);
            return;
        }

        const std::shared_ptr<RequestCancelState> state = state_;
        worker_ = std::jthread([state](std::stop_token stop_token) {
            while (!stop_token.stop_requested()) {
                if (state->cancel->load(std::memory_order_acquire)) {
                    cancel_request(state);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
        });
    }

    [[nodiscard]] bool canceled() const noexcept {
        return state_ && state_->canceled.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<RequestCancelState> state_;
    std::jthread worker_;
};

[[nodiscard]] bool request_was_canceled(const Request& request, const RequestCancelMonitor& cancel_monitor) {
    return cancel_monitor.canceled() ||
           (request.cancel != nullptr && request.cancel->load(std::memory_order_acquire));
}

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port{0};
    bool secure{false};
};

ParsedUrl parse_url(std::string_view url) {
    const std::wstring wide_url = utf8_to_wide(url);

    URL_COMPONENTS url_components{};
    url_components.dwStructSize = sizeof(url_components);
    url_components.dwSchemeLength    = static_cast<DWORD>(-1);
    url_components.dwHostNameLength  = static_cast<DWORD>(-1);
    url_components.dwUrlPathLength   = static_cast<DWORD>(-1);
    url_components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &url_components)) {
        throw std::runtime_error("invalid URL");
    }

    ParsedUrl parsed_url;
    parsed_url.host.assign(url_components.lpszHostName, url_components.dwHostNameLength);
    parsed_url.path.assign(url_components.lpszUrlPath, url_components.dwUrlPathLength);
    if (url_components.dwExtraInfoLength > 0) {
        parsed_url.path.append(url_components.lpszExtraInfo, url_components.dwExtraInfoLength);
    }
    if (parsed_url.path.empty()) {
        parsed_url.path = L"/";
    }
    parsed_url.port = url_components.nPort;
    parsed_url.secure = (url_components.nScheme == INTERNET_SCHEME_HTTPS);
    return parsed_url;
}

void apply_headers(HINTERNET request_handle, const std::vector<std::string>& headers) {
    if (headers.empty()) {
        return;
    }
    std::wstring joined;
    for (const std::string& header : headers) {
        joined.append(utf8_to_wide(header));
        joined.append(L"\r\n");
    }
    WinHttpAddRequestHeaders(
        request_handle, joined.c_str(), static_cast<DWORD>(joined.size()),
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
}

int query_status(HINTERNET request_handle) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(
            request_handle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return static_cast<int>(status);
}

struct PreparedRequest {
    WinHttpHandle session;
    WinHttpHandle connection;
    WinHttpHandle request;
    RequestCancelMonitor cancel_monitor;
};

std::optional<PreparedRequest> prepare_request(const Request& request) {
    const ParsedUrl parsed_url = parse_url(request.url);
    const std::wstring wide_method = utf8_to_wide(request.method);

    WinHttpHandle session{WinHttpOpen(
        L"subtitle-workbench/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) {
        throw std::runtime_error("WinHttpOpen failed");
    }

    WinHttpSetTimeouts(session.get(), request.timeout_ms, request.timeout_ms, request.timeout_ms, request.timeout_ms);

    WinHttpHandle connection{WinHttpConnect(session.get(), parsed_url.host.c_str(), parsed_url.port, 0)};
    if (!connection) {
        throw std::runtime_error("WinHttpConnect failed");
    }

    const DWORD flags = parsed_url.secure ? WINHTTP_FLAG_SECURE : 0u;
    WinHttpHandle request_handle{WinHttpOpenRequest(
        connection.get(), wide_method.c_str(), parsed_url.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!request_handle) {
        throw std::runtime_error("WinHttpOpenRequest failed");
    }

    RequestCancelMonitor cancel_monitor{request.cancel, request_handle.state()};
    if (request_was_canceled(request, cancel_monitor)) {
        return std::nullopt;
    }

    apply_headers(request_handle.get(), request.headers);

    std::string request_body = request.body;
    void* request_body_pointer = request_body.empty() ? nullptr : request_body.data();
    const DWORD request_body_length = static_cast<DWORD>(request_body.size());

    if (!WinHttpSendRequest(
            request_handle.get(),
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            request_body_pointer, request_body_length, request_body_length, 0)) {
        if (request_was_canceled(request, cancel_monitor)) {
            return std::nullopt;
        }
        throw std::runtime_error("WinHttpSendRequest failed");
    }
    if (!WinHttpReceiveResponse(request_handle.get(), nullptr)) {
        if (request_was_canceled(request, cancel_monitor)) {
            return std::nullopt;
        }
        throw std::runtime_error("WinHttpReceiveResponse failed");
    }

    return PreparedRequest{
        std::move(session),
        std::move(connection),
        std::move(request_handle),
        std::move(cancel_monitor),
    };
}

}

Response send(const Request& request) {
    std::optional<PreparedRequest> prepared_request = prepare_request(request);
    if (!prepared_request.has_value()) {
        return {};
    }

    Response response;
    response.status = query_status(prepared_request->request.get());

    std::array<char, 8192> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(prepared_request->request.get(), &available)) {
            if (request_was_canceled(request, prepared_request->cancel_monitor)) {
                return response;
            }
            throw std::runtime_error("WinHttpQueryDataAvailable failed");
        }
        if (available == 0) {
            break;
        }
        while (available > 0) {
            const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            DWORD got = 0;
            if (!WinHttpReadData(prepared_request->request.get(), buffer.data(), want, &got)) {
                if (request_was_canceled(request, prepared_request->cancel_monitor)) {
                    return response;
                }
                throw std::runtime_error("WinHttpReadData failed");
            }
            if (got == 0) {
                available = 0;
                break;
            }
            response.body.append(buffer.data(), got);
            available -= got;
        }
    }
    return response;
}

int send_streaming(const Request& request, const ChunkCallback& on_chunk) {
    std::optional<PreparedRequest> prepared_request = prepare_request(request);
    if (!prepared_request.has_value()) {
        return 0;
    }
    const int status = query_status(prepared_request->request.get());

    std::array<char, 8192> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(prepared_request->request.get(), &available)) {
            if (request_was_canceled(request, prepared_request->cancel_monitor)) {
                return status;
            }
            throw std::runtime_error("WinHttpQueryDataAvailable failed");
        }
        if (available == 0) {
            break;
        }
        while (available > 0) {
            const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            DWORD got = 0;
            if (!WinHttpReadData(prepared_request->request.get(), buffer.data(), want, &got)) {
                if (request_was_canceled(request, prepared_request->cancel_monitor)) {
                    return status;
                }
                throw std::runtime_error("WinHttpReadData failed");
            }
            if (got == 0) {
                available = 0;
                break;
            }
            if (on_chunk && !on_chunk(std::string_view{buffer.data(), got})) {
                return status;
            }
            available -= got;
        }
    }
    return status;
}

}
