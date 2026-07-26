#include "swb/transcribe.h"

#include "swb/text.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ranges>

namespace swb {

namespace {

[[nodiscard]] bool is_cjk_language(std::string_view language) {
    return language == "zh" || language == "ja" || language == "yue";
}

[[nodiscard]] bool starts_with_ascii_whitespace(std::string_view text) {
    return !text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n');
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>> decode_utf8(std::string_view text) {
    std::vector<std::uint32_t> code_points;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t continuation_count = 0;
        std::uint32_t minimum_code_point = 0;
        std::uint32_t code_point = 0;
        if (lead <= 0x7fu) {
            code_points.push_back(lead);
            ++index;
            continue;
        }
        if ((lead & 0xe0u) == 0xc0u) {
            continuation_count = 1;
            minimum_code_point = 0x80u;
            code_point = lead & 0x1fu;
        } else if ((lead & 0xf0u) == 0xe0u) {
            continuation_count = 2;
            minimum_code_point = 0x800u;
            code_point = lead & 0x0fu;
        } else if ((lead & 0xf8u) == 0xf0u) {
            continuation_count = 3;
            minimum_code_point = 0x10000u;
            code_point = lead & 0x07u;
        } else {
            return std::nullopt;
        }
        if (index + continuation_count >= text.size()) {
            return std::nullopt;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const unsigned char continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xc0u) != 0x80u) {
                return std::nullopt;
            }
            code_point = (code_point << 6u) | (continuation & 0x3fu);
        }
        if (code_point < minimum_code_point
            || code_point > 0x10ffffu
            || (code_point >= 0xd800u && code_point <= 0xdfffu)) {
            return std::nullopt;
        }
        code_points.push_back(code_point);
        index += continuation_count + 1;
    }
    return code_points;
}

[[nodiscard]] bool is_unicode_punctuation(std::uint32_t code_point) {
    if (code_point < 0x80u) {
        return (code_point >= 0x21u && code_point <= 0x2fu)
            || (code_point >= 0x3au && code_point <= 0x40u)
            || (code_point >= 0x5bu && code_point <= 0x60u)
            || (code_point >= 0x7bu && code_point <= 0x7eu);
    }
    return (code_point >= 0x2000u && code_point <= 0x206fu)
        || (code_point >= 0x3000u && code_point <= 0x303fu)
        || (code_point >= 0xfe10u && code_point <= 0xfe1fu)
        || (code_point >= 0xfe30u && code_point <= 0xfe4fu)
        || (code_point >= 0xff01u && code_point <= 0xff0fu)
        || (code_point >= 0xff1au && code_point <= 0xff20u)
        || (code_point >= 0xff3bu && code_point <= 0xff40u)
        || (code_point >= 0xff5bu && code_point <= 0xff65u);
}

[[nodiscard]] bool is_punctuation_text(std::string_view text) {
    const std::optional<std::vector<std::uint32_t>> code_points = decode_utf8(text);
    if (!code_points.has_value() || code_points->empty()) {
        return false;
    }
    return std::ranges::all_of(*code_points, [](std::uint32_t code_point) {
        return code_point == ' ' || code_point == '\t' || is_unicode_punctuation(code_point);
    });
}

[[nodiscard]] std::vector<std::uint32_t> normalize_word_for_deduplication(std::string_view word) {
    const std::string_view trimmed = trim_ascii_whitespace(word);
    const std::optional<std::vector<std::uint32_t>> decoded = decode_utf8(trimmed);
    if (!decoded.has_value()) {
        return {};
    }

    std::vector<std::uint32_t> normalized;
    normalized.reserve(decoded->size());
    for (std::uint32_t code_point : *decoded) {
        if (code_point == ' ' || code_point == '\t' || is_unicode_punctuation(code_point)) {
            continue;
        }
        if (code_point >= 'A' && code_point <= 'Z') {
            code_point = code_point - 'A' + 'a';
        }
        normalized.push_back(code_point);
    }
    return normalized;
}

[[nodiscard]] bool duplicate_words(const TranscriptWord& left, const TranscriptWord& right) {
    const std::vector<std::uint32_t> normalized_left = normalize_word_for_deduplication(left.word);
    const std::vector<std::uint32_t> normalized_right = normalize_word_for_deduplication(right.word);
    if (normalized_left.empty() || normalized_left != normalized_right) {
        return false;
    }
    const double overlap = std::min(left.end_seconds, right.end_seconds) - std::max(left.start_seconds, right.start_seconds);
    const double left_midpoint = (left.start_seconds + left.end_seconds) * 0.5;
    const double right_midpoint = (right.start_seconds + right.end_seconds) * 0.5;
    return overlap >= 0.0 || std::abs(left_midpoint - right_midpoint) <= 0.8;
}

}

