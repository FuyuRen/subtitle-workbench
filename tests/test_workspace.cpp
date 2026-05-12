#include "swb_test.h"
#include "swb/workspace.h"

#include "swb/win32_headers.h"

#include <fstream>

using swb::test::expect_eq;
using swb::test::expect_true;

namespace {

const swb::test::Registrar case_1{
    "workspace: detects http/https urls",
    [] {
        expect_true(swb::is_url("http://example.com/v"));
        expect_true(swb::is_url("https://example.com/v"));
        expect_true(!swb::is_url("ftp://x"));
        expect_true(!swb::is_url("C:/videos/clip.mp4"));
        expect_true(!swb::is_url(""));
    },
};

const swb::test::Registrar case_2{
    "workspace: sanitize strips reserved characters",
    [] {
        expect_eq(swb::sanitize_path_component("a/b\\c:d?e*"), std::string{"a_b_c_d_e_"});
        expect_eq(swb::sanitize_path_component(""), std::string{"video"});
        expect_eq(swb::sanitize_path_component("trailing dots..."), std::string{"trailing dots"});
    },
};

const swb::test::Registrar case_3{
    "workspace: short hash is deterministic and 8 hex chars",
    [] {
        const auto first_hash = swb::short_hash("https://example.com/v?x=1");
        const auto second_hash = swb::short_hash("https://example.com/v?x=1");
        expect_eq(first_hash, second_hash);
        expect_eq(first_hash.size(), std::size_t{8});
        for (const auto character : first_hash) {
            const bool is_hex = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            expect_true(is_hex);
        }
        expect_true(swb::short_hash("a") != swb::short_hash("b"));
    },
};

const swb::test::Registrar case_4{
    "workspace: workdir name combines title and hash",
    [] {
        const auto name = swb::make_workdir_name("Some Title", "https://example.com/v");
        expect_true(name.starts_with("Some Title_"));
        expect_eq(name.size(), std::string_view{"Some Title_"}.size() + 8);
    },
};

const swb::test::Registrar case_5{
    "workspace: path to utf8 roundtrips unicode path",
    [] {
        const std::filesystem::path path{L"C:\\Users\\戴尔\\Downloads\\字幕工作流测试"};
        const std::string utf8 = swb::path_to_utf8(path);
        expect_eq(std::filesystem::u8path(utf8), path);
    },
};

const swb::test::Registrar case_6{
    "workspace: resolve tool from path",
    [] {
        const std::filesystem::path temporary_directory = std::filesystem::current_path() / "swb-tool-path-test";
        const std::filesystem::path tool_path = temporary_directory / "__swb_tool_path_test__.exe";

        std::error_code error_code;
        std::filesystem::create_directories(temporary_directory, error_code);
        {
            std::ofstream stream{tool_path};
            expect_true(stream.good());
        }

        const DWORD path_length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        std::wstring original_path;
        if (path_length > 0) {
            original_path.resize(path_length - 1);
            GetEnvironmentVariableW(L"PATH", original_path.data(), path_length);
        }

        SetEnvironmentVariableW(L"PATH", temporary_directory.c_str());
        const std::filesystem::path resolved = swb::resolve_tool(L"__swb_tool_path_test__.exe");
        expect_eq(resolved, tool_path);

        if (path_length > 0) {
            SetEnvironmentVariableW(L"PATH", original_path.c_str());
        } else {
            SetEnvironmentVariableW(L"PATH", nullptr);
        }
        std::filesystem::remove(tool_path, error_code);
        std::filesystem::remove(temporary_directory, error_code);
    },
};

const swb::test::Registrar case_7{
    "workspace: resolve tool returns empty when missing",
    [] {
        constexpr auto missing_tool_name = L"__swb_missing_tool_for_test_6ce544f7__.exe";
        const auto tool = swb::resolve_tool(missing_tool_name);
        expect_true(tool.empty());
    },
};

}
