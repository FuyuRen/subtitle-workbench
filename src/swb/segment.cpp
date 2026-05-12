#include "swb/segment.h"

#include <algorithm>
#include <string_view>

namespace swb {

namespace {

constexpr double split_gap_seconds = 0.6;
constexpr double split_gap_with_duration_seconds = 0.35;
constexpr double split_gap_min_duration_seconds = 1.4;
constexpr double sentence_punctuation_min_duration_seconds = 1.0;
constexpr double clause_punctuation_min_duration_seconds = 2.2;
constexpr std::size_t max_words_per_segment = 18;
constexpr double max_segment_duration_seconds = 6.0;
constexpr double max_segment_text_duration_gate_seconds = 1.2;
constexpr std::size_t max_segment_text_length = 84;
constexpr double merge_gap_seconds = 0.2;
constexpr double merge_max_duration_seconds = 6.8;
constexpr double merge_right_max_duration_seconds = 1.8;
constexpr double merge_right_short_duration_seconds = 1.4;
constexpr std::size_t merge_short_text_length = 12;
constexpr double start_padding_seconds = 0.0;
constexpr double end_padding_seconds = 0.12;
constexpr double adjacent_gap_seconds = 0.05;
constexpr double minimum_cue_duration_seconds = 0.25;

struct Slice {
    std::size_t begin{0};
    std::size_t end{0};
};

[[nodiscard]] bool starts_with_punctuation(std::string_view word) {
    return !word.empty() && std::string_view{",.;:?!)]}"}.find(word.front()) != std::string_view::npos;
}

[[nodiscard]] bool ends_with_any(std::string_view word, std::string_view punctuation) {
    return !word.empty() && punctuation.find(word.back()) != std::string_view::npos;
}

void append_word_text(std::string& text, std::string_view word) {
    if (!text.empty() && !starts_with_punctuation(word)) {
        text.push_back(' ');
    }
    text.append(word);
}

[[nodiscard]] std::string build_text(std::span<const TranscriptWord> words, const Slice& slice) {
    std::string text;
    for (std::size_t index = slice.begin; index < slice.end; ++index) {
        append_word_text(text, words[index].word);
    }
    return text;
}

[[nodiscard]] std::size_t text_length_with_next(std::span<const TranscriptWord> words, const Slice& slice, std::string_view next_word) {
    std::string text = build_text(words, slice);
    append_word_text(text, next_word);
    return text.size();
}

[[nodiscard]] double segment_duration(std::span<const TranscriptWord> words, const Slice& slice) {
    return words[slice.end - 1].end_seconds - words[slice.begin].start_seconds;
}

[[nodiscard]] double merged_duration(std::span<const TranscriptWord> words, const Slice& left, const Slice& right) {
    return words[right.end - 1].end_seconds - words[left.begin].start_seconds;
}

[[nodiscard]] bool should_split_before_next(
    std::span<const TranscriptWord> words,
    const Slice& current,
    const TranscriptWord& next_word) {
    const TranscriptWord& last_word = words[current.end - 1];
    const double gap_seconds = next_word.start_seconds - last_word.end_seconds;
    const double current_duration = segment_duration(words, current);
    const double next_timing_span_seconds = next_word.start_seconds - words[current.begin].start_seconds;

    if (gap_seconds >= split_gap_seconds) {
        return true;
    }
    if (gap_seconds >= split_gap_with_duration_seconds && current_duration >= split_gap_min_duration_seconds) {
        return true;
    }
    if (ends_with_any(last_word.word, ".?!") && current_duration >= sentence_punctuation_min_duration_seconds) {
        return true;
    }
    if (ends_with_any(last_word.word, ",;:") && current_duration >= clause_punctuation_min_duration_seconds) {
        return true;
    }
    if ((current.end - current.begin) >= max_words_per_segment) {
        return true;
    }
    if (next_timing_span_seconds > max_segment_duration_seconds && current_duration >= max_segment_text_duration_gate_seconds) {
        return true;
    }
    if (text_length_with_next(words, current, next_word.word) > max_segment_text_length && current_duration >= max_segment_text_duration_gate_seconds) {
        return true;
    }
    return false;
}

[[nodiscard]] bool should_merge_right(std::span<const TranscriptWord> words, const Slice& left, const Slice& right) {
    const double gap_seconds = words[right.begin].start_seconds - words[left.end - 1].end_seconds;
    if (gap_seconds > merge_gap_seconds) {
        return false;
    }
    if (merged_duration(words, left, right) > merge_max_duration_seconds) {
        return false;
    }

    const std::size_t right_word_count = right.end - right.begin;
    const double right_duration = segment_duration(words, right);
    const std::string right_text = build_text(words, right);
    if (right_word_count == 1 && right_text.size() <= merge_short_text_length) {
        return true;
    }
    if (right_word_count <= 2 && right_duration <= merge_right_max_duration_seconds) {
        return true;
    }
    return right_text.size() <= merge_short_text_length && right_duration <= merge_right_short_duration_seconds;
}

[[nodiscard]] bool should_shift_trailing_bridge_word(std::span<const TranscriptWord> words, const Slice& left, const Slice& right) {
    if ((left.end - left.begin) < 2 || segment_duration(words, left) <= max_segment_duration_seconds) {
        return false;
    }

    const TranscriptWord& trailing_word = words[left.end - 1];
    const double gap_seconds = words[right.begin].start_seconds - trailing_word.end_seconds;
    if (gap_seconds > merge_gap_seconds || ends_with_any(trailing_word.word, ".?!,;:")) {
        return false;
    }
    if (trailing_word.word.size() > 2) {
        return false;
    }

    const Slice shifted_right{
        .begin = right.begin - 1,
        .end = right.end,
    };
    if (segment_duration(words, shifted_right) > merge_max_duration_seconds) {
        return false;
    }
    return build_text(words, shifted_right).size() <= max_segment_text_length;
}

}

std::vector<SubtitleCue> segment_transcript(std::span<const TranscriptWord> words) {
    if (words.empty()) {
        return {};
    }

    std::vector<Slice> slices;
    slices.push_back({.begin = 0, .end = 1});
    for (std::size_t index = 1; index < words.size(); ++index) {
        if (should_split_before_next(words, slices.back(), words[index])) {
            slices.push_back({.begin = index, .end = index + 1});
        } else {
            ++slices.back().end;
        }
    }

    for (std::size_t index = 0; index + 1 < slices.size();) {
        if (should_merge_right(words, slices[index], slices[index + 1])) {
            slices[index].end = slices[index + 1].end;
            slices.erase(slices.begin() + static_cast<std::ptrdiff_t>(index + 1));
        } else {
            ++index;
        }
    }

    for (std::size_t index = 0; index + 1 < slices.size(); ++index) {
        if (should_shift_trailing_bridge_word(words, slices[index], slices[index + 1])) {
            --slices[index].end;
            --slices[index + 1].begin;
        }
    }

    std::vector<SubtitleCue> cues;
    cues.reserve(slices.size());
    for (std::size_t index = 0; index < slices.size(); ++index) {
        const Slice& slice = slices[index];
        const TranscriptWord& first_word = words[slice.begin];
        const TranscriptWord& last_word = words[slice.end - 1];

        SubtitleCue cue;
        cue.start_seconds = std::max(0.0, first_word.start_seconds - start_padding_seconds);
        cue.end_seconds = last_word.end_seconds + end_padding_seconds;
        cue.text = build_text(words, slice);

        if (index + 1 < slices.size()) {
            const TranscriptWord& next_first_word = words[slices[index + 1].begin];
            const double next_start_seconds = std::max(0.0, next_first_word.start_seconds - start_padding_seconds);
            const double latest_non_overlap_end = next_start_seconds - adjacent_gap_seconds;
            if (cue.end_seconds > latest_non_overlap_end) {
                cue.end_seconds = std::max(last_word.end_seconds, (last_word.end_seconds + next_first_word.start_seconds) * 0.5);
            } else {
                cue.end_seconds = std::min(cue.end_seconds, latest_non_overlap_end);
            }
        }

        cue.end_seconds = std::max(cue.end_seconds, last_word.end_seconds);
        cue.end_seconds = std::max(cue.end_seconds, cue.start_seconds + minimum_cue_duration_seconds);
        cues.push_back(std::move(cue));
    }

    normalize_sequential_cue_timings(cues);
    return cues;
}

}