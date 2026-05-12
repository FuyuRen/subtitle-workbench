#include "swb_test.h"
#include "test_support.h"
#include "swb/ytdlp.h"

#include <atomic>
#include <filesystem>
#include <optional>

using swb::test::contains_argument;
using swb::test::expect_eq;
using swb::test::expect_true;
using swb::test::make_temp_directory;
using swb::test::write_text_file;

namespace {

const swb::test::Registrar case_1{
    "ytdlp: fetch title forces utf8 output encoding",
    [] {
        std::optional<swb::ProcessOptions> captured_options;
        const swb::YtDlp ytdlp{
            std::filesystem::path{"yt-dlp.exe"},
            [&](const swb::ProcessOptions& options) {
                captured_options = options;
                return swb::ProcessResult{
                    .exit_code = 0,
                    .stdout_data = "标题\n",
                };
            }};

        const std::atomic<bool> cancel{false};
        const std::optional<std::string> title = ytdlp.fetch_title("https://example.test/video", cancel);

        expect_true(captured_options.has_value());
        expect_true(title.has_value());
        expect_eq(*title, std::string{"标题"});
        expect_true(contains_argument(captured_options->arguments, "--encoding"));
        expect_true(contains_argument(captured_options->arguments, "utf-8"));
    },
};

const swb::test::Registrar case_2{
    "ytdlp: download video removes stale fragment residue first",
    [] {
        const std::filesystem::path directory = make_temp_directory("ytdlp", "cleanup-fragment");
        const std::filesystem::path fragment = directory / "video.f30080.mp4";
        const std::filesystem::path merged = directory / "video.mp4";
        write_text_file(fragment, "stale");

        bool fragment_exists_during_run = true;
        const swb::YtDlp ytdlp{
            std::filesystem::path{"yt-dlp.exe"},
            [&](const swb::ProcessOptions&) {
                fragment_exists_during_run = std::filesystem::exists(fragment);
                write_text_file(merged, "merged");
                return swb::ProcessResult{
                    .exit_code = 0,
                };
            }};

        const std::atomic<bool> cancel{false};
        const std::optional<std::filesystem::path> result = ytdlp.download_video(
            "https://example.test/video",
            directory,
            cancel);

        expect_true(result.has_value());
        expect_true(!fragment_exists_during_run);
        expect_true(std::filesystem::exists(merged));

        std::filesystem::remove_all(directory);
    },
};

}