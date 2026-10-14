#include "dev/session/session_transport.hpp"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <stdexcept>

/** @summary Dispatch private wakeups on a hidden fixture window, without foreground activation or input messages. */
LRESULT CALLBACK FixtureProcedure(HWND window, UINT message, WPARAM word, LPARAM value) {
    if (sc13::dev::session::DispatchSessionMessage(window, message)) return 0;
    return DefWindowProcW(window, message, word, value);
}
/** @summary Require protocol/thread conditions before interpreting a fixture result as success. */
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
/** @summary Use the native transport's Unix-millisecond deadline coordinate. */
std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
/** @summary Publish a complete fixture request before waking the private window pump. */
void Submit(const std::filesystem::path& directory, const std::string& nonce, std::int64_t expires) {
    std::ofstream file(directory / L"session-control.request", std::ios::binary);
    file << "SIMODERSESSION1\n" << GetCurrentProcessId() << '\n' << nonce << '\n' << expires << "\nobserve\n\n\n\n\n";
}
/** @summary Read a completed correlated fixture acknowledgement rather than only a queued wakeup. */
std::string Reply(const std::filesystem::path& directory, const std::string& nonce) {
    std::ifstream input(directory / ("session-control-result-" + nonce + ".json"), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
/** @summary Exercise actual Win32 message delivery, service revocation and expiration without invoking game code. */
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"SimoderSessionFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    HWND window{};
    bool permitted = true;
    unsigned int executed{};
    try {
        std::filesystem::create_directories(directory);
        WNDCLASSW definition{};
        definition.lpfnWndProc = FixtureProcedure;
        definition.hInstance = GetModuleHandleW(nullptr);
        definition.lpszClassName = L"SimoderNativeTransportFixture";
        Require(RegisterClassW(&definition) != 0, "Fixture window class unavailable");
        window = CreateWindowW(definition.lpszClassName, L"Hidden native transport fixture", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, definition.hInstance, nullptr);
        Require(window && !IsWindowVisible(window) && GetForegroundWindow() != window, "Fixture must remain hidden and inactive");
        sc13::dev::session::ConfigureSessionTransport([&executed](HWND target, std::string_view packet) {
            Require(GetWindowThreadProcessId(target, nullptr) == GetCurrentThreadId(), "Fixture executor ran on the wrong thread");
            Require(packet.find("\"op\":\"observe\"") != std::string_view::npos, "Typed operation changed");
            ++executed;
            return sc13::dev::session::ScriptReply{true, false, "{\"ok\":true,\"result\":{\"phase\":\"main_menu\"}}", "", GetCurrentThreadId(), 0};
        }, [&permitted] { return permitted; });
        sc13::dev::session::BindSessionWindow(window);
        const std::string first(32, 'a');
        Submit(directory, first, Now() + 10000);
        sc13::dev::session::PollSessionTransport(directory, true);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        sc13::dev::session::PollSessionTransport(directory, true);
        Require(executed == 1 && Reply(directory, first).find("\"success\":true") != std::string::npos,
            "Inactive window did not execute and acknowledge the command");
        const std::string revoked(32, 'b');
        Submit(directory, revoked, Now() + 10000);
        sc13::dev::session::PollSessionTransport(directory, true);
        permitted = false;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        sc13::dev::session::PollSessionTransport(directory, false);
        Require(executed == 1 && Reply(directory, revoked).find("\"success\":false") != std::string::npos, "Revoked command crossed the executor boundary");
        const std::string expired(32, 'c');
        permitted = true;
        Submit(directory, expired, Now() - 1);
        sc13::dev::session::PollSessionTransport(directory, true);
        Require(executed == 1 && Reply(directory, expired).find("\"success\":false") != std::string::npos, "Expired command was executed");
        sc13::dev::session::ShutdownSessionTransport();
        DestroyWindow(window); window = nullptr;
        std::filesystem::remove_all(directory);
        std::cout << "Native transport: hidden-window delivery, thread identity, correlated replies, lease revocation and expiration passed.\n";
        return 0;
    } catch (const std::exception& error) {
        sc13::dev::session::ShutdownSessionTransport();
        if (window) DestroyWindow(window);
        std::error_code ignored; std::filesystem::remove_all(directory, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
