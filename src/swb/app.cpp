#include "swb/app.h"

#include "swb/text.h"
#include "swb/workspace.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <ShlObj.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <numbers>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr int dialog_width = 480;
constexpr int dialog_height = 860;

std::string browse_path(
    HWND owner_window,
    DWORD additional_options,
    std::wstring_view title,
    std::span<const COMDLG_FILTERSPEC> filters = {}) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    const HRESULT result = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.ReleaseAndGetAddressOf()));
    if (FAILED(result)) {
        return {};
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | additional_options | FOS_NOCHANGEDIR);
    dialog->SetTitle(title.data());
    if (!filters.empty()) {
        dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
    }

    std::string selected_path;
    if (SUCCEEDED(dialog->Show(owner_window))) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (SUCCEEDED(dialog->GetResult(item.ReleaseAndGetAddressOf())) && item) {
            PWSTR raw_path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) && raw_path != nullptr) {
                std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw_path, CoTaskMemFree);
                selected_path = swb::wide_to_utf8(path.get());
            }
        }
    }
    return selected_path;
}

std::string browse_directory(HWND owner_window, std::wstring_view title) {
    return browse_path(owner_window, FOS_PICKFOLDERS, title);
}

std::string browse_folder(HWND owner_window) {
    return browse_directory(owner_window, L"选择输出目录");
}

std::string browse_working_directory(HWND owner_window) {
    return browse_directory(owner_window, L"选择工作目录");
}

std::string browse_source_file(HWND owner_window) {
    constexpr std::array<COMDLG_FILTERSPEC, 2> filters{{
        {L"视频文件", L"*.mp4;*.mkv;*.mov;*.avi;*.webm"},
        {L"所有文件", L"*.*"},
    }};
    return browse_path(owner_window, FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST, L"选择本地视频", filters);
}

bool file_exists(std::string_view path) {
    return GetFileAttributesA(path.data()) != INVALID_FILE_ATTRIBUTES;
}

std::string_view find_existing_path(std::span<const std::string_view> candidates) {
    for (const std::string_view candidate : candidates) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

void add_font(ImGuiIO& io, std::string_view path, float size, const ImWchar* glyph_ranges, bool merge_mode) {
    ImFontConfig font_configuration{};
    font_configuration.MergeMode = merge_mode;
    font_configuration.OversampleH = 2;
    font_configuration.OversampleV = 1;
    font_configuration.PixelSnapH = true;
    io.Fonts->AddFontFromFileTTF(path.data(), size, &font_configuration, glyph_ranges);
}

void configure_fonts(ImGuiIO& io, float font_size) {
    io.Fonts->Clear();

    constexpr std::array chinese_fonts{
        std::string_view{"C:\\Windows\\Fonts\\msyh.ttc"},
    };
    constexpr std::array japanese_fonts{
        std::string_view{"C:\\Windows\\Fonts\\YuGothM.ttc"},
        std::string_view{"C:\\Windows\\Fonts\\meiryo.ttc"},
        std::string_view{"C:\\Windows\\Fonts\\msgothic.ttc"},
    };
    constexpr std::array korean_fonts{
        std::string_view{"C:\\Windows\\Fonts\\malgun.ttf"},
    };

    const std::string_view chinese_font = find_existing_path(chinese_fonts);
    if (!chinese_font.empty()) {
        add_font(io, chinese_font, font_size, io.Fonts->GetGlyphRangesChineseSimplifiedCommon(), false);
    } else {
        io.Fonts->AddFontDefault();
    }

    if (const std::string_view japanese_font = find_existing_path(japanese_fonts); !japanese_font.empty()) {
        add_font(io, japanese_font, font_size, io.Fonts->GetGlyphRangesJapanese(), true);
    }
    if (const std::string_view korean_font = find_existing_path(korean_fonts); !korean_font.empty()) {
        add_font(io, korean_font, font_size, io.Fonts->GetGlyphRangesKorean(), true);
    }
}

int string_resize_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

bool input_text(const char* label, std::string& text, ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    if (text.capacity() < 64) {
        text.reserve(64);
    }
    return ImGui::InputText(
        label,
        text.data(),
        text.capacity() + 1,
        flags,
        string_resize_callback,
        &text);
}

bool button(const char* label) {
    return ImGui::Button(label, {ImGui::GetContentRegionAvail().x, 0.0f});
}

void help_marker(std::string_view text) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextDisabled("(?)");
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        return;
    }

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

[[nodiscard]] std::string format_mebibytes(std::uint64_t bytes) {
    std::ostringstream output;
    output.setf(std::ios::fixed);
    output.precision(1);
    output << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MiB";
    return output.str();
}

[[nodiscard]] std::string_view availability_label(swb::ModelAvailability availability) noexcept {
    switch (availability) {
    case swb::ModelAvailability::missing:
        return "未下载";
    case swb::ModelAvailability::available:
        return "可用";
    case swb::ModelAvailability::corrupt:
        return "已损坏";
    }
    return "未知";
}

[[nodiscard]] std::string runtime_backend_label(const swb::TranscriptionRuntime& runtime) {
    switch (runtime.backend) {
    case swb::ActualTranscriptionBackend::api:
        return "API";
    case swb::ActualTranscriptionBackend::gpu:
        return runtime.backend_name.empty() ? "GPU" : "GPU：" + runtime.backend_name;
    case swb::ActualTranscriptionBackend::cpu:
        return "CPU";
    case swb::ActualTranscriptionBackend::not_initialized:
        break;
    }
    return "尚未初始化";
}

constexpr std::array hard_subtitle_font_presets{
    std::string_view{"Microsoft YaHei"},
    std::string_view{"SimHei"},
    std::string_view{"SimSun"},
    std::string_view{"Segoe UI"},
    std::string_view{"Arial"},
    std::string_view{"Noto Sans CJK SC"},
};

[[nodiscard]] std::array<float, 4> color_edit_values(const swb::SubtitleColor& color) {
    return {
        static_cast<float>(color.red) / 255.0f,
        static_cast<float>(color.green) / 255.0f,
        static_cast<float>(color.blue) / 255.0f,
        static_cast<float>(color.alpha) / 255.0f,
    };
}

void apply_color_edit_values(const std::array<float, 4>& color_components, swb::SubtitleColor& color) {
    const auto to_byte = [](float value) {
        return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0f), 0l, 255l));
    };
    color.red = to_byte(color_components[0]);
    color.green = to_byte(color_components[1]);
    color.blue = to_byte(color_components[2]);
    color.alpha = to_byte(color_components[3]);
}

