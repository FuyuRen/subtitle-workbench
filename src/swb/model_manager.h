#pragma once

#include "swb/http.h"
#include "swb/operation_status.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace swb {

enum class ManagedModelKind : int {
    speech_recognition,
    voice_activity_detection,
};

enum class WhisperModelType : int {
    base_multilingual,
    silero_vad,
};

struct ModelManifestEntry {
    std::string_view id;
    ManagedModelKind kind{ManagedModelKind::speech_recognition};
    std::string_view display_name;
    std::string_view version;
    std::string_view filename;
    std::string_view download_url;
    std::uint64_t file_size{0};
    std::string_view sha256;
    WhisperModelType model_type{WhisperModelType::base_multilingual};
    std::string_view alignment_heads;
    std::string_view license;
    std::string_view source;
};

enum class ModelAvailability : int {
    missing,
    available,
    corrupt,
};

struct ModelStatus {
    ModelAvailability availability{ModelAvailability::missing};
    std::filesystem::path path;
    std::uint64_t size{0};
    std::string message;
};

struct ModelDownloadProgress {
    std::uint64_t downloaded_bytes{0};
    std::uint64_t total_bytes{0};
};

struct ModelDownloadResult : OperationStatus {
    std::filesystem::path path;
};

using ModelDownloadProgressCallback = std::function<void(const ModelDownloadProgress&)>;
using StreamingHttpSender = std::function<int(const http::Request&, const http::ChunkCallback&)>;

[[nodiscard]] std::span<const ModelManifestEntry> model_manifest() noexcept;

[[nodiscard]] const ModelManifestEntry* find_model_manifest_entry(std::string_view id) noexcept;

[[nodiscard]] const ModelManifestEntry& default_local_asr_model();

[[nodiscard]] const ModelManifestEntry& default_vad_model();

[[nodiscard]] std::filesystem::path default_model_directory();

[[nodiscard]] std::filesystem::path resolve_model_directory(std::string_view configured_directory);

[[nodiscard]] std::filesystem::path model_file_path(
    const ModelManifestEntry& entry,
    const std::filesystem::path& model_directory);

[[nodiscard]] std::optional<std::string> sha256_file(const std::filesystem::path& path);

[[nodiscard]] ModelStatus inspect_model(
    const ModelManifestEntry& entry,
    const std::filesystem::path& model_directory);

[[nodiscard]] ModelDownloadResult download_model(
    const ModelManifestEntry& entry,
    const std::filesystem::path& model_directory,
    bool force,
    const std::atomic<bool>& cancel,
    ModelDownloadProgressCallback on_progress = {},
    StreamingHttpSender sender = http::send_streaming);

[[nodiscard]] OperationStatus remove_managed_model(
    const ModelManifestEntry& entry,
    const std::filesystem::path& model_directory);

}
