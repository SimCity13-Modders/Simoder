#include "platform/game_window_dispatch.hpp"
#include "dev/session/session_transport.hpp"
#include "hooks/runtime_hooks.hpp"
#include <Windows.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <array>

namespace {
unsigned originalCalls{}, nativeCalls{}, stops{};
DWORD nativeThread{};
bool shutdownDuringMessage{}, sawRetained{};
bool stopObservedBeforeClose{};
WNDPROC foreignOriginal{};
constexpr UINT kNative = WM_APP + 77;
constexpr UINT kForward = WM_APP + 78;
/** @summary Fail the owned fixture without interacting with game/user windows.
 * @param condition Required invariant.
 * @param message Failure detail.
 */
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
/** @summary Preserve close requests as vetoed requests, rather than claiming actual quit.
 * @param window Fixture receiver.
 * @param message Message number.
 * @param word First value.
 * @param value Second value.
 * @return Fixture-specific result or Windows default.
 */
LRESULT CALLBACK Original(HWND window, UINT message, WPARAM word, LPARAM value) {
    if (message == kForward || message == WM_CLOSE) {
        if (message == WM_CLOSE) stopObservedBeforeClose = stops != 0;
        ++originalCalls; return 123;
    }
    return DefWindowProcW(window, message, word, value);
}
/** @summary Simulate a later foreign subclass without stealing its procedure ownership.
 * @param window Fixture receiver.
 * @param message Message number.
 * @param word First value.
 * @param value Second value.
 * @return Captured chain result.
 */
LRESULT CALLBACK Foreign(HWND window, UINT message, WPARAM word, LPARAM value) {
    return CallWindowProcW(foreignOriginal, window, message, word, value);
}
/** @summary Consume only a private fixture message without any overlay context.
 * @param window Unused fixture handle.
 * @param message Message number.
 * @param word Unused first value.
 * @param value Unused second value.
 * @return Whether the native private message was consumed.
 */
bool Native(HWND, UINT message, WPARAM, LPARAM) noexcept {
    if (message != kNative) return false;
    ++nativeCalls;
    nativeThread = GetCurrentThreadId();
    if (shutdownDuringMessage) {
        sc13::platform::ShutdownGameWindowDispatch();
        sawRetained = !sc13::platform::GameWindowDispatchDrained();
    }
    return true;
}
/** @summary Observe conservative stop without waiting or shutting the fixture window. */
void Stop() noexcept { ++stops; }
/** @summary Create one invisible owned window.
 * @return Fixture handle or null on failure.
 */
HWND Create() { return CreateWindowW(L"SimoderCoreRouterFixture", L"hidden fixture", 0, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr); }
/** @summary Adapt the existing bounded native session consumer without a UI dependency.
 * @param window Receiver.
 * @param message Message number.
 * @param word Unused first value.
 * @param value Unused second value.
 * @return Whether the existing transport consumed its private wakeup.
 */
bool Session(HWND window, UINT message, WPARAM, LPARAM) noexcept { return sc13::dev::session::DispatchSessionMessage(window, message); }
/** @summary Write a bounded correlated test request in a new test-owned directory.
 * @param directory Owned fixture directory.
 * @param nonce Exact correlation identifier.
 */
void Submit(const std::filesystem::path& directory, const std::string& nonce) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::ofstream file(directory / L"session-control.request", std::ios::binary);
    file << "SIMODERSESSION1\n" << GetCurrentProcessId() << '\n' << nonce << '\n' << now + 10000 << "\nobserve\n\n\n\n\n";
    file.close();
    Require(static_cast<bool>(file), "fixture request write/close failed");
}
/** @summary Read the completed result corresponding to one fixture request.
 * @param directory Owned fixture directory.
 * @param nonce Exact correlation identifier.
 * @return Full correlated response text.
 */
std::string Reply(const std::filesystem::path& directory, const std::string& nonce) {
    std::ifstream file(directory / ("session-control-result-" + nonce + ".json"), std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
/** @summary Dispatch only already-posted fixture window messages without input or foreground changes. */
void Pump() { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message); }
/** @summary Verify generic hook retention and retirement without patching any process code. */
void ExerciseHookActivity() {
    sc13::hooks::RuntimeHookActivity activity;
    Require(activity.Drained() && activity.Open(), "hook activity initial admission");
    {
        const sc13::hooks::RuntimeHookActivity::Entry active(activity);
        Require(active.admitted() && !activity.Drained(), "active hook dependency not retained");
        activity.Close();
        Require(!activity.Open(), "hook activity reopened with a retained entry");
        const sc13::hooks::RuntimeHookActivity::Entry retiring(activity);
        Require(!retiring.admitted() && !activity.Drained(), "retirement did not retain forwarding entry");
    }
    Require(activity.Drained(), "nested hook entry leaked");
    // @summary A pre-entry callback is invisible to the counter; its later retirement forwarding must keep the trampoline.
    {
        const sc13::hooks::RuntimeHookActivity::Entry late(activity);
        Require(!late.admitted() && !activity.Drained(), "late hook entry regained optional service authority");
    }
    try {
        const sc13::hooks::RuntimeHookActivity::Entry unwinding(activity);
        throw std::runtime_error("owned fixture unwind");
    } catch (const std::runtime_error&) {}
    Require(activity.Drained(), "exception unwinding leaked hook entry");
}
}
/** @summary Exercise actual hidden Win32 subclassing, independent of rendering and game code.
 * @return Zero only after all owned-window checks pass.
 */
