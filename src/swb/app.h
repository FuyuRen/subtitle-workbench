#pragma once

#include "swb/win32_headers.h"
#include "swb/windows_task_feedback.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>
#include <string>

#include "swb/config.h"
#include "swb/task.h"

class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run();

private:
    void build_ui();
    void create_render_target();
    void sync_task_feedback(bool is_running, std::span<const swb::StepInfo, swb::step_count> steps);

    [[nodiscard]] std::string current_task_subject() const;

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    swb::ComApartment com_apartment_;
    HWND window_handle_ = nullptr;
    WNDCLASSEXW window_class_ = {};

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> device_context_;
    ComPtr<IDXGISwapChain> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_view_;

    bool is_occluded_ = false;
    float dpi_scale_ = 1.0f;
    bool is_imgui_ready_ = false;
    bool should_close_ = false;
    bool was_task_running_ = false;

    std::filesystem::path configuration_path_;
    swb::Config configuration_;
    std::string source_text_;
    std::string output_name_;
    std::string last_synchronized_title_;
    bool is_output_name_pending_autofill_{false};
    bool reuse_working_directory_{false};
    swb::Task current_task_;
    swb::TaskbarProgressController taskbar_progress_;
    swb::TaskCompletionNotifier completion_notifier_;
};
