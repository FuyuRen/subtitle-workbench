#pragma once

#include "swb/task.h"
#include "swb/win32_headers.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <string_view>

namespace swb {

class ComApartment {
public:
    ComApartment();
    ~ComApartment();

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_{false};
};

class TaskbarProgressController {
public:
    void initialize(HWND window_handle);
    void set_progress(const TaskProgress& progress) const;
    void clear() const;

private:
    HWND window_handle_{nullptr};
    Microsoft::WRL::ComPtr<ITaskbarList3> taskbar_list_;
};

class TaskCompletionNotifier {
public:
    TaskCompletionNotifier() = default;
    ~TaskCompletionNotifier();

    TaskCompletionNotifier(const TaskCompletionNotifier&) = delete;
    TaskCompletionNotifier& operator=(const TaskCompletionNotifier&) = delete;

    void initialize(HWND window_handle);
    void shutdown();
    void show_info(std::wstring_view title, std::wstring_view message) const;
    void show_error(std::wstring_view title, std::wstring_view message) const;

private:
    void show(std::wstring_view title, std::wstring_view message, DWORD info_flags) const;

    HWND window_handle_{nullptr};
    bool notify_icon_added_{false};
};

}