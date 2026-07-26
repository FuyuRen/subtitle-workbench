#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace swb {

struct AudioChunk {
    std::filesystem::path path;
    double start_seconds{0.0};
};

struct AudioWindow {
    std::vector<std::int16_t> samples;
    double start_seconds{0.0};
    double duration_seconds{0.0};
};

class WavWindowReader {
public:
    explicit WavWindowReader(const std::filesystem::path& path);

    WavWindowReader(const WavWindowReader&) = delete;
    WavWindowReader& operator=(const WavWindowReader&) = delete;

    [[nodiscard]] bool valid() const noexcept { return error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] double duration_seconds() const noexcept;
    [[nodiscard]] std::optional<AudioWindow> read_next(int window_seconds, int overlap_seconds);

private:
    std::ifstream input_;
    std::uint64_t data_offset_{0};
    std::uint64_t data_size_{0};
    std::uint64_t next_frame_{0};
    std::uint32_t sample_rate_{0};
    std::uint16_t block_align_{0};
    std::string error_;
};

[[nodiscard]] std::vector<float> pcm16_to_float(std::span<const std::int16_t> samples);

[[nodiscard]] bool write_pcm16_wav(
    const std::filesystem::path& path,
    std::span<const std::int16_t> samples,
    std::uint32_t sample_rate = 16'000);

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
