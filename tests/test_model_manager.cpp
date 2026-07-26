#include "swb_test.h"
#include "test_support.h"
#include "swb/model_manager.h"

#include <atomic>
#include <filesystem>
#include <string>

using swb::test::expect_eq;
using swb::test::expect_true;
using swb::test::make_temp_directory;
using swb::test::read_text_file;
using swb::test::write_text_file;

namespace {

[[nodiscard]] swb::ModelManifestEntry small_model() {
    return {
        .id = "test-small",
        .kind = swb::ManagedModelKind::speech_recognition,
        .display_name = "Test Small",
        .version = "1",
        .filename = "small.bin",
        .download_url = "https://models.example/small.bin",
        .file_size = 3,
        .sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        .model_type = swb::WhisperModelType::base_multilingual,
        .alignment_heads = "base",
        .license = "MIT",
        .source = "https://models.example",
    };
}

[[nodiscard]] std::filesystem::path part_path(const std::filesystem::path& final_path) {
    std::filesystem::path result = final_path;
    result += L".part";
    return result;
}

const swb::test::Registrar case_1{
    "model: manifest pins multilingual asr and vad artifacts",
    [] {
        const auto manifest = swb::model_manifest();
        expect_true(manifest.size() >= 2);
        const swb::ModelManifestEntry& asr = swb::default_local_asr_model();
        const swb::ModelManifestEntry& vad = swb::default_vad_model();
        expect_eq(asr.id, std::string_view{"base-q5_1"});
        expect_eq(asr.file_size, std::uint64_t{59'707'625});
        expect_eq(asr.sha256, std::string_view{"422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898"});
        expect_true(asr.download_url.find("3eed2a5fe2724c340a3cf93f0802610a2ab06a0d") != std::string_view::npos);
        expect_eq(vad.file_size, std::uint64_t{885'098});
        expect_true(vad.download_url.find("9ffd54a1e1ee413ddf265af9913beaf518d1639b") != std::string_view::npos);
        expect_true(swb::find_model_manifest_entry("missing") == nullptr);
    },
};

const swb::test::Registrar case_2{
    "model: hashes and inspects files",
    [] {
        const std::filesystem::path directory = make_temp_directory("model-hash");
        const swb::ModelManifestEntry entry = small_model();
        const std::filesystem::path path = swb::model_file_path(entry, directory);
        write_text_file(path, "abc");

        const auto hash = swb::sha256_file(path);
        expect_true(hash.has_value());
        expect_eq(*hash, std::string{entry.sha256});
        const swb::ModelStatus status = swb::inspect_model(entry, directory);
        expect_true(status.availability == swb::ModelAvailability::available);
        expect_eq(status.path, path);
    },
};

const swb::test::Registrar case_3{
    "model: streaming download validates and atomically commits",
    [] {
        const std::filesystem::path directory = make_temp_directory("model-download-success");
        const swb::ModelManifestEntry entry = small_model();
        const std::atomic<bool> cancel{false};
        bool request_verified = false;
        std::uint64_t last_progress = 0;

        const swb::ModelDownloadResult result = swb::download_model(
            entry,
            directory,
            false,
            cancel,
            [&](const swb::ModelDownloadProgress& progress) {
                expect_true(progress.downloaded_bytes >= last_progress);
                last_progress = progress.downloaded_bytes;
            },
            [&](const swb::http::Request& request, const swb::http::ChunkCallback& on_chunk) {
                request_verified = request.method == "GET"
                    && request.url == entry.download_url
                    && request.cancel == &cancel;
                expect_true(on_chunk("a"));
                expect_true(on_chunk("bc"));
                return 200;
            });

        const std::filesystem::path final_path = swb::model_file_path(entry, directory);
        expect_true(result.success);
        expect_true(request_verified);
        expect_eq(last_progress, std::uint64_t{3});
        expect_eq(read_text_file(final_path), std::string{"abc"});
        expect_true(!std::filesystem::exists(part_path(final_path)));
    },
};

const swb::test::Registrar case_4{
    "model: interrupted and corrupt downloads remove part files",
    [] {
        const swb::ModelManifestEntry entry = small_model();
        for (const bool corrupt : {false, true}) {
            const std::filesystem::path directory = make_temp_directory(
                corrupt ? "model-download-corrupt" : "model-download-interrupted");
            const std::atomic<bool> cancel{false};
            const swb::ModelDownloadResult result = swb::download_model(
                entry,
                directory,
                false,
                cancel,
                {},
                [corrupt](const swb::http::Request&, const swb::http::ChunkCallback& on_chunk) {
                    expect_true(on_chunk(corrupt ? "abd" : "ab"));
                    return 200;
                });

            const std::filesystem::path final_path = swb::model_file_path(entry, directory);
            expect_true(!result.success);
            expect_true(!std::filesystem::exists(final_path));
            expect_true(!std::filesystem::exists(part_path(final_path)));
        }
    },
};

const swb::test::Registrar case_5{
    "model: cancellation stops stream and leaves no usable artifact",
    [] {
        const std::filesystem::path directory = make_temp_directory("model-download-cancel");
        const swb::ModelManifestEntry entry = small_model();
        std::atomic<bool> cancel{false};
        const swb::ModelDownloadResult result = swb::download_model(
            entry,
            directory,
            false,
            cancel,
            {},
            [&](const swb::http::Request&, const swb::http::ChunkCallback& on_chunk) {
                expect_true(on_chunk("a"));
                cancel.store(true, std::memory_order_release);
                expect_true(!on_chunk("bc"));
                return 200;
            });

        const std::filesystem::path final_path = swb::model_file_path(entry, directory);
        expect_true(!result.success);
        expect_eq(result.message, std::string{"已取消"});
        expect_true(!std::filesystem::exists(final_path));
        expect_true(!std::filesystem::exists(part_path(final_path)));
    },
};

const swb::test::Registrar case_6{
    "model: valid existing file skips transport",
    [] {
        const std::filesystem::path directory = make_temp_directory("model-download-skip");
        const swb::ModelManifestEntry entry = small_model();
        write_text_file(swb::model_file_path(entry, directory), "abc");
        const std::atomic<bool> cancel{false};
        bool sender_called = false;

        const swb::ModelDownloadResult result = swb::download_model(
            entry,
            directory,
            false,
            cancel,
            {},
            [&](const swb::http::Request&, const swb::http::ChunkCallback&) {
                sender_called = true;
                return 500;
            });

        expect_true(result.success);
        expect_true(!sender_called);
    },
};

const swb::test::Registrar case_7{
    "model: deletion rejects files outside manifest",
    [] {
        const std::filesystem::path directory = make_temp_directory("model-delete-scope");
        const swb::ModelManifestEntry entry = small_model();
        const std::filesystem::path path = swb::model_file_path(entry, directory);
        write_text_file(path, "abc");

        const swb::OperationStatus result = swb::remove_managed_model(entry, directory);
        expect_true(!result.success);
        expect_true(std::filesystem::exists(path));
    },
};

const swb::test::Registrar case_8{
    "model: deletion removes only the selected manifest artifacts",
    [] {
        const std::filesystem::path directory = make_temp_directory("model-delete-managed");
        const swb::ModelManifestEntry& entry = swb::default_vad_model();
        const std::filesystem::path path = swb::model_file_path(entry, directory);
        const std::filesystem::path incomplete = part_path(path);
        const std::filesystem::path unrelated = directory / "keep.bin";
        write_text_file(path, "corrupt");
        write_text_file(incomplete, "partial");
        write_text_file(unrelated, "keep");

        const swb::OperationStatus result = swb::remove_managed_model(entry, directory);
        expect_true(result.success);
        expect_true(!std::filesystem::exists(path));
        expect_true(!std::filesystem::exists(incomplete));
        expect_true(std::filesystem::exists(unrelated));
    },
};

const swb::test::Registrar case_9{
    "model: transport exception removes the part file",
    [] {
        const std::filesystem::path directory = make_temp_directory("model-download-exception");
        const swb::ModelManifestEntry entry = small_model();
        const std::atomic<bool> cancel{false};
        const swb::ModelDownloadResult result = swb::download_model(
            entry,
            directory,
            false,
            cancel,
            {},
            [](const swb::http::Request&, const swb::http::ChunkCallback&) -> int {
                throw 7;
            });

        const std::filesystem::path final_path = swb::model_file_path(entry, directory);
        expect_true(!result.success);
        expect_true(!std::filesystem::exists(final_path));
        expect_true(!std::filesystem::exists(part_path(final_path)));
    },
};

}