[[nodiscard]] bool edit_subtitle_color(const char* label, swb::SubtitleColor& color) {
    std::array<float, 4> color_components = color_edit_values(color);
    if (!ImGui::ColorEdit4(
            label,
            color_components.data(),
            ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
        return false;
    }
    apply_color_edit_values(color_components, color);
    return true;
}

[[nodiscard]] ImU32 to_im_color(const swb::SubtitleColor& color) {
    return IM_COL32(color.red, color.green, color.blue, color.alpha);
}

[[nodiscard]] std::string notification_title(swb::TaskState state) {
    switch (state) {
    case swb::TaskState::succeeded:
        return "任务完成";
    case swb::TaskState::failed:
        return "任务失败";
    case swb::TaskState::canceled:
        return "任务已取消";
    case swb::TaskState::idle:
    case swb::TaskState::running:
        break;
    }
    return "Subtitle Workbench";
}

[[nodiscard]] std::string notification_message(std::string_view subject, const swb::TaskTerminalSummary& summary) {
    const std::string task_subject = subject.empty() ? std::string{"任务"} : std::string{subject};
    switch (summary.state) {
    case swb::TaskState::succeeded:
        if (!summary.status.empty()) {
            return task_subject + "：" + summary.status;
        }
        return task_subject + "已完成";
    case swb::TaskState::failed:
        if (summary.step.has_value() && !summary.status.empty()) {
            return task_subject + "：" + std::string{swb::step_label(*summary.step)} + "失败，" + summary.status;
        }
        if (!summary.status.empty()) {
            return task_subject + "：" + summary.status;
        }
        return task_subject + "失败";
    case swb::TaskState::canceled:
        return task_subject + "已取消";
    case swb::TaskState::idle:
    case swb::TaskState::running:
        break;
    }
    return task_subject;
}

void draw_preview_text(
    ImDrawList* draw_list,
    ImFont* font,
    float font_size,
    ImVec2 position,
    ImU32 fill_color,
    ImU32 outline_color,
    float outline_thickness,
    std::string_view text) {
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    if (outline_thickness > 0.0f) {
        const int radius_step_count = std::max(1, static_cast<int>(std::ceil(outline_thickness)));
        for (int radius_step = 1; radius_step <= radius_step_count; ++radius_step) {
            const float radius = std::min(outline_thickness, static_cast<float>(radius_step));
            const int sample_count = std::max(12, static_cast<int>(std::ceil(std::numbers::pi_v<float> * radius * 8.0f)));
            for (int sample_index = 0; sample_index < sample_count; ++sample_index) {
                const float angle = (static_cast<float>(sample_index) / static_cast<float>(sample_count)) * (std::numbers::pi_v<float> * 2.0f);
                draw_list->AddText(
                    font,
                    font_size,
                    {position.x + std::cos(angle) * radius, position.y + std::sin(angle) * radius},
                    outline_color,
                    begin,
                    end);
            }
        }
    }
    draw_list->AddText(font, font_size, position, fill_color, begin, end);
}

void draw_hard_subtitle_preview(const swb::Config& configuration) {
    const swb::HardSubtitleStyle& style = configuration.hard_subtitle_style;
    const float chinese_font_size = static_cast<float>(std::max(style.chinese_font_size, 1));
    const float english_font_size = static_cast<float>(std::max(style.english_font_size, 1));
    const float bottom_margin = static_cast<float>(std::max(style.bottom_margin, 0));
    const float line_spacing = configuration.bilingual_subtitles ? std::max(style.bilingual_line_gap, 0.0f) : 0.0f;
    const float preview_height = std::clamp(
        40.0f + bottom_margin + chinese_font_size + (configuration.bilingual_subtitles ? english_font_size + line_spacing : 0.0f),
        116.0f,
        260.0f);
    if (!ImGui::BeginChild("##hard_subtitle_preview", {0.0f, preview_height}, true)) {
        ImGui::EndChild();
        return;
    }

    const ImVec2 preview_origin = ImGui::GetCursorScreenPos();
    const ImVec2 preview_size = ImGui::GetContentRegionAvail();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 preview_max{preview_origin.x + preview_size.x, preview_origin.y + preview_size.y};
    draw_list->AddRectFilled(preview_origin, preview_max, to_im_color(style.preview_background_color), 10.0f);
    draw_list->AddRect(preview_origin, preview_max, ImGui::GetColorU32(ImGuiCol_Border), 10.0f, 0, 1.0f);

    ImFont* font = ImGui::GetFont();
    constexpr std::string_view translated_sample{"中文字幕预览"};
    constexpr std::string_view source_sample{"English subtitle preview"};

    const ImVec2 translated_size = font->CalcTextSizeA(chinese_font_size, 10000.0f, 0.0f, translated_sample.data(), translated_sample.data() + translated_sample.size());
    const ImVec2 source_size = configuration.bilingual_subtitles
        ? font->CalcTextSizeA(english_font_size, 10000.0f, 0.0f, source_sample.data(), source_sample.data() + source_sample.size())
        : ImVec2{0.0f, 0.0f};
    const float total_height = translated_size.y + source_size.y + line_spacing;
    const float start_y = std::max(preview_origin.y + 12.0f, preview_max.y - bottom_margin - total_height);

    const ImU32 fill_color = to_im_color(style.fill_color);
    const ImU32 outline_color = to_im_color(style.outline_color);
    const float outline_thickness = std::max(style.outline_thickness, 0.0f);

    draw_preview_text(
        draw_list,
        font,
        chinese_font_size,
        {preview_origin.x + (preview_size.x - translated_size.x) * 0.5f, start_y},
        fill_color,
        outline_color,
        outline_thickness,
        translated_sample);

    if (configuration.bilingual_subtitles) {
        draw_preview_text(
            draw_list,
            font,
            english_font_size,
            {preview_origin.x + (preview_size.x - source_size.x) * 0.5f, start_y + translated_size.y + line_spacing},
            fill_color,
            outline_color,
            outline_thickness,
            source_sample);
    }

    ImGui::Dummy(preview_size);
    ImGui::EndChild();
}

using swb::StepState;

struct SweepAnimationState {
    float sweep = 1.0f;
    float strength = 0.0f;
    bool active = false;
};

struct PulseAnimationState {
    float strength = 0.0f;
};

SweepAnimationState current_sweep_animation() {
    constexpr float sweep_duration = 0.95f;
    constexpr float pause_duration = 1.45f;
    constexpr float cycle_duration = sweep_duration + pause_duration;

    const float cycle_time = std::fmod(static_cast<float>(ImGui::GetTime()), cycle_duration);
    if (cycle_time >= sweep_duration) {
        return {};
    }

    const float sweep = cycle_time / sweep_duration;
    const float strength = std::sin(sweep * std::numbers::pi_v<float>);
    return {
        .sweep = sweep,
        .strength = strength,
        .active = true,
    };
}

PulseAnimationState current_pulse_animation() {
    constexpr float pulse_duration = 1.2f;
    constexpr float phase_offset = std::numbers::pi_v<float> * -0.5f;

    const float phase = (static_cast<float>(ImGui::GetTime()) / pulse_duration) * (std::numbers::pi_v<float> * 2.0f);
    return {
        .strength = 0.5f + 0.5f * std::sin(phase + phase_offset),
    };
}

void draw_gradient_text_band(
    ImDrawList* draw_list,
    ImVec2 position,
    ImVec2 text_size,
    std::string_view text,
    float center,
    float half_width,
    ImVec4 color) {
    constexpr int slice_count = 14;
    const char* text_begin = text.data();
    const char* text_end = text.data() + text.size();
    const float band_min_x = center - half_width;
    const float band_width = half_width * 2.0f;

    for (int slice_index = 0; slice_index < slice_count; ++slice_index) {
        const float start_t = static_cast<float>(slice_index) / static_cast<float>(slice_count);
        const float end_t = static_cast<float>(slice_index + 1) / static_cast<float>(slice_count);
        const float center_t = (start_t + end_t) * 0.5f;
        const float falloff = std::max(0.0f, 1.0f - std::abs(center_t * 2.0f - 1.0f));
        const float slice_strength = std::pow(falloff, 1.4f);
        if (slice_strength <= 0.01f) {
            continue;
        }

        ImVec4 slice_color = color;
        slice_color.w *= slice_strength;
        const ImVec2 clip_min{band_min_x + band_width * start_t, position.y - 1.0f};
        const ImVec2 clip_max{band_min_x + band_width * end_t, position.y + text_size.y + 1.0f};
        draw_list->PushClipRect(clip_min, clip_max, true);
        draw_list->AddText(position, ImGui::GetColorU32(slice_color), text_begin, text_end);
        draw_list->PopClipRect();
    }
}

void draw_status_circle(StepState state) {
    const float row_height = ImGui::GetFrameHeight();
    const float radius = ImGui::GetFontSize() * 0.18f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float center_y = cursor.y + row_height * 0.5f;
    const float center_x = cursor.x + row_height * 0.5f;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    switch (state) {
    case StepState::waiting:
        draw_list->AddCircleFilled({center_x, center_y}, radius, IM_COL32(100, 100, 100, 255));
        break;
    case StepState::in_progress: {
        const PulseAnimationState animation = current_pulse_animation();
        const float glow_radius = radius * (1.85f + 0.30f * animation.strength);
        const float core_radius = radius * (0.95f + 0.18f * animation.strength);
        draw_list->AddCircleFilled(
            {center_x, center_y},
            glow_radius,
            ImGui::GetColorU32({0.23f, 0.52f, 0.92f, 0.08f + 0.14f * animation.strength}),
            24);
        draw_list->AddCircleFilled(
            {center_x, center_y},
            core_radius,
            ImGui::GetColorU32({0.30f, 0.60f, 0.98f, 0.70f + 0.22f * animation.strength}),
            24);
        break;
    }
    case StepState::completed:
        draw_list->AddCircleFilled({center_x, center_y}, radius, IM_COL32(80, 200, 80, 255));
        break;
    case StepState::failed:
        draw_list->AddCircleFilled({center_x, center_y}, radius, IM_COL32(220, 80, 80, 255));
        break;
    }

    ImGui::Dummy({row_height, row_height});
}

void draw_in_progress_text(std::string_view text) {
    const float row_height = ImGui::GetFrameHeight();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const char* text_begin = text.data();
    const char* text_end = text.data() + text.size();
    const ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end);
    const ImVec2 position{
        cursor.x,
        cursor.y + std::max(0.0f, (row_height - text_size.y) * 0.5f),
    };
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const SweepAnimationState animation = current_sweep_animation();

    draw_list->AddText(position, ImGui::GetColorU32({0.66f, 0.82f, 1.0f, 0.92f}), text_begin, text_end);

    if (animation.active) {
        const float band_width = std::max(text_size.x * 0.38f, text_size.y * 2.8f);
        const float center = position.x - band_width * 0.5f + (text_size.x + band_width) * animation.sweep;
        draw_gradient_text_band(
            draw_list,
            position,
            text_size,
            text,
            center,
            band_width * 0.92f,
            {0.32f, 0.54f, 0.82f, 0.18f + 0.18f * animation.strength});
        draw_gradient_text_band(
            draw_list,
            position,
            text_size,
            text,
            center,
            band_width * 0.48f,
            {0.20f, 0.40f, 0.66f, 0.58f + 0.18f * animation.strength});
    }

    ImGui::Dummy({text_size.x, row_height});
}

