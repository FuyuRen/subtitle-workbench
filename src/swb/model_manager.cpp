#include "swb/model_manager.h"

#include "swb/text.h"
#include "swb/win32_headers.h"
#include "swb/workspace.h"

#include <bcrypt.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <memory>
#include <ranges>
#include <system_error>
#include <utility>
#include <vector>

namespace swb {

namespace {

constexpr std::array manifest{
    ModelManifestEntry{
        .id = "base-q5_1",
        .kind = ManagedModelKind::speech_recognition,
        .display_name = "Whisper Base Q5_1（多语言）",
        .version = "openai-whisper-base/q5_1",
        .filename = "ggml-base-q5_1.bin",
        .download_url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/3eed2a5fe2724c340a3cf93f0802610a2ab06a0d/ggml-base-q5_1.bin?download=true",
        .file_size = 59'707'625,
        .sha256 = "422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898",
        .model_type = WhisperModelType::base_multilingual,
        .alignment_heads = "base",
        .license = "MIT",
        .source = "https://huggingface.co/ggerganov/whisper.cpp",
    },
    ModelManifestEntry{
        .id = "silero-v6.2.0",
        .kind = ManagedModelKind::voice_activity_detection,
        .display_name = "Silero VAD",
        .version = "6.2.0",
        .filename = "ggml-silero-v6.2.0.bin",
        .download_url = "https://huggingface.co/ggml-org/whisper-vad/resolve/9ffd54a1e1ee413ddf265af9913beaf518d1639b/ggml-silero-v6.2.0.bin?download=true",
        .file_size = 885'098,
        .sha256 = "2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987",
        .model_type = WhisperModelType::silero_vad,
        .alignment_heads = "none",
        .license = "MIT",
        .source = "https://huggingface.co/ggml-org/whisper-vad",
    },
};

class BCryptAlgorithm {
public:
    BCryptAlgorithm() {
        if (BCryptOpenAlgorithmProvider(&handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            handle_ = nullptr;
        }
    }

    ~BCryptAlgorithm() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    BCryptAlgorithm(const BCryptAlgorithm&) = delete;
    BCryptAlgorithm& operator=(const BCryptAlgorithm&) = delete;

    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_{nullptr};
};

class Sha256 {
public:
    Sha256() {
        if (algorithm_.get() == nullptr) {
            return;
        }

        DWORD object_size = 0;
        DWORD bytes_written = 0;
        if (BCryptGetProperty(
                algorithm_.get(),
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size),
                &bytes_written,
                0) != 0) {
            return;
        }

        object_.resize(object_size);
        if (BCryptCreateHash(
                algorithm_.get(),
                &hash_,
                object_.data(),
                static_cast<ULONG>(object_.size()),
                nullptr,
                0,
                0) != 0) {
            hash_ = nullptr;
        }
    }

    ~Sha256() {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
        }
    }

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    [[nodiscard]] bool valid() const noexcept { return hash_ != nullptr; }

    [[nodiscard]] bool update(std::string_view bytes) {
        if (!valid() || bytes.size() > std::numeric_limits<ULONG>::max()) {
            return false;
        }
        return BCryptHashData(
            hash_,
            reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
            static_cast<ULONG>(bytes.size()),
            0) == 0;
    }

    [[nodiscard]] std::optional<std::string> finish() {
        if (!valid()) {
            return std::nullopt;
        }
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
            return std::nullopt;
        }
        BCryptDestroyHash(hash_);
        hash_ = nullptr;

        constexpr std::string_view hex_digits = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2);
        for (const unsigned char byte : digest) {
            result.push_back(hex_digits[(byte >> 4u) & 0x0fu]);
            result.push_back(hex_digits[byte & 0x0fu]);
        }
        return result;
    }

private:
    BCryptAlgorithm algorithm_;
    BCRYPT_HASH_HANDLE hash_{nullptr};
    std::vector<unsigned char> object_;
};

