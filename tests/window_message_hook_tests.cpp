#include "ui/window_message_hook.hpp"
#include <iostream>

namespace {
unsigned routed{}, owned{}, forwarded{}, duplicate{}, characters{};
/** @summary Observe synthetic fixture input solely in the test-owned window queue. */
bool Input(HWND window, UINT message, WPARAM, LPARAM) {
    ++routed;
    if (message == WM_LBUTTONDOWN) {
        ++owned;
        SendMessageW(window, WM_APP + 99, 0, 0);
        return true;
    }
    if (message == WM_KEYDOWN) return true;
    if (message == WM_CHAR) { ++characters; return true; }
    return false;
}
/** @summary Verify unowned input can reach dispatch without a second delivery to the overlay. */
LRESULT CALLBACK Window(HWND window, UINT message, WPARAM word, LPARAM value) {
    if (message == WM_APP + 99) sc13::ui::RouteUnqueuedWindowInput(window, message, word, value);
    if (message == WM_RBUTTONDOWN) {
        ++forwarded;
        if (sc13::ui::IsObservedWindowInput(window, message)) ++duplicate;
    }
    return DefWindowProcW(window, message, word, value);
}
}
/** @summary Exercise queue filtering, duplicate suppression and lifecycle cleanup without a game process. */
int main() {
    WNDCLASSW descriptor{};
    descriptor.hInstance = GetModuleHandleW(nullptr);
    descriptor.lpfnWndProc = Window;
    descriptor.lpszClassName = L"SimoderWindowQueueTest";
    if (!RegisterClassW(&descriptor)) return 1;
    HWND window = CreateWindowW(descriptor.lpszClassName, L"fixture", 0, 0, 0, 100, 100, nullptr, nullptr, descriptor.hInstance, nullptr);
    std::string error;
    if (!sc13::ui::InitializeWindowMessageHook(window, Input, error)) { std::cerr << error; return 1; }
    PostMessageW(window, WM_LBUTTONDOWN, 0, 0);
    PostMessageW(window, WM_RBUTTONDOWN, 0, 0);
    PostMessageW(window, WM_RBUTTONDOWN, 0, 0);
    PostMessageW(window, WM_KEYDOWN, 'A', 1);
    PostMessageW(window, WM_APP + 5, 0, 0);
    MSG message{};
    if (!PeekMessageW(&message, window, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_NOREMOVE) || routed) return 1;
    MSG batch[32]{};
    unsigned count{};
    while (count < 32 && PeekMessageW(&batch[count], nullptr, 0, 0, PM_REMOVE)) ++count;
    for (unsigned index = 0; index < count; ++index) DispatchMessageW(&batch[index]);
    sc13::ui::RouteUnqueuedWindowInput(window, WM_KILLFOCUS, 0, 0);
    sc13::ui::ShutdownWindowMessageHook();
    if (sc13::ui::RouteUnqueuedWindowInput(window, WM_KILLFOCUS, 0, 0)) return 1;
    PostMessageW(window, WM_RBUTTONDOWN, 0, 0);
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    DestroyWindow(window);
    UnregisterClassW(descriptor.lpszClassName, descriptor.hInstance);
    if (routed != 7 || owned != 1 || forwarded != 3 || duplicate != 2 || characters != 1) {
        std::cerr << "routed=" << routed << " owned=" << owned << " forwarded=" << forwarded << " observed=" << duplicate << " chars=" << characters;
        return 1;
    }
    std::cout << "Owned queue input consumed once; unowned dispatch, peek and shutdown preserved.\n";
}
