#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace swb::http {

struct Request {
    std::string url;
    std::string method{"POST"};
    std::vector<std::string> headers;
    std::string body;
    int timeout_ms{60'000};
    const std::atomic<bool>* cancel{nullptr};
};

struct Response {
    int status{0};
    std::string body;
};

[[nodiscard]] Response send(const Request& request);

using ChunkCallback = std::function<bool(std::string_view)>;

[[nodiscard]] int send_streaming(const Request& request, const ChunkCallback& on_chunk);

}
