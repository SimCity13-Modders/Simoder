#include "ui/window_message_hook.hpp"
#include <atomic>
#include <mutex>

namespace sc13::ui {
namespace {
HHOOK g_hook{};
std::atomic<HWND> g_window{};
using InputCallback = bool (*)(HWND, UINT, WPARAM, LPARAM);
std::atomic<InputCallback> g_input{};
std::atomic_bool g_active{};
std::recursive_mutex g_dispatchMutex;

/** @summary Admit only actual keyboard and mouse messages, leaving lifecycle and native dispatch intact. */
bool IsInput(UINT message) noexcept {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        (message >= WM_KEYFIRST && message <= WM_KEYLAST);
}
/** @summary Route removed queue messages once and suppress only gestures owned by the overlay. */
LRESULT CALLBACK ObserveQueue(int code, WPARAM mode, LPARAM parameter) noexcept {
    if (code >= 0 && mode == PM_REMOVE && parameter) {
        auto& message = *reinterpret_cast<MSG*>(parameter);
        if (message.hwnd == g_window.load(std::memory_order_acquire) && IsInput(message.message)) {
            std::lock_guard<std::recursive_mutex> guard(g_dispatchMutex);
            const auto callback = g_input.load(std::memory_order_acquire);
            if (callback) {
                if (callback(message.hwnd, message.message, message.wParam, message.lParam)) {
                    if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
                        TranslateMessage(&message);
                    }
                    message.message = WM_NULL;
                    message.wParam = 0;
                    message.lParam = 0;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, mode, parameter);
}
}
bool InitializeWindowMessageHook(HWND window, InputCallback input, std::string& error) noexcept {
    if (g_hook) return true;
    DWORD process{};
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    HMODULE module{};
    if (!window || !input || !thread || process != GetCurrentProcessId() ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ObserveQueue), &module)) {
        error = "Cannot resolve the owned game window queue.";
        return false;
    }
    g_window = window;
    g_input.store(input, std::memory_order_release);
    g_hook = SetWindowsHookExW(WH_GETMESSAGE, &ObserveQueue, module, thread);
    if (!g_hook) {
        error = "Cannot observe game window input queue: " + std::to_string(GetLastError());
        return false;
    }
    g_active.store(true, std::memory_order_release);
    return true;
}
bool IsObservedWindowInput(HWND window, UINT message) noexcept {
    return g_active.load(std::memory_order_acquire) &&
        g_window.load(std::memory_order_acquire) == window && IsInput(message);
}
bool RouteUnqueuedWindowInput(HWND window, UINT message, WPARAM word, LPARAM value) noexcept {
    std::lock_guard<std::recursive_mutex> guard(g_dispatchMutex);
    const auto callback = g_input.load(std::memory_order_acquire);
    return callback && g_window.load(std::memory_order_acquire) == window &&
        callback(window, message, word, value);
}
void ShutdownWindowMessageHook() noexcept {
    g_input.store(nullptr, std::memory_order_release);
    g_active.store(false, std::memory_order_release);
    if (g_hook) UnhookWindowsHookEx(g_hook);
    std::lock_guard<std::recursive_mutex> guard(g_dispatchMutex);
    g_hook = nullptr;
    g_window = nullptr;
}
}
