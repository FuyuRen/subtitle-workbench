#pragma once

#include <string>
#include <utility>

namespace swb {

struct OperationStatus {
    bool success{false};
    std::string message;

    OperationStatus() = default;

    OperationStatus(bool operation_succeeded, std::string status_message)
        : success(operation_succeeded),
          message(std::move(status_message)) {}
};

struct HttpOperationStatus : OperationStatus {
    int http_status{0};

    HttpOperationStatus() = default;

    HttpOperationStatus(bool operation_succeeded, std::string status_message, int http_status_code = 0)
        : OperationStatus(operation_succeeded, std::move(status_message)),
          http_status(http_status_code) {}
};

}