[[nodiscard]] bool is_success_status(int status) noexcept {
    return status >= 200 && status < 300;
}

void remove_part_file(const std::filesystem::path& part_path) noexcept {
    std::error_code error_code;
    std::filesystem::remove(part_path, error_code);
}

[[nodiscard]] std::filesystem::path part_file_path(const std::filesystem::path& final_path) {
    std::filesystem::path part_path = final_path;
    part_path += L".part";
    return part_path;
}

[[nodiscard]] bool atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination) {
    return MoveFileExW(
        source.c_str(),
        destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

}

std::span<const ModelManifestEntry> model_manifest() noexcept {
    return manifest;
}

const ModelManifestEntry* find_model_manifest_entry(std::string_view id) noexcept {
    const auto iterator = std::ranges::find(manifest, id, &ModelManifestEntry::id);
    return iterator == manifest.end() ? nullptr : &*iterator;
}

const ModelManifestEntry& default_local_asr_model() {
    return manifest.front();
}

const ModelManifestEntry& default_vad_model() {
    return manifest.back();
}

std::filesystem::path default_model_directory() {
    PWSTR raw_path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw_path)) && raw_path != nullptr) {
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> local_app_data{raw_path, CoTaskMemFree};
        return std::filesystem::path{local_app_data.get()} / L"SubtitleWorkbench" / L"models";
    }
    return executable_directory() / L"models";
}

std::filesystem::path resolve_model_directory(std::string_view configured_directory) {
    const std::string_view trimmed = trim_ascii_whitespace(configured_directory);
    if (trimmed.empty()) {
        return default_model_directory();
    }
    std::filesystem::path path = std::filesystem::u8path(std::string{trimmed});
    if (path.is_relative()) {
        path = executable_directory() / path;
    }
    return path.lexically_normal();
}

std::filesystem::path model_file_path(
    const ModelManifestEntry& entry,
    const std::filesystem::path& model_directory) {
    return model_directory / std::filesystem::u8path(std::string{entry.filename});
}

std::optional<std::string> sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    Sha256 hash;
    if (!hash.valid()) {
        return std::nullopt;
    }
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && !hash.update(std::string_view{buffer.data(), static_cast<std::size_t>(count)})) {
            return std::nullopt;
        }
    }
    if (!input.eof()) {
        return std::nullopt;
    }
    return hash.finish();
}

ModelStatus inspect_model(
    const ModelManifestEntry& entry,
    const std::filesystem::path& model_directory) {
    const std::filesystem::path path = model_file_path(entry, model_directory);
    std::error_code error_code;
    if (!std::filesystem::exists(path, error_code) || error_code) {
        return {
            .availability = ModelAvailability::missing,
            .path = path,
            .message = "未下载",
        };
    }

    const std::uint64_t size = std::filesystem::file_size(path, error_code);
    if (error_code || size != entry.file_size) {
        return {
            .availability = ModelAvailability::corrupt,
            .path = path,
            .size = error_code ? 0 : size,
            .message = "模型大小不符",
        };
    }
    const std::optional<std::string> hash = sha256_file(path);
    if (!hash.has_value() || *hash != entry.sha256) {
        return {
            .availability = ModelAvailability::corrupt,
            .path = path,
            .size = size,
            .message = hash.has_value() ? "模型SHA-256校验失败" : "无法校验模型",
        };
    }
    return {
        .availability = ModelAvailability::available,
        .path = path,
        .size = size,
        .message = "可用",
    };
}

