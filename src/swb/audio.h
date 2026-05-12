#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <vector>

namespace swb {

struct AudioChunk {
    std::filesystem::path path;
    double start_seconds{0.0};
};

[[nodiscard]] std::optional<std::vector<AudioChunk>> split_wav_into_chunks(
    const std::filesystem::path& input_wav,
    const std::filesystem::path& output_directory,
    int chunk_seconds = 300);

class Ffmpeg {
public:
    explicit Ffmpeg(std::filesystem::path executable);

    [[nodiscard]] static std::filesystem::path resolve_executable();

    [[nodiscard]] std::optional<std::filesystem::path> extract_wav_16k_mono(
        const std::filesystem::path& video,
        const std::filesystem::path& output_wav,
        const std::atomic<bool>& cancel) const;

    [[nodiscard]] const std::filesystem::path& executable() const noexcept { return executable_; }

private:
    std::filesystem::path executable_;
};

}