TokenAggregationResult aggregate_transcript_tokens(
    std::span<const TranscriptToken> tokens,
    std::string_view language,
    double audio_duration_seconds) {
    TokenAggregationResult result;
    if (!std::isfinite(audio_duration_seconds) || audio_duration_seconds <= 0.0) {
        result.diagnostics.emplace_back("音频时长无效");
        return result;
    }

    TranscriptWord current_word;
    bool has_current_word = false;
    double last_end = 0.0;
    const auto flush_current = [&] {
        if (!has_current_word || current_word.word.empty()) {
            return;
        }
        current_word.start_seconds = std::max(current_word.start_seconds, last_end);
        current_word.end_seconds = std::max(current_word.end_seconds, current_word.start_seconds);
        last_end = current_word.end_seconds;
        result.words.push_back(std::move(current_word));
        current_word = {};
        has_current_word = false;
    };

    std::string pending_utf8;
    double pending_start = 0.0;
    double pending_end = 0.0;
    for (const TranscriptToken& token : tokens) {
        if (token.control || token.text.empty()) {
            continue;
        }
        if (!std::isfinite(token.start_seconds)
            || !std::isfinite(token.end_seconds)
            || token.end_seconds < 0.0
            || token.start_seconds > audio_duration_seconds) {
            result.diagnostics.emplace_back("丢弃时间戳无效的Token");
            continue;
        }

        double start = std::clamp(token.start_seconds, 0.0, audio_duration_seconds);
        double end = std::clamp(token.end_seconds, 0.0, audio_duration_seconds);
        if (end < start) {
            end = start;
            result.diagnostics.emplace_back("校正逆序Token时间戳");
        }

        if (pending_utf8.empty()) {
            pending_start = start;
        }
        pending_utf8.append(token.text);
        pending_end = end;
        if (!decode_utf8(pending_utf8).has_value()) {
            continue;
        }

        const bool has_leading_space = starts_with_ascii_whitespace(pending_utf8);
        const std::string_view trimmed = trim_ascii_whitespace(pending_utf8);
        if (trimmed.empty()) {
            if (!is_cjk_language(language)) {
                flush_current();
            }
            pending_utf8.clear();
            continue;
        }

        const bool punctuation = is_punctuation_text(trimmed);
        if (is_cjk_language(language)) {
            flush_current();
            if (punctuation && !result.words.empty()) {
                result.words.back().word.append(trimmed);
                result.words.back().end_seconds = std::max(result.words.back().end_seconds, pending_end);
                last_end = result.words.back().end_seconds;
            } else {
                current_word = {
                    .word = std::string{trimmed},
                    .start_seconds = pending_start,
                    .end_seconds = pending_end,
                };
                has_current_word = true;
                flush_current();
            }
        } else {
            if (punctuation) {
                if (has_current_word) {
                    current_word.word.append(trimmed);
                    current_word.end_seconds = pending_end;
                } else if (!result.words.empty()) {
                    result.words.back().word.append(trimmed);
                    result.words.back().end_seconds = std::max(result.words.back().end_seconds, pending_end);
                    last_end = result.words.back().end_seconds;
                } else {
                    current_word = {
                        .word = std::string{trimmed},
                        .start_seconds = pending_start,
                        .end_seconds = pending_end,
                    };
                    has_current_word = true;
                }
                pending_utf8.clear();
                continue;
            }
            if (has_leading_space && has_current_word) {
                flush_current();
            }
            if (!has_current_word) {
                current_word = {
                    .word = std::string{trimmed},
                    .start_seconds = pending_start,
                    .end_seconds = pending_end,
                };
                has_current_word = true;
            } else {
                current_word.word.append(trimmed);
                current_word.end_seconds = pending_end;
            }
        }
        pending_utf8.clear();
    }
    if (!pending_utf8.empty()) {
        result.diagnostics.emplace_back("丢弃不完整的UTF-8 Token序列");
    }
    flush_current();
    return result;
}

std::vector<TranscriptWord> merge_overlapping_transcript_words(
    std::span<const TranscriptWord> existing,
    std::span<const TranscriptWord> incoming,
    double incoming_window_start_seconds) {
    std::vector<TranscriptWord> merged;
    merged.reserve(existing.size() + incoming.size());

    const auto suffix = std::ranges::lower_bound(
        existing,
        incoming_window_start_seconds,
        {},
        &TranscriptWord::end_seconds);
    merged.insert(merged.end(), existing.begin(), suffix);

    std::vector<TranscriptWord> overlap;
    overlap.reserve(static_cast<std::size_t>(existing.end() - suffix) + incoming.size());
    overlap.insert(overlap.end(), suffix, existing.end());
    overlap.insert(overlap.end(), incoming.begin(), incoming.end());
    std::ranges::stable_sort(overlap, {}, &TranscriptWord::start_seconds);

    for (TranscriptWord& candidate : overlap) {
        if (candidate.word.empty()
            || !std::isfinite(candidate.start_seconds)
            || !std::isfinite(candidate.end_seconds)
            || candidate.end_seconds < candidate.start_seconds) {
            continue;
        }
        const bool duplicate = std::ranges::any_of(merged, [&](const TranscriptWord& word) {
            return word.end_seconds + 2.0 >= candidate.start_seconds && duplicate_words(word, candidate);
        });
        if (!duplicate) {
            merged.push_back(std::move(candidate));
        }
    }
    std::ranges::stable_sort(merged, {}, &TranscriptWord::start_seconds);
    double previous_end = 0.0;
    for (TranscriptWord& word : merged) {
        word.start_seconds = std::max(word.start_seconds, previous_end);
        word.end_seconds = std::max(word.end_seconds, word.start_seconds);
        previous_end = word.end_seconds;
    }
    return merged;
}

}