int main() {
    using namespace sc13::platform;
    HWND first{}, second{};
    const auto directory = std::filesystem::temp_directory_path() /
        (L"SimoderCoreDispatchFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        ExerciseHookActivity();
        WNDCLASSW descriptor{};
        descriptor.hInstance = GetModuleHandleW(nullptr); descriptor.lpfnWndProc = Original;
        descriptor.lpszClassName = L"SimoderCoreRouterFixture";
        Require(RegisterClassW(&descriptor) != 0, "class registration");
        first = Create(); second = Create();
        Require(first && second && !IsWindowVisible(first) && !IsWindowVisible(second), "hidden fixture creation");
        const auto foreground = GetForegroundWindow();
        ConfigureGameWindowDispatch(Native, Stop);
        Require(!BindGameWindow(GetDesktopWindow()), "foreign process accepted");
        Require(BindGameWindow(first), "owned bind");
        const auto old = CurrentGameWindow();
        Require(IsCurrentGameWindow(old) && old.process == GetCurrentProcessId() && old.thread == GetCurrentThreadId(), "identity");
        Require(BindGameWindow(first) && CurrentGameWindow().generation == old.generation, "idempotent bind");
        SendMessageW(first, kNative, 0, 0);
        Require(nativeCalls == 1 && originalCalls == 0, "native dispatch without overlay");
        Require(SendMessageW(first, kForward, 0, 0) == 123 && originalCalls == 1, "exact original forwarding");
        Require(SendMessageW(first, WM_CLOSE, 0, 0) == 123 && stops == 1 && stopObservedBeforeClose && IsWindow(first), "vetoed close still forwarded");
        Require(BindGameWindow(nullptr), "detach before render-thread registration");
        std::atomic_bool renderThreadBound{};
        std::thread render([&] { renderThreadBound.store(BindGameWindow(first)); }); render.join();
        Require(renderThreadBound && CurrentGameWindow().thread == GetCurrentThreadId(), "same-process render-thread registration rejected");
        PostMessageW(first, kNative, 0, 0);
        Pump();
        Require(nativeCalls == 2 && nativeThread == GetCurrentThreadId(), "render-thread registration changed execution thread");
        const auto renderBound = CurrentGameWindow();
        Require(BindGameWindow(second) && !IsCurrentGameWindow(old) && !IsCurrentGameWindow(renderBound) && stops == 3, "replacement does not invalidate old generation");
        SendMessageW(first, kNative, 0, 0);
        Require(nativeCalls == 2, "detached window dispatched");
        const auto secondIdentity = CurrentGameWindow();
        auto stale = secondIdentity; --stale.generation;
        Require(!IsCurrentGameWindow(stale), "stale generation accepted");
        foreignOriginal = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(second, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Foreign)));
        ShutdownGameWindowDispatch();
        Require(!BindGameWindow(first), "shutdown silently reopened attachment");
        Require(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(second, GWLP_WNDPROC)) == Foreign, "foreign subclass overwritten");
        Require(SendMessageW(second, kForward, 0, 0) == 123 && originalCalls == 3, "retained foreign chain lost original");
        SendMessageW(second, kNative, 0, 0);
        Require(nativeCalls == 2, "shutdown admits native callback");
        DestroyWindow(second); second = nullptr;
        ConfigureGameWindowDispatch(Native, Stop);
        Require(BindGameWindow(first), "rebind after shutdown");
        shutdownDuringMessage = true;
        SendMessageW(first, kNative, 0, 0);
        Require(sawRetained && GameWindowDispatchDrained() && !CurrentGameWindow().window, "callback lifetime drain");
        std::filesystem::create_directory(directory);
        ConfigureGameWindowDispatch(Session, nullptr);
        unsigned executions{};
        bool retainedSession{};
        sc13::dev::session::ConfigureSessionTransport([&](HWND, std::string_view) {
            ++executions;
            sc13::dev::session::ShutdownSessionTransport();
            retainedSession = !sc13::dev::session::SessionTransportDrained();
            return sc13::dev::session::ScriptReply{true, false, "{}", "", GetCurrentThreadId(), 0};
        }, [] { return true; });
        sc13::dev::session::BindSessionWindow(first);
        const std::string staleRequest(32, 'd');
        Submit(directory, staleRequest);
        const auto queuedIdentity = CurrentGameWindow();
        sc13::dev::session::PollSessionTransport(directory, true);
        std::error_code consumeError;
        const bool requestRemains = std::filesystem::exists(directory / L"session-control.request", consumeError);
        Require(!consumeError && !requestRemains, "fixture request was not consumed before the window generation changed");
        std::atomic_bool wrongThreadConsumed{};
        std::thread wrongExecutor([&] {
            wrongThreadConsumed.store(sc13::dev::session::DispatchSessionMessage(first,
                RegisterWindowMessageW(L"Simoder.NativeSession.Dispatch.v1")));
        });
        wrongExecutor.join();
        Require(wrongThreadConsumed && executions == 0, "wrong-thread native execution was not rejected");
        Require(BindGameWindow(nullptr), "detach before queued execution");
        sc13::dev::session::BindSessionWindow(first);
        const auto replacementIdentity = CurrentGameWindow();
        Require(replacementIdentity.generation != queuedIdentity.generation, "session fixture did not change window generation");
        Pump();
        sc13::dev::session::PollSessionTransport(directory, true);
        const auto staleReply = Reply(directory, staleRequest);
        if (executions != 0 || staleReply.find("\"success\":false") == std::string::npos) {
            std::cerr << "stale-request executions=" << executions << " queued-generation=" << queuedIdentity.generation
                << " replacement-generation=" << replacementIdentity.generation << " reply=" << staleReply
                << " transport=" << sc13::dev::session::SessionTransportJson() << '\n';
            std::wcerr << L"Retained fixture directory: " << directory.c_str() << L'\n';
        }
        Require(executions == 0, "stale queued request executed");
        Require(staleReply.find("\"success\":false") != std::string::npos, "stale queued request did not publish a correlated rejection");
        const std::string runningRequest(32, 'e');
        Submit(directory, runningRequest);
        sc13::dev::session::PollSessionTransport(directory, true);
        Pump();
        sc13::dev::session::PollSessionTransport(directory, true);
        Require(executions == 1 && retainedSession && sc13::dev::session::SessionTransportDrained() &&
            Reply(directory, runningRequest).find("\"success\":true") != std::string::npos, "running completion was freed or falsely cancelled");
        const std::string stoppedRequest(32, 'f');
        Submit(directory, stoppedRequest);
        sc13::dev::session::PollSessionTransport(directory, true);
        Pump();
        sc13::dev::session::PollSessionTransport(directory, true);
        Require(executions == 1 && Reply(directory, stoppedRequest).find("\"success\":false") != std::string::npos, "stopped transport accepted work");
        sc13::dev::session::ConfigureSessionTransport([&](HWND, std::string_view) {
            ++executions;
            return sc13::dev::session::ScriptReply{true, false, "{}", "", GetCurrentThreadId(), 0};
        }, [] { sc13::dev::session::ShutdownSessionTransport(); return true; });
        sc13::dev::session::BindSessionWindow(first);
        const std::string revokedAtCommit(32, 'a');
        Submit(directory, revokedAtCommit);
        sc13::dev::session::PollSessionTransport(directory, true);
        Pump();
        sc13::dev::session::PollSessionTransport(directory, true);
        Require(executions == 1 && Reply(directory, revokedAtCommit).find("\"success\":false") != std::string::npos,
            "revocation between permission and execution bypassed commit gate");
        Require(GetForegroundWindow() == foreground, "fixture changed focus");
        sc13::dev::session::ShutdownSessionTransport();
        ShutdownGameWindowDispatch();
        DestroyWindow(first); first = nullptr;
        std::filesystem::remove_all(directory);
        std::cout << "Core window router: hidden-window identity, chaining, generation, replacement and drain checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        ShutdownGameWindowDispatch();
        sc13::dev::session::ShutdownSessionTransport();
        if (first) DestroyWindow(first);
        if (second) DestroyWindow(second);
        std::wcerr << L"Fixture failure evidence retained: " << directory.c_str() << L'\n';
        std::error_code listingError;
        for (const auto& file : std::filesystem::directory_iterator(directory, listingError)) {
            std::error_code sizeError;
            const auto size = file.file_size(sizeError);
            std::wcerr << L"Owned failure file: " << file.path().filename().c_str() << L" attributes="
                << GetFileAttributesW(file.path().c_str()) << L" bytes=" << size << L" sizeError=" << sizeError.value() << L'\n';
            std::ifstream snapshot(file.path(), std::ios::binary);
            std::array<char, 4096> bytes{};
            snapshot.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            std::cerr << "Owned failure file bounded contents: " << std::string(bytes.data(), static_cast<std::size_t>(snapshot.gcount())) << '\n';
            snapshot.close();
            if (file.path().extension() == L".tmp") {
                auto destination = file.path(); destination.replace_extension();
                SetLastError(ERROR_SUCCESS);
                const auto moved = MoveFileExW(file.path().c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                const auto moveError = GetLastError();
                std::cerr << "Post-failure publication diagnostic only; assertion remains failed: moved=" << moved << " win32Error=" << moveError << '\n';
            } else if (file.path().filename() == L"session-control.request") {
                SetLastError(ERROR_SUCCESS);
                const auto removed = DeleteFileW(file.path().c_str());
                const auto removeError = GetLastError();
                std::cerr << "Post-failure request deletion diagnostic only; assertion remains failed: removed=" << removed << " win32Error=" << removeError << '\n';
            }
        }
        std::cerr << "Failure directory enumeration error=" << listingError.value() << '\n';
        std::cerr << error.what() << '\n'; return 1;
    }
}
