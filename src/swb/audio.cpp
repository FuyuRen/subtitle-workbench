#include "swb/audio.h"

#include "swb/file_io.h"
#include "swb/little_endian.h"
#include "swb/process.h"
#include "swb/workspace.h"

#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace swb {

namespace {

struct ParsedWav {
    std::string fmt_chunk;
    std::string data_chunk;
    std::uint32_t sample_rate{0};
    std::uint16_t block_align{0};
};

[[nodiscard]] std::optional<ParsedWav> parse_wav(std::string_view bytes) {
    if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE") {
        return std::nullopt;
    }

    ParsedWav parsed;
    std::size_t position = 12;
    while (position + 8 <= bytes.size()) {
        const std::string_view chunk_id = bytes.substr(position, 4);
        const std::uint32_t chunk_size = read_little_endian_32(bytes.data() + position + 4);
        position += 8;
        if (position + chunk_size > bytes.size()) {
            return std::nullopt;
        }

        const std::string_view chunk_data = bytes.substr(position, chunk_size);
        if (chunk_id == "fmt ") {
            if (chunk_size < 16) {
                return std::nullopt;
            }
            parsed.fmt_chunk.assign(chunk_data);
            parsed.sample_rate = read_little_endian_32(chunk_data.data() + 4);
            parsed.block_align = read_little_endian_16(chunk_data.data() + 12);
        } else if (chunk_id == "data") {
            parsed.data_chunk.assign(chunk_data);
        }

        position += chunk_size;
        if ((chunk_size & 1u) != 0u) {
            ++position;
        }
    }

    if (parsed.fmt_chunk.empty() || parsed.data_chunk.empty() || parsed.sample_rate == 0 || parsed.block_align == 0) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::string build_wav_file(std::string_view fmt_chunk, std::string_view data_chunk) {
    const std::uint32_t padded_data_size = static_cast<std::uint32_t>(data_chunk.size() + (data_chunk.size() % 2u));
    const std::uint32_t riff_size = 4u + 8u + static_cast<std::uint32_t>(fmt_chunk.size()) + 8u + padded_data_size;

    std::string output;
    output.reserve(8 + riff_size);
    output.append("RIFF", 4);
    append_little_endian_32(output, riff_size);
    output.append("WAVE", 4);
    output.append("fmt ", 4);
    append_little_endian_32(output, static_cast<std::uint32_t>(fmt_chunk.size()));
    output.append(fmt_chunk);
    output.append("data", 4);
    append_little_endian_32(output, static_cast<std::uint32_t>(data_chunk.size()));
    output.append(data_chunk);
    if ((data_chunk.size() & 1u) != 0u) {
        output.push_back('\0');
    }
    return output;
}

[[nodiscard]] std::string chunk_filename(std::size_t index) {
    std::ostringstream output;
    output << "chunk-" << std::setw(3) << std::setfill('0') << index << ".wav";
    return output.str();
}

}

std::optional<std::vector<AudioChunk>> split_wav_into_chunks(
    const std::filesystem::path& input_wav,
    const std::filesystem::path& output_directory,
    int chunk_seconds) {
    if (chunk_seconds <= 0) {
        return std::nullopt;
    }

    const std::optional<std::string> wav_bytes = read_binary_file(input_wav);
    if (!wav_bytes.has_value()) {
        return std::nullopt;
    }

    const std::optional<ParsedWav> parsed = parse_wav(*wav_bytes);
    if (!parsed.has_value()) {
        return std::nullopt;
    }

    const std::uint64_t bytes_per_second = static_cast<std::uint64_t>(parsed->sample_rate) * parsed->block_align;
    if (bytes_per_second == 0) {
        return std::nullopt;
    }

    std::uint64_t chunk_bytes = static_cast<std::uint64_t>(chunk_seconds) * bytes_per_second;
    chunk_bytes -= chunk_bytes % parsed->block_align;
    if (chunk_bytes == 0) {
        return std::nullopt;
    }

    std::error_code error_code;
    std::filesystem::remove_all(output_directory, error_code);
    std::filesystem::create_directories(output_directory, error_code);
    if (error_code) {
        return std::nullopt;
    }

    std::vector<AudioChunk> chunks;
    for (std::uint64_t offset = 0; offset < parsed->data_chunk.size(); offset += chunk_bytes) {
        const std::size_t size = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_bytes, parsed->data_chunk.size() - offset));
        const std::filesystem::path chunk_path = output_directory / std::filesystem::u8path(chunk_filename(chunks.size()));
        const std::string chunk_wav = build_wav_file(
            parsed->fmt_chunk,
            std::string_view{parsed->data_chunk}.substr(static_cast<std::size_t>(offset), size));

        std::ofstream output(chunk_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::nullopt;
        }
        output.write(chunk_wav.data(), static_cast<std::streamsize>(chunk_wav.size()));
        if (!output) {
            return std::nullopt;
        }

        chunks.push_back({
            .path = chunk_path,
            .start_seconds = static_cast<double>(offset) / static_cast<double>(bytes_per_second),
        });
    }

    if (chunks.empty()) {
        return std::nullopt;
    }
    return chunks;
}

Ffmpeg::Ffmpeg(std::filesystem::path executable) : executable_(std::move(executable)) {}

std::filesystem::path Ffmpeg::resolve_executable() {
    return resolve_tool(L"ffmpeg.exe");
}

std::optional<std::filesystem::path> Ffmpeg::extract_wav_16k_mono(
    const std::filesystem::path& video,
    const std::filesystem::path& output_wav,
    const std::atomic<bool>& cancel) const {
    std::error_code error_code;
    std::filesystem::remove(output_wav, error_code);

    ProcessOptions process_options;
    process_options.executable = executable_;
    process_options.cancel = &cancel;
    process_options.timeout_ms = 600'000;
    process_options.arguments = {
        "-y",
        "-hide_banner",
        "-loglevel", "error",
        "-nostdin",
        "-i", path_to_utf8(video),
        "-vn",
        "-ac", "1",
        "-ar", "16000",
        "-f", "wav",
        path_to_utf8(output_wav),
    };

    const ProcessResult result = run_process(process_options);
    if (!result.launched || result.canceled || result.exit_code != 0) {
        return std::nullopt;
    }
    if (!std::filesystem::exists(output_wav, error_code)) {
        return std::nullopt;
    }
    return output_wav;
}

}
