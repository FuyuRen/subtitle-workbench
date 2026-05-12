#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace swb::test {

struct CaseEntry {
    std::string_view name;
    std::function<void()> function;
};

inline std::vector<CaseEntry>& registry() {
    static std::vector<CaseEntry> cases;
    return cases;
}

struct Registrar {
    Registrar(std::string_view name, std::function<void()> function) {
        registry().push_back({name, std::move(function)});
    }
};

struct AssertionFailure : std::exception {
    std::string message;
    explicit AssertionFailure(std::string text) : message(std::move(text)) {}
    [[nodiscard]] const char* what() const noexcept override { return message.c_str(); }
};

template <typename A, typename B>
void expect_eq(
    const A& left,
    const B& right,
    std::source_location location = std::source_location::current()) {
    if (!(left == right)) {
        std::ostringstream output;
        output << location.file_name() << ':' << location.line() << " expect_eq failed";
        throw AssertionFailure(output.str());
    }
}

inline void expect_true(
    bool condition,
    std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::ostringstream output;
        output << location.file_name() << ':' << location.line() << " expect_true failed";
        throw AssertionFailure(output.str());
    }
}

inline int run_all() {
    int failed = 0;
    for (const auto& test_case : registry()) {
        try {
            test_case.function();
            std::cout << "[ OK   ] " << test_case.name << '\n';
        } catch (const AssertionFailure& e) {
            std::cout << "[ FAIL ] " << test_case.name << '\n' << e.what() << '\n';
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "[ FAIL ] " << test_case.name << "\n  unhandled exception: " << e.what() << '\n';
            ++failed;
        }
    }
    std::cout << '\n' << registry().size() << " cases, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

}
