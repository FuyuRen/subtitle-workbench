#include "swb/audio.h"

#include "swb/file_io.h"
#include "swb/little_endian.h"
#include "swb/process.h"
#include "swb/workspace.h"

#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace swb {

namespace {

constexpr std::uint32_t required_sample_rate = 16'000;
constexpr std::uint16_t required_channel_count = 1;
constexpr std::uint16_t required_bits_per_sample = 16;
constexpr std::uint16_t required_block_align = 2;

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

}

WavWindowReader::WavWindowReader(const std::filesystem::path& path)
    : input_(path, std::ios::binary) {
    std::array<char, 12> riff_header{};
    if (!input_.read(riff_header.data(), static_cast<std::streamsize>(riff_header.size()))
        || std::string_view{riff_header.data(), 4} != "RIFF"
        || std::string_view{riff_header.data() + 8, 4} != "WAVE") {
        error_ = "WAV文件格式无效";
        return;
    }

    bool has_format = false;
    for (;;) {
        std::array<char, 8> chunk_header{};
        if (!input_.read(chunk_header.data(), static_cast<std::streamsize>(chunk_header.size()))) {
            break;
        }
        const std::string_view chunk_id{chunk_header.data(), 4};
        const std::uint32_t chunk_size = read_little_endian_32(chunk_header.data() + 4);
        const std::streamoff chunk_data_offset = input_.tellg();
        if (chunk_data_offset < 0) {
            error_ = "无法读取WAV文件";
            return;
        }

        if (chunk_id == "fmt ") {
            if (chunk_size < 16) {
                error_ = "WAV格式块无效";
                return;
            }
            std::array<char, 16> format{};
            if (!input_.read(format.data(), static_cast<std::streamsize>(format.size()))) {
                error_ = "WAV格式块不完整";
                return;
            }
            const std::uint16_t format_code = read_little_endian_16(format.data());
            const std::uint16_t channels = read_little_endian_16(format.data() + 2);
            sample_rate_ = read_little_endian_32(format.data() + 4);
            block_align_ = read_little_endian_16(format.data() + 12);
            const std::uint16_t bits_per_sample = read_little_endian_16(format.data() + 14);
            if (format_code != 1
                || channels != required_channel_count
                || sample_rate_ != required_sample_rate
                || bits_per_sample != required_bits_per_sample
                || block_align_ != required_block_align) {
                error_ = "本地识别需要16kHz单声道PCM16 WAV";
                return;
            }
            has_format = true;
        } else if (chunk_id == "data") {
            data_offset_ = static_cast<std::uint64_t>(chunk_data_offset);
            data_size_ = chunk_size;
        }

        const std::uint64_t padded_size = static_cast<std::uint64_t>(chunk_size) + (chunk_size & 1u);
        if (padded_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            error_ = "WAV数据过大";
            return;
        }
        input_.clear();
        input_.seekg(chunk_data_offset + static_cast<std::streamoff>(padded_size));
        if (!input_) {
            error_ = "WAV数据块不完整";
            return;
        }
    }

    if (!has_format || data_offset_ == 0 || data_size_ == 0 || data_size_ % block_align_ != 0) {
        error_ = "WAV文件缺少有效音频数据";
        return;
    }
    input_.clear();
}

double WavWindowReader::duration_seconds() const noexcept {
    if (!valid() || sample_rate_ == 0 || block_align_ == 0) {
        return 0.0;
    }
    const std::uint64_t frame_count = data_size_ / block_align_;
    return static_cast<double>(frame_count) / static_cast<double>(sample_rate_);
}

std::optional<AudioWindow> WavWindowReader::read_next(int window_seconds, int overlap_seconds) {
    if (!valid() || window_seconds <= 0 || overlap_seconds < 0 || overlap_seconds >= window_seconds) {
        return std::nullopt;
    }

    const std::uint64_t total_frames = data_size_ / block_align_;
    if (next_frame_ >= total_frames) {
        return std::nullopt;
    }
    const std::uint64_t window_frames = static_cast<std::uint64_t>(window_seconds) * sample_rate_;
    const std::uint64_t overlap_frames = static_cast<std::uint64_t>(overlap_seconds) * sample_rate_;
    const std::uint64_t frame_count = std::min(window_frames, total_frames - next_frame_);
    if (frame_count > std::numeric_limits<std::size_t>::max() / sizeof(std::int16_t)) {
        error_ = "WAV窗口过大";
        return std::nullopt;
    }

    AudioWindow window;
    window.samples.resize(static_cast<std::size_t>(frame_count));
    window.start_seconds = static_cast<double>(next_frame_) / static_cast<double>(sample_rate_);
    window.duration_seconds = static_cast<double>(frame_count) / static_cast<double>(sample_rate_);

    const std::uint64_t byte_offset = data_offset_ + next_frame_ * block_align_;
    if (byte_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        error_ = "WAV读取位置越界";
        return std::nullopt;
    }
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(byte_offset));
    const std::streamsize byte_count = static_cast<std::streamsize>(frame_count * block_align_);
    if (!input_.read(reinterpret_cast<char*>(window.samples.data()), byte_count)) {
        error_ = "WAV音频数据不完整";
        return std::nullopt;
    }

    if (next_frame_ + frame_count >= total_frames) {
        next_frame_ = total_frames;
    } else {
        next_frame_ += window_frames - overlap_frames;
    }
    return window;
}

std::vector<float> pcm16_to_float(std::span<const std::int16_t> samples) {
    std::vector<float> floats;
    floats.reserve(samples.size());
    constexpr float scale = 1.0f / 32768.0f;
    for (const std::int16_t sample : samples) {
        floats.push_back(static_cast<float>(sample) * scale);
    }
    return floats;
}

bool write_pcm16_wav(
    const std::filesystem::path& path,
    std::span<const std::int16_t> samples,
    std::uint32_t sample_rate) {
    constexpr std::uint32_t wav_header_payload_size = 36u;
    if (sample_rate == 0
        || samples.size_bytes() > std::numeric_limits<std::uint32_t>::max() - wav_header_payload_size) {
        return false;
    }
    constexpr std::uint16_t block_align = required_channel_count * (required_bits_per_sample / 8);
    const std::uint32_t data_size = static_cast<std::uint32_t>(samples.size_bytes());
    const std::uint32_t riff_size = wav_header_payload_size + data_size;

    std::string header;
    header.reserve(44);
    header.append("RIFF", 4);
    append_little_endian_32(header, riff_size);
    header.append("WAVEfmt ", 8);
    append_little_endian_32(header, 16);
    append_little_endian_16(header, 1);
    append_little_endian_16(header, required_channel_count);
    append_little_endian_32(header, sample_rate);
    append_little_endian_32(header, sample_rate * block_align);
    append_little_endian_16(header, block_align);
    append_little_endian_16(header, required_bits_per_sample);
    header.append("data", 4);
    append_little_endian_32(header, data_size);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size_bytes()));
    return static_cast<bool>(output);
}

namespace {

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
        "-c:a", "pcm_s16le",
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