ModelDownloadResult download_model(
    const ModelManifestEntry& entry,
    const std::filesystem::path& model_directory,
    bool force,
    const std::atomic<bool>& cancel,
    ModelDownloadProgressCallback on_progress,
    StreamingHttpSender sender) {
    if (cancel.load(std::memory_order_acquire)) {
        return {OperationStatus{false, "已取消"}, {}};
    }
    if (!sender) {
        return {OperationStatus{false, "模型下载器不可用"}, {}};
    }

    const ModelStatus existing = inspect_model(entry, model_directory);
    if (!force && existing.availability == ModelAvailability::available) {
        return {OperationStatus{true, "模型已存在"}, existing.path};
    }

    std::error_code error_code;
    std::filesystem::create_directories(model_directory, error_code);
    if (error_code) {
        return {OperationStatus{false, "无法创建模型目录"}, {}};
    }

    const std::filesystem::path final_path = model_file_path(entry, model_directory);
    const std::filesystem::path part_path = part_file_path(final_path);
    remove_part_file(part_path);

    std::ofstream output(part_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return {OperationStatus{false, "无法创建模型临时文件"}, {}};
    }

    Sha256 hash;
    if (!hash.valid()) {
        output.close();
        remove_part_file(part_path);
        return {OperationStatus{false, "无法初始化SHA-256校验"}, {}};
    }

    std::uint64_t downloaded_bytes = 0;
    bool stream_failed = false;
    http::Request request;
    request.url = std::string{entry.download_url};
    request.method = "GET";
    request.timeout_ms = 600'000;
    request.cancel = &cancel;

    int status = 0;
    try {
        status = sender(request, [&](std::string_view bytes) {
            if (cancel.load(std::memory_order_acquire)
                || downloaded_bytes > entry.file_size
                || bytes.size() > entry.file_size - downloaded_bytes) {
                stream_failed = !cancel.load(std::memory_order_acquire);
                return false;
            }
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!output || !hash.update(bytes)) {
                stream_failed = true;
                return false;
            }
            downloaded_bytes += bytes.size();
            if (on_progress) {
                on_progress({downloaded_bytes, entry.file_size});
            }
            return true;
        });
    } catch (const std::exception& exception) {
        output.close();
        remove_part_file(part_path);
        return {OperationStatus{false, std::string{"模型下载失败："} + exception.what()}, {}};
    } catch (...) {
        output.close();
        remove_part_file(part_path);
        return {OperationStatus{false, "模型下载发生未知错误"}, {}};
    }
    output.close();

    if (cancel.load(std::memory_order_acquire)) {
        remove_part_file(part_path);
        return {OperationStatus{false, "已取消"}, {}};
    }
    if (!is_success_status(status)) {
        remove_part_file(part_path);
        return {OperationStatus{false, "模型下载返回HTTP " + std::to_string(status)}, {}};
    }
    if (stream_failed || !output || downloaded_bytes != entry.file_size) {
        remove_part_file(part_path);
        return {OperationStatus{false, "模型下载不完整"}, {}};
    }

    const std::optional<std::string> downloaded_hash = hash.finish();
    if (!downloaded_hash.has_value() || *downloaded_hash != entry.sha256) {
        remove_part_file(part_path);
        return {OperationStatus{false, "模型SHA-256校验失败"}, {}};
    }
    if (!atomic_replace(part_path, final_path)) {
        remove_part_file(part_path);
        return {OperationStatus{false, "无法提交模型文件"}, {}};
    }
    return {OperationStatus{true, "下载完成"}, final_path};
}

OperationStatus remove_managed_model(
    const ModelManifestEntry& entry,
    const std::filesystem::path& model_directory) {
    const ModelManifestEntry* managed_entry = find_model_manifest_entry(entry.id);
    if (managed_entry == nullptr || managed_entry->filename != entry.filename) {
        return {false, "拒绝删除模型清单之外的文件"};
    }
    const std::filesystem::path path = model_file_path(*managed_entry, model_directory);
    const std::filesystem::path part_path = part_file_path(path);
    std::error_code error_code;
    std::filesystem::remove(path, error_code);
    if (error_code) {
        return {false, "无法删除模型文件"};
    }
    std::filesystem::remove(part_path, error_code);
    if (error_code) {
        return {false, "无法删除模型临时文件"};
    }
    return {true, "模型已删除"};
}

}
