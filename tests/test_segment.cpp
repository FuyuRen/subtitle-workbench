#include "swb_test.h"
#include "swb/segment.h"

#include <vector>

using swb::test::expect_eq;
using swb::test::expect_true;

namespace {

const swb::test::Registrar case_1{
    "segment: splits on punctuation and gap",
    [] {
        const std::vector<swb::TranscriptWord> words{
            swb::TranscriptWord{.word = "hello", .start_seconds = 0.00, .end_seconds = 0.30},
            swb::TranscriptWord{.word = "world.", .start_seconds = 0.30, .end_seconds = 1.30},
            swb::TranscriptWord{.word = "again", .start_seconds = 2.10, .end_seconds = 2.40},
            swb::TranscriptWord{.word = "there", .start_seconds = 2.40, .end_seconds = 2.80},
        };

        const std::vector<swb::SubtitleCue> cues = swb::segment_transcript(words);

        expect_eq(cues.size(), std::size_t{2});
        expect_eq(cues[0].text, std::string{"hello world."});
        expect_eq(cues[1].text, std::string{"again there"});
        expect_eq(swb::format_srt_timestamp(cues[0].start_seconds), std::string{"00:00:00,000"});
        expect_eq(swb::format_srt_timestamp(cues[0].end_seconds), std::string{"00:00:01,420"});
        expect_eq(swb::format_srt_timestamp(cues[1].start_seconds), std::string{"00:00:02,100"});
    },
};

const swb::test::Registrar case_2{
    "segment: merges short right tail",
    [] {
        const std::vector<swb::TranscriptWord> words{
            swb::TranscriptWord{.word = "this", .start_seconds = 0.00, .end_seconds = 0.40},
            swb::TranscriptWord{.word = "works.", .start_seconds = 0.40, .end_seconds = 1.35},
            swb::TranscriptWord{.word = "okay", .start_seconds = 1.50, .end_seconds = 1.80},
        };

        const std::vector<swb::SubtitleCue> cues = swb::segment_transcript(words);

        expect_eq(cues.size(), std::size_t{1});
        expect_eq(cues[0].text, std::string{"this works. okay"});
    },
};

const swb::test::Registrar case_3{
    "segment: adjacent cues do not overlap",
    [] {
        const std::vector<swb::TranscriptWord> words{
            swb::TranscriptWord{.word = "hello", .start_seconds = 0.00, .end_seconds = 0.40},
            swb::TranscriptWord{.word = "there.", .start_seconds = 0.40, .end_seconds = 1.50},
            swb::TranscriptWord{.word = "again", .start_seconds = 1.50, .end_seconds = 1.90},
            swb::TranscriptWord{.word = "old", .start_seconds = 1.90, .end_seconds = 2.30},
            swb::TranscriptWord{.word = "friend", .start_seconds = 2.30, .end_seconds = 2.70},
        };

        const std::vector<swb::SubtitleCue> cues = swb::segment_transcript(words);

        expect_eq(cues.size(), std::size_t{2});
        expect_true(cues[0].end_seconds <= cues[1].start_seconds);
    },
};

const swb::test::Registrar case_4{
    "segment: overlapping sentence boundary is normalized",
    [] {
        const std::vector<swb::TranscriptWord> words{
            swb::TranscriptWord{.word = "basically", .start_seconds = 91.28, .end_seconds = 91.72},
            swb::TranscriptWord{.word = "this", .start_seconds = 91.72, .end_seconds = 92.04},
            swb::TranscriptWord{.word = "place", .start_seconds = 92.04, .end_seconds = 92.34},
            swb::TranscriptWord{.word = "blows", .start_seconds = 92.34, .end_seconds = 92.92},
            swb::TranscriptWord{.word = "up.", .start_seconds = 92.92, .end_seconds = 93.58},
            swb::TranscriptWord{.word = "it", .start_seconds = 93.26, .end_seconds = 93.54},
            swb::TranscriptWord{.word = "started", .start_seconds = 93.54, .end_seconds = 94.06},
            swb::TranscriptWord{.word = "small.", .start_seconds = 94.06, .end_seconds = 95.18},
        };

        const std::vector<swb::SubtitleCue> cues = swb::segment_transcript(words);

        expect_eq(cues.size(), std::size_t{2});
        expect_true(cues[0].end_seconds <= cues[1].start_seconds);
        expect_eq(cues[0].text, std::string{"basically this place blows up."});
        expect_eq(cues[1].text, std::string{"it started small."});
    },
};

const swb::test::Registrar case_5{
    "segment: keeps short fluent sentence together despite stretched timings",
    [] {
        const std::vector<swb::TranscriptWord> words{
            swb::TranscriptWord{.word = "oh", .start_seconds = 172.38, .end_seconds = 172.86},
            swb::TranscriptWord{.word = "my", .start_seconds = 172.86, .end_seconds = 173.14},
            swb::TranscriptWord{.word = "god", .start_seconds = 173.14, .end_seconds = 174.02},
            swb::TranscriptWord{.word = "I'm", .start_seconds = 174.02, .end_seconds = 175.90},
            swb::TranscriptWord{.word = "gonna", .start_seconds = 175.90, .end_seconds = 177.30},
            swb::TranscriptWord{.word = "quit", .start_seconds = 177.30, .end_seconds = 177.54},
            swb::TranscriptWord{.word = "this", .start_seconds = 177.54, .end_seconds = 178.16},
            swb::TranscriptWord{.word = "game", .start_seconds = 178.16, .end_seconds = 178.22},
        };

        const std::vector<swb::SubtitleCue> cues = swb::segment_transcript(words);

        expect_eq(cues.size(), std::size_t{1});
        expect_eq(cues[0].text, std::string{"oh my god I'm gonna quit this game"});
    },
};

const swb::test::Registrar case_6{
    "segment: merges short one-word tail after word-limit split",
    [] {
        const std::vector<swb::TranscriptWord> words{
            swb::TranscriptWord{.word = "wait", .start_seconds = 40.78, .end_seconds = 41.24},
            swb::TranscriptWord{.word = "i", .start_seconds = 41.24, .end_seconds = 41.48},
            swb::TranscriptWord{.word = "just", .start_seconds = 41.48, .end_seconds = 41.76},
            swb::TranscriptWord{.word = "blew", .start_seconds = 41.76, .end_seconds = 41.90},
            swb::TranscriptWord{.word = "up", .start_seconds = 41.90, .end_seconds = 42.04},
            swb::TranscriptWord{.word = "my", .start_seconds = 42.04, .end_seconds = 42.28},
            swb::TranscriptWord{.word = "spawner", .start_seconds = 42.28, .end_seconds = 44.06},
            swb::TranscriptWord{.word = "all", .start_seconds = 44.06, .end_seconds = 45.20},
            swb::TranscriptWord{.word = "right", .start_seconds = 45.20, .end_seconds = 45.20},
            swb::TranscriptWord{.word = "it's", .start_seconds = 45.20, .end_seconds = 45.34},
            swb::TranscriptWord{.word = "uh", .start_seconds = 45.34, .end_seconds = 45.56},
            swb::TranscriptWord{.word = "it's", .start_seconds = 45.56, .end_seconds = 45.70},
            swb::TranscriptWord{.word = "a", .start_seconds = 45.70, .end_seconds = 45.88},
            swb::TranscriptWord{.word = "four", .start_seconds = 45.88, .end_seconds = 46.10},
            swb::TranscriptWord{.word = "eye", .start_seconds = 46.10, .end_seconds = 47.38},
        };

        const std::vector<swb::SubtitleCue> cues = swb::segment_transcript(words);

        expect_eq(cues.size(), std::size_t{1});
        expect_eq(cues[0].text, std::string{"wait i just blew up my spawner all right it's uh it's a four eye"});
    },
};

const swb::test::Registrar case_7{
    "segment: shifts trailing bridge word onto the next cue",
    [] {
        const std::vector<swb::TranscriptWord> words{
            swb::TranscriptWord{.word = "oh", .start_seconds = 172.38, .end_seconds = 172.86},
            swb::TranscriptWord{.word = "my", .start_seconds = 172.86, .end_seconds = 173.14},
            swb::TranscriptWord{.word = "god", .start_seconds = 173.14, .end_seconds = 174.02},
            swb::TranscriptWord{.word = "I'm", .start_seconds = 174.02, .end_seconds = 175.90},
            swb::TranscriptWord{.word = "gonna", .start_seconds = 175.90, .end_seconds = 177.30},
            swb::TranscriptWord{.word = "quit", .start_seconds = 177.30, .end_seconds = 177.54},
            swb::TranscriptWord{.word = "this", .start_seconds = 177.54, .end_seconds = 178.16},
            swb::TranscriptWord{.word = "game", .start_seconds = 178.16, .end_seconds = 178.22},
            swb::TranscriptWord{.word = "I", .start_seconds = 178.22, .end_seconds = 182.24},
            swb::TranscriptWord{.word = "bet", .start_seconds = 182.24, .end_seconds = 182.40},
            swb::TranscriptWord{.word = "the", .start_seconds = 182.40, .end_seconds = 182.62},
            swb::TranscriptWord{.word = "same", .start_seconds = 182.62, .end_seconds = 182.78},
            swb::TranscriptWord{.word = "thing", .start_seconds = 182.78, .end_seconds = 183.00},
            swb::TranscriptWord{.word = "is", .start_seconds = 183.00, .end_seconds = 183.18},
            swb::TranscriptWord{.word = "gonna", .start_seconds = 183.18, .end_seconds = 183.30},
            swb::TranscriptWord{.word = "happen", .start_seconds = 183.30, .end_seconds = 183.50},
            swb::TranscriptWord{.word = "to", .start_seconds = 183.50, .end_seconds = 183.74},
        };

        const std::vector<swb::SubtitleCue> cues = swb::segment_transcript(words);

        expect_eq(cues.size(), std::size_t{2});
        expect_eq(cues[0].text, std::string{"oh my god I'm gonna quit this game"});
        expect_eq(cues[1].text, std::string{"I bet the same thing is gonna happen to"});
    },
};

const swb::test::Registrar case_8{
    "segment: keeps minecraft for fun sentence together",
    [] {
        const std::vector<swb::TranscriptWord> words{
            swb::TranscriptWord{.word = "and", .start_seconds = 7.34, .end_seconds = 7.52},
            swb::TranscriptWord{.word = "these", .start_seconds = 7.52, .end_seconds = 7.78},
            swb::TranscriptWord{.word = "are", .start_seconds = 7.78, .end_seconds = 7.98},
            swb::TranscriptWord{.word = "20", .start_seconds = 7.98, .end_seconds = 8.14},
            swb::TranscriptWord{.word = "of", .start_seconds = 8.14, .end_seconds = 8.28},
            swb::TranscriptWord{.word = "my", .start_seconds = 8.28, .end_seconds = 8.42},
            swb::TranscriptWord{.word = "friends", .start_seconds = 8.42, .end_seconds = 8.86},
            swb::TranscriptWord{.word = "who", .start_seconds = 8.86, .end_seconds = 9.02},
            swb::TranscriptWord{.word = "let's", .start_seconds = 9.02, .end_seconds = 9.28},
            swb::TranscriptWord{.word = "just", .start_seconds = 9.28, .end_seconds = 9.52},
            swb::TranscriptWord{.word = "say", .start_seconds = 9.52, .end_seconds = 9.74},
            swb::TranscriptWord{.word = "like", .start_seconds = 9.74, .end_seconds = 9.96},
            swb::TranscriptWord{.word = "to", .start_seconds = 9.96, .end_seconds = 10.10},
            swb::TranscriptWord{.word = "play", .start_seconds = 10.10, .end_seconds = 10.32},
            swb::TranscriptWord{.word = "Minecraft", .start_seconds = 10.32, .end_seconds = 10.98},
            swb::TranscriptWord{.word = "for", .start_seconds = 10.98, .end_seconds = 11.26},
            swb::TranscriptWord{.word = "fun", .start_seconds = 11.26, .end_seconds = 12.04},
        };

        const std::vector<swb::SubtitleCue> cues = swb::segment_transcript(words);

        expect_eq(cues.size(), std::size_t{1});
        expect_eq(cues[0].text, std::string{"and these are 20 of my friends who let's just say like to play Minecraft for fun"});
    },
};

}