struct LanguageEntry {
    const char* code;
    const char* label;
};

constexpr std::array languages{
    LanguageEntry{"en", "English"},
    LanguageEntry{"zh", "简体中文"},
    LanguageEntry{"ja", "日本語"},
    LanguageEntry{"ko", "한국어"},
};

int find_lang_index(std::string_view code) {
    for (int index = 0; index < static_cast<int>(std::size(languages)); ++index) {
        if (code == languages[index].code) {
            return index;
        }
    }
    return 0;
}

bool lang_combo(const char* label, std::string& code) {
    int selected_index = find_lang_index(code);
    const char* preview = languages[selected_index].label;
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (int index = 0; index < static_cast<int>(std::size(languages)); ++index) {
            const bool selected = (index == selected_index);
            if (ImGui::Selectable(languages[index].label, selected)) {
                code = languages[index].code;
                changed = true;
                selected_index = index;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

}

App::App() {
    ImGui_ImplWin32_EnableDpiAwareness();
    dpi_scale_ = ImGui_ImplWin32_GetDpiScaleForMonitor(
        MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    window_class_.cbSize = sizeof(WNDCLASSEXW);
    window_class_.style = CS_CLASSDC;
    window_class_.lpfnWndProc = wnd_proc;
    window_class_.hInstance = GetModuleHandleW(nullptr);
    window_class_.lpszClassName = L"SubtitleWorkbench";

    if (!RegisterClassExW(&window_class_)) {
        throw std::runtime_error("failed to register window class");
    }

    constexpr DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    window_handle_ = CreateWindowExW(
        0,
        window_class_.lpszClassName,
        L"Subtitle Workbench",
        window_style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        static_cast<int>(dialog_width * dpi_scale_),
        static_cast<int>(dialog_height * dpi_scale_),
        nullptr,
        nullptr,
        window_class_.hInstance,
        nullptr);

    if (!window_handle_) {
        UnregisterClassW(window_class_.lpszClassName, window_class_.hInstance);
        throw std::runtime_error("failed to create window");
    }

    DXGI_SWAP_CHAIN_DESC swap_chain_description{};
    swap_chain_description.BufferCount = 2;
    swap_chain_description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_description.BufferDesc.RefreshRate = {60, 1};
    swap_chain_description.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    swap_chain_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_description.OutputWindow = window_handle_;
    swap_chain_description.SampleDesc.Count = 1;
    swap_chain_description.Windowed = TRUE;
    swap_chain_description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr std::array feature_levels = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    HRESULT device_creation_result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        feature_levels.data(),
        static_cast<UINT>(feature_levels.size()),
        D3D11_SDK_VERSION,
        &swap_chain_description,
        swap_chain_.GetAddressOf(),
        device_.GetAddressOf(),
        nullptr,
        device_context_.GetAddressOf());

    if (device_creation_result == DXGI_ERROR_UNSUPPORTED) {
        device_creation_result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            feature_levels.data(),
            static_cast<UINT>(feature_levels.size()),
            D3D11_SDK_VERSION,
            &swap_chain_description,
            swap_chain_.GetAddressOf(),
            device_.GetAddressOf(),
            nullptr,
            device_context_.GetAddressOf());
    }

    if (FAILED(device_creation_result)) {
        DestroyWindow(window_handle_);
        UnregisterClassW(window_class_.lpszClassName, window_class_.hInstance);
        throw std::runtime_error("failed to create D3D11 device");
    }

    create_render_target();

    BOOL use_dark = TRUE;
    DwmSetWindowAttribute(window_handle_, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));

    ShowWindow(window_handle_, SW_SHOWDEFAULT);
    UpdateWindow(window_handle_);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FramePadding = {8.0f, 5.0f};
    style.ItemSpacing = {8.0f, 8.0f};
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.PopupRounding = 5.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowPadding = {14.0f, 12.0f};
    style.HoverStationaryDelay = 0.0f;

    auto& colors = style.Colors;
    colors[ImGuiCol_Border] = {0.20f, 0.28f, 0.40f, 0.85f};
    colors[ImGuiCol_ButtonHovered] = {0.35f, 0.45f, 0.65f, 1.0f};
    colors[ImGuiCol_ButtonActive] = {0.25f, 0.35f, 0.55f, 1.0f};
    colors[ImGuiCol_Header] = {0.22f, 0.30f, 0.43f, 0.45f};
    colors[ImGuiCol_FrameBgHovered] = {0.28f, 0.32f, 0.40f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = {0.22f, 0.26f, 0.34f, 1.0f};
    colors[ImGuiCol_HeaderHovered] = {0.30f, 0.38f, 0.55f, 0.80f};
    colors[ImGuiCol_HeaderActive] = {0.34f, 0.47f, 0.70f, 1.0f};
    colors[ImGuiCol_CheckMark] = {0.45f, 0.70f, 1.00f, 1.0f};
    colors[ImGuiCol_SliderGrab] = {0.40f, 0.55f, 0.80f, 1.0f};

    style.ScaleAllSizes(dpi_scale_);
    style.FontScaleDpi = dpi_scale_;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    configure_fonts(io, 18.0f);

    ImGui_ImplWin32_Init(window_handle_);
    ImGui_ImplDX11_Init(device_.Get(), device_context_.Get());
    is_imgui_ready_ = true;

    taskbar_progress_.initialize(window_handle_);
    completion_notifier_.initialize(window_handle_);

    configuration_path_ = swb::default_config_path();
    configuration_ = swb::load_config(configuration_path_);
    local_asr_gpu_devices_ = swb::enumerate_local_asr_gpu_devices();
    refresh_model_statuses();
}

App::~App() {
    cancel_model_download();
    join_model_worker();
    taskbar_progress_.clear();
    completion_notifier_.shutdown();

    if (is_imgui_ready_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (window_handle_) {
        DestroyWindow(window_handle_);
    }
    if (window_class_.lpszClassName) {
        UnregisterClassW(window_class_.lpszClassName, window_class_.hInstance);
    }
}

void App::refresh_model_statuses() {
    const std::filesystem::path directory = swb::resolve_model_directory(configuration_.local_asr_model_dir);
    const swb::ModelStatus asr_status = swb::inspect_model(swb::default_local_asr_model(), directory);
    const swb::ModelStatus vad_status = swb::inspect_model(swb::default_vad_model(), directory);
    std::scoped_lock lock(model_mutex_);
    asr_model_status_ = asr_status;
    vad_model_status_ = vad_status;
    model_status_dirty_ = false;
}

void App::start_model_download(const swb::ModelManifestEntry& entry, bool force) {
    {
        std::scoped_lock lock(model_mutex_);
        if (model_operation_running_) {
            return;
        }
    }
    join_model_worker();

    const swb::ModelManifestEntry manifest_entry = entry;
    const std::filesystem::path directory = swb::resolve_model_directory(configuration_.local_asr_model_dir);
    cancel_model_download_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(model_mutex_);
        model_operation_running_ = true;
        model_download_fraction_ = 0.0;
        active_model_id_ = std::string{manifest_entry.id};
        model_operation_message_ = "正在下载";
    }

    model_worker_ = std::thread([this, manifest_entry, directory, force] {
        const swb::ModelDownloadResult result = swb::download_model(
            manifest_entry,
            directory,
            force,
            cancel_model_download_,
            [this](const swb::ModelDownloadProgress& progress) {
                const double fraction = progress.total_bytes == 0
                    ? 0.0
                    : static_cast<double>(progress.downloaded_bytes) / static_cast<double>(progress.total_bytes);
                std::scoped_lock lock(model_mutex_);
                model_download_fraction_ = std::clamp(fraction, 0.0, 1.0);
            });
        const swb::ModelStatus status = swb::inspect_model(manifest_entry, directory);
        std::scoped_lock lock(model_mutex_);
        if (manifest_entry.kind == swb::ManagedModelKind::speech_recognition) {
            asr_model_status_ = status;
        } else {
            vad_model_status_ = status;
        }
        model_operation_message_ = result.message;
        model_download_fraction_ = result.success ? 1.0 : model_download_fraction_;
        model_operation_running_ = false;
        model_status_dirty_ = false;
    });
}

void App::cancel_model_download() noexcept {
    cancel_model_download_.store(true, std::memory_order_release);
}

void App::join_model_worker() {
    if (model_worker_.joinable()) {
        model_worker_.join();
    }
}

std::string App::current_task_subject() const {
    if (!output_name_.empty()) {
        return output_name_;
    }
    const std::string detected_title = current_task_.detected_title();
    if (!detected_title.empty()) {
        return detected_title;
    }
    return {};
}

void App::sync_task_feedback(bool is_running, std::span<const swb::StepInfo, swb::step_count> steps) {
    if (is_running) {
        taskbar_progress_.set_progress(swb::summarize_task_progress(steps));
    } else {
        taskbar_progress_.clear();
    }

    if (was_task_running_ && !is_running) {
        const swb::TaskTerminalSummary summary = swb::summarize_task_state(steps);
        if (summary.state == swb::TaskState::succeeded
            || summary.state == swb::TaskState::failed
            || summary.state == swb::TaskState::canceled) {
            const std::string subject = current_task_subject();
            const std::wstring title = swb::utf8_to_wide(notification_title(summary.state));
            const std::wstring message = swb::utf8_to_wide(notification_message(subject, summary));
            if (summary.state == swb::TaskState::failed) {
                completion_notifier_.show_error(title, message);
            } else {
                completion_notifier_.show_info(title, message);
            }
        }
    }

    was_task_running_ = is_running;
}

int App::run() {
    while (!should_close_) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                should_close_ = true;
            }
        }
        if (should_close_) {
            break;
        }

        if (is_occluded_ && swap_chain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }
        is_occluded_ = false;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        build_ui();

        ImGui::Render();
        constexpr float clear_color[] = {0.1f, 0.1f, 0.1f, 1.0f};
        device_context_->OMSetRenderTargets(1, render_target_view_.GetAddressOf(), nullptr);
        device_context_->ClearRenderTargetView(render_target_view_.Get(), clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT present_result = swap_chain_->Present(1, 0);
        is_occluded_ = (present_result == DXGI_STATUS_OCCLUDED);
    }
    return 0;
}

