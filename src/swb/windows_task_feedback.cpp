#include "swb/windows_task_feedback.h"

#include <shellapi.h>

#include <cwchar>
#include <stdexcept>

namespace swb {

namespace {

constexpr wchar_t app_user_model_id[] = L"SubtitleWorkbench";
constexpr UINT notify_icon_id = 1;

template <std::size_t N>
void copy_text(std::wstring_view source, wchar_t (&destination)[N]) {
    const std::size_t copy_length = std::min(source.size(), N - 1);
    if (copy_length > 0) {
        std::wmemcpy(destination, source.data(), copy_length);
    }
    destination[copy_length] = L'\0';
}

[[nodiscard]] NOTIFYICONDATAW make_notify_icon_data(HWND window_handle) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_handle;
    data.uID = notify_icon_id;
    return data;
}

}

ComApartment::ComApartment() {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(result)) {
        throw std::runtime_error("failed to initialize COM");
    }
    initialized_ = true;
}

ComApartment::~ComApartment() {
    if (initialized_) {
        CoUninitialize();
    }
}

void TaskbarProgressController::initialize(HWND window_handle) {
    window_handle_ = window_handle;
    if (window_handle_ == nullptr || taskbar_list_) {
        return;
    }

    Microsoft::WRL::ComPtr<ITaskbarList3> taskbar_list;
    const HRESULT result = CoCreateInstance(
        CLSID_TaskbarList,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(taskbar_list.GetAddressOf()));
    if (FAILED(result) || !taskbar_list || FAILED(taskbar_list->HrInit())) {
        return;
    }
    taskbar_list_ = std::move(taskbar_list);
}

void TaskbarProgressController::set_progress(const TaskProgress& progress) const {
    if (!taskbar_list_ || window_handle_ == nullptr || progress.total_units == 0) {
        return;
    }
    taskbar_list_->SetProgressState(window_handle_, TBPF_NORMAL);
    taskbar_list_->SetProgressValue(window_handle_, progress.completed_units, progress.total_units);
}

void TaskbarProgressController::clear() const {
    if (!taskbar_list_ || window_handle_ == nullptr) {
        return;
    }
    taskbar_list_->SetProgressState(window_handle_, TBPF_NOPROGRESS);
}

TaskCompletionNotifier::~TaskCompletionNotifier() {
    shutdown();
}

void TaskCompletionNotifier::initialize(HWND window_handle) {
    window_handle_ = window_handle;
    if (window_handle_ == nullptr || notify_icon_added_) {
        return;
    }

    SetCurrentProcessExplicitAppUserModelID(app_user_model_id);

    NOTIFYICONDATAW data = make_notify_icon_data(window_handle_);
    data.uFlags = NIF_ICON | NIF_TIP | NIF_STATE;
    data.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    data.dwState = NIS_HIDDEN;
    data.dwStateMask = NIS_HIDDEN;
    copy_text(L"Subtitle Workbench", data.szTip);

    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        return;
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    notify_icon_added_ = true;
}

void TaskCompletionNotifier::shutdown() {
    if (!notify_icon_added_ || window_handle_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW data = make_notify_icon_data(window_handle_);
    Shell_NotifyIconW(NIM_DELETE, &data);
    notify_icon_added_ = false;
}

void TaskCompletionNotifier::show_info(std::wstring_view title, std::wstring_view message) const {
    show(title, message, NIIF_INFO | NIIF_RESPECT_QUIET_TIME);
}

void TaskCompletionNotifier::show_error(std::wstring_view title, std::wstring_view message) const {
    show(title, message, NIIF_ERROR | NIIF_RESPECT_QUIET_TIME);
}

void TaskCompletionNotifier::show(std::wstring_view title, std::wstring_view message, DWORD info_flags) const {
    if (!notify_icon_added_ || window_handle_ == nullptr) {
        return;
    }

    NOTIFYICONDATAW data = make_notify_icon_data(window_handle_);
    data.uFlags = NIF_INFO | NIF_REALTIME;
    data.dwInfoFlags = info_flags;
    copy_text(title, data.szInfoTitle);
    copy_text(message, data.szInfo);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

}