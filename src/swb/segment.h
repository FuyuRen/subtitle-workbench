#pragma once

#include "swb/srt.h"
#include "swb/transcribe.h"

#include <span>
#include <vector>

namespace swb {

[[nodiscard]] std::vector<SubtitleCue> segment_transcript(std::span<const TranscriptWord> words);

}