void App::build_ui() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##main", nullptr, flags);

    const bool is_running = current_task_.running();
    const swb::Task::Snapshot steps = current_task_.read();
    sync_task_feedback(is_running, steps);

    bool is_dirty = false;
    const ImGuiStyle& style = ImGui::GetStyle();

    {
        const float continue_toggle_width = ImGui::GetFrameHeight() + style.ItemSpacing.x * 0.75f;
        const float browse_button_width = ImGui::CalcTextSize("浏览").x + style.FramePadding.x * 2;
        const float start_button_width = ImGui::CalcTextSize("开始").x + style.FramePadding.x * 2;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {style.ItemSpacing.x * 0.5f, 0.0f});
        if (ImGui::BeginTable("toolbar", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
            ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("browse", ImGuiTableColumnFlags_WidthFixed, browse_button_width + continue_toggle_width);
            ImGui::TableSetupColumn("start", ImGuiTableColumnFlags_WidthFixed, start_button_width);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (source_text_.capacity() < 64) {
                source_text_.reserve(64);
            }
            ImGui::InputTextWithHint(
                "##source",
                reuse_working_directory_ ? "选择工作目录" : "输入视频链接或本地视频路径",
                source_text_.data(),
                source_text_.capacity() + 1,
                ImGuiInputTextFlags_CallbackResize,
                string_resize_callback,
                &source_text_);

            ImGui::TableSetColumnIndex(1);
            if (is_running) {
                ImGui::BeginDisabled();
            }
            ImGui::Checkbox("##reuse_workdir", &reuse_working_directory_);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("继续模式");
            }
            if (is_running) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine(0.0f, style.ItemSpacing.x * 0.75f);
            if (button("浏览")) {
                const std::string selected_source = reuse_working_directory_
                    ? browse_working_directory(window_handle_)
                    : browse_source_file(window_handle_);
                if (!selected_source.empty()) {
                    source_text_ = std::move(selected_source);
                }
            }

            ImGui::TableSetColumnIndex(2);
            if (button(is_running ? "取消" : "开始")) {
                if (is_running) {
                    current_task_.cancel();
                } else if (!source_text_.empty()) {
                    if (configuration_.transcription_backend == swb::TranscriptionBackend::local) {
                        bool status_dirty = false;
                        {
                            std::scoped_lock lock(model_mutex_);
                            status_dirty = model_status_dirty_;
                        }
                        if (status_dirty) {
                            refresh_model_statuses();
                        }
                    }

                    swb::ModelStatus asr_status;
                    bool model_operation_running = false;
                    {
                        std::scoped_lock lock(model_mutex_);
                        asr_status = asr_model_status_;
                        model_operation_running = model_operation_running_;
                    }
                    if (configuration_.transcription_backend == swb::TranscriptionBackend::local
                        && asr_status.availability != swb::ModelAvailability::available) {
                        if (!model_operation_running) {
                            start_model_download(
                                swb::default_local_asr_model(),
                                asr_status.availability == swb::ModelAvailability::corrupt);
                        }
                    } else {
                        is_output_name_pending_autofill_ = !reuse_working_directory_ && output_name_.empty();
                        last_synchronized_title_.clear();
                        current_task_.start(source_text_, configuration_, {
                            .output_name = output_name_,
                            .reuse_working_directory = reuse_working_directory_,
                        });
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        ImGui::Spacing();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (output_name_.capacity() < 64) {
            output_name_.reserve(64);
        }
        if (is_running || reuse_working_directory_) {
            ImGui::BeginDisabled();
        }
        const bool output_name_changed = ImGui::InputTextWithHint(
            "##output_name",
            reuse_working_directory_ ? "继续工作时直接使用当前目录" : "文件名（可选，下载完成后自动填充）",
            output_name_.data(),
            output_name_.capacity() + 1,
            ImGuiInputTextFlags_CallbackResize,
            string_resize_callback,
            &output_name_);
        if (is_running || reuse_working_directory_) {
            ImGui::EndDisabled();
        }

        if (output_name_changed) {
            is_output_name_pending_autofill_ = false;
        }

        if (is_output_name_pending_autofill_) {
            if (!output_name_.empty()) {
                is_output_name_pending_autofill_ = false;
            } else {
                std::string detected_title = current_task_.detected_title();
                if (!detected_title.empty() && detected_title != last_synchronized_title_) {
                    output_name_ = detected_title;
                    last_synchronized_title_ = std::move(detected_title);
                    is_output_name_pending_autofill_ = false;
                }
            }
        }
    }

    ImGui::Separator();

    {
        if (ImGui::BeginTable("pipeline", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
            ImGui::TableSetupColumn("step", ImGuiTableColumnFlags_WidthStretch, 0.3f);
            ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthStretch, 0.7f);

            for (std::size_t index = 0; index < steps.size(); ++index) {
                const swb::StepInfo& step_info = steps[index];
                const std::string_view label = swb::step_label(static_cast<swb::StepId>(index));
                const char* status_text = step_info.status.empty() ? "等待" : step_info.status.c_str();

                ImGui::TableNextRow(0, ImGui::GetFrameHeight());

                ImGui::TableSetColumnIndex(0);
                draw_status_circle(step_info.state);
                ImGui::SameLine(0.0f, style.ItemInnerSpacing.x * 0.65f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label.data(), label.data() + label.size());

                ImGui::TableSetColumnIndex(1);
                switch (step_info.state) {
                case StepState::completed:
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored({0.35f, 0.78f, 0.35f, 1.0f}, "%s", status_text);
                    break;
                case StepState::in_progress:
                    draw_in_progress_text(status_text);
                    break;
                case StepState::failed:
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored({0.85f, 0.40f, 0.40f, 1.0f}, "%s", status_text);
                    break;
                case StepState::waiting:
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", status_text);
                    break;
                }
            }

            ImGui::EndTable();
        }
    }

    ImGui::Separator();

    {
        ImGui::TextUnformatted("设置");
        ImGui::Spacing();
        bool should_open_output_settings_popup = false;

        constexpr std::array row_labels{
            std::string_view{"语音识别"},
            std::string_view{"模型目录"},
            std::string_view{"实际后端"},
            std::string_view{"Whisper Base URL"},
            std::string_view{"Whisper API Key"},
            std::string_view{"Whisper Model"},
            std::string_view{"LLM Base URL"},
            std::string_view{"LLM API Key"},
            std::string_view{"LLM Model"},
            std::string_view{"重试次数"},
            std::string_view{"源语言"},
            std::string_view{"目标语言"},
            std::string_view{"输出目录"},
            std::string_view{"编码配置"},
        };

        float label_width = 0.0f;
        for (const std::string_view row_label : row_labels) {
            label_width = std::max(label_width, ImGui::CalcTextSize(row_label.data()).x);
        }
        label_width += style.ItemInnerSpacing.x;

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {0.0f, style.ItemSpacing.y * 0.5f});
        if (ImGui::BeginTable("settings", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_width);
            ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);

            const auto begin_row = [&](std::string_view row_label, std::string_view help_text = {}) {
                ImGui::TableNextRow(0, ImGui::GetFrameHeight());
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(row_label.data());
                if (!help_text.empty()) {
                    help_marker(help_text);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
            };

            const auto row_input = [&](std::string_view row_label, std::string& value, ImGuiInputTextFlags flags = 0) {
                begin_row(row_label);
                std::string identifier{"##"};
                identifier += row_label;
                is_dirty |= input_text(identifier.c_str(), value, flags);
            };

            begin_row("语音识别");
            const char* backend_preview = configuration_.transcription_backend == swb::TranscriptionBackend::local
                ? "本地whisper.cpp"
                : "Whisper API";
            if (ImGui::BeginCombo("##transcription_backend", backend_preview)) {
                if (ImGui::Selectable(
                        "本地whisper.cpp",
                        configuration_.transcription_backend == swb::TranscriptionBackend::local)) {
                    configuration_.transcription_backend = swb::TranscriptionBackend::local;
                    is_dirty = true;
                    swb::ModelStatus asr_status;
                    bool model_running = false;
                    {
                        std::scoped_lock lock(model_mutex_);
                        asr_status = asr_model_status_;
                        model_running = model_operation_running_;
                    }
                    if (!model_running && asr_status.availability == swb::ModelAvailability::missing) {
                        start_model_download(swb::default_local_asr_model(), false);
                    }
                }
                if (ImGui::Selectable(
                        "Whisper API",
                        configuration_.transcription_backend == swb::TranscriptionBackend::api)) {
                    configuration_.transcription_backend = swb::TranscriptionBackend::api;
                    is_dirty = true;
                }
                ImGui::EndCombo();
            }

            if (configuration_.transcription_backend == swb::TranscriptionBackend::api) {
                row_input("Whisper Base URL", configuration_.whisper_base_url);
                row_input("Whisper API Key", configuration_.whisper_api_key, ImGuiInputTextFlags_Password);
                row_input("Whisper Model", configuration_.whisper_model);
            } else {
                bool model_operation_for_controls = false;
                {
                    std::scoped_lock lock(model_mutex_);
                    model_operation_for_controls = model_operation_running_;
                }
                begin_row("本地模型");
                ImGui::TextUnformatted("Whisper base multilingual q5_1");

                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("模型目录");
                    help_marker(
                        "保存本地语音识别使用的Whisper模型和VAD模型。\n"
                        "留空时使用应用的默认位置，一般为%LOCALAPPDATA%\\SubtitleWorkbench\\models。\n"
                        "选择其他目录可将模型保存在其他磁盘。");
                    ImGui::TableSetColumnIndex(1);
                    if (model_operation_for_controls) {
                        ImGui::BeginDisabled();
                    }
                    const float browse_width = ImGui::CalcTextSize("浏览").x + style.FramePadding.x * 2;
                    ImGui::SetNextItemWidth(-(browse_width + style.ItemSpacing.x));
                    const bool directory_changed = input_text("##local_model_directory", configuration_.local_asr_model_dir);
                    if (directory_changed) {
                        is_dirty = true;
                        std::scoped_lock lock(model_mutex_);
                        model_status_dirty_ = true;
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        refresh_model_statuses();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("浏览##modeldir", {browse_width, 0.0f})) {
                        if (std::string folder = browse_directory(window_handle_, L"选择本地模型目录"); !folder.empty()) {
                            configuration_.local_asr_model_dir = std::move(folder);
                            is_dirty = true;
                            refresh_model_statuses();
                        }
                    }
                    if (model_operation_for_controls) {
                        ImGui::EndDisabled();
                    }
                }

                swb::ModelStatus asr_status;
                swb::ModelStatus vad_status;
                bool model_running = false;
                double model_progress = 0.0;
                std::string operation_message;
                {
                    std::scoped_lock lock(model_mutex_);
                    asr_status = asr_model_status_;
                    vad_status = vad_model_status_;
                    model_running = model_operation_running_;
                    model_progress = model_download_fraction_;
                    operation_message = model_operation_message_;
                }

                begin_row("模型状态");
                const std::string asr_size = format_mebibytes(
                    asr_status.size == 0 ? swb::default_local_asr_model().file_size : asr_status.size);
                ImGui::Text("%s，%s", availability_label(asr_status.availability).data(), asr_size.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", swb::path_to_utf8(asr_status.path).c_str());
                }

                begin_row("模型路径");
                const std::string asr_path = swb::path_to_utf8(asr_status.path);
                ImGui::TextWrapped("%s", asr_path.c_str());

                begin_row("模型操作");
                if (model_running) {
                    ImGui::ProgressBar(static_cast<float>(model_progress), {-78.0f, 0.0f});
                    ImGui::SameLine();
                    if (ImGui::Button("取消##model_download")) {
                        cancel_model_download();
                    }
                } else {
                    const char* download_label = asr_status.availability == swb::ModelAvailability::available
                        ? "重新下载##asr"
                        : "下载##asr";
                    if (ImGui::Button(download_label)) {
                        start_model_download(
                            swb::default_local_asr_model(),
                            asr_status.availability != swb::ModelAvailability::missing);
                    }
                    if (asr_status.availability != swb::ModelAvailability::missing) {
                        ImGui::SameLine();
                        if (ImGui::Button("删除##asr")) {
                            pending_model_deletion_ = std::string{swb::default_local_asr_model().id};
                            ImGui::OpenPopup("删除本地模型");
                        }
                    }
                }
                if (!operation_message.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", operation_message.c_str());
                }

                begin_row(
                    "计算方式",
                    "自动：尝试所选GPU，初始化失败后改用CPU，适合日常使用。\n"
                    "GPU优先：明确使用所选GPU，初始化失败后改用CPU并显示原因。\n"
                    "仅CPU：直接使用CPU。");
                const char* compute_preview = "自动";
                if (configuration_.local_asr_compute == swb::LocalAsrCompute::gpu) {
                    compute_preview = "GPU优先";
                } else if (configuration_.local_asr_compute == swb::LocalAsrCompute::cpu) {
                    compute_preview = "仅CPU";
                }
                if (ImGui::BeginCombo("##local_compute", compute_preview)) {
                    constexpr std::array compute_options{
                        std::pair{swb::LocalAsrCompute::automatic, "自动"},
                        std::pair{swb::LocalAsrCompute::gpu, "GPU优先"},
                        std::pair{swb::LocalAsrCompute::cpu, "仅CPU"},
                    };
                    for (const auto& [value, label] : compute_options) {
                        if (ImGui::Selectable(label, configuration_.local_asr_compute == value)) {
                            configuration_.local_asr_compute = value;
                            is_dirty = true;
                        }
                    }
                    ImGui::EndCombo();
                }

                if (configuration_.local_asr_compute != swb::LocalAsrCompute::cpu) {
                    begin_row("GPU设备");
                    const auto selected_device = std::ranges::find(
                        local_asr_gpu_devices_,
                        configuration_.local_asr_gpu_device,
                        &swb::LocalAsrGpuDevice::index);
                    std::string device_preview = "选择GPU设备";
                    if (selected_device != local_asr_gpu_devices_.end()) {
                        device_preview = std::to_string(selected_device->index) + " · " + selected_device->name;
                    } else if (local_asr_gpu_devices_.empty()) {
                        device_preview = "仅CPU可用";
                    }

                    ImGui::BeginDisabled(local_asr_gpu_devices_.empty());
                    if (ImGui::BeginCombo("##local_gpu_device", device_preview.c_str())) {
                        for (const swb::LocalAsrGpuDevice& device : local_asr_gpu_devices_) {
                            const bool selected = device.index == configuration_.local_asr_gpu_device;
                            const std::string label = std::to_string(device.index) + " · " + device.name;
                            if (ImGui::Selectable(label.c_str(), selected)) {
                                configuration_.local_asr_gpu_device = device.index;
                                is_dirty = true;
                            }
                            if (selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::EndDisabled();
                }

                begin_row(
                    "推理线程",
                    "控制whisper.cpp在推理阶段使用的CPU线程数。\n"
                    "推荐值为0，程序会按系统报告的逻辑处理器数量自动设置。\n"
                    "需要给其他程序留出处理器资源时，可设为逻辑处理器数量的一半，最低为1。");
                int threads = std::max(configuration_.local_asr_threads, 0);
                if (ImGui::InputInt("##local_threads", &threads)) {
                    configuration_.local_asr_threads = std::max(threads, 0);
                    is_dirty = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("0为自动");

                begin_row(
                    "VAD",
                    "VAD是语音活动检测，用于定位人声范围，减少静音片段参与Whisper识别。\n"
                    "访谈、会议等长音频适合启用，下载VAD模型后生效。");
                bool use_vad = configuration_.local_asr_use_vad;
                if (ImGui::Checkbox("启用##local_vad", &use_vad)) {
                    configuration_.local_asr_use_vad = use_vad;
                    is_dirty = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", availability_label(vad_status.availability).data());
                if (!model_running) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(vad_status.availability == swb::ModelAvailability::available
                            ? "重新下载##vad"
                            : "下载##vad")) {
                        start_model_download(
                            swb::default_vad_model(),
                            vad_status.availability != swb::ModelAvailability::missing);
                    }
                    if (vad_status.availability != swb::ModelAvailability::missing) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("删除##vad")) {
                            pending_model_deletion_ = std::string{swb::default_vad_model().id};
                            ImGui::OpenPopup("删除本地模型");
                        }
                    }
                }

                const swb::TranscriptionRuntime runtime = current_task_.transcription_runtime();
                begin_row("实际后端");
                const std::string runtime_label = runtime_backend_label(runtime);
                ImGui::TextUnformatted(runtime_label.c_str());
                if (!runtime.fallback_reason.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", runtime.fallback_reason.c_str());
                }
            }

            row_input("LLM Base URL", configuration_.language_model_base_url);
            row_input("LLM API Key", configuration_.language_model_api_key, ImGuiInputTextFlags_Password);
            row_input("LLM Model", configuration_.language_model_name);

            begin_row("全局重试次数");
            int retry_count = std::max(configuration_.retry_count, 0);
            if (ImGui::InputInt("##retry_count", &retry_count, 1, 5)) {
                configuration_.retry_count = std::max(retry_count, 0);
                is_dirty = true;
            }
            ImGui::SameLine(0.0f, style.ItemSpacing.x);
            ImGui::TextDisabled("0代表不重试");

            begin_row("源语言");
            is_dirty |= lang_combo("##src_lang", configuration_.source_lang);

            begin_row("目标语言");
            is_dirty |= lang_combo("##dst_lang", configuration_.target_lang);

            {
                ImGui::TableNextRow(0, ImGui::GetFrameHeight());
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("输出目录");
                ImGui::TableSetColumnIndex(1);

                const float browse_button_width = ImGui::CalcTextSize("浏览").x + style.FramePadding.x * 2;
                ImGui::SetNextItemWidth(-(browse_button_width + style.ItemSpacing.x));
                is_dirty |= input_text("##output_directory", configuration_.output_dir);
                ImGui::SameLine();
                if (ImGui::Button("浏览##outdir", {browse_button_width, 0.0f})) {
                    if (std::string folder = browse_folder(window_handle_); !folder.empty()) {
                        configuration_.output_dir = std::move(folder);
                        is_dirty = true;
                    }
                }
            }

            begin_row("编码配置");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("硬编码字幕");
            ImGui::SameLine(0.0f, style.ItemSpacing.x);
            if (ImGui::Button("设置##output_settings", {96.0f, 0.0f})) {
                should_open_output_settings_popup = true;
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        if (ImGui::BeginPopupModal("删除本地模型", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const swb::ModelManifestEntry* entry = swb::find_model_manifest_entry(pending_model_deletion_);
            ImGui::TextWrapped("只会删除模型清单管理的文件。删除后需要重新下载才能使用对应功能。");
            if (entry != nullptr) {
                ImGui::TextDisabled("%s", entry->display_name.data());
            }
            bool operation_running = false;
            {
                std::scoped_lock lock(model_mutex_);
                operation_running = model_operation_running_;
            }
            if (is_running || operation_running || entry == nullptr) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("确认删除", {120.0f, 0.0f})) {
                const swb::OperationStatus removal = swb::remove_managed_model(
                    *entry,
                    swb::resolve_model_directory(configuration_.local_asr_model_dir));
                {
                    std::scoped_lock lock(model_mutex_);
                    model_operation_message_ = removal.message;
                }
                refresh_model_statuses();
                pending_model_deletion_.clear();
                ImGui::CloseCurrentPopup();
            }
            if (is_running || operation_running || entry == nullptr) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", {120.0f, 0.0f})) {
                pending_model_deletion_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (should_open_output_settings_popup) {
            ImGui::OpenPopup("编码配置");
        }

        ImGui::SetNextWindowSize({460.0f, 0.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal("编码配置", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            bool bilingual_subtitles = configuration_.bilingual_subtitles;
            if (ImGui::Checkbox("双语字幕", &bilingual_subtitles)) {
                configuration_.bilingual_subtitles = bilingual_subtitles;
                is_dirty = true;
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("字体");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##hard_subtitle_font_preset", configuration_.hard_subtitle_style.font_name.c_str())) {
                for (const std::string_view font_name : hard_subtitle_font_presets) {
                    const bool selected = configuration_.hard_subtitle_style.font_name == font_name;
                    if (ImGui::Selectable(font_name.data(), selected)) {
                        configuration_.hard_subtitle_style.font_name = std::string{font_name};
                        is_dirty = true;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SetNextItemWidth(-1.0f);
            is_dirty |= input_text("##hard_subtitle_font_name", configuration_.hard_subtitle_style.font_name);

            int chinese_font_size = configuration_.hard_subtitle_style.chinese_font_size;
            if (ImGui::SliderInt("中文字幕号", &chinese_font_size, 8, 96)) {
                configuration_.hard_subtitle_style.chinese_font_size = chinese_font_size;
                is_dirty = true;
            }

            int english_font_size = configuration_.hard_subtitle_style.english_font_size;
            if (ImGui::SliderInt("英文字号", &english_font_size, 8, 96)) {
                configuration_.hard_subtitle_style.english_font_size = english_font_size;
                is_dirty = true;
            }

            int bottom_margin = configuration_.hard_subtitle_style.bottom_margin;
            if (ImGui::SliderInt("底边距离", &bottom_margin, 0, 160)) {
                configuration_.hard_subtitle_style.bottom_margin = bottom_margin;
                is_dirty = true;
            }

            is_dirty |= edit_subtitle_color("字体颜色", configuration_.hard_subtitle_style.fill_color);
            is_dirty |= edit_subtitle_color("描边颜色", configuration_.hard_subtitle_style.outline_color);

            float outline_thickness = configuration_.hard_subtitle_style.outline_thickness;
            if (ImGui::SliderFloat("描边厚度", &outline_thickness, 0.0f, 8.0f, "%.1f")) {
                configuration_.hard_subtitle_style.outline_thickness = outline_thickness;
                is_dirty = true;
            }

            if (configuration_.bilingual_subtitles) {
                float bilingual_line_gap = configuration_.hard_subtitle_style.bilingual_line_gap;
                if (ImGui::SliderFloat("双语距离", &bilingual_line_gap, 0.0f, 40.0f, "%.1f")) {
                    configuration_.hard_subtitle_style.bilingual_line_gap = bilingual_line_gap;
                    is_dirty = true;
                }
            }

            is_dirty |= edit_subtitle_color("预览背景", configuration_.hard_subtitle_style.preview_background_color);
            ImGui::TextDisabled("仅用于预览，不会写入实际字幕。");

            ImGui::Spacing();
            ImGui::TextUnformatted("最终样式预览");
            draw_hard_subtitle_preview(configuration_);

            ImGui::Spacing();
            if (ImGui::Button("关闭", {120.0f, 0.0f})) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    ImGui::End();

    if (is_dirty) {
        swb::save_config(configuration_, configuration_path_);
    }
}

void App::create_render_target() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
    device_->CreateRenderTargetView(back_buffer.Get(), nullptr, render_target_view_.GetAddressOf());
}

LRESULT CALLBACK App::wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) {
        return true;
    }

    switch (message) {
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) {
            return 0;
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
