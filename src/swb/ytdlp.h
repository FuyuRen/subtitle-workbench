#pragma once

#include "swb/process.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace swb {

enum class SubtitleOrigin : int {
    manual,
    automatic,
};

struct SubtitleDownload {
    std::filesystem::path srt_path;
    SubtitleOrigin origin{SubtitleOrigin::manual};
};

struct DownloadProgress {
    std::int64_t downloaded_bytes{-1};
    std::int64_t total_bytes{-1};
    double speed_bps{-1.0};
};

using DownloadProgressCallback = std::function<void(const DownloadProgress&)>;

class YtDlp {
public:
    using Runner = std::function<ProcessResult(const ProcessOptions&)>;

    explicit YtDlp(std::filesystem::path executable, Runner runner = run_process);

    [[nodiscard]] static std::filesystem::path resolve_executable();

    [[nodiscard]] std::optional<std::string> fetch_title(
        std::string_view url,
        const std::atomic<bool>& cancel) const;

    [[nodiscard]] std::optional<std::filesystem::path> download_video(
        std::string_view url,
        const std::filesystem::path& working_directory,
        const std::atomic<bool>& cancel,
        const DownloadProgressCallback& on_progress = {}) const;

    [[nodiscard]] std::optional<SubtitleDownload> download_subtitle(
        std::string_view url,
        std::string_view language,
        const std::filesystem::path& working_directory,
        const std::atomic<bool>& cancel,
        SubtitleOrigin origin) const;

    [[nodiscard]] const std::filesystem::path& executable() const noexcept { return executable_; }

private:
    std::filesystem::path executable_;
    Runner runner_;
};

}
