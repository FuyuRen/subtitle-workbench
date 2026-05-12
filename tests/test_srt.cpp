#include "swb_test.h"
#include "swb/srt.h"

#include <filesystem>

using swb::test::expect_eq;
using swb::test::expect_true;

namespace {

[[nodiscard]] std::filesystem::path make_srt_path(std::string_view stem) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "subtitle-workbench-tests" / "srt";
    std::filesystem::create_directories(directory);
    return directory / (std::string{stem} + ".srt");
}

const swb::test::Registrar case_1{
    "srt: serializes and parses roundtrip",
    [] {
        const std::vector<swb::SubtitleCue> original{
            swb::SubtitleCue{.start_seconds = 0.05, .end_seconds = 1.2, .text = "hello world"},
            swb::SubtitleCue{.start_seconds = 1.5, .end_seconds = 3.0, .text = "line one\nline two"},
        };

        const std::string content = swb::serialize_srt(original);
        const swb::SrtReadResult parsed = swb::parse_srt(content);

        expect_true(parsed.success);
        expect_eq(parsed.cues.size(), std::size_t{2});
        expect_eq(parsed.cues[0].text, std::string{"hello world"});
        expect_eq(parsed.cues[1].text, std::string{"line one\nline two"});
        expect_eq(swb::format_srt_timestamp(parsed.cues[0].start_seconds), std::string{"00:00:00,050"});
        expect_eq(swb::format_srt_timestamp(parsed.cues[1].end_seconds), std::string{"00:00:03,000"});
    },
};

const swb::test::Registrar case_2{
    "srt: saves and loads file",
    [] {
        const std::filesystem::path path = make_srt_path("roundtrip");
        const std::vector<swb::SubtitleCue> cues{
            swb::SubtitleCue{.start_seconds = 12.345, .end_seconds = 15.678, .text = "persisted cue"},
        };

        expect_true(swb::save_srt(cues, path));
        const swb::SrtReadResult loaded = swb::load_srt(path);

        expect_true(loaded.success);
        expect_eq(loaded.cues.size(), std::size_t{1});
        expect_eq(loaded.cues[0].text, std::string{"persisted cue"});
    },
};

}