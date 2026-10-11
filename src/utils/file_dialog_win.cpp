#include "utils/failure_shutdown.hpp"
#include "utils/file_dialog.hpp"
#include "utils/filesystem.hpp"

#include <Windows.h>
#include <condition_variable>
#include <mutex>
#include <shobjidl.h>
#include <stop_token>
#include <thread>
#include <utility>
#include <wrl/client.h>

namespace miximus::utils {
namespace {
using Microsoft::WRL::ComPtr;

struct cancellation_s
{
    IFileDialog*    dialog;
    std::stop_token stop;
};
void CALLBACK poll_shutdown(HWND window, UINT /*message*/, UINT_PTR /*timer*/, DWORD /*time*/)
{
    // Win32 stores the callback pointer in pointer-sized window userdata.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto* cancellation = reinterpret_cast<const cancellation_s*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (cancellation != nullptr && cancellation->stop.stop_requested()) {
        ComPtr<IOleWindow> dialog_window;
        HWND               handle{};
        if (SUCCEEDED(cancellation->dialog->QueryInterface(IID_PPV_ARGS(&dialog_window))) &&
            SUCCEEDED(dialog_window->GetWindow(&handle))) {
            PostMessageW(handle, WM_CLOSE, 0, 0);
        }
    }
}

file_dialog_result_s show_dialog(const file_dialog_request_s& request, const std::stop_token& stop)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return {.error = error_e::unavailable};
    }
    DWORD options{};
    if (FAILED(dialog->GetOptions(&options)) ||
        FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR))) {
        return {.error = error_e::internal_error};
    }
    struct wide_filter_s
    {
        std::wstring label;
        std::wstring pattern;
    };
    std::vector<wide_filter_s> owned_filters;
    owned_filters.reserve(request.filters.size());
    for (const auto& filter : request.filters) {
        owned_filters.push_back(
            {.label = path_from_utf8(filter.label).wstring(), .pattern = path_from_utf8(filter.pattern).wstring()});
    }
    std::vector<COMDLG_FILTERSPEC> filters;
    filters.reserve(owned_filters.size());
    for (const auto& filter : owned_filters) {
        filters.push_back({.pszName = filter.label.c_str(), .pszSpec = filter.pattern.c_str()});
    }
    if (!filters.empty()) {
        dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
    }
    const auto title = path_from_utf8(request.title).wstring();
    dialog->SetTitle(title.c_str());
    if (!request.initial_path.empty()) {
        const auto         path = path_from_utf8(request.initial_path);
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
        dialog->SetFileName(path.filename().c_str());
    }
    cancellation_s cancellation{.dialog = dialog.Get(), .stop = stop};
    // Shell modal loops can ignore thread messages. A message-only window keeps
    // shutdown delivery in the window-message path on this same STA.
    const auto window = CreateWindowExW(0,
                                        L"STATIC",
                                        L"Miximus file dialog control",
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        HWND_MESSAGE,
                                        nullptr,
                                        GetModuleHandleW(nullptr),
                                        nullptr);
    if (window == nullptr) {
        return {.error = error_e::unavailable};
    }
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&cancellation));
    const auto timer = SetTimer(window, 1, 100, poll_shutdown);
    if (timer == 0) {
        DestroyWindow(window);
        return {.error = error_e::unavailable};
    }
    const auto result = stop.stop_requested() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : dialog->Show(nullptr);
    KillTimer(window, timer);
    DestroyWindow(window);
    if (stop.stop_requested()) {
        return {.error = error_e::cancelled};
    }
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {};
    }
    ComPtr<IShellItem> item;
    if (FAILED(result) || FAILED(dialog->GetResult(&item))) {
        return {.error = error_e::internal_error};
    }
    PWSTR raw_path{};
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path))) {
        return {.error = error_e::internal_error};
    }
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_path(raw_path, CoTaskMemFree);
    return {.path = path_to_utf8(std::filesystem::path(owned_path.get()))};
}
} // namespace

struct file_dialog_s::impl_s
{
    struct request_s
    {
        file_dialog_request_s options;
        completion_t          completion;
    };
    std::mutex                  mutex;
    std::condition_variable_any ready;
    std::optional<request_s>    pending;
    bool                        busy{};
    bool                        stopped{};
    std::jthread                thread{[this](const std::stop_token& stop) noexcept {
        try {
            run(stop);
        } catch (const std::exception& error) {
            request_failure_shutdown("File dialog worker failed", error.what());
        } catch (...) {
            request_failure_shutdown("File dialog worker failed");
        }
    }};

    void run(const std::stop_token& stop)
    {
        const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        for (;;) {
            std::unique_lock lock(mutex);
            ready.wait(lock, stop, [this] { return pending.has_value(); });
            if (!pending) {
                break;
            }
            auto request = std::move(*pending);
            pending.reset();
            lock.unlock();
            file_dialog_result_s result{.error = error_e::unavailable};
            try {
                if (SUCCEEDED(initialized)) {
                    result = show_dialog(request.options, stop);
                }
            } catch (...) {
                result = {.error = error_e::internal_error};
            }
            request.completion(std::move(result));
            lock.lock();
            busy = false;
        }
        if (SUCCEEDED(initialized)) {
            CoUninitialize();
        }
    }
};

file_dialog_s::file_dialog_s()
    : impl_(std::make_unique<impl_s>())
{
}
file_dialog_s::~file_dialog_s() { stop(); }
bool    file_dialog_s::supported() { return true; }
error_e file_dialog_s::open(file_dialog_request_s request, completion_t completion)
{
    const std::scoped_lock lock(impl_->mutex);
    if (impl_->stopped) {
        return error_e::unavailable;
    }
    if (impl_->busy) {
        return error_e::busy;
    }
    impl_->pending = impl_s::request_s{.options = std::move(request), .completion = std::move(completion)};
    impl_->busy    = true;
    impl_->ready.notify_one();
    return error_e::no_error;
}
void file_dialog_s::stop()
{
    {
        const std::scoped_lock lock(impl_->mutex);
        impl_->stopped = true;
    }
    impl_->thread.request_stop();
    impl_->ready.notify_one();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}
} // namespace miximus::utils
