#include "swb_test.h"
#include "swb/audio.h"
#include "swb/little_endian.h"

#include <filesystem>
#include <fstream>
#include <string>

using swb::test::expect_eq;
using swb::test::expect_true;

namespace {

[[nodiscard]] std::filesystem::path make_audio_directory(std::string_view stem) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "subtitle-workbench-tests" / "audio" / std::string{stem};
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

[[nodiscard]] std::string make_test_wav_bytes(int seconds) {
    constexpr std::uint32_t sample_rate = 16000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits_per_sample = 16;
    constexpr std::uint16_t block_align = channels * (bits_per_sample / 8);
    constexpr std::uint32_t bytes_per_second = sample_rate * block_align;

    std::string data(static_cast<std::size_t>(seconds) * bytes_per_second, '\0');
    const std::uint32_t riff_size = 4u + 8u + 16u + 8u + static_cast<std::uint32_t>(data.size());

    std::string output;
    output.reserve(8 + riff_size);
    output.append("RIFF", 4);
    swb::append_little_endian_32(output, riff_size);
    output.append("WAVE", 4);
    output.append("fmt ", 4);
    swb::append_little_endian_32(output, 16);
    swb::append_little_endian_16(output, 1);
    swb::append_little_endian_16(output, channels);
    swb::append_little_endian_32(output, sample_rate);
    swb::append_little_endian_32(output, bytes_per_second);
    swb::append_little_endian_16(output, block_align);
    swb::append_little_endian_16(output, bits_per_sample);
    output.append("data", 4);
    swb::append_little_endian_32(output, static_cast<std::uint32_t>(data.size()));
    output.append(data);
    return output;
}

void write_binary_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

[[nodiscard]] std::uint32_t wav_data_size(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (bytes.size() < 44) {
        return 0;
    }
    return swb::read_little_endian_32(bytes.data() + 40);
}

const swb::test::Registrar case_1{
    "audio: splits wav into five minute chunks",
    [] {
        const std::filesystem::path directory = make_audio_directory("split-five-minute-chunks");
        const std::filesystem::path input_path = directory / "audio.wav";
        write_binary_file(input_path, make_test_wav_bytes(620));

        const auto chunks = swb::split_wav_into_chunks(input_path, directory / "chunks", 300);

        expect_true(chunks.has_value());
        expect_eq(chunks->size(), std::size_t{3});
        expect_true(std::filesystem::exists((*chunks)[0].path));
        expect_true(std::filesystem::exists((*chunks)[1].path));
        expect_true(std::filesystem::exists((*chunks)[2].path));
        expect_true((*chunks)[0].start_seconds == 0.0);
        expect_true((*chunks)[1].start_seconds == 300.0);
        expect_true((*chunks)[2].start_seconds == 600.0);
        expect_eq(wav_data_size((*chunks)[0].path), 300u * 16000u * 2u);
        expect_eq(wav_data_size((*chunks)[1].path), 300u * 16000u * 2u);
        expect_eq(wav_data_size((*chunks)[2].path), 20u * 16000u * 2u);
    },
};

const swb::test::Registrar case_2{
    "audio: streams bounded overlapping wav windows",
    [] {
        const std::filesystem::path directory = make_audio_directory("stream-overlap");
        const std::filesystem::path input_path = directory / "audio.wav";
        write_binary_file(input_path, make_test_wav_bytes(65));

        swb::WavWindowReader reader{input_path};
        expect_true(reader.valid());
        expect_true(reader.duration_seconds() == 65.0);

        const auto first = reader.read_next(30, 2);
        const auto second = reader.read_next(30, 2);
        const auto third = reader.read_next(30, 2);
        const auto end = reader.read_next(30, 2);
        expect_true(first.has_value() && second.has_value() && third.has_value());
        expect_true(!end.has_value());
        expect_true(first->start_seconds == 0.0);
        expect_true(second->start_seconds == 28.0);
        expect_true(third->start_seconds == 56.0);
        expect_eq(first->samples.size(), std::size_t{30 * 16000});
        expect_eq(third->samples.size(), std::size_t{9 * 16000});
    },
};

}
