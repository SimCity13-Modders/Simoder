#include "platform/game_window_dispatch.hpp"
#include <map>
#include <mutex>
#include <atomic>
#include <limits>

namespace sc13::platform {
namespace {
/** @summary Preserve the chain even if a later third-party subclass still forwards to us. */
struct Attachment final { GameWindowIdentity identity; WNDPROC original{}; };
std::mutex g_mutex;
std::map<HWND, Attachment> g_chains;
GameWindowIdentity g_current;
std::uint64_t g_generation{};
WindowMessageConsumer g_native{}, g_input{};
WindowStopObserver g_stop{};
WindowResultObserver g_result{};
bool g_closed{};
std::atomic_uint g_active{};
constexpr auto kIdentityProperty = L"Simoder.Core.WindowAttachment.v1";

/** @summary Forward one message through the captured procedure exactly once.
 * @param window Receiver.
 * @param message Win32 message.
 * @param word First message value.
 * @param value Second message value.
 * @return Consumer result or original procedure result.
 */
LRESULT CALLBACK Router(HWND window, UINT message, WPARAM word, LPARAM value) noexcept {
    WNDPROC original{};
    WindowMessageConsumer native{}, input{};
    WindowStopObserver stop{};
    WindowResultObserver observer{};
    {
        std::lock_guard lock(g_mutex);
        g_active.fetch_add(1);
        const auto found = g_chains.find(window);
        if (found != g_chains.end()) original = found->second.original;
        if (!g_closed && found != g_chains.end() && GetPropW(window, kIdentityProperty) == &found->second &&
            g_current.window == window && g_current.thread == GetCurrentThreadId()) {
            native = g_native; input = g_input;
            observer = g_result;
            if (message == WM_CLOSE || message == WM_DESTROY || message == WM_NCDESTROY) stop = g_stop;
            if (message == WM_NCDESTROY) g_current = {};
        }
    }
    /** @summary Retain callback state until every copied callback and original procedure returns. */
    struct ActiveCall final { ~ActiveCall() { g_active.fetch_sub(1); } } activeCall;
    if (stop) stop();
    const bool lifecycle = message == WM_CLOSE || message == WM_DESTROY || message == WM_NCDESTROY;
    if (!lifecycle && native && native(window, message, word, value)) return 0;
    const bool captured = !lifecycle && input && input(window, message, word, value);
    const auto result = captured ? 1 : original ? CallWindowProcW(original, window, message, word, value) : DefWindowProcW(window, message, word, value);
    if (observer) observer(message, word, value, result, captured);
    if (message == WM_NCDESTROY) { std::lock_guard lock(g_mutex); RemovePropW(window, kIdentityProperty); g_chains.erase(window); }
    return result;
}
/** @summary Remove only our topmost procedure, preserving subsequent foreign subclasses. */
void DetachLocked() noexcept {
    const auto found = g_chains.find(g_current.window);
    if (found != g_chains.end() && IsWindow(g_current.window) &&
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_current.window, GWLP_WNDPROC)) == Router) {
        SetLastError(ERROR_SUCCESS);
        const auto previous = SetWindowLongPtrW(g_current.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(found->second.original));
        if (previous || GetLastError() == ERROR_SUCCESS) { RemovePropW(g_current.window, kIdentityProperty); g_chains.erase(found); }
    }
    g_current = {};
}
}
void ConfigureGameWindowDispatch(WindowMessageConsumer consumer, WindowStopObserver stop) noexcept {
    std::lock_guard lock(g_mutex);
    if (g_active.load()) { g_closed = true; g_native = nullptr; g_stop = nullptr; return; }
    g_native = consumer; g_stop = stop; g_closed = false;
}
bool BindGameWindow(HWND window) noexcept {
    DWORD process{};
    const DWORD thread = window ? GetWindowThreadProcessId(window, &process) : 0;
    if (window && (!IsWindow(window) || !thread || process != GetCurrentProcessId())) return false;
    std::unique_lock lock(g_mutex);
    if (window && g_closed) return false;
    const auto existing = g_chains.find(window);
    if (window && existing != g_chains.end() && GetPropW(window, kIdentityProperty) == &existing->second &&
        g_current.window == window && g_current.thread == thread && g_current.process == process) return true;
    const auto stop = g_current.window ? g_stop : nullptr;
    DetachLocked();
    if (stop) { g_active.fetch_add(1); lock.unlock(); stop(); g_active.fetch_sub(1); lock.lock(); }
    if (!window) return true;
    if (g_closed) return false;
    // A retained chain is not safe to attach again until its foreign owner removes it.
    if (g_chains.contains(window) || g_generation == std::numeric_limits<std::uint64_t>::max()) return false;
    try {
        auto [entry, inserted] = g_chains.emplace(window, Attachment{});
        if (!inserted) return false;
        if (!SetPropW(window, kIdentityProperty, &entry->second)) { g_chains.erase(entry); return false; }
        SetLastError(ERROR_SUCCESS);
        const auto previous = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Router));
        if (!previous && GetLastError() != ERROR_SUCCESS) { RemovePropW(window, kIdentityProperty); g_chains.erase(entry); return false; }
        g_current = {window, process, thread, ++g_generation};
        entry->second = {g_current, reinterpret_cast<WNDPROC>(previous)};
        return true;
    } catch (...) { return false; }
}
GameWindowIdentity CurrentGameWindow() noexcept {
    std::lock_guard lock(g_mutex);
    DWORD process{};
    const auto thread = GetWindowThreadProcessId(g_current.window, &process);
    const auto found = g_chains.find(g_current.window);
    if (!g_current.window || found == g_chains.end() || GetPropW(g_current.window, kIdentityProperty) != &found->second ||
        !IsWindow(g_current.window) || thread != g_current.thread || process != g_current.process) return {};
    return g_current;
}
bool IsCurrentGameWindow(const GameWindowIdentity& identity) noexcept {
    const auto current = CurrentGameWindow();
    return identity.window && current.window == identity.window && current.process == identity.process &&
        current.thread == identity.thread && current.generation == identity.generation;
}
void SetGameWindowInputConsumer(WindowMessageConsumer consumer) noexcept { std::lock_guard lock(g_mutex); g_input = consumer; }
void SetGameWindowResultObserver(WindowResultObserver observer) noexcept { std::lock_guard lock(g_mutex); g_result = observer; }
void ShutdownGameWindowDispatch() noexcept {
    std::lock_guard lock(g_mutex); g_closed = true; g_native = nullptr; g_input = nullptr; g_stop = nullptr; g_result = nullptr; DetachLocked();
}
bool GameWindowDispatchDrained() noexcept { return g_active.load() == 0; }
}
